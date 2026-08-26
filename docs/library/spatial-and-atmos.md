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

### Extent and rendering constraints

An object is not necessarily a point. `ObjectPlacement` also carries TS 103 420 §5.6.1's
extent and rendering-constraint fields, all of which reach OAMD:

```cpp
placement[obj] = {
    .position = {.x = 0.5, .y = 0.2, .z = 0.4},
    .gain = 1.0,
    // §5.6.1.2 Table 17, each axis in [0, 1]. Table 17 has three shapes and
    // only three: a point source, one value shared by all three axes, and
    // three separate ones - build_payload picks the cheapest that fits.
    .size = {.width = 0.4, .depth = 0.1, .height = 0.4},
    // §5.6.1.5.1 b_object_snap - what ADM calls channelLock: render to the
    // nearest speaker instead of panning between speakers.
    .snap = false,
    // §5.6.1.6 Table 20/21: which horizontal zones the renderer may use,
    // and whether the Top-Bottom zone is in play at all.
    .zone = ac3::oba::ZoneConstraint::kScreenOnly,
    .enable_elevation = true,
};
```

These are **transmitted, not rendered here**. §4.3 makes the renderer, not the decoder,
responsible for turning an extent into loudspeaker feeds, and the 5.1 downmix this encoder
builds is a point-source VBAP pan by construction — a downmix that spread the object would
then be spread *again* by the receiving renderer. So a sized object is transmitted as sized and
folded into the bed as a point, which is the honest split.

`Keyframe` carries the same four fields. `size` interpolates between keyframes the way position
and gain do (BS.2076-2 §10.3 lists width/height/depth among its interpolatable parameters);
`snap`, `zone` and `enable_elevation` do not — they are discrete decisions with no meaningful
halfway point, so `evaluate()` holds the earlier keyframe's value until the later one is reached.

Full program: [`examples/atmos_objects.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/atmos_objects.cpp).

| `AtmosConfig` | Default | Notes |
|---|---|---|
| `sample_rate` | `k48000` | Handed straight to the bed's own `eac3::FrameConfig`; nothing in the object layer reads it. |
| `bitrate_kbps` | 448 | Per substream, as everywhere else. The object metadata competes with the mantissas for the same frame, so an object stream needs headroom a plain 5.1 stream does not. |
| `dialnorm` | 31 | 1–31 (§5.4.2.8), as on the plain encoders. |
| `num_bands_idx` | 4 | Index into `oba::joc::kNumBands` (Table 50). More bands cost codewords without giving the matrix anything new to say. |
| `fine_quant` | `false` | §6.3.3.7's half-step quantizer, roughly one more bit per coefficient. Worth it when objects are nearly degenerate. |
| `fast_mdct` | `true` | The §7.9.4 fast forward MDCT for the whole object encode: the bed's substream (via `eac3::FrameConfig::fast_mdct`) **and** the per-object `band_energy` transforms feeding the reconstruction-matrix solve. `false` forces the direct §8.2.3.2 reference form everywhere, for validation — the CLI spells that `fast-mdct=off` on the `atmos*` commands. |
| `joc_domain` | `oba::joc::Domain::kQmf` | Where the reconstruction matrix is estimated. `kQmf` is §7.1's 64-band complex filterbank — what §6.6.6 describes and what a licensed decoder reconstructs in. `kMdctBand` is the 256-bin MDCT approximation this project used before it had a filterbank: cheaper, about 5 dB worse per object, and only correct against a decoder told the same thing. CLI: `joc-domain=mdct`. |

At most 16 objects (`oba::joc::kMaxObjects`, per TS 103 420 §8.3.2.2). `encoder.bed()` returns the
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

## Getting the objects back: `oba::joc::reconstruct`

`Eac3Decoder` reconstructs object audio into `DecodedSubstream::object_audio` whenever a frame
carries JOC, using `DecoderConfig::joc_domain` — `kQmf` by default, the same pair the encoder
estimated in. The result **lags the bed**, and by how much depends on the domain:

```cpp
// 256 samples of encode+decode, plus the JOC transform pair's own delay.
const int delay = 256 + ac3::oba::joc::reconstruction_delay(config.joc_domain);
```

`oba::joc::reconstruction_delay()` returns 576 for `kQmf` (the filterbank's 640-tap window less one
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
caller doesn't need to know which one it holds. It is the *per-object* layer: one object, one
path, no notion of a scene. `ac3cli atmos`'s built-in orbit and `live`'s `atmos` mode use it
directly; anything with more than one object and a file to load from wants `ObjectScene` below.

## The scene: `ac3::oba::ObjectScene`

`ac3/oba/scene.hpp`. `AtmosEncoder::encode_frame` takes one `ObjectPlacement` per object per
frame and nothing more, so every caller that wanted a *scene* — objects with names, a bed
assignment, automation, a file it can be saved to and reloaded from — used to build its own.
`ac3cli atmos-path` grew a keyframe-file grammar; the GUI's timeline grew a parallel one it
exports in that grammar; the station-broadcast example hard-coded a cue table in C++. This is
the one description they share.

It is **metadata and authoring**, deliberately. A scene says where an object is at a moment in
time; turning that into speaker feeds is the encoder's job, and a room-corrected render is
[Cavern](https://github.com/VoidXH/Cavern)'s rather than this project's. `Orientation` below is the
same kind of thing: it rewrites the coordinates that go into OAMD, so what reaches the
bitstream is an ordinary scene that happens to have been turned.

```cpp
using ac3::oba::Interpolation;
auto built = ac3::oba::ObjectScene::create({
    {.name = "flyby",
     .automation = {{.time_s = 0.0, .position = {.x = 0.0, .y = 0.5, .z = 0.5}, .gain = 0.6,
                     .interp = Interpolation::kSmooth},
                    {.time_s = 1.5, .position = {.x = 0.5, .y = 0.1, .z = 0.5}, .gain = 0.6,
                     .interp = Interpolation::kSmooth},
                    {.time_s = 3.0, .position = {.x = 1.0, .y = 0.5, .z = 0.5}, .gain = 0.6}}},
});
const auto& scene = *built;

std::vector<ac3::oba::ObjectPlacement> placement(scene.object_count());
scene.evaluate_into(seconds, placement);        // allocation-free, once per frame
const auto unit = encoder.encode_frame(views, placement);
```

Full program: [`examples/scripted_object_motion.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/scripted_object_motion.cpp).

