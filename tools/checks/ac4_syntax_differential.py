"""Compare the two AC-4 syntax transcriptions where no encoded stream reaches.

The committed DEE streams, and the local census, exercise only part of the
syntax the decoder in src/ac4dec and tools/references/ac4_syntax.py read. This
script makes streams that reach the rest, reads each through both
transcriptions and compares their traces.

Streams:
  mutations  a DEE stream's frames 0 to k (so every substream's state builds
             up as a decoder's would), with one substream of frame k altered
             and every substream size kept, so the table of contents stays
             valid: from a random bit to the end replaced by random bits
             ("tail"), one to eight bits inverted ("flips"), or everything
             after the audio_size header random, which randomises the codec
             mode ("mode");
  synthetic  a table of contents for a channel mode no encoder here writes
             (mono, 3.0, 5.0 and the 7.X modes, with stereo and 5.1 among
             them) over random substream payloads.

Comparison, of each stream's last frame substream by substream: where both
transcriptions read a substream to the end the records must be identical;
where either stops they must agree up to where the first stopped. The frames
before it are DEE's own, which the digest tests already compare. Findings:
  DIVERGE   a record differs in offset, width or value;
  END       both finished without an error, one having read more;
  STOP      one stopped with an error where the other read on - the two check
            some values at different elements, so this is reported and does
            not fail the run;
  TOC       the two tables of contents (src/ac4's and ac4_parse.py's) disagree
            on whether the frame parses or where its substreams are, so that
            frame's substreams are not compared, and only the first such frame
            of a case is reported. A frame both sides refuse is no finding, and
            either way the frames after it are still compared: what each side
            carries from a frame it refused is where the two can part company;
  KIND      the two read one substream as different kinds, which a table of
            contents naming it in two roles can cause.
The exit status is 1 when there is a DIVERGE, END, TOC or KIND finding.

The C++ side is tools/checks/ac4_syntax_trace.cpp, built by hand (see its
header). Run from the repo root:

  python tools/checks/ac4_syntax_differential.py --trace-tool PATH --work-dir DIR
      [--streams DIR ...] [--mutations N] [--synthetic N] [--seed S]
      [--inputs DIR ...]

--streams adds every *.ac4 under each directory (the local census, say) to the
committed streams under tests/golden/external-baseline/. --inputs compares the
files under each directory instead, every frame of each: a corpus
fuzz/run.sh grew for fuzz_ac4_decode reaches syntax random streams do not.
"""

import argparse
import random
import subprocess
import sys
from collections import Counter, defaultdict
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO / "tools" / "references"))

import ac4_parse  # noqa: E402  (the path above has to come first)
import ac4_syntax  # noqa: E402

# --- Streams ----------------------------------------------------------------


def sync_frame(raw):
    n = len(raw)
    if n < 0xFFFF:
        return bytes([0xAC, 0x40, n >> 8, n & 0xFF]) + raw
    return bytes([0xAC, 0x40, 0xFF, 0xFF, n >> 16, (n >> 8) & 0xFF, n & 0xFF]) + raw


def substream_spans(raw):
    toc = ac4_parse.parse_ac4_toc(ac4_parse.Reader(raw))
    offset = toc["toc_bytes"] + toc["payload_base"]
    # With one substream and no transmitted size, the substream runs to the end
    # of the frame, as python_layout() and ac4_syntax.StreamWalker read it. This
    # took the sizes alone and so returned nothing for such a frame, which left
    # that shape - the one fuzz_ac4_parse once crashed on - unmutated.
    sizes = toc["substream_sizes"] if toc["b_size_present"] else [max(0, len(raw) - offset)]
    spans = []
    for size in sizes:
        spans.append((offset, size))
        offset += size
    return spans


def randomise_from(rng, buf, start_bit):
    out = bytearray(buf)
    for bit in range(start_bit, len(out) * 8):
        if rng.random() < 0.5:
            out[bit >> 3] ^= 0x80 >> (bit & 7)
    return out


