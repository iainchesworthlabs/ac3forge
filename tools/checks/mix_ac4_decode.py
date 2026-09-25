"""Check the mixes of ac3cli's AC-4 presentations against Part 1's and Part 2's formulas.

For each presentation of several substreams in the multiplexed streams under
tests/golden/ac4dec/presentations/ (planning/ac4.md, phase D7; tests/ac4dec/ac4dec_mux.hpp builds
them from DEE's tone legs and the encoder's tone streams), this decodes the presentation with
`ac3cli decode presentation-id=`, and each of its substreams alone through the single-group
presentations the streams also carry, and holds the mix to the matrix the texts give. The
stream's own values come from `ac3cli decode ... syntax-trace=` and its table of contents from the
Python reference parser (tools/references/ac4_parse.py, ac4_presentations.py):

  mix      ETSI TS 103 190-2 clauses 4.8.3.15 to 4.8.4 and TS 103 190-1 clause 6.2.16: the main
           or music and effects substream after its dialogue enhancement, by its group's gain
           (Part 2 Table 70, -0.25 dB a step) and, with associated audio, scale_main on every
           channel, scale_main_front on L and R and scale_main_centre on C (-0.3 dB a step); the
           dialogue by its group's gain, g_dialog up to g_dialog_max ((1 + dialog_max_gain) x 3
           dB, 0 dB where none is sent) and, with associated audio, the same scales on its own
           channels; the associated audio by its group's gain and g_assoc, which a premixed
           service does not take; a mono substream panned by pan_dialog or pan_associated (1.5
           degrees a step, 0 where none is sent), the rest channel to channel; summed, not
           divided by the number of substreams (src/ac4dec/ERRATA.md, "The mixer's sum"). The
           pan puts L at 330 degrees, C at 0 and R at 30 (Part 1 clause 4.3.12.4.9 and Table
           216), the surrounds at Table D.1's 110 degrees either side, and shares a signal
           linearly between the two channels either side of its angle, which Table 216's 0.5 and
           0.5 at 0 degrees between L and R fixes.
  hybrid   Part 1 clauses 5.7.8.7 to 5.7.8.9, at dialogue-enhancement=6: the main substream's
           processed channels by (1 - alpha_c) g of the parametric method and the dialogue
           enhancement substream's waveform by alpha_c g, g = 10^(G/20) - 1 with G capped at
           G_max; the waveform's channels go to the processed channels in order with the channel
           independent method, halved into L and R for the Mid, and by r with the cross-channel
           method; and the result takes the main substream's gains.
  level    Part 2 clause 4.8.5.2: a version 1 presentation takes its presentation substream's
           dialnorm; a version 0 one takes Table 16's substream's (the dialogue's in
           configurations 0 and 3, the main one's otherwise), and levels its associated audio
           to it from the associated substream's own by 2^((dialnorm - own) / 6) (Part 1 clause
           6.2.16.0, in dB2 as the output level gain is). At output-level=-31 (drcmode=off) each
           presentation goes to that level from its dialnorm, 2^((Lout - dialnorm) / 6).

Each output channel is fitted, by least squares over the steady frames, on every channel of the
substreams decoded alone, each carrying tones of its own: every coefficient must equal the
formula's to 0.01 dB (one tone per substream and channel), those the formula makes 0 must be 60
dB under, and what the formula leaves of the output must be 80 dB under it. The mixes run at
g_dialog and g_assoc of 0 dB, at -6 and -10 dB, and at +9 dB, which g_dialog_max caps, as coded;
and at 0 dB at the output level, against the substreams alone at the same level.

Usage:
    python tools/checks/mix_ac4_decode.py --cli build/config-linux-llvm/bin/ac3cli
"""

import argparse
import math
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO / "tools" / "checks"))
sys.path.insert(0, str(REPO / "tools" / "references"))
import ac4_parse  # noqa: E402
import ac4_presentations  # noqa: E402
import ac4_tables  # noqa: E402
from score_ac4_decode import read_wav  # noqa: E402

