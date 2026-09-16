"""A scripted Sendspin player on aiosendspin 9.1.1, standing in for Sendspin's reference player.

planning/hearth-sendspin-extension.md, Decisions: A4's exit plays PCM, FLAC and Opus from Hearth's
server to this player, which tools/sendspin/aiosendspin_exit.py runs. The player listens for a
server on a port, prints its URL, client_id and the SP:0 token that pairs it by its pairing PSK,
takes player@v1 in the one codec it offers, and decodes every chunk with the SDK's decoders. When
the server goes, it writes what it decoded to a WAV file and each chunk's timestamp and frame count
to a CSV file.

aiosendspin 9.1.1 decodes PCM and FLAC only, and refuses Opus in the formats a player offers (the
extension page's C32). With --codec opus this player widens the SDK's list of decodable codecs
before the client is made, and decodes Opus with the SDK's own PyAV decoder.

Needs Python 3.12 or later and tools/sendspin/requirements.txt.
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import json
import logging
import socket
import sys
import wave
from dataclasses import dataclass, field
from pathlib import Path

from aiosendspin.audio.codecs import create_decoder
from aiosendspin.client import AudioFormat, ClientListener, SendspinClient
from aiosendspin.client import client as client_module
from aiosendspin.client import connection as connection_module
from aiosendspin.models.core import DeviceInfo
from aiosendspin.models.player import ClientHelloPlayerSupport, SupportedAudioFormat
from aiosendspin.models.types import AudioCodec, PlayerCommand, Roles
from aiosendspin.noise.keys import Identity, generate_psk, psk_id_for
from aiosendspin.noise.pairing_token import PSKPairingToken, encode_token
from aiosendspin.noise.trust_store import InMemoryClientPairingStore, PairingPsk

SAMPLE_RATE = 48000
CHANNELS = 2
BIT_DEPTH = 16
PATH = "/sendspin"


def allow_opus() -> None:
    """Let the SDK offer and start Opus, which 9.1.1 refuses although PyAV decodes it (C32)."""
    widened = (*connection_module.DECODABLE_CODECS, AudioCodec.OPUS)
    connection_module.DECODABLE_CODECS = widened
    client_module.DECODABLE_CODECS = widened


def free_port(host: str) -> int:
    """A port nothing listens on now, for the listener to take."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind((host, 0))
        return int(probe.getsockname()[1])


@dataclass
class Received:
    """One stream as the player took it."""

    codec: str = ""
    # Each chunk's server timestamp in microseconds and the frames it decoded to.
    chunks: list[tuple[int, int]] = field(default_factory=list)
    pcm: bytearray = field(default_factory=bytearray)
    streams: int = 0
    ended: bool = False


