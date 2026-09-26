#include <array>
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
// frames that fail, are pressed too; and its choice of presentation, level and
// mixing gains from the byte before it, so that the selection among the
// presentations a table of contents offers and the mixing of the substreams
// they name are pressed as well (the seeds include the multiplexed streams
// of tests/golden/ac4dec/presentations/). The byte before that chooses core
// decoding and any of the layouts Part 2's channel renderer takes the
// immersive element to, so that the core's paths and the renderer are pressed
// too (the seeds include DEE's 5.1.4 legs, and the fuzz script replays the
// constructed immersive streams of tests/golden/ac4dec/constructed/ and the
// object streams of tests/golden/ac4dec/objects/, which A-JOC, the object
// audio metadata and the intermediate spatial format renderer decode). Half
// way through the stream, that decoder's output processing and presentation
// change as a player's settings do (set_output(), set_presentation()), and
// every other frame goes by block (decode_by_block(), then flush()), with
// what presentations() and metadata() report read at each frame.
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
            processing.output.output_level_dbfs =
                -31.0 + static_cast<double>((pick >> 1U) % 8U) * 4.0;
        }
        processing.output.drc = static_cast<ac4::DrcMode>((pick >> 1U) % 6U);
        processing.output.dialogue_enhancement_db = (pick & 8U) != 0 ? 12.0 : 0.0;
        processing.output.downmix = static_cast<ac4::DownmixTarget>((pick >> 4U) % 6U);
        processing.output.mix_lfe = (pick & 16U) == 0;
        processing.concealment = static_cast<ac4::ConcealmentPolicy>((pick >> 6U) % 3U);
    }
    if (size > 1) {
        // Bits 0 and 1: no preference, a position, a presentation_id, or the
        // preferences; bits 2 to 5 their values; bits 6 and 7 the gains, or
        // a level other than the default.
        const auto choose = static_cast<unsigned>(data[size - 2]);
        constexpr std::array<const char*, 4> kLanguages = {"", "en", "de-AT", "fr"};
        switch (choose & 3U) {
            case 1:
                processing.presentation.index = (choose >> 2U) % 16U;
                break;
            case 2:
                processing.presentation.presentation_id = static_cast<int>((choose >> 2U) % 16U);
                break;
            case 3:
                processing.presentation.language = kLanguages[(choose >> 2U) % 4U];
                if ((choose & 16U) != 0) {
                    processing.presentation.associated = 0b010;
                    processing.presentation.associated_type = static_cast<ac4::AssociatedType>((choose >> 2U) % 5U);
                }
                processing.presentation.headphones = (choose & 32U) != 0;
                break;
            default:
                break;
        }
        switch (choose >> 6U) {
            case 1:
                processing.output.dialogue_gain_db = 12.0;
                processing.output.associated_gain_db = -10.0;
                break;
            case 2:
                processing.output.dialogue_gain_db = -150.0;
                processing.output.associated_gain_db = -150.0;
                break;
            case 3:
                processing.level = static_cast<int>((choose >> 2U) % 8U);
                break;
            default:
                break;
        }
    }
    if (size > 2) {
        // Bit 0: core decoding; bit 1: a layout of DownmixTarget's eleven in
        // bits 2 to 7, in place of the one the last byte chose.
        const auto render = static_cast<unsigned>(data[size - 3]);
        processing.decoding = (render & 1U) != 0 ? ac4::DecodingMode::kCore : ac4::DecodingMode::kFull;
        if ((render & 2U) != 0) {
            processing.output.downmix = static_cast<ac4::DownmixTarget>((render >> 2U) % 11U);
        }
    }
    ac4::Decoder decoding(processing);
    ac4::OutputConfig later = processing.output;
    later.downmix =
        static_cast<ac4::DownmixTarget>((static_cast<unsigned>(later.downmix) + 3U) % 6U);
    later.output_level_dbfs = later.output_level_dbfs ? std::nullopt : std::optional<double>{-20.0};
    later.dialogue_enhancement_db = 6.0;
    std::uint64_t samples = 0;
    const auto sink = [&samples](const ac4::PcmBlock& block) { samples += block.samples; };
    const ac4::ScanResult scan = ac4::scan(bytes);
    for (std::size_t f = 0; f < scan.frames.size(); ++f) {
        const std::span<const std::byte> frame = scan.frames[f].raw_ac4_frame;
        (void)framed.parse(frame);
        if (f == scan.frames.size() / 2) {
            decoding.set_output(later);
            decoding.set_presentation(ac4::PresentationChoice{});
        }
        if (f % 2 == 0) {
            (void)decoding.decode(frame);
        } else {
            (void)decoding.decode_by_block(frame, sink);
        }
        for (const ac4::PresentationInfo& info : decoding.presentations()) {
            samples += info.name.size() + info.members.size();
        }
        samples += decoding.metadata().drc ? decoding.metadata().drc->modes.size() : 0U;
        samples += static_cast<std::uint64_t>(decoding.latency_samples());
    }
    (void)decoding.flush(sink);
    (void)samples;

    ac4::Decoder raw;
    (void)raw.decode(bytes);
    (void)records;
    return 0;
}