STREAMS = REPO / "tests" / "golden" / "ac4dec" / "presentations"
TOLERANCE_DB = 0.01
ABSENT_DB = -60.0
RESIDUAL_DB = -80.0
SKIP = 5 * 2048
SETTINGS = ((0.0, 0.0), (-6.0, -10.0), (9.0, 0.0))  # dialogue-gain, associated-gain
DE_GAIN_DB = 6.0
LEVEL = -31.0  # output-level=, with drcmode=off
# ac3cli's WAV channel order for each channel count the streams decode to.
WAV_ORDER = {1: ("C",), 2: ("L", "R"), 6: ("L", "R", "C", "LFE", "Ls", "Rs")}
# Where each channel sits for panning, clockwise from the front (5.X surrounds).
AZIMUTH = {"L": 330.0, "C": 0.0, "R": 30.0, "Ls": 250.0, "Rs": 110.0}
# Part 1 Table 172.
MIX_COEF = (0.0, 6.32e-3, 1e-2, 1.79e-2, 3.16e-2, 5.65e-2, 7.87e-2, 0.111, 0.156, 0.218, 0.303,
            0.37, 0.448, 0.533, 0.577, 0.622, 0.7071, 0.783, 0.846, 0.894, 0.929, 0.953, 0.976,
            0.9877, 0.9938, 0.9969, 0.9984, 0.9995, 0.99984, 0.99995, 0.99998, 1.0)
PREMIX = ("qax", "qtx", "qsx", "qex")  # Part 1 Table 92
DE_BANDS = 8


def db(x):
    return 20.0 * math.log10(x)


def from_db(value):
    return 10.0 ** (value / 20.0)


def pan(degrees, into):
    """Each channel of `into`'s gain for a mono signal at `degrees`."""
    ring = sorted((AZIMUTH[s], i) for i, s in enumerate(into) if s in AZIMUTH)
    gains = [0.0] * len(into)
    t = degrees % 360.0
    for az, i in ring:
        if abs(az - t) < 1e-9:
            gains[i] = 1.0
            return gains
    if len(ring) == 1:
        gains[ring[0][1]] = 1.0
        return gains
    below = [k for k, (az, _) in enumerate(ring) if az < t]
    a = below[-1] if below else len(ring) - 1
    b = (a + 1) % len(ring)
    span = (ring[b][0] - ring[a][0]) % 360.0 or 360.0
    into_span = (t - ring[a][0]) % 360.0
    gains[ring[a][1]] = 1.0 - into_span / span
    gains[ring[b][1]] = into_span / span
    return gains


def decode(cli, stream, out_wav, *options):
    command = [str(cli), "decode", str(stream), str(out_wav), *options]
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise SystemExit(f"{stream.name}: ac3cli decode {' '.join(options)} failed "
                         f"({result.returncode}):\n{result.stdout}{result.stderr}")
    samples, _ = read_wav(out_wav)
    return samples


def trace_records(path, frame):
    """{substream: [(name, value), ...]} of one frame of a syntax trace, in order."""
    out = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        fields = line.split("\t")
        if len(fields) == 6 and int(fields[0]) == frame:
            out.setdefault(int(fields[1]), []).append((fields[5], int(fields[4])))
    return out


def values(records, name):
    """Every value of `name`, or of its elements (`name[0]`, `name[1]`, ...), in order."""
    return [v for n, v in records if n == name or (n.startswith(name + "[") and n.endswith("]"))]


def gain_slot(p, position):
    """Which sg_gain a group takes: one per ac4_sgi_specifier(), none for a single group or
    configuration 1 (n_substream_groups is 1, and Part 2 6.2.2.3 sends gains for more than
    one), and for configuration 4 the main and associated groups' two."""
    config = p['presentation_config']
    if config is None or config == 1:
        return None
    if config == 4:
        return {0: 0, 2: 1}.get(position)
    return position


def de_parameter(index, cross):
    """Part 1 Table 210 for the cross-channel methods, Table 209 for the others."""
    if cross:
        return 0.1 * max(-30, min(30, index))
    index = max(0, min(31, index))
    if index <= 15:
        return 0.1 * index
    return {16: 1.75, 17: 2.0}.get(index, 2.5 + 0.5 * (index - 18))


