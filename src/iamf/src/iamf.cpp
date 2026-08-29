#include "iamf/iamf.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

#include "isobmff_detail.hpp"
#include "obu_detail.hpp"

namespace iamf {

namespace {

using detail::Bytes;

constexpr std::uint64_t kAudioElementId = 0;
constexpr std::uint64_t kCodecConfigId = 0;
constexpr std::uint64_t kMixPresentationId = 0;

[[nodiscard]] bool valid_sample_rate(std::uint32_t rate) {
    // IAMF §3.11.4 LPCM DecoderConfig: "SHALL take a value from the set {44.1k, 16k, 32k, 48k,
    // 96k}".
    return rate == 44100 || rate == 16000 || rate == 32000 || rate == 48000 || rate == 96000;
}

[[nodiscard]] bool valid_bit_depth(int depth) {
    // IAMF §3.11.4: "SHALL take a value from the set {16, 24, 32}".
    return depth == 16 || depth == 24 || depth == 32;
}

// The four Descriptor OBUs (IAMF §6.2.4's configOBUs, "identical to Descriptors"), in the order
// §6.2.4 requires: IA Sequence Header, Codec Config, Audio Element(s), Mix Presentation(s).
[[nodiscard]] Bytes build_config_obus(const AudioTrack& track) {
    Bytes out;
    detail::put_obu(out, detail::ObuType::kSequenceHeader,
                    detail::build_ia_sequence_header_obu_payload());
    detail::put_obu(out, detail::ObuType::kCodecConfig,
                    detail::build_codec_config_obu_payload(kCodecConfigId, track));
    detail::put_obu(out, detail::ObuType::kAudioElement,
                    detail::build_audio_element_obu_payload(kAudioElementId, kCodecConfigId));
    detail::put_obu(
        out, detail::ObuType::kMixPresentation,
        detail::build_mix_presentation_obu_payload(kMixPresentationId, kAudioElementId, track));
    return out;
}

// One IA Sample (IAMF §6.2.5): this frame's 7 Audio Frame OBUs, one per substream in
// kSubstreamLayout order, concatenated - exactly one Temporal Unit's worth of OBUs, with no
// Temporal Delimiter OBU (optional, and this writer omits it - see iamf.hpp's header comment).
[[nodiscard]] Bytes build_ia_sample(const Frame& frame, int bit_depth) {
    Bytes out;
    for (int id = 0; id < detail::kSubstreamCount; ++id) {
        const auto& substream = detail::kSubstreamLayout[static_cast<std::size_t>(id)];
        const Bytes payload =
            detail::build_audio_frame_payload(substream, std::span<const std::vector<float>, 12>(
                                                              frame.channels), bit_depth);
        detail::put_obu(out, detail::audio_frame_id_obu_type(id), payload);
    }
    return out;
}

[[nodiscard]] Bytes build_moov(const AudioTrack& track, const Bytes& config_obus,
                               std::span<const Bytes> samples,
                               std::span<const std::uint32_t> chunk_offsets,
                               std::uint64_t total_samples) {
    Bytes stbl_body;
    detail::put_bytes(stbl_body, detail::build_stsd(config_obus));
    detail::put_bytes(stbl_body, detail::build_stts(static_cast<std::uint32_t>(samples.size()),
                                                     track.samples_per_frame));
    detail::put_bytes(stbl_body, detail::build_stsc(static_cast<std::uint32_t>(samples.size())));
    detail::put_bytes(stbl_body, detail::build_stsz(samples));
    detail::put_bytes(stbl_body, detail::build_stco(chunk_offsets));
    Bytes stbl;
    detail::put_box(stbl, "stbl", stbl_body);

    Bytes minf_body;
    detail::put_bytes(minf_body, detail::build_smhd());
    detail::put_bytes(minf_body, detail::build_dinf());
    detail::put_bytes(minf_body, stbl);
    Bytes minf;
    detail::put_box(minf, "minf", minf_body);

    Bytes mdia_body;
    detail::put_bytes(mdia_body, detail::build_mdhd(track.sample_rate, total_samples));
    detail::put_bytes(mdia_body, detail::build_hdlr(track.writing_app));
    detail::put_bytes(mdia_body, minf);
    Bytes mdia;
    detail::put_box(mdia, "mdia", mdia_body);

    Bytes trak_body;
    detail::put_bytes(trak_body, detail::build_tkhd(total_samples));
    detail::put_bytes(trak_body, mdia);
    Bytes trak;
    detail::put_box(trak, "trak", trak_body);

    Bytes moov_body;
    detail::put_bytes(moov_body, detail::build_mvhd(track.sample_rate, total_samples));
    detail::put_bytes(moov_body, trak);
    Bytes out;
    detail::put_box(out, "moov", moov_body);
    return out;
}

}  // namespace

std::string_view describe(MuxError error) {
    switch (error) {
        case MuxError::kNoFrames:
            return "no frames to mux";
        case MuxError::kInvalidTrack:
            return "invalid track: sample_rate must be one of {44100,16000,32000,48000,96000}, "
                   "bit_depth one of {16,24,32} (IAMF v1.1.0 Section 3.11.4), and "
                   "samples_per_frame non-zero";
        case MuxError::kFrameSizeMismatch:
            return "a frame did not carry exactly samples_per_frame samples on every channel";
    }
    return "unknown error";
}

std::expected<std::vector<std::byte>, MuxError> mux(const AudioTrack& track,
                                                     std::span<const Frame> frames) {
    if (frames.empty()) {
        return std::unexpected(MuxError::kNoFrames);
    }
    if (!valid_sample_rate(track.sample_rate) || !valid_bit_depth(track.bit_depth) ||
        track.samples_per_frame == 0) {
        return std::unexpected(MuxError::kInvalidTrack);
    }
    for (const auto& frame : frames) {
        for (const auto& channel : frame.channels) {
            if (channel.size() != track.samples_per_frame) {
                return std::unexpected(MuxError::kFrameSizeMismatch);
            }
        }
    }

    const Bytes config_obus = build_config_obus(track);
    const Bytes ftyp = detail::build_ftyp();

    std::vector<Bytes> samples;
    samples.reserve(frames.size());
    for (const auto& frame : frames) {
        samples.push_back(build_ia_sample(frame, track.bit_depth));
    }

    const std::uint64_t total_samples =
        static_cast<std::uint64_t>(frames.size()) * track.samples_per_frame;

    // Same two-pass layout mp4::mux() uses: stco's chunk offsets are absolute file positions,
    // which depend on moov's size, which is already fixed (box sizes are self-describing)
    // regardless of what those offset VALUES turn out to be - so moov is built once with
    // placeholder offsets purely to measure it, then again with the real ones.
    const std::vector<std::uint32_t> placeholder_offsets(samples.size(), 0);
    const Bytes moov_measured =
        build_moov(track, config_obus, samples, placeholder_offsets, total_samples);

    constexpr std::uint64_t kMdatHeaderBytes = 8;
    const std::uint64_t mdat_start =
        static_cast<std::uint64_t>(ftyp.size()) + moov_measured.size() + kMdatHeaderBytes;

    std::vector<std::uint32_t> offsets(samples.size());
    std::uint64_t cursor = mdat_start;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        offsets[i] = static_cast<std::uint32_t>(cursor);
        cursor += samples[i].size();
    }
    const std::uint64_t mdat_body_bytes = cursor - mdat_start;

    const Bytes moov = build_moov(track, config_obus, samples, offsets, total_samples);
    assert(moov.size() == moov_measured.size());

    Bytes file;
    file.reserve(ftyp.size() + moov.size() + static_cast<std::size_t>(kMdatHeaderBytes) +
                 static_cast<std::size_t>(mdat_body_bytes));
    detail::put_bytes(file, ftyp);
    detail::put_bytes(file, moov);
    detail::put_u32(file, static_cast<std::uint32_t>(kMdatHeaderBytes + mdat_body_bytes));
    detail::put_fourcc(file, "mdat");
    for (const auto& sample : samples) {
        detail::put_bytes(file, sample);
    }
    return file;
}

}  // namespace iamf
