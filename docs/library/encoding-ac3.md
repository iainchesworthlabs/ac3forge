# AC-3: `ac3::FrameEncoder`

`ac3/encoder/encoder.hpp`. One call, one syncframe.

```cpp
// Heap-allocated: FrameEncoder carries several KB of MDCT scratch/history
// state (PREfast's C6262).
auto encoder = std::make_unique<ac3::FrameEncoder>(ac3::EncoderConfig{
    .bitrate_kbps = 448,
    .acmod = ac3::Acmod::k3_2,  // L, C, R, SL, SR
    .lfe = true,
});

// Table 5.8 order, LFE last, exactly kSamplesPerFrame (1536) samples each.
std::vector<std::vector<float>> pcm(6, std::vector<float>(ac3::kSamplesPerFrame));
// encode_frame takes a span of spans, so the views must outlive the call.
// Build them once and refill the buffers underneath each frame.
const std::vector<std::span<const float>> views{pcm.begin(), pcm.end()};

std::vector<std::byte> stream;
for (int frame = 0; frame < 31; ++frame) {  // 48000 / 1536, near enough
    fill_with_audio(pcm, frame, 48000.0);

    const auto encoded = encoder->encode_frame(views);
    if (!encoded) {
        std::printf("encode failed: %d\n", std::to_underlying(encoded.error()));
        return 1;
    }
    write(stream, *encoded);  // one complete syncframe
}
```