def de_values(records):
    """A main substream's dialogue enhancement from an I-frame's records: (method, G_max in dB,
    processed channels in L, R, C order, each one's parameter (constant over the bands), the Mid
    flag, alpha_c, r), or None where it sends none."""
    if values(records, "b_de_data_present") != [1]:
        return None
    method = values(records, "de_method")[0]
    max_gain_db = 3.0 * (values(records, "de_max_gain")[0] + 1)
    config = values(records, "de_channel_config")[0]
    processed = [c for c, bit in zip(("L", "R", "C"), (4, 2, 1), strict=True) if config & bit]
    cross = method in (1, 3)
    books = ("DE_HCB_ABS_1", "DE_HCB_DIFF_1") if cross else ("DE_HCB_ABS_0", "DE_HCB_DIFF_0")
    abs_off, diff_off = (ac4_tables.HUFFMAN_CODEBOOKS[b]["cb_off"] for b in books)
    mid = values(records, "de_ms_proc_flag") == [1]
    codes = values(records, "de_par_code")
    rows = []
    for ch in range(len(processed) - (1 if mid else 0)):
        row = []
        for band in range(DE_BANDS):
            code = codes[ch * DE_BANDS + band]
            if band == 0:
                row.append(code - abs_off if ch == 0 else rows[-1][0] + code - diff_off)
            else:
                row.append(row[-1] + code - diff_off)
        rows.append(row)
    if any(len(set(row)) != 1 for row in rows):
        raise SystemExit("the hybrid legs' parameters are not constant over the bands")
    parameters = [de_parameter(row[0], cross) for row in rows]
    # 5.7.8.8: the cross-channel methods' rendering vector.
    indices = values(records, "de_mix_coef1_idx") + values(records, "de_mix_coef2_idx")
    coef = [MIX_COEF[c] for c in indices]
    if not cross or len(processed) == 1:
        r = [1.0] * len(processed)
    elif len(processed) == 2:
        r = [coef[0], math.sqrt(max(0.0, 1.0 - coef[0] ** 2))]
    else:
        r = [coef[0], coef[1], math.sqrt(max(0.0, 1.0 - coef[0] ** 2 - coef[1] ** 2))]
    alpha = (values(records, "de_signal_contribution") or [0])[0] / 31.0
    return method, max_gain_db, processed, parameters, mid, alpha, r


def enhanced(speakers, into, de, waveform_channels, de_gain):
    """(H, W): the main substream's output channels after its dialogue enhancement, from its own
    channels and from the dialogue enhancement substream's."""
    h = np.zeros((len(into), len(speakers)))
    for j, s in enumerate(speakers):
        h[into.index(s), j] = 1.0
    w = np.zeros((len(into), waveform_channels))
    if de is None or de_gain <= 0.0:
        return h, w
    method, max_gain_db, processed, parameters, mid, alpha, r = de
    g = from_db(min(de_gain, max_gain_db)) - 1.0
    cross = method in (1, 3)
    mid = mid and not cross and len(processed) == 2
    needed = 1 if cross or mid else len(processed)
    hybrid = method >= 2 and 0 < needed <= waveform_channels
    gp = (1.0 - alpha) * g if hybrid else g
    gs = alpha * g if hybrid else 0.0
    rows = [into.index(c) for c in processed]
    cols = [speakers.index(c) for c in processed]
    if cross:  # 5.7.8.8: Y = (I + g r p^T) m, and r g_s d
        for i, a in enumerate(rows):
            for k, b in enumerate(cols):
                h[a, b] += gp * r[i] * parameters[k]
            if hybrid:
                w[a, 0] += r[i] * gs
    elif mid:  # 5.7.8.7 on the Mid, and half of g_s d to L and to R
        for a in rows:
            for b in cols:
                h[a, b] += gp * parameters[0] / 2.0
            if hybrid:
                w[a, 0] += gs / 2.0
    else:  # 5.7.8.7: Y_i = m_i + g p_i m_i, and g_s d_i
        for i, (a, b) in enumerate(zip(rows, cols, strict=True)):
            h[a, b] += gp * parameters[i]
            if hybrid:
                w[a, i] += gs
    return h, w


def dialnorm(records):
    """The dialnorm in dBFS a substream's records carry, or None."""
    bits = values(records, "dialnorm_bits")
    return -0.25 * bits[0] if bits else None


