"""Build the stimulus set for a subjective listening test over the landscape legs.

Every quality number this project publishes is a waveform or model metric.
README.md and docs/verification.md have carried "no listening test has been
run" through nine releases, and ViSQOL's MOS-LQO - the closest thing here to
a perceptual score - is a *prediction* of what a panel would say, not a
panel. This script builds everything a real session needs; the listening
itself is human time, and docs/landscape.md#listening-test is the protocol
that spends it. tools/listening/score_listening_test.py turns the responses
back into the table there.

It measures the same three legs as the landscape comparison, read from
tests/golden/external-baseline/manifest.json so the two never drift apart:

  ac3-51-448       AC-3,   5.1,    448 kbit/s
  eac3-stereo-192  E-AC-3, stereo, 192 kbit/s
  eac3-51-256      E-AC-3, 5.1,    256 kbit/s

and, per leg, these conditions:

  reference    the source WAV itself, presented as the hidden reference
  ac3forge     this build's encode (ac3cli, run here)
  ffmpeg       the committed tests/golden/external-baseline/<leg>/ffmpeg.*
  dee          the committed .../dee.* - only where the manifest carries a
               real score for it. DEE's own two 5.1 legs are marked
               unverified there (that build drops the Ls channel; see
               gen_external_baseline.py's own header), so the 5.1 legs have
               no DEE arm and the results table says so rather than
               presenting a stimulus nobody should draw a conclusion from.
  anchor3500   BS.1534-3's mandatory low-pass anchor, 3.5 kHz
  anchor7000   BS.1534-3's optional low-pass anchor, 7 kHz

EVERY stimulus is decoded by FFmpeg, including this project's own encode.
That is deliberate and it is the opposite of what the trend legs do (which
score ac3forge through ac3cli's own decoder). A listening test compares
ENCODERS; if each encoder's output went through its own decoder, the panel
would be scoring encoder-and-decoder pairs and no result could be attributed
to either. One decoder for everything makes the decoder a constant. FFmpeg
is the one all three encoders have in common.

FFmpeg's decoder is not silent about what it reads, and this script records
that: any stimulus whose decode printed a decoder-level complaint is flagged
in the trials key with `decode_warnings`. This is not hypothetical - FFmpeg
reports two out-of-range exponents decoding DEE's own committed stereo
stream. A concealed error is a real artifact a listener will hear, but it is
an artifact of THAT decoder reading THAT stream, not of the encoder's
quality, and a results table that does not say so invites the wrong
conclusion.

Neither FFmpeg's nor DEE's ENCODER is ever invoked here - only ac3cli's, and
FFmpeg's decoder. The external side comes from the committed bitstreams, the
same boundary docs/landscape.md documents for the numbers.

Two methods, because how many listeners are available decides which one is
worth running:

  mushra  BS.1534-3. Every condition for one leg presented together against
          a labelled reference, scored 0-100. Needs a panel; the statistics
          below are meaningless with one listener.
  abx     Forced-choice A/B/X, one (leg, condition) pair at a time. Works
          with a single listener and still supports a real significance
          claim - "this listener could tell them apart" - which is a
          narrower question than MUSHRA's but an answerable one.

Both write the same blind-labelled WAVs; only the trials key and the
response template differ.

Usage (repo root, after building ac3cli):

    python tools/listening/gen_listening_stimuli.py --out listening-session
    python tools/listening/gen_listening_stimuli.py --out session-abx --method abx

Set AC3CLI to override the ac3cli binary, same as quality_race.py. Needs
numpy and an `ffmpeg` binary; the scoring script that reads the results back
needs neither.
"""

import argparse
import csv
import json
import random
import subprocess
import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent.parent
# quality_race.py lives in tools/ci/ (the CI-orchestration bucket) but is
# also where this project's WAV IO and cross-correlation alignment live -
# gen_external_baseline.py imports it across the same boundary for the same
# reason, rather than either file growing a second copy.
sys.path.insert(0, str(REPO / "tools" / "ci"))
from quality_race import (  # noqa: E402
    CLI,
    FIXED_ALIGN,
    RATE,
    align,
    read_wav_any,
    read_wav_f32,
    write_wav_f32,
)

