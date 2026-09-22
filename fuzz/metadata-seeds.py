#!/usr/bin/env python3
"""Seed corpora for the metadata-parser harnesses (roadmap VX3).

Two subcommands, both driven from fuzz/generate-seeds.sh:

  extract <out-dir> <stream.ec3>...
      Pulls the real EMDF containers, and the OAMD and JOC payloads inside
      them, out of Atmos streams generate-seeds.sh has just encoded, and
      writes them to fuzz_emdf_parse/, fuzz_oamd_parse/ and fuzz_joc_parse/.
      These bytes are not reachable any other way: nothing in ac3cli dumps a
      raw payload, and the container does not sit at a byte boundary inside
      the frame that carries it (put_skip_field writes it 8 bits at a time
      from wherever the preceding audio happened to end), so it has to be
      located by the same bit-by-bit sync scan emdf::parse_container itself
      does and repacked from that offset.

  adm <out-dir>
      Synthesises BW64/RF64 fixtures for fuzz_adm_parse. Nothing ac3cli
      produces is an ADM file, so unlike every other seed corpus here this
      one cannot come from the encoder - these mirror the fixtures
      tests/adm/test_adm.cpp builds in memory (BS.2088-1 chunk layout,
      BS.2076-2 ADM XML), which are the shapes the reader is known to accept
      and therefore the ones worth mutating from.

The EMDF container syntax below is a deliberate second implementation of
src/forge/src/emdf/emdf.cpp's own reader, in a different language, for the
narrow purpose of finding payload boundaries. It is not a check on that
reader and is not authoritative: if the two ever disagree, this script simply
extracts fewer (or worse) seeds, which shows up as a smaller corpus rather
than as a wrong test result.
"""

from __future__ import annotations

import pathlib
import struct
import sys

# --- EMDF container reader (mirrors src/forge/src/emdf/emdf.cpp) -----------

EMDF_SYNC = 0x5838
PAYLOAD_ID_OAMD = 11
PAYLOAD_ID_JOC = 14


class Bits:
    """MSB-first bit reader with ac3::BitReader's sticky-overflow behaviour."""

    def __init__(self, data: bytes) -> None:
        self.data = data
        self.total = len(data) * 8
        self.pos = 0
        self.overflowed = False

    def read(self, count: int) -> int:
        value = 0
        for _ in range(count):
            if self.pos >= self.total:
                self.overflowed = True
                value <<= 1
                continue
            byte = self.data[self.pos >> 3]
            value = (value << 1) | ((byte >> (7 - (self.pos & 7))) & 1)
            self.pos += 1
        return value

    def skip(self, count: int) -> None:
        self.read(count)


def read_variable_bits(bits: Bits, group_bits: int) -> int:
    """§H.2.1.2.1: groups of `group_bits`, MSB group first, read_more between."""
    value = 0
    while True:
        value += bits.read(group_bits)
        if bits.read(1) == 0:
            return value
        value <<= group_bits
        value += 1 << group_bits


def read_payload_config(bits: Bits) -> bool:
    """§H.2.2.3 restricted to TS 103 420 Table 56, exactly as the C++ reader is."""
    if bits.read(1) != 0:  # smploffste
        return False
    if bits.read(1) != 0:  # duratione
        return False
    if bits.read(1) != 1:  # groupide
        return False
    read_variable_bits(bits, 2)  # groupid
    if bits.read(1) != 0:  # codecdatae
        return False
    if bits.read(1) != 0:  # discard_unknown_payload
        return False
    if bits.read(1) != 1:  # payload_frame_aligned
        return False
    bits.skip(1 + 1 + 5 + 2)  # create_duplicate, remove_duplicate, priority, proc_allowed
    return True