def expected_matrix(p, found, refs, records, setting, level_sources):
    """The matrix from every channel of the substreams decoded alone, in `found`'s order, to
    each output channel of the presentation; `level_sources` names, for each substream, the
    substream whose records carry the dialnorm of its presentation alone."""
    dialogue_gain, associated_gain, de_gain, level = setting
    anchor = next(m for m in found if m['role'] in ('main', 'music_effects'))
    into = refs[anchor['substream_index']][1]
    associated = any(m['role'] == 'associated' for m in found)
    v1 = p.get('presentation_substream') is not None
    if v1:
        # Part 2 clauses 4.8.3.17, 4.8.4 and 4.8.5.2: the presentation substream carries the
        # group gains, the associated audio's values and the dialnorm.
        pres = records.get(p['presentation_substream']['substream_index'], [])
        sg = values(pres, "sg_gain")
        mixing = pres if values(pres, "b_associated") == [1] else []
        mix_dialnorm = dialnorm(pres)
    else:
        # Version 0: the associated substream's extended_metadata() carries its values, and
        # Table 16's substream the dialnorm, the dialogue's in configurations 0 and 3.
        sg = []
        member = anchor
        if p['presentation_config'] in (0, 3):
            member = next((m for m in found if m['role'] == 'dialogue'), anchor)
        mix_dialnorm = dialnorm(records.get(member['substream_index'], []))
        described = next((m for m in found if m['role'] == 'associated'), None)
        mixing = records.get(described['substream_index'], []) if described else []
    scale = {"all": 1.0, "front": 1.0, "centre": 1.0}
    pan_associated = 0
    if associated:
        for key, name in (("all", "scale_main"), ("front", "scale_main_front"),
                          ("centre", "scale_main_centre")):
            code = values(mixing, name)
            if code:
                scale[key] = 0.0 if code[0] == 255 else from_db(-0.3 * code[0])
        pan_associated = (values(mixing, "pan_associated") or [0])[0]

    def loudness(member):
        """The member's gain in the mix against its presentation alone from their dialnorms:
        at an output level, 2^((Lout - dialnorm) / 6) in each (Part 1 clause 5.7.9.3.3); and
        in version 0, the associated audio levelled to the presentation's dialnorm from its
        own (clause 6.2.16.0), 2^((dialnorm - own) / 6)."""
        own = dialnorm(records.get(level_sources[member['substream_index']], []))
        gain = 1.0
        if level is not None:
            gain *= 2.0 ** ((own - mix_dialnorm) / 6.0)
        if not v1 and member['role'] == 'associated':
            gain *= 2.0 ** ((mix_dialnorm - own) / 6.0)
        return gain

    def group_gain(member):
        slot = gain_slot(p, member['position'])
        if slot is None or slot >= len(sg):
            return 1.0
        return 0.0 if sg[slot] == 63 else from_db(-0.25 * sg[slot])

    def scaled(speaker):
        if not associated:
            return 1.0
        s = scale["all"]
        if speaker in ("L", "R"):
            s *= scale["front"]
        elif speaker == "C":
            s *= scale["centre"]
        return s

    blocks = {}
    de_member = next((m for m in found if m['role'] == 'dialogue_enhancement'), None)
    anchor_speakers = refs[anchor['substream_index']][1]
    waveform_channels = len(refs[de_member['substream_index']][1]) if de_member else 0
    de = de_values(records.get(anchor['substream_index'], [])) if anchor['role'] == 'main' else None
    h, w = enhanced(anchor_speakers, into, de, waveform_channels, de_gain)
    rows = np.array([group_gain(anchor) * scaled(s) for s in into])[:, None]
    blocks[anchor['substream_index']] = rows * h * loudness(anchor)
    if de_member is not None:
        blocks[de_member['substream_index']] = rows * w * loudness(de_member)
    for m in found:
        if m['substream_index'] in blocks:
            continue
        speakers = refs[m['substream_index']][1]
        audio = records.get(m['substream_index'], [])
        block = np.zeros((len(into), len(speakers)))
        gain = group_gain(m) * loudness(m)
        if m['role'] == 'dialogue':
            maximum = values(audio, "dialog_max_gain")
            gain *= from_db(min(dialogue_gain, 3.0 * (1 + maximum[0]) if maximum else 0.0))
            angles = [1.5 * a for a in values(audio, "pan_dialog")] if len(speakers) <= 2 else []
        else:
            if m['language'].lower() not in PREMIX:
                gain *= from_db(min(associated_gain, 0.0))
            angles = [1.5 * pan_associated] if len(speakers) == 1 else []
        if len(speakers) == 1 and not angles:
            angles = [0.0]
        for j, s in enumerate(speakers):
            own = gain * (scaled(s) if m['role'] == 'dialogue' else 1.0)
            if j < len(angles):
                block[:, j] = [own * g for g in pan(angles[j], into)]
            elif s in into:
                block[into.index(s), j] = own
        blocks[m['substream_index']] = block
    return np.hstack([blocks[m['substream_index']] for m in found])


def check(name, out, stacked, expected):
    """Failures of the fit of `out` on `stacked` against `expected`, and a summary."""
    failures = []
    fit, *_ = np.linalg.lstsq(stacked, out, rcond=None)
    fitted = fit.T
    worst = 0.0
    for c in range(expected.shape[0]):
        for j in range(expected.shape[1]):
            want, got = expected[c, j], fitted[c, j]
            if want != 0.0:
                error = abs(db(abs(got)) - db(abs(want))) if got * want > 0 else float("inf")
                worst = max(worst, error)
                if error > TOLERANCE_DB:
                    failures.append(f"{name}: channel {c} takes reference {j} at {got:+.6f}, "
                                    f"the formula {want:+.6f}")
            elif abs(got) > from_db(ABSENT_DB):
                failures.append(f"{name}: channel {c} takes reference {j} at {got:+.6f}, "
                                "where the formula has none")
    residual = out - stacked @ expected.T
    left = 10.0 * math.log10(max(float(np.sum(residual ** 2)), 1e-300) / float(np.sum(out ** 2)))
    if left > RESIDUAL_DB:
        failures.append(f"{name}: the formula leaves {left:.1f} dB of the output")
    return failures, f"within {worst:.4f} dB, {left:.0f} dB left"