BASELINE_DIR = REPO / "tests" / "golden" / "external-baseline"
MANIFEST = BASELINE_DIR / "manifest.json"

# BS.1534-3 §5.2: the 3.5 kHz low-pass anchor is mandatory, the 7 kHz one
# optional and recommended. They exist so scores from different panels and
# different laboratories can be put on a common scale - without them a panel
# that heard only near-transparent systems will spread its scores across the
# whole 0-100 range anyway, and the numbers stop meaning anything outside
# that one session.
ANCHOR_HZ = [3500, 7000]

# The filter is a windowed-sinc FIR rather than anything more clever: the
# anchor is defined by its cutoff, and a linear-phase FIR long enough to be
# steep at 3.5 kHz introduces no phase distortion of its own to confound the
# thing being anchored. 511 taps at 48 kHz is a transition band of roughly
# 300 Hz.
ANCHOR_TAPS = 511

# An anchor only anchors anything if removing the band above its cutoff is
# audible, and that depends entirely on the programme material. Below this
# much of the reference's total energy sitting above the cutoff, the anchor
# is a near-transparent copy of the reference and a panel will (correctly)
# score it near 100, which leaves the bottom of the scale undefined and the
# session unscalable against any other panel's. -20 dB is 1% of total energy:
# generous, but the case this guards against is not marginal - the committed
# 5.1 fixture has 0.059% of its energy above 3.5 kHz, so BOTH anchors are
# inaudible on both 5.1 legs. That is a property of synthetic fixture
# material, and roadmap VX7 (real programme material) is what fixes it.
ANCHOR_MIN_BAND_ENERGY_DB = -20.0


def band_energy_above_db(signal, cutoff_hz, rate=RATE):
    """Energy above cutoff_hz as dB relative to the signal's total.

    A whole-signal FFT rather than a windowed STFT: this asks a question
    about the item's overall spectral balance, not about how it varies in
    time, and no window is worth its leakage penalty at the scale (tens of
    dB) that decides whether an anchor is audible at all.
    """
    spectrum = np.abs(np.fft.rfft(signal, axis=0)) ** 2
    freqs = np.fft.rfftfreq(len(signal), 1.0 / rate)
    total = spectrum.sum()
    above = spectrum[freqs > cutoff_hz].sum()
    return float(10.0 * np.log10(max(above / max(total, 1e-30), 1e-30)))


def lowpass(signal, cutoff_hz, rate=RATE, taps=ANCHOR_TAPS):
    """Linear-phase windowed-sinc low-pass, applied per channel.

    The group delay (taps // 2) is removed here rather than left for the
    alignment pass below: an anchor is derived from the reference rather
    than decoded from a bitstream, so it never goes through align() and
    would otherwise be the one stimulus in the set arriving late.
    """
    n = np.arange(taps) - (taps - 1) / 2.0
    kernel = 2.0 * (cutoff_hz / rate) * np.sinc(2.0 * (cutoff_hz / rate) * n)
    kernel *= np.blackman(taps)
    kernel /= kernel.sum()
    delay = taps // 2
    out = np.empty_like(signal)
    for c in range(signal.shape[1]):
        padded = np.concatenate([signal[:, c], np.zeros(delay, dtype=signal.dtype)])
        out[:, c] = np.convolve(padded, kernel, mode="full")[delay:delay + len(signal)]
    return out


# FFmpeg notes that say nothing about decode quality. Raw AC-3/E-AC-3
# elementary streams carry no container duration, so this one is printed for
# every stimulus in every set, ours and everyone else's alike.
BENIGN_FFMPEG_NOTES = ["Estimating duration from bitrate"]