def parse_container(data: bytes) -> tuple[int, list[tuple[int, bytes]]] | None:
    """Reads a container that starts at bit 0 of `data`.

    Returns (container byte length including the 4-byte sync+length header,
    [(payload id, payload bytes)]) or None if these bytes are not one.
    """
    bits = Bits(data)
    if bits.read(16) != EMDF_SYNC:
        return None
    length = bits.read(16)
    if 32 + length * 8 > bits.total:
        return None
    if bits.read(2) != 0:  # emdf_version
        return None
    bits.skip(3)  # key_id

    payloads: list[tuple[int, bytes]] = []
    while True:
        payload_id = bits.read(5)
        if payload_id == 0:
            break
        if payload_id == 0x1F:
            return None
        if not read_payload_config(bits):
            return None
        size = read_variable_bits(bits, 8)
        if size * 8 > bits.total - bits.pos:
            return None
        payloads.append((payload_id, bytes(bits.read(8) for _ in range(size))))

    if bits.read(2) != 0b10 or bits.read(2) != 0b01:
        return None
    bits.skip(32 + 8)  # protection_bits_primary + _secondary
    if bits.overflowed:
        return None
    return 4 + length, payloads


# --- Locating containers inside an E-AC-3 stream ---------------------------


def shifted(frame: bytes, offset: int) -> bytes:
    """`frame` rotated left by `offset` bits (0..7), zero-filled at the tail."""
    if offset == 0:
        return frame
    width = len(frame) * 8
    value = int.from_bytes(frame, "big") << offset
    return (value & ((1 << width) - 1)).to_bytes(len(frame), "big")


def containers_in(frame: bytes):
    """Every parseable EMDF container in `frame`, at any bit alignment.

    The sync scan is done eight bytes-strings at a time rather than one bit at
    a time: bytes.find over a pre-shifted copy is the same search
    emdf::parse_container's find_sync_bit does bit by bit, several orders of
    magnitude faster in Python, and a false positive costs only a failed
    parse.
    """
    seen: set[bytes] = set()
    for offset in range(8):
        buffer = shifted(frame, offset)
        index = 0
        while True:
            index = buffer.find(b"\x58\x38", index)
            if index < 0:
                break
            parsed = parse_container(buffer[index:])
            if parsed is not None:
                container = buffer[index : index + parsed[0]]
                if container not in seen:
                    seen.add(container)
                    yield container, parsed[1]
            index += 1


def syncframes(stream: bytes):
    """Walks an E-AC-3 stream frame by frame, the way ac3::split_frames does."""
    offset = 0
    while offset + 6 <= len(stream):
        if stream[offset] != 0x0B or stream[offset + 1] != 0x77:
            return
        bsid = stream[offset + 5] >> 3
        if not 11 <= bsid <= 16:
            return  # AC-3, or a bsid with no reading here: no EMDF to find
        frame_bytes = ((((stream[offset + 2] & 0x07) << 8) | stream[offset + 3]) + 1) * 2
        if frame_bytes < 6 or offset + frame_bytes > len(stream):
            return
        yield stream[offset : offset + frame_bytes]
        offset += frame_bytes


def cmd_extract(out_root: pathlib.Path, streams: list[pathlib.Path]) -> int:
    dirs = {
        "emdf": out_root / "fuzz_emdf_parse",
        "oamd": out_root / "fuzz_oamd_parse",
        "joc": out_root / "fuzz_joc_parse",
    }
    for directory in dirs.values():
        directory.mkdir(parents=True, exist_ok=True)

    # Keyed by content so the same container repeated across 100 frames of one
    # stream contributes one seed, not 100 - libFuzzer would merge them away
    # anyway, and a corpus directory of identical files is just noise in git.
    # Successive frames of one stream still differ (an object moves, so its
    # OAMD position fields and JOC coefficients change), which is why a
    # per-stream cap is needed on top: a second's worth of Atmos is ~30
    # frames, and the fiftieth position update teaches the mutation engine
    # nothing the sixth did not.
    per_stream_cap = 6
    written = {kind: {} for kind in dirs}
    for path in streams:
        stream = path.read_bytes()
        taken = dict.fromkeys(dirs, 0)
        for frame in syncframes(stream):
            if all(count >= per_stream_cap for count in taken.values()):
                break
            for container, payloads in containers_in(frame):
                found = {"emdf": [container]}
                for payload_id, payload in payloads:
                    kind = {PAYLOAD_ID_OAMD: "oamd", PAYLOAD_ID_JOC: "joc"}.get(payload_id)
                    if kind is not None:
                        found.setdefault(kind, []).append(payload)
                for kind, contents in found.items():
                    for content in contents:
                        if taken[kind] >= per_stream_cap or content in written[kind]:
                            continue
                        written[kind][content] = f"{path.stem}-{kind}"
                        taken[kind] += 1

    total = 0
    for kind, directory in dirs.items():
        for index, (content, stem) in enumerate(written[kind].items()):
            (directory / f"{stem}-{index:02d}.bin").write_bytes(content)
            total += 1
        print(f"    {directory.name:<20} {len(written[kind])} files")
    return 0 if total else 1


