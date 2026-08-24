# Listening test responses

**No listening session has been run yet.** This directory is where the
answers land when one is, and it is empty of data on purpose rather than by
oversight — `README.md` and `docs/verification.md` still say no listening
test has been run because that is still true. See
[`../README.md`](../README.md) for the operator's sequence and
[`docs/landscape.md`](../../../docs/landscape.md#listening-test) for the
protocol.

Commit one CSV per listener per session, under a directory named for the
session, e.g. `2026-09-mushra-stereo/alice.csv`. Commit that session's
`trials.csv` and `session.json` beside them: without the key the answers
cannot be scored, and without `session.json` nobody can tell later which
render mode, seed and baseline version produced them.

## Schema

`gen_listening_stimuli.py` writes a `responses_template.csv` with the right
columns already filled in for the session it generated; copying that is
better than typing one of these from scratch.

**MUSHRA** — one row per (trial, label), which is one row per condition per
leg:

```csv
listener,trial,leg,label,score
alice,1,ac3-51-448,A,100
alice,1,ac3-51-448,B,42
```

`score` is BS.1534-3's 0–100 continuous quality scale. `label` is the blind
label from `trials.csv`; the listener never sees which condition it is.

**ABX** — one row per trial:

```csv
listener,trial,answer
solo,1,a
solo,2,b
```

`answer` is `a` or `b`: which of the two the listener judged X to be a copy
of. The scoring script reads the correct answer out of `trials.csv`.

The `listener` column is a stable identifier for one person across a session,
not a name that has to mean anything. It is what post-screening excludes on,
and what the results table counts, so two people must not share one.