def ffmpeg_decode(coded, wav, channels=None):
    """Decode with FFmpeg and return whatever it said on stderr.

    Deliberately NOT run with -xerror: on an external encoder's stream a
    concealed error is part of what a listener would actually hear, and
    failing the whole run over it would leave the set with a silent hole
    instead of a flagged stimulus. The stderr text is carried into the
    trials key so the results table can name it.
    """
    cmd = ["ffmpeg", "-v", "warning", "-y", "-i", str(coded)]
    if channels is not None:
        cmd += ["-ac", str(channels)]
    cmd += ["-c:a", "pcm_f32le", str(wav)]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"ffmpeg failed on {coded}:\n{result.stderr}")
    # One line per distinct complaint, deduplicated - FFmpeg repeats the same
    # message once per affected block, and "exponent 25 is out-of-range x2"
    # is what a reader needs, not forty identical lines. BENIGN_FFMPEG_NOTES
    # keeps the flag meaning "a listener will hear something FFmpeg could not
    # decode cleanly"; a note about container duration estimation on a raw
    # elementary stream is true of every stimulus in the set and flagging all
    # of them would make the column say nothing.
    seen = {}
    for line in result.stderr.splitlines():
        text = line.strip()
        if not text or any(note in text for note in BENIGN_FFMPEG_NOTES):
            continue
        seen[text] = seen.get(text, 0) + 1
    return "; ".join(f"{text} (x{count})" if count > 1 else text
                     for text, count in seen.items())


def encode_ours(source_wav, coded, codec, kbps):
    """Encode with ac3cli, at the same settings gen_external_baseline.py used
    for the external tools on this leg - AC-3's tools are unconditionally
    automatic, and E-AC-3 gets `auto` (the set this encoder picks from the
    per-channel rate), which is the configuration comparable to another
    encoder's own black-box choice."""
    if codec == "ac3":
        cmd = [CLI, "encode", str(source_wav), str(coded), str(kbps)]
    else:
        cmd = [CLI, "eac3-encode", str(source_wav), str(coded), str(kbps), "auto"]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"ac3cli failed:\n{result.stderr}")


def trim_to_common(reference, decoded):
    """Align a decoded stimulus to the reference and trim both to the overlap.

    Codec delay has to come out before a listener hears the set: MUSHRA
    switching between conditions mid-item is the whole point of the method,
    and a stimulus that starts a few milliseconds late is audible as a click
    on every switch - an artifact of the apparatus that the panel would score
    as an artifact of the encoder. FIXED_ALIGN is quality_race.py's own
    scaled-down window for the short committed fixtures.
    """
    o, d, _ = align(reference, decoded, **FIXED_ALIGN)
    return o, d


def build_conditions(leg_name, leg, out_dir, args):
    """Every stimulus for one leg, already aligned and trimmed, as
    {condition: (samples, warnings)}."""
    source = read_wav_any(REPO / leg["source_wav"])
    channels = 2 if (args.render == "stereo" and source.shape[1] > 2) else None
    codec, kbps = leg["codec"], leg["bitrate_kbps"]
    ext = "ac3" if codec == "ac3" else "ec3"
    scratch = out_dir / "_scratch"
    scratch.mkdir(parents=True, exist_ok=True)

    # ac3forge's own encode, then every stimulus through FFmpeg's decoder.
    ours_coded = scratch / f"{leg_name}_ac3forge.{ext}"
    encode_ours(REPO / leg["source_wav"], ours_coded, codec, kbps)

    decoded = {}
    for condition, coded in [("ac3forge", ours_coded),
                             ("ffmpeg", BASELINE_DIR / leg_name / f"ffmpeg.{ext}"),
                             ("dee", BASELINE_DIR / leg_name / f"dee.{ext}")]:
        if condition == "dee" and leg["scores"].get("dee", {}).get("snr_db") is None:
            # Marked unverified in the manifest - no arm, and the protocol
            # says why rather than the set quietly having one fewer column.
            continue
        wav = scratch / f"{leg_name}_{condition}.wav"
        warnings = ffmpeg_decode(coded, wav, channels)
        decoded[condition] = (read_wav_f32(wav), warnings)

    # The reference the coded stimuli are aligned against has to be the same
    # rendering they are: on a stereo-rendered 5.1 leg that is FFmpeg's own
    # downmix of the source, not the 6-channel source itself.
    if channels is not None:
        source_wav = scratch / f"{leg_name}_reference_src.wav"
        write_wav_f32(source_wav, *[source[:, c] for c in range(source.shape[1])])
        downmixed = scratch / f"{leg_name}_reference.wav"
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", str(source_wav),
                        "-ac", str(channels), "-c:a", "pcm_f32le", str(downmixed)],
                       capture_output=True, check=True)
        reference = read_wav_f32(downmixed)
    else:
        reference = source

    # Align every coded stimulus, then trim all of them - and the reference -
    # to the shortest common length, so switching between conditions never
    # switches between item lengths either.
    aligned = {}
    for condition, (samples, warnings) in decoded.items():
        ref_trimmed, coded_trimmed = trim_to_common(reference, samples)
        aligned[condition] = (ref_trimmed, coded_trimmed, warnings)
    if not aligned:
        raise SystemExit(f"{leg_name}: no coded conditions were built")
    n = min(len(v[0]) for v in aligned.values())
    if args.seconds is not None:
        start = int(args.start * RATE)
        n = min(n - start, int(args.seconds * RATE))
        if n <= 0:
            raise SystemExit(f"{leg_name}: --start/--seconds select nothing from a "
                             f"{len(next(iter(aligned.values()))[0]) / RATE:.2f}s item")
    else:
        start = 0

    trimmed_reference = next(iter(aligned.values()))[0][start:start + n]
    conditions = {"reference": (trimmed_reference, "")}
    for condition, (_, coded_trimmed, warnings) in aligned.items():
        conditions[condition] = (coded_trimmed[start:start + n], warnings)
    for hz in ANCHOR_HZ:
        conditions[f"anchor{hz}"] = (lowpass(trimmed_reference, hz), "")
    return conditions


