# Conformance vectors

Every other check in this project consumes someone else's streams as an oracle — FFmpeg's
decoder, Dolby's Reference Player, Dolby's Media Encoder. This is the one that goes the other
way: a versioned set of streams *this* encoder produces, published so another implementation has
something concrete to test against.

It exists because nothing else free does. ATSC A/52 and ETSI TS 102 366 are both published
documents, but neither body distributes conformance bitstreams publicly, and Dolby's own test
material is licensed. Someone writing an AC-3 or E-AC-3 decoder from the standard has the text
and nothing to check their reading of it against.

## Getting it

Attached to every [GitHub release](https://github.com/iainchesworthlabs/ac3forge/releases) as
`ac3forge-conformance-vectors-<version>.tar.gz`, alongside the packages, the SPDX SBOM, the
`SHA512SUMS` and the Sigstore provenance attestations — it is signed, checksummed and attested
exactly like every other release asset (see [Releasing](releasing.md#what-gets-published)).

## What is in it

60 streams: AC-3 in every coding mode, E-AC-3 at every layout and with each Annex E tool and the
combinations that matter, both VBR shapes, every sample rate including the `fscod2` half rates,
and Dolby Atmos with objects. Each one comes with the PCM it was encoded from, its hashes, and a
sentence naming the syntax it is there for.

```
MANIFEST.json    every vector: what it exercises, its hashes, FFmpeg's ability to read it
README.md        the same usage notes as below, so the bundle stands alone
source/          the PCM each vector was encoded from
vectors/ac3/     AC-3 (A/52 Annex A-D)
vectors/eac3/    E-AC-3 (A/52 Annex E)
vectors/atmos/   E-AC-3 carrying Atmos objects (OAMD + JOC, ETSI TS 103 420)
paths.txt        the authored object-motion keyframes one Atmos vector was built from
```

Coverage is a cross of two axes rather than their full product: every layout and every coding
tool at 48 kHz, and every sample rate at a representative pair of layouts. The full product would
be enormous and mostly redundant. The manifest says so in as many words, so a missing
7.1.4-at-16-kHz vector does not read as a coverage claim.

## Using it to test a decoder

**Decode each vector and measure against its `source`.** That is the check that means something
across implementations. `MANIFEST.json` names the source WAV for every vector that has one (the
synthesis commands — `silence`, `atmos` — have none).

**Do not compare decoded PCM byte-for-byte against `decoded_pcm_sha256`.** That hash is this
project's own decoder's output, on the exact toolchain the manifest's `built_with` names. Two
correct decoders disagree in the last bits of every float sample. The hash is there so a
regenerated bundle can be checked against a published one, not so a third-party decoder can be.

**Do compare `decoded_levels`.** Per-channel peak and RMS in dBFS survive any correct decoder's
own rounding, so a channel more than a fraction of a dB out — or in the wrong slot — is a real
finding. Channel names are A/52 Table 5.8 order for AC-3 and Table E2.5 location order for
E-AC-3.

**Read `exercises` before chasing a failure.** It names what each vector is for, down to the
clause: which Annex E tools are on, whether the stream carries dependent substreams, whether it
is a reduced-rate (`fscod2`) stream, whether object metadata rides in an EMDF container.

**Check `ffmpeg` before reaching for a second opinion.** Three states, not two:

| `support` | Meaning |
|---|---|
| `full` | FFmpeg decodes the audio — it is available to you as an independent oracle |
| `header_only` | FFmpeg walks the framing correctly but refuses the audio |
| `none` | FFmpeg cannot read the stream at all |

The mapping comes straight from [Validation → Where the oracles don't
reach](verification.md#where-the-oracles-dont-reach): a second dependent substream (7.1.4) is
rejected by `ff_ac3_parse_header`; enhanced coupling and transient pre-noise processing have no
syntax in FFmpeg's Annex E parser at all; `fscod2` framing is read but the audio is refused (by
FFmpeg *and* by Dolby's own Reference Player). Those four are exactly where an independent
implementation is most useful, because nothing else public reads them either — and exactly where
this vector set is checked only against this project's own decoder, which is a weaker claim and
is stated as one.

## What it does not prove

- **The source material is synthetic** — sine tones plus seeded, band-limited noise. That is what
  makes the set redistributable, and it exercises every coding tool, but it is not real programme
  material: a defect that only shows on speech or music is not something these vectors can find.
  Redistributable CC0 speech and music now exist in the tree — roadmap VX7 landed them as
  `tests/golden/audio/programme_speech_stereo.flac` and `programme_music_stereo.flac` — but this
  set was never wired to them: `tools/generators/gen_conformance_vectors.py` still synthesizes
  every source from first principles, and its own comment marks the spot where those files would
  join. Pointing the generator at them is outstanding work, not a pending roadmap item.
- **Agreement with these vectors is agreement with one implementation**, not with the standard.
  Where FFmpeg can read a vector, the manifest says so, and cross-checking against it is
  meaningfully stronger than checking against this project alone.
- **The Atmos vectors are unsigned** unless the operator regenerating them supplies a key. A
  licensed decoder gates object decoding on an authenticity tag keyed to a secret this project
  does not have — see [Object signing](concepts/object-signing.md). The unsigned vectors are
  fully decodable by any implementation that does not enforce that gate.

## Hashes are per-toolchain

Encoded output is **not** currently bit-identical across compilers and architectures.
[Building](building.md) records a measured cross-toolchain difference, and the arm64 legs sit
6.0 dB off every x86 leg on the gold-reference gate. So:

- Regenerating with the toolchain the manifest's `built_with` names reproduces every hash in it
  exactly. That is asserted, not assumed — `--check-determinism` generates the whole bundle a
  second time and fails if a single hash moves.
- Regenerating with a different compiler or on a different architecture produces different
  hashes for the same *correct* streams.

Roadmap VX11 asked why that offset (6.02 dB, exactly one exponent) is there, and has closed
without an answer: both hypotheses it proposed — Homebrew's libm, then FMA contraction — were
falsified by direct measurement, so it is architectural in some way still unidentified. What that
leg produced instead is a watch: `tools/checks/check_cross_platform_hash.py` pins a SHA-256 of the
encoded bytes per `(kernel, transform mode)` pair in `tests/golden/bitstream-hashes.json`, so the
divergence cannot change size silently. VX12 — gating byte-identical encodes across every leg —
is the one still open, and until it lands the bundle records exactly what built it and this page
says the hashes are per-platform.

## Regenerating it

```bash
cmake --preset config-linux-gcc && cmake --build --preset build-linux-gcc
python tools/generators/gen_conformance_vectors.py \
    --cli build/config-linux-gcc/bin/ac3cli \
    --out dist/conformance-vectors \
    --check-determinism --archive
```

`--archive` writes the `.tar.gz` with every timestamp, owner and mode pinned, so two identical
bundles produce two identical archives. `--check-determinism` generates a second bundle into a
temporary directory and diffs the manifests. The release workflow runs both.

Adding a vector means adding one `Vector(...)` to `build_vector_list()` in that script, with the
`exercises` sentence that says why it is there. The FFmpeg-support column is derived, not typed:
`_ffmpeg_support()` applies the rules above from the vector's own layout, tools and sample rate,
so it cannot drift out of step with what the oracle table says.

The *list* is hand-maintained, though. `tools/checks/check_matrix_coverage.py` asks whether
`tools/ci/run_codec_matrix.sh` names every CLI token the binary knows about; nothing asks that of
this set, so a new layout or tool token can land without gaining a vector. What CI does catch is
the other direction — the generator drives real `ac3cli` command lines on every push, so a token
that changes spelling fails a pull request rather than a release.

To include the signed Atmos vector, supply a key — the same environment `ac3cli` itself reads:

```bash
AC3FORGE_SIGNING_KEY_FILE=/path/to/key python tools/generators/gen_conformance_vectors.py \
    --cli build/config-linux-gcc/bin/ac3cli --out dist/conformance-vectors --sign
```

Without a key `--sign` is refused outright rather than silently producing an unsigned stream
under a signed name. No key ships with this project and none is invented by the generator.

## Licensing

Every stream in the set is this project's own encoder output, encoded from source PCM this
project generated — the checked-in gate fixtures at 48 kHz, and synthesis by the generator itself
at the other rates. Nothing from `tests/golden/external-baseline/` (Dolby Media Encoder and
FFmpeg output, kept for comparison only) is ever included. The bundle carries the project's own
licence.

## See also

- [Validation](verification.md) — the oracle table this set's FFmpeg column is derived from
- [Threat model](threat-model.md) — the posture for feeding a decoder untrusted bytes
- [Releasing](releasing.md#what-gets-published) — where the bundle sits among the release assets
