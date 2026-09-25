# AC-4 (ETSI TS 103 190): `ac4::decoder` and `ac4::encoder`

`ac4dec/decoder.hpp`, library `ac4::decoder`, and `ac4enc/encoder.hpp`, library `ac4::encoder`,
with the inspector both work through, `ac4/ac4.hpp` in library `ac4::ac4`. An AC-4 decoder and
encoder written from ETSI TS 103 190-1 V1.4.1 (channel-based coding) and TS 103 190-2 V1.3.1
(immersive and personalized audio). The libraries are in namespace `ac4` and link nothing from
`ac3::forge`: AC-4 shares no bitstream syntax with AC-3 or E-AC-3. The encoder is described under
[Encoding a stream](#encoding-a-stream).

It decodes the mono, stereo, 3.0, 5.X and 7.X channel elements in each of Part 1's codec modes
(SIMPLE, ASPX and the three A-CPL modes) at every frame rate; the immersive element of 7.0.4 and
7.1.4 in full and core decoding, rendered by Part 2's channel renderer; object audio, A-JOC in
full and core decoding and direct-coded objects, with each object's metadata ([Objects](#objects));
streams of several presentations, the one a system chooses decoded with all of its substreams
mixed; the output level and dynamic range control, dialogue enhancement and the downmix; and it
conceals a frame that does not decode when asked to. It refuses, per substream and per frame, with
`DecodeError::kUnsupported` and a reason: the speech spectral frontend, the 9.X.4 and 22.2
channel elements, and output at 96 or 192 kHz. [Development status](development-status.md)
has the detail, feature by feature, and [Validation](../verification.md#ac-4) says how each part
is checked.

## Decoding a stream

A player's stream arrives in pieces. `ac4::SyncFrameSplitter` hands over each sync frame once all
of it has arrived, and `ac4::Decoder::decode_by_block` turns it into PCM for one presentation, in
blocks of 256 samples. The settings a television offers go in the configuration:

```cpp
ac4::DecoderConfig config;
config.output.output_level_dbfs = -24.0;     // Lout: the dialogue level the output is taken to
config.output.drc = ac4::DrcMode::kDefault;  // the mode that output level selects
config.output.downmix = ac4::DownmixTarget::kStereo;
config.output.dialogue_enhancement_db = 6.0;  // up to the stream's cap
config.presentation.language = "en";          // where the stream offers a choice
ac4::Decoder decoder(config);

// The splitter owns no memory: this holds the frame being assembled.
std::vector<std::byte> storage(ac4::kSplitterRecommendedBuffer);
ac4::SyncFrameSplitter splitter{storage};
std::size_t frames = 0;
for (;;) {
    const auto next = splitter.next();
    if (next.status == ac4::SyncFrameSplitter::Status::kNeedMoreInput) {
        const std::span<std::byte> space = splitter.writable();
        const std::size_t want = std::min<std::size_t>(space.size(), 4096);
        in.read(reinterpret_cast<char*>(space.data()), static_cast<std::streamsize>(want));
        const auto got = static_cast<std::size_t>(in.gcount());
        if (got == 0) {
            splitter.finish();
        } else {
            splitter.commit(got);
        }
        continue;
    }
    if (next.status != ac4::SyncFrameSplitter::Status::kFrame) {
        break;  // kEndOfStream, or kTruncated at a cut-off last frame
    }
    ++frames;
    const auto info = decoder.decode_by_block(next.frame.raw_ac4_frame, sink);
    if (!info) {
        const std::string_view reason = decoder.refusal_reason();
        fmt::printf("frame %zu: %.*s\n", frames, static_cast<int>(reason.size()),
                    reason.data());
        return 1;
    }
}
decoder.flush(sink);  // the samples held back short of a block, as one shorter block
```

Full program: [`examples/decode_ac4.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/decode_ac4.cpp) —
decodes the stream it is given to stereo, then prints the presentations it found and the metadata
of the one it decoded. `ctest` runs it on a committed DEE stream.

`sink` is any callable taking a `const ac4::PcmBlock&`: one span per channel, the speakers they
are for, the sample rate and the block's position in the output. `ac4::BlockSink` refers to it
without copying it, so it has to outlive the call, which a lambda named before the loop does. A
frame's samples rarely divide into 256 (2 002 at 23.976 fps), so each call hands over the whole
blocks it has and holds the rest for the next frame, and `flush()` hands over what is left at the
end. Once the layout is set, decoding this way allocates nothing per frame. `decode()` returns the
same output a frame at a time instead, as an `ac4::DecodedFrame` of planar channels.

A frame that produces no output, such as the frames of a stream joined before its first I-frame,
returns nothing rather than an error. A frame that does not decode returns its error, and
`refusal_reason()` says why; with `DecoderConfig::concealment` set to `kRepeatFade` or `kMute`, a
concealed frame comes back in its place, marked as one.

## The controls

`OutputConfig` holds what the listener sets, and `Decoder::set_output()` changes it from the next
frame while a stream plays. A stage takes a new value as it takes the stream's own from one frame
to the next, and a new layout starts its channels' synthesis from silence.

| `OutputConfig` field | What it sets | Default |
|---|---|---|
| `output_level_dbfs` | Lout (Part 1 clause 5.7.9.3.3), the level the stream's dialnorm is taken to, cutting or boosting by 2^((Lout − dialnorm) / 6) | unset: the coded level, nothing compressed |
| `drc` | Which of Table 161's DRC decoder modes compresses: `kDefault` takes the one clause 5.7.9.2 gives the output level; `kHomeTheatre`, `kFlatPanelTv`, `kPortableSpeakers` and `kPortableHeadphones` name one; `kOff` applies the level alone | `kDefault` |
| `headphones` | Whether `kDefault` takes portable headphones rather than portable speakers where the level falls in their range, −16 to 0 dBFS | `false` |
| `dialogue_enhancement_db` | G_DE (clause 5.7.8): how far the dialogue is raised, up to the cap the stream sets, 3, 6, 9 or 12 dB | 0, the tool bypassed |
| `downmix` | The layout (clause 6.2.17): `kAsCoded`, `k5X` (a 7.X element folded to 5.X), `kStereo` (the method the stream prefers), `kLoRo`, `kLtRt`, `kMono`; and for the immersive element and an intermediate spatial format, `k7X4`, `k7X2`, `k7X0`, `k5X4` and `k5X2` (Part 2 clauses 5.10.2 and 5.10.3) | `kAsCoded` |
| `mix_lfe` | Whether a two-channel or mono downmix takes the LFE at the stream's `lfe_mixgain`, as Part 1 does | `true` |
| `dialogue_gain_db` | g_dialog (clause 6.2.16.1): a presentation's dialogue against its music and effects, up to the stream's `g_dialog_max` | 0 |
| `associated_gain_db` | g_assoc (clause 6.2.16.2), 0 or less: a presentation's associated audio | 0 |

`DecoderConfig` holds the rest: `output`, `presentation` (below), `concealment`, `level` (the
`md_compat` level the decoder claims, 3 by default; presentations above it are not chosen),
`decoding` (full or core decoding, Part 2 clause 4.7, full by default), and `syntax`, a trace of
every syntax element read. `syntax` is an `ac4::SyntaxTrace`, a
`std::function` the configuration owns, and the decoder keeps a copy of its own, so a lambda
written in place, in a class's constructor for instance, stays valid; what it captures by
reference has to outlive the decoder. The records are described under
[Validation](../verification.md#the-decoders-syntax).

`ac3cli decode` spells each control as an option (`output-level=`, `drcmode=`, `headphones`,
`dialogue-enhancement=`, `channels=`, `downmix=`, `mix-lfe=`, `dialogue-gain=`,
`associated-gain=`, `md-compat=`, `conceal=`); see
[Commands](../forge/cli/commands.md#the-output-stage-channels-downmix-drcmode).

## Choosing a presentation

A stream can carry several presentations of its substreams: music and effects with dialogue in
one language or another, the main audio with audio description, and so on (Part 2 clause 4.8).
`PresentationChoice` says which one to decode, and `Decoder::set_presentation()` changes it from
the next frame. Every presentation's substreams are read in every frame, so a newly chosen one
needs no I-frame; its signal starts from silence.

- `presentation_id`: the presentation carrying it, which stays with the presentation as the table
  of contents changes.
- `index`: its position in the table of contents.
- Otherwise the preferences, in the order clause 4.8.2 lists them: `language`, a BCP 47 tag, a
  presentation whose tag matches it whole before one whose primary subtag does; `associated`, the
  content classifier of the associated audio wanted (Part 1 Table 91), with `associated_type`
  refining it by Table 92; and `headphones`, whether a presentation rendered for headphones before
  it was encoded (`b_pre_virtualized`) comes before one that was not, or after it.

Of the presentations this decoder can decode, the stream has not disabled and whose `md_compat` is
within the decoder's level, the one that meets the choice is decoded, the first in the table of
contents among equals. `ac4::select_presentation(toc, choice, level)` makes the same choice from a
table of contents alone. Where the text leaves the choice open, `src/ac4dec/ERRATA.md` records the
reading taken.

## What the decoder reports

- `presentations()`: each presentation of the last frame's table of contents, as
  `ac4::PresentationInfo`: its `presentation_id`, version, configuration and `md_compat`, whether
  it is enabled, an alternative or pre-virtualized, its name (Part 2 clause 6.3.3.1.4; a name sent
  in chunks over several frames once the decoder has all of it), its language, the channels it
  decodes to, its substreams with the role each plays, and whether this decoder decodes it and may
  choose it.
- `metadata()`: the metadata of the presentation decoded, as the frames read so far have sent it:
  the loudness values (dialnorm and Part 1 clause 4.3.12.3's further values), the DRC
  configuration with each decoder mode and the one applied, dialogue enhancement's method, channels
  and cap, and the downmix gains and preferred method. Values a stream sends only in I-frames are
  kept until a change of source.
- `parse()`: a frame read without decoding, as an `ac4::FrameReport`: every substream of its
  `substream_index_table()` in index order, what it turned out to be, how many bits the syntax
  took of it, and, for one not read to its end, the error and the reason. A substream that no
  element of the table of contents this decoder reads names, an HSF extension substream that
  nothing claims among them, is reported as refused and unread.
- `latency_samples()`: the decoder's delay at the output rate, 1 313 samples at
  `frame_rate_index` 13 and at the other indices the same at the internal rate plus the sample rate
  converter's delay. `decode_by_block()` holds back up to 255 samples more.

`ac3cli probe json=1` writes the same reports for a stream: see
[Commands](../forge/cli/commands.md#ac-4).

## Objects

A presentation with object audio (Part 2 clause 4.8.3) decodes to objects, which
`DecodedFrame::objects` hands over for the application to render: each object's PCM, as long as the
frame, and the properties its object audio metadata sets, which Part 2 Annex F lists as what a
decoder gives an object renderer.

- `DecodedObject`: a dynamic object or a bed object (`kind`), the LFE (`lfe`), a bed object's
  loudspeaker (`speaker`), the samples, the properties in force at the frame's first sample, and
  the updates within the frame, in order.
- `ObjectProperties`: whether the object is active, its gain in dB (−infinity for silence) and
  priority, its position (X from the left wall to the right, 0 to 1; Y from the front wall to the
  back, 0 to 1; Z from the floor through the screen's height to the ceiling, −1 to 1), its zone
  constraint, elevation and snap, its width, screen factor and depth exponent, distance and
  divergence, and the trim and headphone data of `add_per_object_md()`.
- `ObjectUpdate`: the sample of the frame at which an update takes effect, counted with the
  decoder's delay as the samples are (clause 5.9.2), and its ramp, in samples, for the renderer to
  move over.
- `DecodedFrame::object_common`: the group's common data (clause 6.3.9.2), trim among it, as the
  stream codes it.

An A-JOC substream decodes in full decoding to its upmix's objects, reconstructed from the downmix
(clause 5.7), and in core decoding to the downmix's own signals, or a static 5.X downmix's bed, with
the downmix's metadata. Dialogue enhancement raises the dialogue objects in both, and a direct-coded
dialogue substream's objects, up to the stream's cap. The decoder renders only an intermediate
spatial format, into `channels`: 7.X.4 as coded and the `downmix` layout otherwise. A presentation
of objects alone has no channels besides; `decode_by_block()` hands over channels only, so a player
of objects takes them from `decode()`.

`ac3cli decode` renders a presentation with objects to speakers through the layout renderer Hearth
plays E-AC-3's objects with (`ac3::render::LayoutRenderer`, by way of
`apps/common/ac4_object_render.hpp`): each object panned from its position at its gain, moving to
each update over its ramp, to the layout `speakers=`, `channels=` or `downmix=` names, 7.1.4
without them. Width, divergence, zones and the screen factor are not rendered.

## Encoding a stream

`ac4::Encoder` writes mono, stereo, 5.0 and 5.1 (and, as experimental options, 7.0, 7.1 and 3.0)
at 48 kHz at every frame rate of Part 1 Table 83, or at 44.1 kHz in frames of 2 048 samples, in the
SIMPLE, ASPX and A-CPL codec modes, at a constant, average or variable rate. It takes planar
samples at full scale 1.0, in the order the decoder writes them, and returns raw AC-4 frames:

```cpp
ac4::EncoderConfig config{
    .channels = 6,          // 5.1: L R C LFE Ls Rs
    .frame_rate_index = 2,  // 25 fps; 13, the default, is the 2 048-sample frame
    .bitrate_kbps = 384,
    .dialnorm_db = -24.0,
    .drc = ac4::DrcConfig{.profile = ac4::DrcProfile::kFilmStandard},
};
auto encoder = ac4::Encoder::create(config);
if (!encoder) {
    const std::string_view why = ac4::Encoder::refusal_reason(config);
    fmt::println("refused: {}", why);  // e.g. "a rate outside 8 to 3 000 kbps"
    return 1;
}
for (const auto& block : input) {  // any number of samples at a time
    auto frames = encoder->encode(block.channels);
    for (const ac4::EncodedFrame& frame : *frames) {
        write(ac4::sync_frame(frame.raw_ac4_frame, true));  // a raw .ac4 file's sync frame
    }
}
for (const ac4::EncodedFrame& frame : *encoder->flush()) {
    write(ac4::sync_frame(frame.raw_ac4_frame, true));
}
```

Every field of `EncoderConfig` and the structures in it has a default, so a designated initializer
names only the fields it sets, in the order the header declares them. `create()` refuses a
configuration outside what the encoder writes with `EncodeError::kInvalidConfig`, and
`refusal_reason()` names the rule it breaks, as a string literal: a layout it does not write, a
rate its frames cannot hold, a presentation of the wrong number of substreams, and the rest.

| `EncoderConfig` field | What it sets | Default |
|---|---|---|
| `channels`, `sample_rate_hz` | 1, 2, 5 or 6 channels (7 or 8 with `experimental.seven_x`); 48 000 or 44 100 Hz | 2, 48 000 |
| `frame_rate_index`, `bitrate_kbps`, `rate_mode` | Part 1 Table 83's frame rate; the rate over whole frames, 8 to 3 000 kbps; `kConstant`, `kAverage` (within the decoder's buffer, Part 1 clause 6.2.4) or `kVariable` | 13, 192, `kConstant` |
| `codec_mode` | `kAuto` (the rate's choice, as DEE's streams make it), `kSimple`, `kAspx`, or an A-CPL mode | `kAuto` |
| `iframe_interval`, `iframes`, `fragment_starts` | An I-frame every so many frames, at named frames, and where a container's fragments start, which an MP4 lists as its sync samples | 24 |
| `dialnorm_db`, `loudness` | The dialogue level, 0 to -31.75 dBFS, and Part 1's further loudness values | -31, none |
| `drc`, `downmix`, `dialogue` | The DRC decoder modes on their profiles, the stereo downmix's values, and dialogue enhancement from marked channels or a stem | none |
| `substreams`, `presentations` | Several substreams and the presentations of Part 2 Table 53 made of them (below) | one of each |
| `trace`, `experimental` | A record of every syntax element written; the tools and layouts no reader outside this project has checked yet | none |

A frame comes out when the input it needs has arrived: `encode()` returns the frames each call
completes, and `flush()` pads the input with silence to the end of its last frame and returns the
rest. Each `EncodedFrame` holds the raw frame, the samples it decodes to and whether it is an
I-frame. `delay_samples()` and `decoder_delay_samples()` give where an input sample lands in the
decoded output, which an MP4's edit list can skip; at `frame_rate_index` 13 the two are 3 072 and
1 313 samples.

### Substreams and presentations

A stream can carry several substreams, each coding inputs of its own (or, for a hybrid dialogue
enhancement, the dialogue beside another substream), and presentations that play them together in
Part 2 Table 53's roles:

```cpp
ac4::EncoderConfig config{.bitrate_kbps = 448};
config.substreams = {
    {.channels = 6, .content = ac4::ContentClassifier::kMusicAndEffects},
    {.channels = 1, .content = ac4::ContentClassifier::kDialogue, .language = "en"},
    {.channels = 1, .content = ac4::ContentClassifier::kDialogue, .language = "de"},
};
config.presentations = {
    {.config = 0, .substreams = {0, 1}},  // music and effects with English dialogue
    {.config = 0, .substreams = {0, 2}},  // and with German
};
```

`encode()` then takes the substreams' channels one substream after the other: eight here. Each
presentation gets the least `md_compat` its tracks need and a `presentation_id` of its own unless it
sets them, and its own dialnorm, loudness, DRC and downmix where it sets them. The substreams take
shares of the rate in proportion to their full-band channels unless they set their own. What the
encoder refuses there, and why, is in the header and `src/ac4enc/ERRATA.md`.

### Containers

`ac4::sync_frame()` wraps a frame for a raw `.ac4` file or an MPEG-2 transport stream, with Part 2
Annex G's CRC or without it. An MP4 sample holds the raw frame as it is, and the sample entry's
`dac4` box comes from the table of contents the encoder reports: `ac4::build_dac4(encoder->toc())`
describes every presentation, and `ac4::media_timing()` gives the track's time scale. A CMAF track
keeps TS 103 190-2 Annex H.1.2's rules, which `ac4::cmaf_refusal()` checks: a presentation of
configuration 6, EMDF payloads alone, has no field for the `presentation_id` each presentation of
a CMAF track carries.

`ac3cli ac4-encode` spells each setting as an option, raw or MP4 by the output's name: see
[Commands](../forge/cli/commands.md#ac4-encode).

## The inspector

`ac4::ac4` reads the framing the decoder starts from, and works on its own for a muxer or a
probe:

- `ac4::scan(data)` walks the sync frames of a whole buffer, checking each frame's CRC (Annex G).
  The frames it returns view `data`, so the buffer has to outlive them.
- `ac4::SyncFrameSplitter` does the same for a stream that arrives in pieces, in storage the caller
  owns (`kSplitterRecommendedBuffer` holds every frame shorter than 64 KiB). Where the stream does
  not start on a sync word, or something between frames is not a frame, it skips to the next sync
  word, counts the bytes it skipped (`resynchronised_bytes()`) and hands over the frame it found
  only once another sync word follows it.
- `ac4::parse_raw_frame()` reads a frame's table of contents: the presentations, the substream
  groups and the substream index table.
- `ac4::frame_rate(toc)` gives Part 1 Tables 83 and 84's frame rate, frame length and internal
  sample rate; `ac4::build_dac4(toc)` and `ac4::rfc6381_codec_string(toc)` give an MP4 sample
  entry's `dac4` box and the codec string HLS and DASH signal. The box describes every
  presentation (Part 2 Annex E.10), or is empty where the table of contents holds something it
  cannot describe whole, and `ac4::dac4_refusal(toc)` says what; `ac4::cmaf_refusal(toc)` names
  the rule of Annex H.1.2.1 a stream breaks for a CMAF track.

## Linking

**In-tree:**

```cmake
target_link_libraries(your_target PRIVATE ac4::decoder)   # brings ac4::ac4 with it
target_link_libraries(your_target PRIVATE ac4::encoder)   # likewise
```

**Installed package** (`find_package(ac3forge)`, see [Using ac3::forge](index.md)):

```cmake
find_package(ac3forge REQUIRED)
target_link_libraries(your_target PRIVATE ac4::decoder_static)   # or ac4::decoder_shared
target_link_libraries(your_target PRIVATE ac4::encoder_static)   # or ac4::encoder_shared
```

Each decoder and encoder library links the inspector of its own kind, `ac4::ac4_static` or
`ac4::ac4_shared`. A package installed with one linkage, as a vcpkg or Conan one is, also defines
the bare `ac4::decoder`, `ac4::encoder` and `ac4::ac4`. The static decoder and encoder call into
`ac4::core`, a static archive of the tables and transforms the two share (`libac4core_static.a`,
no headers), which their exported targets name as a link-only dependency; each shared library
carries the part of it that it uses. Through pkg-config the decoder is `ac4dec` and the encoder
`ac4enc`, each of which requires `ac4`, and whose static-only forms require `ac4core` privately:

```bash
c++ -std=c++23 player.cpp $(pkg-config --cflags --libs ac4dec)
c++ -std=c++23 packager.cpp $(pkg-config --cflags --libs ac4enc)
```

`AC3FORGE_BUILD_AC4`, on by default, builds the AC-4 libraries; the vcpkg port and the Conan
recipe build and install them with the rest, without a feature of their own yet. Android,
WebAssembly, the Python wheel and the ESP-IDF component build without them until their bindings
arrive (planning/ac4.md, phases I4 and D12).