### Interpolation and ramp semantics

Each automation point states how the segment that *starts* at it reaches the next one, so one
object can hold, then slide, then ease without being split into three:

| `Interpolation` | Between two points |
|---|---|
| `kHold` | Step. The value stays this point's until the next point's instant, then jumps. Mostly for gain gating and cue-accurate teleports — a position step is audible as a click in the panning. |
| `kLinear` (default) | Straight line, component by component. Exactly what `KeyframePath` has always done, which is why a scene built from a legacy keyframe file evaluates to the same doubles that file always produced. |
| `kSmooth` | Smoothstep, `f*f*(3-2f)`: the value leaves and arrives with zero slope. For where a linear ramp corners audibly. |

Outside the authored range, both ends **hold**: an object sits still before its first cue and
stays put after its last, rather than extrapolating into the wall or going silent. An object
with one point never moves. Callers evaluate at the frame's **end** time — every encode loop in
this repository does — because `AtmosEncoder` ramps its bed between successive frames'
placements, so the placement handed in is the value that ramp arrives *at*.

### Orientation

`Orientation` rotates the whole scene about the room's centre on the way out of `evaluate()`.
Angles are radians (`orientation_from_degrees()` converts); rotation runs in a centred cube —
x and y mapped from `[0,1]` to `[-1,+1]`, z already centred per §4.2.1 — applied yaw, then
pitch, then roll, and mapped back with a clamp to the room. Positive yaw turns the scene
clockwise seen from above, positive pitch raises the front, positive roll raises the right. An
all-zero `Orientation` is an *exact* no-op, not a rotation by zero, so an un-turned scene's
positions are bit-identical to the authored doubles.

```cpp
scene.set_orientation(ac3::oba::orientation_from_degrees(90, 0, 0));  // front wall → right wall
```

### The live half: `SceneCursor`

`SceneCursor` is the same timeline with per-object overrides an external source pushes in as
they arrive — the seam a live position source (roadmap `UX4`: OSC, MIDI, a game controller) and
the GUI's live room plug into, and the reason the scene type isn't just a static table.

```cpp
ac3::oba::SceneCursor cursor{std::move(scene)};
cursor.push({.object = 0, .placement = {.position = {.x = 0.75}, .gain = 0.9}});
cursor.sample_into(seconds, placement);   // overridden objects report the pushed value
cursor.release(0);                        // back to the authored timeline
```

Latest-value-wins, with nothing interpolated between updates, deliberately: a controller's
update rate is not the frame rate, guessing an intermediate position would invent motion nobody
authored, and `AtmosEncoder` already ramps its bed between the placements it is handed — which
is the right place for that smoothing, since it is the thing that knows the frame boundary. The
scene's orientation applies to pushed placements too, so a live object and its authored
neighbours never end up in different rooms.

