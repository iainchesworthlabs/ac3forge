# File I/O: `ac3::io::wav`

`ac3/io/wav.hpp`. WAV reading and writing, shared by the CLI and the GUI so neither carries its
own copy. Every other example in this section stays in memory: PCM is synthesized straight into
an encoder and decoded samples are only ever counted. A real pipeline reads a file a caller
handed it and writes one back, which means crossing WAV's own channel order
(`WAVE_FORMAT_EXTENSIBLE`: FL, FR, FC, LFE, BL, BR) against A/52 Table 5.8's (L, C, R, SL, SR,
LFE) twice — once on the way in, once on the way out.

## What reads

| `<fmt >` tag | Widths | Notes |
|---|---|---|
| `WAVE_FORMAT_PCM` (1) | 8, 16, 24, 32 bits | 8-bit is unsigned and biased by 128 — the one integer depth WAV does not store two's-complement |
| `WAVE_FORMAT_IEEE_FLOAT` (3) | 32, 64 bits | 64-bit narrows to `float` on the way in |
| `WAVE_FORMAT_EXTENSIBLE` (0xFFFE) | any of the above | The real tag is the first two bytes of the SubFormat GUID; `wValidBitsPerSample` is deliberately not consulted, since 20-in-24 changes nothing about how the container is read |

Container: `RIFF`, plus `RF64` (EBU Tech 3306) and `BW64` (ITU-R BS.2088-1) for files past
RIFF's 4 GB ceiling — the `data` chunk's 32-bit size reads `0xFFFFFFFF` there and the real
length comes from `ds64`. Chunk lookup is a real RIFF walk rather than a search for the
four-character code anywhere in the file, which is what makes `ds64` findable at all and what
stops a `bext`/`iXML` payload that happens to contain the bytes `data` from being mistaken for
the audio.

Every depth converts to the same `[-1, 1)` floats, so nothing downstream of a reader knows or
cares which it was. 24-bit in particular is the normal professional interchange depth, and
needing an FFmpeg pre-conversion before this encoder could touch one was the gap this closed.

Writing stays deliberately narrow — float32 (`write_wav_f32`, `WavStreamWriter`) and raw PCM16
passthrough (`write_wav_pcm16_raw`, `WavPcm16StreamWriter`) — because those are the only two
shapes this project produces: decoded audio, and an IEC 61937 burst carrier.

## Round-tripping channel order

```cpp
// Synthesize 5.1 in AC-3 order and write it out in WAV order -
// wav_channel_order says where each AC-3 channel belongs in the interleave.
const auto write_order = ac3::io::wav_channel_order(kAcmod, kLfe);
ac3::io::write_wav_f32(source_path, ac3_order, 48000, write_order);
```

```cpp
// Read it back - read_wav hands the samples back in WAV order, so
// ac3_layout_for's wav_index permutes them onto AC-3 channel k.
const auto read = ac3::io::read_wav(source_path);
const auto layout = ac3::io::ac3_layout_for(read->channels.size());
std::vector<std::vector<float>> from_wav(layout->wav_index.size());
for (std::size_t k = 0; k < layout->wav_index.size(); ++k) {
    from_wav[k] = read->channels[layout->wav_index[k]];
}
```

Full program: [`examples/wav_roundtrip.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/wav_roundtrip.cpp)
— writes a 5.1 WAV, reads it back, encodes and decodes it, and writes the decoded result out
as a second WAV.

`ac3_layout_for(wav_channels)` and `wav_channel_order(acmod, lfe)` are exact inverses of each
other for the six widths WAV convention actually names (mono through 5.1); a width neither
covers (2/1, 3/1, 1+1) falls back to the codec's own channel order unchanged, since there is no
WAV convention to translate against.

`WavData::channels` is one `std::vector<float>` per channel, normalized to `[-1, 1)`, in
whatever order the file itself interleaves — `read_wav` does not reorder for you. `WavError`
covers open/parse failure the same way every other module here reports errors:
`kCannotOpen`, `kNotRiffWave`, `kUnsupportedFormat` (a compressed WAVE codec, or an unpacked
integer width that is not a whole number of bytes) and `kTruncated`.

## Streaming

`WavStreamWriter` is the incremental sibling for a take too long to hold in memory — it opens
once, takes interleaved samples as they arrive, and needs a periodic `flush_header()` call so a
process killed mid-session leaves a file whose header matches what was actually written rather
than claiming zero data.

`WavStreamReader` is its read-side counterpart, and reads exactly what `read_wav` does — the
same code, not a parallel copy of it, so a block-at-a-time consumer sees the same samples. It
is also the only one of the two that can read an RF64 file bigger than memory: the whole-file
overloads hold the source *and* its planar float copy resident at once by construction. It
needs a seekable file, which is why the whole-file overloads keep the stdin/pipe case.

```cpp
ac3::io::WavStreamReader in;
if (in.open(path)) {
    std::vector<std::vector<float>> planar(in.channels(), std::vector<float>(1536));
    std::vector<std::span<float>> views(planar.begin(), planar.end());
    while (const auto got = in.read_planar(views, 1536)) {
        if (*got == 0) break;
        // planar[ch][0..*got) — a block, whatever depth the file was
    }
}
```

---

See also: [Encoding AC-3](encoding-ac3.md) — what a WAV's samples are fed to once they're in
AC-3 channel order; [Decoding](decoding.md) — the other end of the round trip.