def write_stimuli(out_dir, leg_name, conditions, labels):
    leg_dir = out_dir / leg_name
    leg_dir.mkdir(parents=True, exist_ok=True)
    for condition, label in labels.items():
        samples = conditions[condition][0]
        write_wav_f32(leg_dir / f"stim_{label}.wav",
                      *[np.ascontiguousarray(samples[:, c]) for c in range(samples.shape[1])])
    # The reference is presented twice in MUSHRA: openly, as the thing to
    # compare against, and blind, as one of the labelled conditions (the
    # hidden reference, which is what post-screening tests a listener with).
    write_wav_f32(leg_dir / "reference.wav",
                  *[np.ascontiguousarray(conditions["reference"][0][:, c])
                    for c in range(conditions["reference"][0].shape[1])])


def mushra_key(out_dir, built, rng):
    """One trial per leg; every condition for that leg under a blind label.

    Labels are shuffled per leg rather than globally, and that is the whole
    blinding: a MUSHRA trial IS one item with all its conditions side by
    side, so within-trial order is the only thing there is to randomize.
    """
    rows = []
    labels_by_leg = {}
    for trial, (leg_name, conditions) in enumerate(built.items(), start=1):
        names = list(conditions)
        shuffled = names[:]
        rng.shuffle(shuffled)
        labels = {name: chr(ord("A") + i) for i, name in enumerate(shuffled)}
        labels_by_leg[leg_name] = labels
        for name in names:
            rows.append({
                "trial": trial,
                "leg": leg_name,
                "label": labels[name],
                "condition": name,
                "decode_warnings": conditions[name][1],
            })
    path = out_dir / "trials.csv"
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=["trial", "leg", "label", "condition",
                                               "decode_warnings"])
        writer.writeheader()
        writer.writerows(rows)

    template = out_dir / "responses_template.csv"
    with template.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["listener", "trial", "leg", "label", "score"])
        for row in rows:
            writer.writerow(["", row["trial"], row["leg"], row["label"], ""])
    return labels_by_leg