### The serialised form

`to_json()` / `scene_from_json()`. **JSON, not YAML**: {fmt} (linked into this library for text
formatting generally) formats a number, it is not a parser for either document format, so
whichever this is still has to be read and written by code in this repository, and RFC 8259 is a
grammar small enough to implement completely and be sure of where YAML 1.2's is not — a
hand-rolled "YAML subset" would accept and reject files no other YAML tool agrees with, which is
worse than not offering YAML. Both other front ends already have a JSON reader to hand (Qt's,
Python's) if they ever want to read a scene without linking this library.

```json
{
  "ac3forge_scene": 1,
  "orientation": { "yaw_rad": 0, "pitch_rad": 0, "roll_rad": 0 },
  "objects": [
    {
      "name": "flyby",
      "bed": [],
      "automation": [
        { "t": 0, "x": 0, "y": 0.5, "z": 0.5, "gain": 0.6, "lfe": 0, "interp": "smooth" }
      ]
    }
  ]
}
```

Numbers are written short-round-tripped (the shortest decimal that reads back as the same
`double`), one automation point per line and members in a fixed order, so a scene under version
control shows real edits rather than formatting churn, and a save/load cycle is bit-exact. The
reader is strict about what it does not recognise — an unknown member is an error, because a
hand-authored file's likeliest fault is a misspelled key and silently defaulting `"gian"` to
`1.0` would be wrong in a way nothing reports. Forward compatibility rides on the
`ac3forge_scene` version number instead. `orientation` accepts `yaw_deg`/`pitch_deg`/`roll_deg`
in place of the radian spellings (but never both for one axis). `bed` names TS 103 420 Table 12
channel labels — `"lr"`, `"c"`, `"lfe"`, `"ls_rs"`, `"lb_rb"`, `"tfl_tfr"`, `"tsl_tsr"`,
`"tbl_tbr"`, `"lw_rw"`, `"lfe2"` — and an empty array means a dynamic object.

`scene_objects_from_keyframe_text()` / `to_keyframe_text()` read and write the older
whitespace-column grammar `ac3cli atmos-path` has always taken, unchanged including its
diagnostics — see [CLI → Commands](../cli/commands.md). `read_scene()` and `scene_from_text()`
take either, told apart by whether the first non-whitespace character is `{`, so a path argument
keeps working whichever form the file is in.

The keyframe form is *indexed and sparse* — a file may mention objects 0 and 2 and say nothing
about 1 — and what object 1 should then be is the caller's policy, not the library's:
`atmos-path` fans it out at room centre under its inverse-root gain law, `atmos-encode` keeps
that channel's existing static placement. That is why `read_scene()` returns raw
`SceneContents` with the gaps still empty; fill them, then `ObjectScene::create`.
`scene_from_text()` is the convenience for a caller with no policy of its own.

### Not in the C API or the Python bindings

Both expose `AtmosEncoder`, and `ObjectScene` deliberately does not follow it there yet. The
shape of the type is expected to move when `UX4`'s live source lands — `SceneCursor` exists
precisely because that seam is not finished — and both of those surfaces are candidates for the
coming API freeze, where an experimental type would be a lasting commitment. Exposing half of
it (say, the serialisation free functions but not the type) would be worse than exposing none:
a C caller would get a scene it could load and not evaluate. Load and save the JSON form from
either language and hand the resulting placements to the existing encoder bindings until the
type settles.

This layer backs `ac3cli atmos-path` and `atmos-encode`'s optional scene argument, the GUI's
object-path export, and the [station broadcast](station-broadcast.md) scene's ten authored
objects.

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

Turning it off drops TS 103 420 §8.3.1's `addbsi` object marker along with the container, so the
stream doesn't *advertise* an object layer either. That marker (`flag_ec3_extension_type_a` plus
§8.3.2.2's `complexity_index_type_a`) is the only thing a reader has to go on: it is what
`ac3::io::scan` reports as `ScannedStream::oba_complexity_index`, what
`ac3::io::build_codec_config_box` turns into the `dec3` box's Dolby Atmos extension, what
`ac3cli fmp4` writes as an HLS `CHANNELS="<N>/JOC"` attribute, and what FFmpeg keys its
"Dolby Digital Plus + Dolby Atmos" profile off. Left in, all four would claim objects that were
never encoded — the same empty-promise this mode exists to avoid.

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
