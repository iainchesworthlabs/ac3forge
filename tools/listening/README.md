# Listening test apparatus

Everything needed to run one subjective listening session over the landscape
legs, and to turn its answers back into the table on
[`docs/landscape.md`](../../docs/landscape.md#listening-test) — which is also
where the protocol lives. This directory is the machinery; that page is the
method and the results.

It exists because every quality number this project publishes is a waveform
or model metric. `README.md` and `docs/verification.md` have carried "no
listening test has been run" through nine releases, and ViSQOL's MOS-LQO —
the closest thing here to a perceptual score — is a *prediction* of what a
panel would say, not a panel.

| | |
|---|---|
| `gen_listening_stimuli.py` | builds the stimulus set: encodes with `ac3cli`, decodes everything with FFmpeg, aligns, blinds, and writes the trials key. Needs numpy and an `ffmpeg` binary. |
| `score_listening_test.py` | reads the key plus one response CSV per listener and prints the results table with confidence intervals. Stdlib-only — no build, no numpy, no FFmpeg. |
| `responses/` | where filled-in response CSVs go. See its own README for the schema and for what has been run so far. |

Each script's own module docstring carries the reasoning behind its choices
(why one decoder for every stimulus, why Wilson rather than Clopper–Pearson,
what the BS.1534-3 anchors are for). This file is the operator's sequence.

## Running a session

**1. Build `ac3cli`**, then generate the stimuli:

```bash
AC3CLI=build/config-linux-llvm/bin/ac3cli python3 tools/listening/gen_listening_stimuli.py --out listening-session
```

Read what it prints. Two things it reports decide how the results can be
read at all, and both are true of the material committed today:

- **Anchor viability.** BS.1534-3's low-pass anchors only anchor the scale if
  removing the band above the cutoff is audible. `reference_51.wav` carries
  0.059% of its energy above 3.5 kHz, so **both anchors are inaudible on both
  5.1 legs** and a MUSHRA session there cannot be scaled against any other
  panel's. The stereo leg's anchors are fine. Roadmap VX7 (real programme
  material) is what fixes this; until then, run MUSHRA on the stereo leg and
  ABX on the 5.1 legs.
- **Decoder complaints.** FFmpeg reports two out-of-range exponents decoding
  DEE's own committed stereo stream. That is audible, and it is that decoder
  reading that stream rather than DEE's encoder being worse — the flag rides
  through to the results table so nobody draws the second conclusion from the
  first.

Pick the method to match the panel you actually have. `--method mushra` (the
default) needs several listeners for its statistics to mean anything;
`--method abx` works with one and still supports a real significance claim.
`--render stereo` renders the 5.1 legs through FFmpeg's own downmix for a
headphone session; every condition gets the same treatment, so the downmixer
is a constant rather than a variable.

**2. Listen.** For MUSHRA, `webmushra_config.yaml` is a starting point for a
[webMUSHRA](https://github.com/audiolabs/webMUSHRA) interface; the
blind-labelled WAVs beside it are a complete stimulus set that any player can
drive. For ABX, `trials.csv` names which condition to play as A, as B and as
X for each trial — do not open it in front of the listener.

Copy `responses_template.csv` once per listener, fill in the `listener`
column and the answers, and put the copies in a `responses/` directory.

**3. Score:**

```bash
python3 tools/listening/score_listening_test.py --key listening-session/trials.csv --responses listening-session/responses/ --markdown-out results.md
```

MUSHRA output applies BS.1534-3 post-screening (a listener who scored the
hidden reference below 90 on more than 15% of trials is excluded, by name and
with their numbers, not quietly). ABX output carries an exact one-sided
binomial p-value against the 0.5 guessing rate.

**4. Land the results** in `docs/landscape.md`'s listening-test section,
commit the response CSVs under `responses/`, and update the "no listening
test has been run" sentences in `README.md` and `docs/verification.md` — they
stop being true at that point and not before.

## What a session needs from a person

- **Listeners.** MUSHRA's statistics need a panel; BS.1534-3 assumes
  experienced listeners and a session runs 20–30 minutes with training. ABX
  with one listener is the fallback and answers a narrower question: not "how
  much worse", only "could this listener tell them apart at all".
- **Monitoring.** `--render native` presents the 5.1 legs as 5.1 and needs a
  5.1 setup; `--render stereo` is the headphone path. Which one was used is
  recorded in `session.json` and belongs in the results table — the two are
  not the same experiment.
- **A quiet room**, and levels set once and left alone for the whole session.
  Level is a condition here: the stimuli are deliberately not re-gained,
  because a level change introduced by the apparatus would be scored as an
  artifact of the encoder.