class ScriptedPlayer:
    """An aiosendspin 9.1.1 client with the player role, in one codec, waiting for a server."""

    def __init__(self, codec: AudioCodec, host: str, port: int, name: str) -> None:
        self.codec = codec
        self.host = host
        self.port = port
        self.name = name
        self.identity = Identity.generate()
        self.pairing_psk = generate_psk()
        self.received = Received()
        self.closed = asyncio.Event()
        self._decoder: object | None = None
        self._format: tuple[object, ...] | None = None
        self._client: SendspinClient | None = None
        self._listener: ClientListener | None = None

    @property
    def url(self) -> str:
        return f"ws://{self.host}:{self.port}{PATH}"

    @property
    def token(self) -> str:
        return encode_token(
            PSKPairingToken(client_id=self.identity.peer_id, pairing_psk=self.pairing_psk)
        )

    async def start(self) -> None:
        if self.codec is AudioCodec.OPUS:
            allow_opus()
        store = InMemoryClientPairingStore()
        await store.set_pairing_psk(PairingPsk(psk_id_for(self.pairing_psk), self.pairing_psk))
        # 9.1.1 lists volume and mute in the hello's support object, and takes none of them in
        # client/state (C28).
        commands = [PlayerCommand.VOLUME, PlayerCommand.MUTE]
        self._client = SendspinClient(
            self.identity,
            self.name,
            [Roles.PLAYER],
            pairing_store=store,
            device_info=DeviceInfo(
                product_name="aiosendspin scripted player",
                manufacturer="AC3Forge",
                software_version="aiosendspin 9.1.1",
            ),
            player_support=ClientHelloPlayerSupport(
                supported_formats=[
                    SupportedAudioFormat(
                        codec=self.codec,
                        channels=CHANNELS,
                        sample_rate=SAMPLE_RATE,
                        bit_depth=BIT_DEPTH,
                    )
                ],
                buffer_capacity=8 * 1024 * 1024,
                supported_commands=commands,
            ),
        )
        self._client.add_stream_start_listener(lambda _message: self._on_stream_start())
        self._client.add_audio_chunk_listener(self._on_audio)
        self._client.add_stream_end_listener(lambda _roles: self._on_stream_end())
        self._listener = ClientListener(
            self.identity.peer_id,
            self._on_connection,
            port=self.port,
            path=PATH,
            host=self.host,
            advertise_mdns=False,
            client_name=self.name,
        )
        await self._listener.start()

    async def stop(self) -> None:
        if self._client is not None:
            with contextlib.suppress(Exception):
                await self._client.disconnect()
        if self._listener is not None:
            await self._listener.stop()

    async def _on_connection(self, ws: object) -> None:
        assert self._client is not None
        try:
            await self._client.attach_websocket(ws)  # type: ignore[arg-type]
        finally:
            self.closed.set()

    def _on_stream_start(self) -> None:
        self.received.streams += 1

    def _on_audio(self, timestamp_us: int, payload: bytes, audio_format: AudioFormat) -> None:
        pcm_format = audio_format.pcm_format
        key = (
            audio_format.codec,
            pcm_format.sample_rate,
            pcm_format.channels,
            pcm_format.bit_depth,
            audio_format.codec_header,
        )
        if key != self._format:
            self._flush()
            self._format = key
            self.received.codec = audio_format.codec.value
            self._decoder = create_decoder(
                audio_format.codec.value,
                sample_rate=pcm_format.sample_rate,
                bit_depth=pcm_format.bit_depth,
                channels=pcm_format.channels,
                codec_header=audio_format.codec_header,
            )
        decoded = self._decoder.decode(payload)  # type: ignore[union-attr]
        frame_bytes = pcm_format.channels * (pcm_format.bit_depth // 8)
        self.received.chunks.append((timestamp_us, len(decoded) // frame_bytes))
        self.received.pcm += decoded

    def _on_stream_end(self) -> None:
        self._flush()
        self.received.ended = True

    def _flush(self) -> None:
        if self._decoder is not None:
            tail = self._decoder.flush()  # type: ignore[attr-defined]
            if tail:
                self.received.pcm += tail
        self._decoder = None
        self._format = None

    def write(self, directory: Path) -> None:
        """The decoded PCM as decoded.wav, and chunks.csv with each chunk's timestamp and frames."""
        directory.mkdir(parents=True, exist_ok=True)
        with wave.open(str(directory / "decoded.wav"), "wb") as out:
            out.setnchannels(CHANNELS)
            out.setsampwidth(BIT_DEPTH // 8)
            out.setframerate(SAMPLE_RATE)
            out.writeframes(bytes(self.received.pcm))
        lines = ["timestamp_us,frames"]
        lines += [f"{timestamp},{frames}" for timestamp, frames in self.received.chunks]
        (directory / "chunks.csv").write_text("\n".join(lines) + "\n", encoding="utf-8")


async def run(arguments: argparse.Namespace) -> int:
    player = ScriptedPlayer(
        AudioCodec(arguments.codec),
        arguments.host,
        arguments.port or free_port(arguments.host),
        arguments.name,
    )
    await player.start()
    started = {"url": player.url, "client_id": player.identity.peer_id, "token": player.token}
    print(json.dumps(started), flush=True)
    try:
        await player.closed.wait()
    finally:
        await player.stop()
        if arguments.out is not None:
            player.write(Path(arguments.out))
    print(
        json.dumps(
            {
                "codec": player.received.codec,
                "streams": player.received.streams,
                "chunks": len(player.received.chunks),
                "frames": len(player.received.pcm) // (CHANNELS * BIT_DEPTH // 8),
                "ended": player.received.ended,
            }
        ),
        flush=True,
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--codec", choices=[codec.value for codec in AudioCodec], default="pcm")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=0, help="0 for a free port")
    parser.add_argument("--name", default="aiosendspin scripted player")
    parser.add_argument("--out", help="where decoded.wav and chunks.csv go when the server leaves")
    parser.add_argument("--verbose", action="store_true")
    arguments = parser.parse_args()
    logging.basicConfig(level=logging.DEBUG if arguments.verbose else logging.WARNING)
    return asyncio.run(run(arguments))


if __name__ == "__main__":
    sys.exit(main())