def mutate(rng, raw):
    spans = [(o, s) for o, s in substream_spans(raw) if s > 0]
    if not spans:
        return None, None
    offset, size = rng.choice(spans)
    sub = bytearray(raw[offset:offset + size])
    how = rng.choices(["tail", "flips", "mode"], weights=[5, 3, 2])[0]
    if how == "tail":
        sub = randomise_from(rng, sub, rng.randrange(0, size * 8))
    elif how == "flips":
        for _ in range(rng.randint(1, 8)):
            bit = rng.randrange(0, size * 8)
            sub[bit >> 3] ^= 0x80 >> (bit & 7)
    else:
        # audio_size_value and b_more_bits, when the size fits in 15 bits.
        sub = randomise_from(rng, sub, min(16, size * 8))
    out = bytearray(raw)
    out[offset:offset + size] = sub
    return bytes(out), how


class Bits:
    def __init__(self):
        self.bits = []

    def put(self, value, n):
        self.bits.extend((value >> i) & 1 for i in range(n - 1, -1, -1))

    def put_variable_bits(self, value, n_bits):
        """Table 3: groups of n_bits, MSB group first, each with a
        continuation bit. Only the values these frames need, so one group."""
        if value >= (1 << n_bits):
            raise ValueError(f'{value} needs more than one {n_bits}-bit group')
        self.put(value, n_bits)
        self.put(0, 1)

    def to_bytes(self):
        while len(self.bits) % 8:
            self.bits.append(0)
        out = bytearray(len(self.bits) // 8)
        for i, b in enumerate(self.bits):
            if b:
                out[i >> 3] |= 0x80 >> (i & 7)
        return bytes(out)


# Part 2 Table 56 channel_mode codes as (code, width).
SYNTHETIC_MODES = [
    (0b0, 1), (0b10, 2), (0b1100, 4), (0b1101, 4), (0b1110, 4),
    (0b1111000, 7), (0b1111001, 7), (0b1111010, 7), (0b1111011, 7), (0b1111100, 7),
    (0b1111101, 7),
]


def synthetic(rng):
    """One raw_ac4_frame: bitstream_version 2 at frame_rate_index 13, one
    presentation of one channel-coded substream group, an audio substream
    and a presentation substream of random bytes."""
    code, width = rng.choice(SYNTHETIC_MODES)
    # A frame rate factor above 1 (Table 87) makes the substream info name a
    # series of 2 or 4 consecutive substreams, each covering frame_len_base /
    # factor samples and sharing the series' carried state. No encoder here
    # writes one, so these frames are the only place the two transcriptions
    # meet that path. Index 2 (25 fps, 2048 samples) is the one that takes
    # both factors; index 13 keeps the unmultiplied shape.
    factor = rng.choice((1, 1, 1, 2, 4))
    frame_rate_index = 13 if factor == 1 else 2
    w = Bits()
    w.put(2, 2)                         # bitstream_version
    w.put(rng.randrange(1, 1024), 10)   # sequence_counter, not 0: no splice
    w.put(0, 1)                         # b_wait_frames
    w.put(1, 1)                         # fs_index
    w.put(frame_rate_index, 4)          # frame_rate_index
    w.put(1, 1)                         # b_iframe_global
    w.put(1, 1)                         # b_single_presentation
    w.put(0, 1)                         # b_payload_base
    w.put(0, 1)                         # b_program_id
    w.put(1, 1)                         # b_single_substream_group
    w.put(0b10, 2)                      # presentation_version 1
    w.put(rng.randrange(8), 3)          # mdcompat
    w.put(0, 1)                         # b_presentation_id
    if factor == 1:
        if frame_rate_index != 13:
            w.put(0, 1)                 # b_multiplier
    else:
        w.put(1, 1)                     # b_multiplier
        w.put(1 if factor == 4 else 0, 1)   # b_multiplier_is_4
    for value, n in ((0, 2), (0, 3), (0, 1), (0, 2), (0, 2)):
        w.put(value, n)                 # emdf_info() with nothing in it
    w.put(0, 1)                         # b_presentation_filter
    w.put(0, 3)                         # ac4_sgi_specifier(): group 0
    w.put(0, 1)                         # b_pre_virtualized
    w.put(0, 1)                         # b_add_emdf_substreams
    w.put(rng.randrange(2), 1)          # b_alternative
    w.put(1, 1)                         # b_pres_ndot
    # The audio element names substreams 0..factor-1, so the presentation
    # substream is the row after them.
    w.put(factor, 2)                    # presentation substream_index
    for value in (1, 0, 1, 1):          # b_substreams_present, b_hsf_ext,
        w.put(value, 1)                 # b_single_substream, b_channel_coded
    w.put(code, width)                  # channel_mode
    w.put(0, 1)                         # b_sf_multiplier
    w.put(0, 1)                         # b_bitrate_info
    if code in (0b1111010, 0b1111011, 0b1111100, 0b1111101):
        w.put(rng.randrange(2), 1)      # add_ch_base
    for i in range(factor):
        # The series' own b_audio_ndot per instance: the first carries the
        # I-frame, the rest do not, which is the shape 4.3.3.2.7 describes.
        w.put(1 if i == 0 else 0, 1)
    w.put(0, 2)                         # substream_index 0
    w.put(0, 1)                         # b_content_type
    audio_lens = [rng.randint(16, 900) for _ in range(factor)]
    pres_len = rng.randint(4, 120)
    sizes = [*audio_lens, pres_len]
    if len(sizes) <= 3:
        w.put(len(sizes), 2)            # n_substreams
    else:
        w.put(0, 2)                     # the escape: variable_bits(2) + 4
        w.put_variable_bits(len(sizes) - 4, 2)
    for size in sizes:
        w.put(0, 1)                     # b_size_present, then a 10-bit size
        w.put(size, 10)
    payload = bytearray()
    for audio_len in audio_lens:
        audio = bytearray(rng.getrandbits(8) for _ in range(audio_len))
        audio_size = rng.randint(max(1, audio_len // 2), audio_len - 2)
        audio[0] = audio_size >> 7                  # audio_size_value, then
        audio[1] = (audio_size << 1) & 0xFE         # b_more_bits 0
        payload += audio
    payload += bytes(rng.getrandbits(8) for _ in range(pres_len))
    return w.to_bytes() + bytes(payload)


def generate(streams, cases, n_mutations, n_synthetic, seed):
    rng = random.Random(seed)
    cases.mkdir(parents=True, exist_ok=True)
    frames_of = {}
    index = []
    for n in range(n_mutations):
        path = rng.choice(streams)
        if path not in frames_of:
            data = path.read_bytes()
            frames_of[path] = [raw for _, _, raw, _ in ac4_parse.iter_sync_frames(data)]
        frames = frames_of[path]
        if not frames:
            continue  # a file with no sync frame at all
        k = rng.randrange(0, min(len(frames), 12))
        try:
            mutated, how = mutate(rng, frames[k])
        except (ValueError, IndexError, ac4_parse.OamdCommonDataPresent):
            continue  # a table of contents the reference parser cannot read
        if mutated is None:
            continue
        name = f"m{n:05d}"
        body = b"".join(sync_frame(f) for f in frames[:k]) + sync_frame(mutated)
        (cases / f"{name}.ac4").write_bytes(body)
        index.append(f"{name}\t{path}\tframe {k}\t{how}")
    for n in range(n_synthetic):
        name = f"s{n:05d}"
        (cases / f"{name}.ac4").write_bytes(sync_frame(synthetic(rng)))
        index.append(f"{name}\tsynthetic\tframe 0\t-")
    (cases / "index.tsv").write_text("\n".join(index) + "\n", encoding="utf-8")
    return len(index)


# --- Traces -----------------------------------------------------------------


def sync_frames(data):
    """The raw frames of ac4_syncframe()s back to back, stopping where sync is
    lost or a frame runs past the data, as ac4::scan does."""
    pos = 0
    while pos + 4 <= len(data):
        sync = (data[pos] << 8) | data[pos + 1]
        if sync not in (0xAC40, 0xAC41):
            return
        size = (data[pos + 2] << 8) | data[pos + 3]
        header = 4
        if size == 0xFFFF:
            if pos + 7 > len(data):
                return
            size = (data[pos + 4] << 16) | (data[pos + 5] << 8) | data[pos + 6]
            header = 7
        end = pos + header + size
        total = end + (2 if sync == 0xAC41 else 0)
        if total > len(data):
            return
        yield data[pos + header:end]
        pos = total


def python_layout(raw):
    """offset:size of every substream, as the table of contents gives them."""
    toc = ac4_parse.parse_ac4_toc(ac4_parse.Reader(raw))
    offset = toc["toc_bytes"] + toc["payload_base"]
    sizes = toc["substream_sizes"] if toc["b_size_present"] else [max(0, len(raw) - offset)]
    layout = []
    for size in sizes:
        layout.append(f"{offset}:{size}")
        offset += size
    return layout


def python_trace(path, last_only):
    """Each frame's layout, and each substream's kind, records and outcome, or
    with `last_only` the last frame's; every frame is read so that the state
    later ones depend on builds up."""
    walker = ac4_syntax.StreamWalker(True)
    frames = {}
    raws = list(sync_frames(path.read_bytes()))
    for fi, raw in enumerate(raws):
        try:
            subs = walker.frame(raw)
            outcome = {"layout": python_layout(raw),
                       "subs": {idx: (kind, recs, err) for idx, kind, recs, err in subs}}
        except (ValueError, IndexError, ac4_parse.OamdCommonDataPresent) as exc:
            outcome = ("TOC", repr(exc))
        if not last_only or fi == len(raws) - 1:
            frames[fi] = outcome
    return frames


def cpp_trace(path):
    frames = {}
    records = defaultdict(list)
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        parts = line.split("\t")
        fi = int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else None
        if parts[0] == "T":
            frames.setdefault(fi, {"layout": [], "subs": {}})["layout"] = parts[2:]
        elif parts[0] == "R":
            name = parts[6] if len(parts) > 6 else ""
            records[(fi, int(parts[2]))].append((int(parts[3]), int(parts[4]), int(parts[5]), name))
        elif parts[0] == "S":
            reason = parts[5] if len(parts) > 5 else ""
            status = None if parts[4] == "ok" else f"{parts[4]}: {reason}"
            frame = frames.setdefault(fi, {"layout": [], "subs": {}})
            frame["subs"][int(parts[2])] = (parts[3], status)
        elif parts[0] == "F":
            frames[fi] = ("TOC", parts[2])
    for fi, frame in frames.items():
        if not isinstance(frame, tuple):
            frame["subs"] = {idx: (kind, records.get((fi, idx), []), err)
                             for idx, (kind, err) in frame["subs"].items()}
    return frames


def compare_case(args):
    case, cpp_dir, last_only = args
    py = python_trace(case, last_only)
    cpp = cpp_trace(cpp_dir / f"{case.stem}.cpp.tsv")
    findings = []
    toc_reported = False
    for fi in sorted(set(py) | set(cpp)):
        # Each frame is compared on its own: one frame both sides refuse, or
        # read differently, says nothing about the frames after it, and the
        # state each side carries makes those later frames worth comparing.
        # Only the first table-of-contents finding of a case is reported, so a
        # case that differs in every frame does not bury the rest.
        stopped = set()
        p, c = py.get(fi), cpp.get(fi)
        if isinstance(p, tuple) or isinstance(c, tuple) or p is None or c is None:
            if not (isinstance(p, tuple) and isinstance(c, tuple)) and not toc_reported:
                python_says = p if isinstance(p, tuple) else "reads it"
                cpp_says = c if isinstance(c, tuple) else "reads it"
                findings.append(("TOC", "one side reads the table of contents", "",
                                 f"{case.stem} frame {fi}: python {python_says}; c++ {cpp_says}"))
                toc_reported = True
            continue
        if p["layout"] != c["layout"]:
            if not toc_reported:
                findings.append(("TOC", "substream layouts differ", "",
                                 f"{case.stem} frame {fi}: python {p['layout'][:4]}; "
                                 f"c++ {c['layout'][:4]}"))
                toc_reported = True
            continue
        p, c = p["subs"], c["subs"]
        for idx in sorted(set(p) | set(c)):
            if idx in stopped:
                continue
            pkind, precs, perr = p.get(idx, ("absent", [], None))
            ckind, crecs, cerr = c.get(idx, ("absent", [], None))
            if not precs and not crecs:
                continue
            where = f"{case.stem} frame {fi} substream {idx}"
            if pkind != ckind:
                findings.append(("KIND", f"python {pkind}, c++ {ckind}", "",
                                 f"{where}: python {perr}; c++ {cerr}"))
                stopped.add(idx)
                continue
            common = min(len(precs), len(crecs))
            differ = next((i for i in range(common) if precs[i][:3] != crecs[i][:3]), None)
            if differ is not None:
                before = precs[differ - 1][3] if differ else "-"
                findings.append(("DIVERGE", f"after {before}",
                                 f"python {precs[differ][3]}, c++ {crecs[differ][3]}",
                                 f"{where} record {differ}: python {precs[differ][:3]}, "
                                 f"c++ {crecs[differ][:3]}"))
                stopped.add(idx)
                continue
            if len(precs) != len(crecs):
                last = precs[common - 1][3] if common else "-"
                longer_python = len(precs) > len(crecs)
                next_name = (precs if longer_python else crecs)[common][3]
                shorter_err = cerr if longer_python else perr
                if shorter_err is not None:
                    side = "c++" if longer_python else "python"
                    findings.append(("STOP", f"{side} stops after {last}",
                                     f"the other reads {next_name}",
                                     f"{where}: python {perr}; c++ {cerr}"))
                else:
                    findings.append(("END", f"after {last}",
                                     f"{'python' if longer_python else 'c++'} reads {next_name}",
                                     f"{where}: python {perr}; c++ {cerr}"))
            elif (perr is None) != (cerr is None):
                side = "python" if perr is not None else "c++"
                findings.append(("STOP", f"{side} fails at the end", "",
                                 f"{where}: python {perr}; c++ {cerr}"))
            if perr is not None or cerr is not None:
                stopped.add(idx)
    return findings


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--trace-tool", type=Path, required=True,
                        help="the built tools/checks/ac4_syntax_trace.cpp")
    parser.add_argument("--work-dir", type=Path, required=True,
                        help="where the streams, the C++ traces and report.tsv go")
    parser.add_argument("--streams", type=Path, action="append", default=[],
                        help="a directory of further .ac4 streams to mutate")
    parser.add_argument("--inputs", type=Path, action="append", default=[],
                        help="compare every file under this directory, every frame, "
                             "instead of generating streams (a fuzzing corpus, say)")
    parser.add_argument("--mutations", type=int, default=2000)
    parser.add_argument("--synthetic", type=int, default=500)
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()

    cases = args.work_dir / "cases"
    cpp_dir = args.work_dir / "cpp"
    last_only = not args.inputs
    if args.inputs:
        cases.mkdir(parents=True, exist_ok=True)
        inputs = [f for d in args.inputs for f in sorted(d.rglob("*")) if f.is_file()]
        for n, f in enumerate(inputs):
            (cases / f"i{n:06d}.ac4").write_bytes(f.read_bytes())
        print(f"{len(inputs)} inputs copied to {cases}")
    else:
        streams = sorted((REPO / "tests" / "golden" / "external-baseline").glob("ac4-*/*.ac4"))
        for directory in args.streams:
            streams += sorted(directory.rglob("*.ac4"))
        count = generate(streams, cases, args.mutations, args.synthetic, args.seed)
        print(f"{count} streams in {cases}")

    files = sorted(cases.glob("*.ac4"))
    cpp_dir.mkdir(parents=True, exist_ok=True)
    tool = [str(args.trace_tool), *(["--last-frame"] if last_only else []), str(cpp_dir)]
    for start in range(0, len(files), 100):
        batch = [str(f) for f in files[start:start + 100]]
        subprocess.run([*tool, *batch], check=True)

    with ProcessPoolExecutor() as pool:
        jobs = [(f, cpp_dir, last_only) for f in files]
        results = list(pool.map(compare_case, jobs, chunksize=8))
    findings = [f for r in results for f in r]
    with (args.work_dir / "report.tsv").open("w", encoding="utf-8") as out:
        out.writelines("\t".join(f) + "\n" for f in findings)
    groups = Counter()
    example = {}
    for kind, at, what, detail in findings:
        groups[(kind, at, what)] += 1
        example.setdefault((kind, at, what), detail)
    print(f"{len(files)} streams, {len(findings)} findings")
    for (kind, at, what), n in groups.most_common():
        print(f"{n:6d}  {kind:7s} {at} {what}")
        print(f"          e.g. {example[(kind, at, what)]}")
    return 1 if any(k != "STOP" for k, _, _, _ in findings) else 0


if __name__ == "__main__":
    sys.exit(main())