def abx_key(out_dir, built, rng, trials_per_pair):
    """One row per A/B/X trial: which condition is under test, which of A/B
    carries it, and which of A/B X is a copy of.

    Every one of those three is drawn independently per trial. Fixing any of
    them - always putting the system under test in B, say - would let a
    listener score above chance on the pattern rather than on the audio, and
    the binomial test the scoring script runs assumes they cannot.
    """
    rows = []
    trial = 0
    for leg_name, conditions in built.items():
        systems = [c for c in conditions if c not in ("reference",)]
        for system in systems:
            for _ in range(trials_per_pair):
                trial += 1
                system_is = rng.choice(["a", "b"])
                rows.append({
                    "trial": trial,
                    "leg": leg_name,
                    "system": system,
                    "a": "reference" if system_is == "b" else system,
                    "b": system if system_is == "b" else "reference",
                    "x": rng.choice(["a", "b"]),
                    "decode_warnings": conditions[system][1],
                })
    path = out_dir / "trials.csv"
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=["trial", "leg", "system", "a", "b", "x",
                                               "decode_warnings"])
        writer.writeheader()
        writer.writerows(rows)

    template = out_dir / "responses_template.csv"
    with template.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["listener", "trial", "answer"])
        for row in rows:
            writer.writerow(["", row["trial"], ""])
    return rows


