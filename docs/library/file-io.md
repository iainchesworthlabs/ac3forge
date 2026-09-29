# File I/O: `ac3::io::wav`

`ac3/io/wav.hpp`. WAV reading and writing, shared by the CLI and the GUI so neither carries its
own copy. Every other example in this section stays in memory: PCM is synthesized straight into
an encoder and decoded samples are only ever counted. A pipeline that reads a file a caller
handed it and writes one back has to cross WAV's own channel order
(`WAVE_FORMAT_EXTENSIBLE`: FL, FR, FC, LFE, BL, BR) against A/52 Table 5.8's (L, C, R, SL, SR,
LFE) twice — once on the way in, once on the way out.

This header reads and writes WAV only. The readers for coded streams (`ac3/io/elementary.hpp`) take
AC-3 and E-AC-3; none of `ac3::io` reads AC-4, whose decoder is a separate library (see
[AC-4 decoding](ac4.md)).

## What reads

| `<fmt >` tag | Widths | Notes |
|---|---|---|
| `WAVE_FORMAT_PCM` (1) | 8, 16, 24, 32 bits | 8-bit is unsigned and biased by 128 — the one integer depth WAV does not store two's-complement |
| `WAVE_FORMAT_IEEE_FLOAT` (3) | 32, 64 bits | 64-bit narrows to `float` on the way in |
| `WAVE_FORMAT_EXTENSIBLE` (0xFFFE) | any of the above | The tag that counts is the first two bytes of the SubFormat GUID; `wValidBitsPerSample` is not consulted, since 20-in-24 changes nothing about how the container is read |

Container: `RIFF`, plus `RF64` (EBU Tech 3306) and `BW64` (ITU-R BS.2088-1) for files past
RIFF's 4 GB ceiling — the `data` chunk's 32-bit size reads `0xFFFFFFFF` there and the length
comes from `ds64`. Chunk lookup walks the RIFF chunk list, which is what makes `ds64` findable and
what stops a `bext`/`iXML` payload that happens to contain the bytes `data` from being mistaken for
the audio. Only when the walk finds no such chunk does it fall back to a plain search for the
four-character code, so a file with one malformed chunk length ahead of `fmt ` or `data` still
loads. A `data` chunk shorter than its header declares reads at the length the file actually
holds.

Every depth converts to the same `[-1, 1)` floats, so nothing downstream of a reader knows or
cares which it was.

`read_wav` and `write_wav_f32` each have a path overload and a `std::istream`/`std::ostream`
overload, for stdin and stdout (the CLI's `-` convention). The whole-file readers read the entire
source into memory before parsing, so a stream need not be seekable. The stream writer never seeks
back to patch a header, so it also works on a pipe.

Writing stays narrow — float32 (`write_wav_f32`, `WavStreamWriter`) and raw PCM16
passthrough (`write_wav_pcm16_raw`, `WavPcm16StreamWriter`) — because those are the only two
shapes this project produces: decoded audio, and an IEC 61937 burst carrier. The writers emit
plain RIFF with 32-bit size fields and have no RF64 form, so the sizes of a file holding more than
4 GiB of audio are wrong: the value wraps modulo 2^32. A six-channel 48 kHz float32 take reaches
that after about an hour.

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

The two functions cover different sets. `ac3_layout_for(wav_channels)` maps the channel counts one
to six onto mono, stereo, 3/0, 2/2, 3/2 and 3/2 with LFE, and returns nothing for none or for
seven and more. `wav_channel_order(acmod, lfe)` places every `acmod` by its
`WAVE_FORMAT_EXTENSIBLE` speaker position, so the LFE moves from last to fourth and centre swaps
with right, and the two single-surround modes, 2/1 and 3/1, sit on the back-centre position (3/1
goes out L R C S). The one exception is 1+1, two independent programmes with no speaker positions,
which goes out in the codec's own order. The two are exact inverses for the counts one to six
except that a 2/1 or 3/1 file comes back as 3/0 or 2/2, since a channel count alone cannot tell
them apart.

`WavData::channels` is one `std::vector<float>` per channel, normalized to `[-1, 1)`, in
whatever order the file itself interleaves — `read_wav` does not reorder for you. `WavError`
covers open/parse failure the same way every other module here reports errors:
`kCannotOpen`, `kNotRiffWave`, `kUnsupportedFormat` (a compressed WAVE codec, an unpacked
integer width that is not a whole number of bytes, or zero channels) and `kTruncated`.

## Streaming

`WavStreamWriter` is the incremental sibling for a take too long to hold in memory — it opens
once, takes interleaved samples as they arrive, and needs a periodic `flush_header()` call so a
process killed mid-session leaves a file whose header matches what was actually written rather
than claiming zero data. `WavPcm16StreamWriter` does the same for the raw PCM16 form and, after
`close()`, produces the file `write_wav_pcm16_raw` would have written for the same payload.

`WavStreamReader` is the read-side counterpart, and reads exactly what `read_wav` does — the
same code, not a parallel copy of it, so a block-at-a-time consumer sees the same samples. After
`open(path)`, `channels()`, `sample_rate()` and `frame_count()` describe the file, and
`read_planar(spans, frames)` deinterleaves up to `frames` frames into one span per channel,
returning how many it read (0 once the data chunk is exhausted). It is the only reader that can
take an RF64 file bigger than memory: the whole-file overloads hold the source and its planar
float copy resident at once by construction. It needs a seekable file, which is why the whole-file
overloads keep the stdin/pipe case, and the `fmt ` and `data` chunk headers must sit within the
first 64 KiB of the file, or `open` refuses it with `kNotRiffWave` (`read_wav` still reads such a
file).

`tests/io/test_wav_stream_reader.cpp` exercises the reader, including the depths above.

---

See also: [Encoding AC-3](encoding-ac3.md) — what a WAV's samples are fed to once they're in
AC-3 channel order; [Decoding](decoding.md) — the other end of the round trip.
