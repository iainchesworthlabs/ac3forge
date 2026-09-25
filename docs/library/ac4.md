# AC-4 (ETSI TS 103 190) decoding: `ac4::decoder`

`ac4dec/decoder.hpp`, library `ac4::decoder`, with the inspector it reads the table of contents
through, `ac4/ac4.hpp` in library `ac4::ac4`. An AC-4 decoder written from ETSI TS 103 190-1
V1.4.1 (channel-based coding) and TS 103 190-2 V1.3.1 (immersive and personalized audio). Both
libraries are in namespace `ac4` and link nothing from `ac3::forge`: AC-4 shares no bitstream
syntax with AC-3 or E-AC-3.

It decodes the mono, stereo, 3.0, 5.X and 7.X channel elements in each of Part 1's codec modes
(SIMPLE, ASPX and the three A-CPL modes) at every frame rate; streams of several presentations,
the one a system chooses decoded with all of its substreams mixed; the output level and dynamic
range control, dialogue enhancement and the downmix; and it conceals a frame that does not decode
when asked to. It refuses, per substream and per frame, with `DecodeError::kUnsupported` and a
reason: the speech spectral frontend, the immersive and 22.2 channel elements, object substreams
(A-JOC among them), and output at 96 or 192 kHz. [Development status](development-status.md)
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
| `downmix` | The layout (clause 6.2.17): `kAsCoded`, `k5X` (a 7.X element folded to 5.X), `kStereo` (the method the stream prefers), `kLoRo`, `kLtRt`, `kMono` | `kAsCoded` |
| `mix_lfe` | Whether a two-channel or mono downmix takes the LFE at the stream's `lfe_mixgain`, as Part 1 does | `true` |
| `dialogue_gain_db` | g_dialog (clause 6.2.16.1): a presentation's dialogue against its music and effects, up to the stream's `g_dialog_max` | 0 |
| `associated_gain_db` | g_assoc (clause 6.2.16.2), 0 or less: a presentation's associated audio | 0 |

`DecoderConfig` holds the rest: `output`, `presentation` (below), `concealment`, `level` (the
`md_compat` level the decoder claims, 3 by default; presentations above it are not chosen), and
`syntax`, a trace of every syntax element read. `syntax` is an `ac4::SyntaxTrace`, a
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
  entry's `dac4` box and the codec string HLS and DASH signal.

## Linking

**In-tree:**

```cmake
target_link_libraries(your_target PRIVATE ac4::decoder)   # brings ac4::ac4 with it
```

**Installed package** (`find_package(ac3forge)`, see [Using ac3::forge](index.md)):

```cmake
find_package(ac3forge REQUIRED)
target_link_libraries(your_target PRIVATE ac4::decoder_static)   # or ac4::decoder_shared
```

Each decoder library links the inspector of its own kind, `ac4::ac4_static` or `ac4::ac4_shared`.
A package installed with one linkage, as a vcpkg or Conan one is, also defines the bare
`ac4::decoder` and `ac4::ac4`. The static decoder calls into `ac4::core`, a static archive of the
tables and transforms the decoder and the encoder share (`libac4core_static.a`, no headers), which
its exported target names as a link-only dependency; the shared decoder carries the part of it
that it uses. Through pkg-config the decoder is `ac4dec`, which requires `ac4`, and whose
static-only form requires `ac4core` privately:

```bash
c++ -std=c++23 player.cpp $(pkg-config --cflags --libs ac4dec)
```

`AC3FORGE_BUILD_AC4`, on by default, builds the AC-4 libraries; the vcpkg port and the Conan
recipe build and install them with the rest, without a feature of their own yet. Android,
WebAssembly, the Python wheel and the ESP-IDF component build without them until their bindings
arrive (planning/ac4.md, phases I4 and D12).

The AC-4 encoder, `ac4::encoder` (`ac4enc/encoder.hpp`), is built by the same switch and used by
`ac3cli ac4-encode`. It is in-tree only until its API is final and phase E7 installs it.