def write_webmushra_config(out_dir, built, labels_by_leg):
    """A webMUSHRA (github.com/audiolabs/webMUSHRA) page config for the set.

    Optional: the blind-labelled WAVs beside it are a complete stimulus set
    that any media player can drive, and this file only exists so a session
    that wants a real MUSHRA slider interface does not have to be hand-
    authored. Written for the `mushra` page type webMUSHRA ships; nothing
    here validates it against a particular webMUSHRA release, so treat it as
    a starting point rather than something guaranteed to load unedited.
    """
    lines = [
        "testname: ac3forge listening test",
        "testId: ac3forge-landscape",
        "bufferSize: 2048",
        "stopOnErrors: true",
        "showButtonPreviousPage: true",
        "remoteService: service/write.php",
        "",
        "pages:",
        "  - type: generic",
        "    id: first",
        "    name: ac3forge listening test",
        "    content: >-",
        "      Rate each labelled item against the reference on the 0-100 scale.",
        "      One of the labelled items IS the reference; find it and give it 100.",
        "      See docs/landscape.md for the full protocol.",
    ]
    for trial, (leg_name, _) in enumerate(built.items(), start=1):
        labels = labels_by_leg[leg_name]
        lines += [
            "  - type: mushra",
            f"    id: {leg_name}",
            f"    name: {leg_name}",
            f"    content: Trial {trial} of {len(built)}.",
            "    showWaveform: false",
            "    enableLooping: true",
            f"    reference: configs/resources/{leg_name}/reference.wav",
            "    createAnchor35: false",
            "    createAnchor70: false",
            "    stimuli:",
        ]
        for label in sorted(labels.values()):
            lines.append(f"      {label}: configs/resources/{leg_name}/stim_{label}.wav")
    lines.append("")
    (out_dir / "webmushra_config.yaml").write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, required=True,
                        help="Directory to write the stimulus set into.")
    parser.add_argument("--method", choices=("mushra", "abx"), default="mushra")
    parser.add_argument("--seed", type=int, default=1534,
                        help="Randomization seed, recorded in session.json so a session is "
                             "reproducible after the fact (default: 1534).")
    parser.add_argument("--trials-per-pair", type=int, default=24,
                        help="abx only: trials per (leg, system) pair. 24 puts the "
                             "one-sided binomial significance threshold at 17 correct "
                             "(p < 0.05) (default: 24).")
    parser.add_argument("--render", choices=("native", "stereo"), default="native",
                        help="native presents the 5.1 legs as 5.1 (needs a 5.1 monitoring "
                             "setup); stereo renders them through FFmpeg's own downmix, "
                             "which applies the stream's cmixlev/surmixlev, for a headphone "
                             "session. Every condition gets the same treatment either way, "
                             "so the downmixer is a constant, not a variable (default: "
                             "native).")
    parser.add_argument("--start", type=float, default=0.0,
                        help="Seconds into the aligned item to start the excerpt.")
    parser.add_argument("--seconds", type=float, default=None,
                        help="Excerpt length. Default: the whole item. BS.1534-3 asks for "
                             "items of about 10 s; the committed fixtures are 2.5-3.0 s, "
                             "which is the shortfall roadmap VX7 exists to fix.")
    args = parser.parse_args()

    if not MANIFEST.exists():
        raise SystemExit(f"missing {MANIFEST}")
    manifest = json.loads(MANIFEST.read_text())
    rng = random.Random(args.seed)
    args.out.mkdir(parents=True, exist_ok=True)

    built = {}
    for leg_name, leg in manifest["legs"].items():
        print(f"building {leg_name} ...")
        built[leg_name] = build_conditions(leg_name, leg, args.out, args)

    if args.method == "mushra":
        labels_by_leg = mushra_key(args.out, built, rng)
        for leg_name, conditions in built.items():
            write_stimuli(args.out, leg_name, conditions, labels_by_leg[leg_name])
        write_webmushra_config(args.out, built, labels_by_leg)
    else:
        rows = abx_key(args.out, built, rng, args.trials_per_pair)
        # ABX needs no blinding in the filenames - the trials key says which
        # of A/B/X to play from which condition - so the conditions are
        # written under their own names.
        for leg_name, conditions in built.items():
            write_stimuli(args.out, leg_name, conditions,
                          {name: name for name in conditions})
        print(f"{len(rows)} ABX trials")

    # Anchor viability, per leg, recorded rather than only printed: whether
    # BS.1534's anchors actually anchored anything is a precondition for
    # reading that leg's MUSHRA scores at all, so it belongs with the session
    # it applies to rather than in a terminal scrollback.
    anchors = {}
    unusable = []
    for leg_name, conditions in built.items():
        reference = conditions["reference"][0]
        anchors[leg_name] = {}
        for hz in ANCHOR_HZ:
            headroom = band_energy_above_db(reference, hz)
            usable = headroom >= ANCHOR_MIN_BAND_ENERGY_DB
            anchors[leg_name][f"anchor{hz}"] = {
                "band_energy_above_cutoff_db": round(headroom, 2),
                "usable": usable,
            }
            if not usable:
                unusable.append((leg_name, hz, headroom))

    session = {
        "method": args.method,
        "seed": args.seed,
        "render": args.render,
        "start_s": args.start,
        "seconds": args.seconds,
        "baseline_version": manifest["baseline_version"],
        "external_tools": manifest["tools"],
        "anchor_min_band_energy_db": ANCHOR_MIN_BAND_ENERGY_DB,
        "legs": {name: {"conditions": sorted(c), "anchors": anchors[name]}
                 for name, c in built.items()},
    }
    (args.out / "session.json").write_text(json.dumps(session, indent=2) + "\n",
                                           encoding="utf-8")

    if unusable:
        print("\nANCHORS NOT USABLE on some legs - this decides how the results can be read:")
        for leg_name, hz, headroom in unusable:
            print(f"  {leg_name}/anchor{hz}: only {headroom:.1f} dB of the reference's energy "
                  f"sits above {hz} Hz")
        print("A low-pass anchor over a band the material barely occupies is a transparent copy")
        print("of the reference, so it cannot pin the bottom of the MUSHRA scale and that leg's")
        print("scores are not comparable to any other panel's. Run ABX on those legs instead, or")
        print("wait for roadmap VX7's real programme material. Recorded in session.json;")
        print("docs/landscape.md's protocol says the same.")

    warned = [(leg, name, conditions[name][1])
              for leg, conditions in built.items() for name in conditions
              if conditions[name][1]]
    if warned:
        print("\nFFmpeg's decoder complained about some stimuli - flagged in trials.csv:")
        for leg, name, text in warned:
            print(f"  {leg}/{name}: {text}")
        print("A concealed decode error is audible, but it is that decoder reading that")
        print("stream, not the encoder's quality - see this script's own header.")

    print(f"\nwrote the {args.method} stimulus set to {args.out}")
    print(f"  trials.csv               the key: which label is which condition")
    print(f"  responses_template.csv   fill one copy in per listener")
    print(f"  session.json             seed, render mode, baseline version")
    return 0


if __name__ == "__main__":
    sys.exit(main())
