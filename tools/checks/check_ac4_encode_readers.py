"""Readers outside this project against ac3cli's AC-4 encoder: MediaInfo and DEE's muxer.

planning/ac4.md, the encoder's ladder, item 3, as phases E1 to E3 need it. For each configuration
below, mono to 5.1 and the experimental options of 5.X and 7.X, `ac3cli ac4-encode` writes a raw
stream and an MP4 file, and:

  MediaInfo  its frame-by-frame trace (`--Details=1`) of the raw stream holds the values the encoder
             was configured with, field by field, in every frame it details: the sync word and
             frame_size, the table of contents (bitstream_version, sequence_counter, wait_frames,
             fs_index, frame_rate_index, b_iframe_global), the presentation and its substream
             group's channel mode, the presentation substream's dialnorm_bits and its empty DRC and
             loudness fields, the audio substream's metadata() with no dialogue enhancement or EMDF
             payloads, and each frame's crc_word, computed again here;
  DEE's muxer  dee_mp4muxer takes the raw stream and writes an MP4 file whose 'dac4' box is the one
             the encoder's MP4 file carries; for the 3/2/2 layout, but for channel group 4, which
             the muxer leaves out (src/ac4enc/ERRATA.md, "The 3/2/2 layout's top front pair").

MediaInfo and DEE's muxer come from DEE's install, so this runs locally, never in CI
(tools/generators/gen_ac4_baseline.py's DEE_DIR).

Usage:
    python tools/checks/check_ac4_encode_readers.py --cli ac3cli.exe [--dee-dir DIR] [--work DIR]
"""

import argparse
import random
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO / "tools" / "ci"))
import fuzz_ac4_encoder_space as space  # noqa: E402  (sync_frames, crc16)
import fuzz_encoder_space as ac3space  # noqa: E402  (write_wav)

DEE_DIR = Path(r"C:\Program Files\Dolby\Dolby Media Encoder\resources\dee-dir")
IFRAME_INTERVAL = 24  # the encoder's default, which ac4-encode keeps

# (channels, sample rate, kbps, dialnorm, ac4-encode's options)
CONFIGURATIONS = [
    (2, 48000, 192, 24, ()),
    (2, 48000, 64, 31, ()),
    (1, 48000, 96, 18, ()),
    (2, 44100, 256, 27, ()),
    (1, 44100, 48, 1, ()),
    (6, 48000, 384, 27, ()),
    (6, 48000, 192, 20, ()),
    (5, 44100, 320, 31, ()),
    (6, 48000, 256, 24, ("experimental=coding-configs",)),
    (8, 48000, 640, 24, ("experimental=7x-back",)),
    (8, 48000, 448, 24, ("experimental=7x-wide",)),
    (7, 48000, 448, 24, ("experimental=7x-top-front",)),
    (8, 48000, 512, 24, ("experimental=7x-top-front",)),
]

# The channel mode MediaInfo names for each configuration: Part 1 Table 88's, as its trace
# prints it.
MODES = {1: "Mono", 2: "Stereo", 5: "5.0", 6: "5.1"}
SEVEN_X_MODES = {"7x-back": "3/4/0", "7x-wide": "5/2/0", "7x-top-front": "3/2/2"}


def channel_mode(channels, options):
    for option in options:
        for tool, layout in SEVEN_X_MODES.items():
            if tool in option:
                return f"7.{channels - 7} {layout}" + (".1" if channels == 8 else "")
    return MODES[channels]

# The fields check_frame() holds to the configuration.
FIELDS = ("sync_word", "frame_size", "bitstream_version", "sequence_counter", "b_wait_frames",
          "wait_frames", "fs_index", "frame_rate_index", "b_iframe_global", "presentation_version",
          "channel_mode", "dialnorm_bits", "b_further_loudness_info", "drc_metadata_size_value",
          "b_drc_present", "tools_metadata_size", "b_de_data_present", "b_emdf_payloads_substream",
          "crc_word")

