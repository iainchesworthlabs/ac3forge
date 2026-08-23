# Spatial & Atmos objects

## The spatial object layer

`ac3/spatial/spatial.hpp`. Mono sources placed on the ITU-R BS.775 ring, rendered to a 5.1
bed. This is the plain-AC-3 object path: the output is an ordinary 5.1 stream and *nothing
survives about where the object was*.

```cpp
ac3::spatial::BedRenderer renderer;
// add_object allocates, so call it before rendering starts.
const std::size_t object = renderer.add_object({.azimuth_deg = 0.0, .gain = 0.7});
```

Rendering is clocked at the 256-sample block, because that is the rate automation runs at;
gains ramp linearly within a block, so moving an object does not click. The render path itself
does not allocate.

```cpp
// One full turn every two seconds.
renderer.set_target(object, {.azimuth_deg = 180.0 * seconds, .gain = 0.7});

// Six writable 256-sample spans into this block of the frame:
// L, C, R, SL, SR, LFE. render_block overwrites them.
std::array<std::span<float>, 6> block_out{};
for (std::size_t ch = 0; ch < 6; ++ch) {
    block_out[ch] = std::span<float>{bed[ch]}.subspan(
        static_cast<std::size_t>(block * ac3::spatial::kBlockSamples),
        ac3::spatial::kBlockSamples);
}
const std::array<std::span<const float>, 1> audio{std::span<const float>{source}};
renderer.render_block(audio, block_out);
```

Full program: [`examples/spatial_objects.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/spatial_objects.cpp).

`pan_azimuth(deg)` and `pan_room(x, y)` expose the panner directly if you want the gains
without the renderer. Both are energy-normalized pairwise (VBAP on the horizontal ring),
Σg² = 1.

There is no `z`. A 5.1 ring has no height speakers, so a raised source folds onto the ring at
its azimuth, at full level. Objects never reach the LFE by panning — `lfe_send` is the only
route.

## Objects with metadata: `ac3::oba::AtmosEncoder`

`ac3/oba/atmos.hpp`. The same objects, but their positions survive: the output is one ordinary
5.1 E-AC-3 stream with OAMD and JOC payloads riding beside it in an EMDF container
(TS 102 366 Annex H, carried in a block skip field). A decoder that knows about neither plays
the bed unchanged, at full level — that is the design target, not a fallback.

```cpp
constexpr int kObjects = 3;
// Object metadata competes with the mantissas for the same frame, so an
// object stream wants more headroom than a plain 5.1 one.
ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 448}, kObjects};
```

```cpp
// Positions are room-anchored per §4.2.1: x 0 at the left wall to 1 at
// the right, y 0 front to 1 back, z -1 at the floor to +1 at the
// ceiling (0 is listener height).
std::array<ac3::oba::ObjectPlacement, kObjects> placement{};
placement[obj] = {
    .position = {.x = 0.5 + 0.45 * std::cos(angle),
                 .y = 0.5 + 0.45 * std::sin(angle),
                 .z = 0.25 * static_cast<double>(obj)},
    .gain = 1.0,
};

const auto unit = encoder.encode_frame(views, placement);
```

Full program: [`examples/atmos_objects.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/atmos_objects.cpp).

| `AtmosConfig` | Default | Notes |
|---|---|---|
| `bitrate_kbps` | 448 | Per substream, as everywhere else. |
| `num_bands_idx` | 4 | Index into `joc::kNumBands` (Table 50). More bands cost codewords without giving the matrix anything new to say. |
| `fine_quant` | `false` | §6.3.3.7's half-step quantizer, roughly one more bit per coefficient. Worth it when objects are nearly degenerate. |
| `fast_mdct` | `true` | The §7.9.4 fast forward MDCT for the whole object encode: the bed's substream (via `eac3::FrameConfig::fast_mdct`) **and** the per-object `band_energy` transforms feeding the reconstruction-matrix solve. `false` forces the direct §8.2.3.2 reference form everywhere, for validation — the CLI spells that `fast-mdct=off` on the `atmos*` commands. |
| `joc_domain` | `joc::Domain::kQmf` | Where the reconstruction matrix is estimated. `kQmf` is §7.1's 64-band complex filterbank — what §6.6.6 describes and what a licensed decoder reconstructs in. `kMdctBand` is the 256-bin MDCT approximation this project used before it had a filterbank: cheaper, about 5 dB worse per object, and only correct against a decoder told the same thing. CLI: `joc-domain=mdct`. |

At most 16 objects (`joc::kMaxObjects`, per TS 103 420 §8.3.2.2). `encoder.bed()` returns the
5.1 bed the last frame encoded — what a legacy decoder hears, and the thing most worth
checking — and `encoder.parameters()` the pre-quantization reconstruction matrix.