# --- BW64/ADM fixtures for fuzz_adm_parse ----------------------------------

# BS.2076-2 Annex 1: one Objects-type channel, fully cross-referenced. Kept in
# step with tests/adm/test_adm.cpp's kCarAdmXml, which is the document the
# reader is actually tested against.
CAR_ADM_XML = b"""<?xml version="1.0" encoding="UTF-8"?>
<audioFormatExtended version="ITU-R_BS.2076-2">
  <audioProgramme audioProgrammeID="APR_1001" audioProgrammeName="CarsSounds">
    <audioContentIDRef>ACO_1001</audioContentIDRef>
  </audioProgramme>
  <audioContent audioContentID="ACO_1001" audioContentName="Cars">
    <audioObjectIDRef>AO_1001</audioObjectIDRef>
  </audioContent>
  <audioObject audioObjectID="AO_1001" audioObjectName="Car" start="00:00:00.00000">
    <audioPackFormatIDRef>AP_00031001</audioPackFormatIDRef>
    <audioTrackUIDRef>ATU_00000001</audioTrackUIDRef>
  </audioObject>
  <audioPackFormat audioPackFormatID="AP_00031001" audioPackFormatName="Car" typeLabel="0003" typeDefinition="Objects">
    <audioChannelFormatIDRef>AC_00031001</audioChannelFormatIDRef>
  </audioPackFormat>
  <audioChannelFormat audioChannelFormatID="AC_00031001" audioChannelFormatName="Car1" typeLabel="0003" typeDefinition="Objects">
    <audioBlockFormat audioBlockFormatID="AB_00031001_00000001">
      <position coordinate="azimuth">-22.5</position>
      <position coordinate="elevation">5.0</position>
      <position coordinate="distance">1.0</position>
      <width>12.5</width>
    </audioBlockFormat>
  </audioChannelFormat>
  <audioStreamFormat audioStreamFormatID="AS_00031001" audioStreamFormatName="PCM_Car1" formatLabel="0001" formatDefinition="PCM">
    <audioChannelFormatIDRef>AC_00031001</audioChannelFormatIDRef>
    <audioTrackFormatIDRef>AT_00031001_01</audioTrackFormatIDRef>
  </audioStreamFormat>
  <audioTrackFormat audioTrackFormatID="AT_00031001_01" audioTrackFormatName="PCM_Car1" formatLabel="0001" formatDefinition="PCM">
    <audioStreamFormatIDRef>AS_00031001</audioStreamFormatIDRef>
  </audioTrackFormat>
  <audioTrackUID UID="ATU_00000001" sampleRate="48000" bitDepth="16">
    <audioTrackFormatIDRef>AT_00031001_01</audioTrackFormatIDRef>
    <audioPackFormatIDRef>AP_00031001</audioPackFormatIDRef>
  </audioTrackUID>
</audioFormatExtended>
"""

# BS.2076-2 §5.4.3.1 Table 12: a DirectSpeakers channel, the bed-shaped half
# of the model. The "9001" ID suffix avoids libadm's pre-loaded common
# definitions - see test_adm.cpp's own note on the collision that caused.
SPEAKER_ADM_XML = b"""<?xml version="1.0" encoding="UTF-8"?>
<audioFormatExtended version="ITU-R_BS.2076-2">
  <audioChannelFormat audioChannelFormatID="AC_00019001" audioChannelFormatName="FrontLeft" typeLabel="0001" typeDefinition="DirectSpeakers">
    <audioBlockFormat audioBlockFormatID="AB_00019001_00000001">
      <speakerLabel>M+030</speakerLabel>
      <position coordinate="azimuth">30.0</position>
      <position coordinate="elevation">0.0</position>
      <position coordinate="distance">1.0</position>
    </audioBlockFormat>
  </audioChannelFormat>
</audioFormatExtended>
"""


