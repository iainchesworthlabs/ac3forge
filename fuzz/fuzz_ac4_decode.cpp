#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "ac4/ac4.hpp"
#include "ac4dec/decoder.hpp"

// ac4::Decoder::parse and ac4::Decoder::decode (src/ac4dec) - the AC-4
// decoder's syntax layer, and the reconstruction to PCM behind decode().
//
// Below the table of contents that fuzz_ac4_parse presses, every substream is
// a run of counts the stream chooses: section lengths and escapes, Huffman
// codewords that decide how many lines follow, A-SPX envelope and noise counts
// derived from its own configuration, DRC gain sets sized by
// drc_gainset_size, EMDF payloads sized by variable_bits(8). The decoder reads
// all of it with a bounded reader, and this harness is what holds it to never
// reading outside a substream, never looping on a count with no data behind
// it, and never tripping ASan or UBSan on any of those numbers.
//
// Two ways into the same parse, as fuzz_ac4_parse does for the inspector:
// each sync frame scan() finds, through one decoder so that I-frame
// configuration carries from frame to frame as it does in a stream; and the
// whole input as one raw_ac4_frame, so a mutated table of contents reaches
// the substreams without a well-formed sync frame having to be found first.
// The framed decoder has a syntax sink attached, so the trace path is
// pressed too. A second framed decoder, and the whole-input one, decode to
// PCM: scale factors, band layouts and block lengths the stream chooses reach
// the reconstruction and the transforms, with the overlap buffers carried
// from frame to frame. The second framed decoder's output processing and
// concealment policy come from the input's last byte, so the DRC, dialogue
// enhancement and downmix values the stream sends, and the concealment of the
// frames that fail, are pressed too.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data), size);

    std::uint64_t records = 0;
    const auto count = [&records](const ac4::SyntaxRecord&) { ++records; };
    ac4::DecoderConfig config;
    config.syntax = count;
    ac4::Decoder framed(config);
    ac4::DecoderConfig processing;
    if (size > 0) {
        const auto pick = static_cast<unsigned>(data[size - 1]);
        if ((pick & 1U) != 0) {
            processing.output.output_level_dbfs = -31.0 + static_cast<double>((pick >> 1U) % 8U) * 4.0;
        }
        processing.output.drc = static_cast<ac4::DrcMode>((pick >> 1U) % 6U);
        processing.output.dialogue_enhancement_db = (pick & 8U) != 0 ? 12.0 : 0.0;
        processing.output.downmix = static_cast<ac4::DownmixTarget>((pick >> 4U) % 6U);
        processing.output.mix_lfe = (pick & 16U) == 0;
        processing.concealment = static_cast<ac4::ConcealmentPolicy>((pick >> 6U) % 3U);
    }
    ac4::Decoder decoding(processing);
    const ac4::ScanResult scan = ac4::scan(bytes);
    for (const ac4::SyncFrame& frame : scan.frames) {
        (void)framed.parse(frame.raw_ac4_frame);
        (void)decoding.decode(frame.raw_ac4_frame);
    }

    ac4::Decoder raw;
    (void)raw.decode(bytes);
    (void)records;
    return 0;
}