Full program: [`examples/encode_ac3.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/encode_ac3.cpp).

## `EncoderConfig`

| Field | Default | Notes |
|---|---|---|
| `sample_rate` | `k48000` | Also `k44100`, `k32000`. |
| `bitrate_kbps` | 192 | Must be one of the 19 Table 5.18 rates; `ac3::is_valid_bitrate` checks. |
| `dialnorm` | 31 | 1–31 (§5.4.2.8). 31 means "no attenuation", which is a claim about your content. |
| `chbwcod` | -1 | Coded bandwidth, 0–60. -1 derives it from the bit rate. |
| `acmod` | `k2_0` | Table 5.8, including `kDualMono` (1+1) — see below. |
| `dialnorm2` | none | `std::optional<int>`. Ch2's own dialnorm (§5.4.2.16); required when `acmod` is `kDualMono`, meaningless otherwise. |
| `lfe` | `false` | Adds one channel, coded last. |
| `coupling` | `false` | §7.4. Needs ≥ 2 full-bandwidth channels. |
| `cplbegf`, `cplendf` | -1, -1 | Sub-band indices; -1 lets the encoder choose from the per-channel rate. |
| `fast_mdct` | `true` | The §7.9.4 fast N/4-FFT forward MDCT instead of the direct §8.2.3.2 evaluation (~25× on the long-transform kernel, identical streams to within ~3e-12 coefficient error). `false` forces the direct reference form, kept as the validation oracle — the CLI spells that `fast-mdct=off`. |
| `drc` | none | `std::optional<meta::Profile>`. Absent leaves `dynrnge` clear in every block. |
| `heavy` | none | `std::optional<meta::HeavyConfig>`. Independent of `drc`. |
| `drc2` | none | `std::optional<meta::Profile>`. Ch2's own DRC, meaningful only under `kDualMono` — no fallback to `drc` when unset (see below). |
| `heavy2` | none | `std::optional<meta::HeavyConfig>`. Ch2's own heavy compression, same rule as `drc2`. |
| `cmixlev`, `surmixlev` | −4.5 dB, −6 dB | Tables 5.9/5.10. Always define the §7.8 downmix, whatever `acmod` is. |

Coupling is what makes 5.1 viable below 448 kbit/s: above the coupling frequency the
full-bandwidth channels stop carrying their own coefficients and share one coupling channel
plus per-band coordinates.

### Block switching

Automatic — no config field toggles it. A §8.2.2 transient detector runs per full-bandwidth
channel per block, and when **any** eligible channel switches anywhere in the frame, coupling is
turned off for that entire frame, for **every** channel — not just the one that switched.
`chincpl` in this encoder is frame-wide all-or-nothing rather than a per-channel flag, so the
§8.2.4.1 exclusion of a switching channel from coupling can only be honoured by disabling the
tool outright. The LFE never switches.

### Delta bit allocation

Automatic too (§7.2.2.6) — no config field. For each exponent run the encoder builds the masking
curve a second time from the real pre-quantization coefficient magnitudes and compares it against
the curve the transmitted exponents alone imply — which is all a decoder's allocator ever sees.
Any band where the two diverge by at least one Table 5.17 step (128 units, 6 dB) gets a
transmitted correction. The decision is per channel; the LFE is excluded, because §5.4.3.49's
`deltbae` loop stops short of it — no bitstream field exists to carry an LFE correction at all.
On AC-3 the coupling channel is in scope like any full-bandwidth channel, and `cpldeltbae` is
emitted as new information whenever it has segments to send. Two narrowings apply today: the
E-AC-3 encoder skips delta entirely for any frame where coupling is active (the added side
information broke the tightest coupling budgets — exactly the rates coupling exists to rescue),
and both encoders treat delta as a pure quality refinement — when its side-information cost would
make an otherwise-fittable frame fail to fit, the corrections are dropped and the frame
re-measured rather than refused.

### Rematrixing

Also automatic, and 2/0 only (§7.5.3) — no config field. Per Table 7.25 band, per block, the
encoder compares the minimum power of the L/R pair against the minimum power of their sum and
difference and codes whichever pair is smaller (`rematflg` per band) — the spec's minimum-power
decision. Strongly correlated stereo then puts nearly everything in the sum channel, and the
difference channel's bits go where they are actually needed.

### Dual mono (`acmod` 0, "1+1")

Not a channel layout — two independent, single-channel programmes sharing one syncframe (a
second language track, a commentary track), each levelled and compressed on its own. Set
`acmod = ac3::Acmod::kDualMono` and `dialnorm2`; `channels[0]` is Ch1, `channels[1]` is Ch2
(`fullbw_channel_count(kDualMono)` is 2, same as stereo, but the two channels are never
downmixed, coupled or rematrixed together — coupling silently stays off even if `coupling` is
set, since averaging two unrelated programmes together would leak one into the other). The
compression fields split the same way `dialnorm`/`dialnorm2` do: `drc` and `heavy` control Ch1
only, and Ch2's compressor/range controller is built **solely** from `drc2`/`heavy2` — there is
no fallback to `drc`/`heavy` when they are unset, so configuring only `drc`/`heavy` sends Ch2
out uncompressed. Each programme then carries its own words: `dynrng`/`compr` for Ch1,
`dynrng2`/`compr2` for Ch2. By convention 1+1 carries no LFE — two unrelated mono programmes
have no shared soundfield for a subwoofer to sit in, and the plan layer never builds one — but
the encoder itself does not reject `lfe = true` here, so a caller assembling an `EncoderConfig`
by hand has to honour the convention itself.

## Latency

`ac3/latency.hpp`. The first question an engine or conferencing integrator asks, answered as a
number rather than a shrug:

```cpp
const auto budget = encoder->latency();
std::printf("%d samples, %.2f ms
",
            budget.total_samples(),
            ac3::latency_ms(budget, encoder->config().sample_rate));
// 1792 samples, 37.33 ms
```

Everything below is **algorithmic** delay — what the coding scheme costs on an infinitely fast
machine. Compute time is a separate question, answered by
[the performance trend](../performance-trend.md); transport, device buffers and resampling are
yours to add.

| Term | AC-3 | Where it comes from |
|---|---|---|
| `frame_samples` | 1536 | `encode_frame` takes exactly one frame of PCM per channel, so nothing leaves the encoder until a whole frame has gone in. |
| `transform_samples` | 256 | The MDCT/IMDCT time-domain-alias-cancellation overlap. |
| `lookahead_samples` | 0 | The encoder never needs input beyond the frame it is coding. |
| `holdback_samples` | 0 | The §3.7 hold-back is an Annex E tool; AC-3 does not have it. |
| **`total_samples()`** | **1792** | 37.33 ms at 48 kHz, 40.63 ms at 44.1 kHz, 56.00 ms at 32 kHz. |

Two of those deserve more than a table row.

**The transform term is a shift, not a wait.** Block *b*'s analysis window spans input samples
`[256b - 256, 256b + 256)`, and a decoder cannot finish any 256-sample segment until it holds
both blocks whose windows cover it. So the frame the encoder emits reconstructs input samples
`[-256, 1280)`: the frame's last block-worth of input is still in the encoder's overlap history
and only reaches the wire in the next frame. Put the other way round, **decoded output sample
`k` is input sample `k - 256`** — and that is a claim you can check, not a description of intent.
[`tests/decoder/test_latency.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/tests/decoder/test_latency.cpp)
puts an impulse into silence at a known absolute position, encodes, decodes, and finds it again
256 samples later; a decaying tone burst through the same chain gives the same 256 by
cross-correlation, which is immune to a quantizer having moved the peak.

**Lookahead is zero because of how `blksw` is decided.** `TransientDetector::detect()` reads
only the 256 *new* samples of the block it decides — §8.2.2 defines the decision on the
analysis window's second half and nothing else — so the block-switch decision never waits for
audio the frame does not already contain. A psychoacoustic encoder that deferred that decision
by a block would add 256 samples here.

`total_samples()` is a bound, not an average: a sample entering the encoder is delayed by
between `transform_samples` (the last sample of a frame) and `total_samples() - 1` (the first),
and never more. The decoder adds nothing of its own — `ac3::FrameDecoder::latency_samples()` is
zero, structurally, because `decode_frame` returns a frame's full PCM from the call that
supplies its bytes.

The same budget is on the C API as `ac3forge_encoder_latency()` /
`ac3forge_encoder_latency_samples()` (see [the C API](c-api.md)) and in Python as
`FrameEncoder.latency` / `.latency_samples` (see [the Python API](python-api.md)).

---

See also: [Metadata](metadata.md) — `dialnorm`, `drc`, `heavy` and the mix-level fields above
are all configured via the metadata layer; [Encoding E-AC-3](encoding-eac3.md) — same shape,
different container, and one tool that changes the latency budget.