def chunk(fourcc: bytes, content: bytes, declared: int | None = None) -> bytes:
    """BS.2088-1 §4: id, 32-bit size, content, pad byte when odd."""
    size = len(content) if declared is None else declared
    return fourcc + struct.pack("<I", size) + content + (b"\0" if len(content) % 2 else b"")


def fmt_chunk(channels: int = 1, sample_rate: int = 48000, bits: int = 16) -> bytes:
    block_align = channels * (bits // 8)
    return struct.pack("<HHIIHH", 1, channels, sample_rate, sample_rate * block_align,
                       block_align, bits)


def chna_chunk() -> bytes:
    """BS.2088-1 §8.3.1, one track and one UID, matching the one-channel fmt."""
    return (struct.pack("<HHH", 1, 1, 1) + b"ATU_00000001" + b"AT_00031001_01" +
            b"AP_00031001" + b"\0")


def pcm16(frames: int = 4) -> bytes:
    return b"".join(struct.pack("<h", (frame * 4000) - 12000) for frame in range(frames))


def build_riff(axml: bytes, with_chna: bool = True) -> bytes:
    body = chunk(b"fmt ", fmt_chunk())
    if with_chna:
        body += chunk(b"chna", chna_chunk())
    if axml:
        body += chunk(b"axml", axml)
    body += chunk(b"data", pcm16())
    return b"RIFF" + struct.pack("<I", 4 + len(body)) + b"WAVE" + body


def build_rf64(axml: bytes) -> bytes:
    """BS.2088-1 §§2.4/4: <data>'s 32-bit size is 0xFFFFFFFF, resolved by <ds64>."""
    data = pcm16()
    fmt = chunk(b"fmt ", fmt_chunk())
    chna = chunk(b"chna", chna_chunk())
    axml_chunk = chunk(b"axml", axml)
    data_chunk = chunk(b"data", data, declared=0xFFFFFFFF)
    ds64_content = struct.pack("<QQQI", 0, len(data), 0, 0)
    ds64 = chunk(b"ds64", ds64_content)
    bw64_size = 4 + len(ds64) + len(fmt) + len(chna) + len(axml_chunk) + len(data_chunk)
    ds64 = chunk(b"ds64", struct.pack("<QQQI", bw64_size, len(data), 0, 0))
    return (b"RF64" + struct.pack("<I", 0xFFFFFFFF) + b"WAVE" + ds64 + fmt + chna +
            axml_chunk + data_chunk)


def cmd_adm(out_root: pathlib.Path) -> int:
    directory = out_root / "fuzz_adm_parse"
    directory.mkdir(parents=True, exist_ok=True)
    fixtures = {
        "riff-objects.wav": build_riff(CAR_ADM_XML),
        "riff-speakers.wav": build_riff(SPEAKER_ADM_XML, with_chna=False),
        "rf64-objects.wav": build_rf64(CAR_ADM_XML),
        # No <axml> at all: BS.2088-1 §9 rule 2 makes it optional, so this is
        # the container half of the reader with the XML half switched off -
        # the shape a plain WAV takes on the way in.
        "riff-no-axml.wav": build_riff(b""),
    }
    for name, content in fixtures.items():
        (directory / name).write_bytes(content)
    print(f"    {directory.name:<20} {len(fixtures)} files")
    return 0


def main(argv: list[str]) -> int:
    if len(argv) >= 3 and argv[1] == "extract":
        return cmd_extract(pathlib.Path(argv[2]), [pathlib.Path(p) for p in argv[3:]])
    if len(argv) == 3 and argv[1] == "adm":
        return cmd_adm(pathlib.Path(argv[2]))
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