def level_options(level):
    """At an output level, its gain alone: no compression."""
    return [] if level is None else [f"output-level={level:g}", "drcmode=off"]


def check_stream(cli, stream, work):
    """(failures, mixes checked) for one multiplexed stream."""
    _, _, raw, _ = next(ac4_parse.iter_sync_frames(stream.read_bytes()))
    toc, _ = ac4_parse.parse_raw_frame(raw)
    presentations = toc['presentations']
    # Each substream alone, by the presentation of its group or substream alone, as coded and
    # at an output level; and where that presentation's dialnorm is.
    refs = {None: {}, LEVEL: {}}
    level_sources = {}
    for i, p in enumerate(presentations):
        found, decodable = ac4_presentations.members(toc, p)
        if decodable and p['presentation_config'] is None and p['presentation_id'] is not None:
            substream = found[0]['substream_index']
            for level, alone in refs.items():
                samples = decode(cli, stream, work / f"{stream.stem}-{i}.wav",
                                 f"presentation-id={p['presentation_id']}", *level_options(level))
                alone[substream] = (samples, WAV_ORDER[samples.shape[1]])
            pres = p.get('presentation_substream')
            level_sources[substream] = pres['substream_index'] if pres else substream
    hybrid = "hybrid" in stream.stem
    settings = [(0.0, 0.0, DE_GAIN_DB, None), (0.0, 0.0, DE_GAIN_DB, LEVEL)] if hybrid else \
        [(dg, ag, 0.0, None) for dg, ag in SETTINGS] + [(0.0, 0.0, 0.0, LEVEL)]
    failures = []
    checked = 0
    for i, p in enumerate(presentations):
        found, decodable = ac4_presentations.members(toc, p)
        if not decodable or p['presentation_config'] is None:
            continue
        if any(m['substream_index'] not in level_sources for m in found):
            failures.append(f"{stream.stem} presentation {i}: a substream has no presentation "
                            "of its own to measure it by")
            continue
        for setting in settings:
            dialogue_gain, associated_gain, de_gain, level = setting
            trace = work / f"{stream.stem}-{i}.trace"
            options = [f"presentation-id={p['presentation_id']}", f"syntax-trace={trace}",
                       f"dialogue-gain={dialogue_gain:g}", f"associated-gain={associated_gain:g}",
                       *level_options(level)]
            if de_gain:
                options.append(f"dialogue-enhancement={de_gain:g}")
            out = decode(cli, stream, work / f"{stream.stem}-{i}-mix.wav", *options)[SKIP:]
            stacked = np.hstack([refs[level][m['substream_index']][0] for m in found])[SKIP:]
            expected = expected_matrix(p, found, refs[level], trace_records(trace, 0), setting,
                                       level_sources)
            at = f"{dialogue_gain:+g}/{associated_gain:+g} dB"
            if level is not None:
                at += f", {level:g} dBFS"
            name = (f"{stream.stem} presentation {i} (id {p['presentation_id']}, config "
                    f"{p['presentation_config']}) at {at}")
            found_failures, summary = check(name, out, stacked, expected)
            failures += found_failures
            checked += 1
            print(f"{name:<84} {summary}", flush=True)
    return failures, checked


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cli", required=True, type=Path, help="the ac3cli to decode with")
    parser.add_argument("--work", type=Path, help="scratch directory (default: a temporary one)")
    args = parser.parse_args()
    failures = []
    checked = 0
    with tempfile.TemporaryDirectory() as temporary:
        work = args.work or Path(temporary)
        work.mkdir(parents=True, exist_ok=True)
        for stream in sorted(STREAMS.glob("*.ac4")):
            found_failures, found_checked = check_stream(args.cli, stream, work)
            failures += found_failures
            checked += found_checked
    for failure in failures:
        print(f"FAIL {failure}")
    print(f"{checked} mixes, {len(failures)} failures")
    return 1 if failures or checked == 0 else 0


if __name__ == "__main__":
    sys.exit(main())