The matrix is the minimum mean-square estimate `M = P Dᵀ (D P Dᵀ + εI)⁻¹`. Because the encoder
built the downmix it knows `D` exactly rather than estimating it, which makes the solve
near-exact for well-separated objects. `P` — each object's per-band power — is read off §7.1's
complex QMF, which is the domain the decoder will apply the result in; see
[which domain the matrix lives in](../concepts/atmos-joc.md#which-domain-the-matrix-lives-in).
Two limits are structural, not bugs: objects sharing a direction cannot be separated by any
linear combination of the bed, and Dolby's decoder will not treat these as objects at all. Both
are covered in [Atmos & JOC](../concepts/atmos-joc.md#two-honest-limitations).

## Getting the objects back: `joc::reconstruct`

`Eac3Decoder` reconstructs object audio into `DecodedSubstream::object_audio` whenever a frame
carries JOC, using `DecoderConfig::joc_domain` — `kQmf` by default, the same pair the encoder
estimated in. The result **lags the bed**, and by how much depends on the domain:

```cpp
// 256 samples of encode+decode, plus the JOC transform pair's own delay.
const int delay = 256 + ac3::joc::reconstruction_delay(config.joc_domain);
```

`joc::reconstruction_delay()` returns 576 for `kQmf` (the filterbank's 640-tap window less one
64-sample hop) and 256 for `kMdctBand`. Ask it rather than hard-coding either: code that compares
reconstructed objects against a known source and gets the shift wrong measures the latency
instead of the reconstruction, and still looks plausible.

The filterbank is usable on its own as `ac3::dsp::QmfAnalysis` / `QmfSynthesis` (`ac3/dsp/qmf.hpp`)
— 64 complex subbands, one timeslot per 64 samples, perfect reconstruction.

## Scripted motion: `ac3::oba::motion`

`ac3/oba/motion.hpp`. `AtmosEncoder::encode_frame` always took a fresh `ObjectPlacement` per
call; what this adds is a shared way to say *where* an object is at a given moment, so a caller
stops reimplementing that per-frame math independently the way `atmos_objects.cpp` does.

```cpp
// A closed-form orbit - evaluated exactly rather than decimated into
// keyframes, so it stays an exact circle.
const auto orbit = ac3::oba::make_orbit_path(/*rate_hz=*/0.5, /*phase_rad=*/0.0,
                                             /*height=*/0.5, /*gain=*/0.6, /*lfe_send=*/0.0);

// Sparse authored points, linearly interpolated between neighbours and held
// at the ends rather than extrapolated.
auto keyframed = ac3::oba::KeyframePath::create({
    {.time_s = 0.0, .position = {.x = 0.0, .y = 0.5, .z = 0.0}, .gain = 0.0},
    {.time_s = 0.8, .position = {.x = 0.5, .y = 0.9, .z = 0.0}, .gain = 0.8},
    {.time_s = 1.6, .position = {.x = 1.0, .y = 0.5, .z = 0.0}, .gain = 0.0},
});
```

```cpp
// One call per frame gets every object's placement at that instant, in
// path order - exactly the span encode_frame() wants.
const auto placement = ac3::oba::evaluate_placements(paths, seconds);
const auto unit = encoder.encode_frame(views, placement);
```

Full program: [`examples/scripted_object_motion.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/scripted_object_motion.cpp).

`ObjectPath` is a `std::variant` of the two kinds behind one `evaluate(time_s)` interface, so a
caller (CLI, GUI) doesn't need to know which one it holds. This layer is scoped to
authored/batch motion — `evaluate(time_s)` is deliberately time-based so a future live-driven
cursor could reuse it, but that plumbing isn't built here. It backs `ac3cli atmos-path` and
`live`'s `atmos` mode, and the [station broadcast](station-broadcast.md) scene's ten authored
object paths.

## Objects-or-nothing: `AtmosConfig::emit_object_metadata`

Whether to emit the EMDF object container (OAMD + JOC) at all. On by default: an object-aware
decoder gets the objects, and one that ignores the container plays the 5.1 bed underneath it —
the design target. Turning it off is the *only* way to keep the bed playable on a decoder that
**validates** `emdf_protection` (see [Object signing](../concepts/object-signing.md)): such a
decoder treats the container's sync word as a commitment to object decoding and refuses the
whole stream if the tag doesn't check out, rather than falling back. With no container there is
no sync word to find, so it decodes the bed as ordinary 5.1. The choice is objects-or-nothing,
never both.

```cpp
ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 448, .emit_object_metadata = false}, kObjects};
```

Full program: [`examples/atmos_fallback.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/atmos_fallback.cpp)
— encodes the same objects both ways and confirms both decode as an ordinary 5.1 bed.

The stream size is unaffected either way — this is CBR, so `frmsiz` follows `bitrate_kbps`
regardless of what rides in the skip field. What differs is where those bits *go*: with the
container left out, the frame's rate control gives the freed skip-field bytes back to the
mantissas, so the two configurations' decoded bed is close but not bit-identical.

---

See also: [Encoding E-AC-3](encoding-eac3.md) — Atmos objects ride inside an ordinary E-AC-3
stream; [Object signing](../concepts/object-signing.md) — what makes a validating decoder
accept the container in the first place; [A worked scene: the station broadcast](station-broadcast.md)
— a complete 115-second authored scene built on this API, from synthesis to `.ec3`;
[ADM → Atmos bridging](adm-bridge.md) — mapping a professional ADM BWF master's beds and objects
onto this same `ObjectPath`/`AtmosEncoder` surface.