LINE = re.compile(r"^([0-9A-F]{4,})\s+(.*?):\s+(.*)$")
FRAME = re.compile(r"^([0-9A-F]{4,}) ac4_syncframe - (\d+) ")


def run(command):
    result = subprocess.run([str(c) for c in command], capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise SystemExit(f"{' '.join(str(c) for c in command)} failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr}")
    return result.stdout


def mediainfo_frames(mediainfo, stream):
    """Per sync frame MediaInfo details, a list of (name, value) pairs in order."""
    frames = []
    for line in run([mediainfo, "--Details=1", stream]).splitlines():
        if FRAME.match(line):
            frames.append([])
            continue
        match = LINE.match(line)
        if match and frames:
            frames[-1].append((match.group(2).strip(), match.group(3).strip()))
    return frames


def number(value):
    return int(value.split()[0])


def expected_counter(frame):
    return 0 if frame == 0 else (frame - 1) % 1020 + 1


def check_frame(index, fields, raw, crc, mode, rate, dialnorm):
    """What is wrong with one frame's MediaInfo fields, as a list of messages."""
    wrong = []
    seen = {}
    for name, value in fields:
        seen.setdefault(name, value)

    def want(name, test, description):
        if name in seen and not test(seen[name]):
            wrong.append(f"frame {index}: {name} is {seen[name]!r}, expected {description}")

    want("sync_word", lambda v: number(v) == 0xAC41, "0xAC41")
    want("frame_size", lambda v: number(v) == len(raw), str(len(raw)))
    want("bitstream_version", lambda v: number(v) == 2, "2")
    counter = expected_counter(index)
    want("sequence_counter", lambda v: number(v) == counter, str(counter))
    # A constant rate: b_wait_frames set, and wait_frames 0 - decode at once.
    want("b_wait_frames", lambda v: v == "Yes", "Yes")
    want("wait_frames", lambda v: number(v) == 0, "0")
    want("fs_index", lambda v: v.endswith(f"{rate} Hz"), f"{rate} Hz")
    want("frame_rate_index", lambda v: number(v) == 13, "13")
    iframe = index % IFRAME_INTERVAL == 0
    want("b_iframe_global", lambda v: v == ("Yes" if iframe else "No"), "Yes" if iframe else "No")
    want("presentation_version", lambda v: number(v) == 1, "1")
    want("channel_mode", lambda v: v.endswith(mode), mode)
    want("dialnorm_bits", lambda v: number(v) == 4 * dialnorm, str(4 * dialnorm))
    want("b_further_loudness_info", lambda v: v == "No", "No")
    want("drc_metadata_size_value", lambda v: number(v) == 1, "1")
    want("b_drc_present", lambda v: v == "No", "No")
    want("tools_metadata_size", lambda v: number(v) == 1, "1")
    want("b_de_data_present", lambda v: v == "No", "No")
    want("b_emdf_payloads_substream", lambda v: v == "No", "No")
    want("crc_word", lambda v: number(v) == crc, f"0x{crc:04X}")
    return wrong, set(seen)


# The channel groups (Part 2 Table A.27) of the 7.X 3/2/2 layouts, 7.0 and 7.1, which DEE's muxer
# writes without group 4, their top front pair.
TOP_FRONT_GROUPS = {7: 0x17, 8: 0x57}


def without_group_4(box, groups):
    """`box` with each 24-bit channel group mask `groups` in it written without group 4."""
    bits = "".join(f"{b:08b}" for b in box)
    bits = bits.replace(f"{groups:024b}", f"{groups & ~(1 << 4):024b}")
    return bytes(int(bits[i : i + 8], 2) for i in range(0, len(bits), 8))


def dac4(path):
    """The payload of the first 'dac4' box in an MP4 file."""
    data = Path(path).read_bytes()
    at = data.find(b"dac4")
    if at < 4:
        raise SystemExit(f"{path}: no dac4 box")
    size = struct.unpack(">I", data[at - 4:at])[0]
    return data[at + 4:at - 4 + size]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cli", required=True, type=Path)
    parser.add_argument("--dee-dir", type=Path, default=DEE_DIR)
    parser.add_argument("--work", type=Path)
    args = parser.parse_args()
    mediainfo = args.dee_dir / "MediaInfo.exe"
    muxer = args.dee_dir / "dee_mp4muxer.exe"
    for tool in (mediainfo, muxer):
        if not tool.exists():
            raise SystemExit(f"{tool} not found; this check needs DEE's install")

    failures = []
    with tempfile.TemporaryDirectory() as temporary:
        work = args.work or Path(temporary)
        work.mkdir(parents=True, exist_ok=True)
        for channels, rate, kbps, dialnorm, options in CONFIGURATIONS:
            name = f"{channels}ch-{rate}-{kbps}-dn{dialnorm}" + "".join(
                f"-{o.split('=')[1]}" for o in options
            )
            rng = random.Random(kbps * 7 + channels)
            pcm = ac3space.generate_pcm(rng, channels, 30 * 8, rate, "chaotic", "pairs")
            wav = work / f"{name}.wav"
            ac3space.write_wav(wav, pcm, rate, False)
            stream = work / f"{name}.ac4"
            ours_mp4 = work / f"{name}.mp4"
            command = [args.cli, "ac4-encode", wav, stream, kbps, f"dialnorm={dialnorm}", "quiet"]
            run([*command, *options])
            command[3] = ours_mp4
            run([*command, *options])

            data = stream.read_bytes()
            raw_frames, why = space.sync_frames(data)
            if raw_frames is None:
                failures.append(f"{name}: {why}")
                continue
            frames = mediainfo_frames(mediainfo, stream)
            covered = set()
            detailed = 0
            for index, fields in enumerate(frames[:len(raw_frames)]):
                raw = raw_frames[index]
                crc = space.crc16(len(raw).to_bytes(2, "big") + raw)
                wrong, seen = check_frame(
                    index, fields, raw, crc, channel_mode(channels, options), rate, dialnorm
                )
                failures += [f"{name}: {w}" for w in wrong]
                covered |= seen
                detailed += len(seen) > 3
            missing = sorted(set(FIELDS) - covered)
            print(f"{name}: MediaInfo found {len(frames)} sync frames and detailed {detailed}; "
                  f"{len(FIELDS) - len(missing)} of {len(FIELDS)} fields checked"
                  + (f", never shown: {', '.join(missing)}" if missing else ""))
            if len(frames) != len(raw_frames):
                failures.append(f"{name}: MediaInfo found {len(frames)} sync frames, "
                                f"the stream has {len(raw_frames)}")

            theirs = work / f"{name}.dee.mp4"
            run([muxer, "--track", stream, "-o", theirs, "--overwrite", "1"])
            ours_box, their_box = dac4(ours_mp4), dac4(theirs)
            same = ours_box == their_box
            note = ""
            if not same and any("7x-top-front" in o for o in options):
                same = without_group_4(ours_box, TOP_FRONT_GROUPS[channels]) == their_box
                note = ", but for group 4, which DEE's muxer leaves out of 3/2/2" if same else ""
            print(f"{name}: dac4 {'equal' if same else 'DIFFERS'} ({len(ours_box)} bytes){note}"
                  + ("" if same else f": ours {ours_box.hex()}, DEE's muxer {their_box.hex()}"))
            if not same:
                failures.append(f"{name}: the dac4 boxes differ")
    if failures:
        print("\nFAILED:")
        for failure in failures:
            print(f"  {failure}")
        return 1
    print(f"\n{len(CONFIGURATIONS)} configurations: MediaInfo reads each as configured, and DEE's "
          "muxer writes the encoder's dac4")
    return 0


if __name__ == "__main__":
    sys.exit(main())
