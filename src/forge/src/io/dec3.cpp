#include "ac3/io/dec3.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ac3/core/bitwriter.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/io/elementary.hpp"

namespace ac3::io {

namespace {

// Annex F's fscod has no fscod2 counterpart - E-AC-3's reduced-rate
// extension (§E2.3.1.3) postdates ETSI TS 102 366 Annex F, so a reduced-rate
// stream has no exact 2-bit code to report here. Annex F's fscod is
// fundamentally a SAMPLE RATE FAMILY selector (Table 5.6's three families:
// 48/44.1/32 kHz, each with its reduced-rate half), and fscod_family() is
// exactly that mapping (tables.hpp), so this reports the family and leaves
// the exact rate to the sample entry's own samplerate field
// (mp4::AudioTrack::sample_rate, set from the same ScannedStream) - the box
// exists for capability signalling (E-AC-3? how many channels? Atmos?), not
// as the sample rate's source of truth.
[[nodiscard]] std::uint32_t box_fscod(SampleRate sr) {
    return static_cast<std::uint32_t>(fscod_family(sr));
}

}  // namespace

std::vector<std::byte> build_codec_config_box(const ScannedStream& stream) {
    BitWriter w;

    if (stream.kind == StreamKind::kAc3) {
        // ETSI TS 102 366 Annex F §F.4 AC3SpecificBox: fscod(2) + bsid(5) +
        // bsmod(3) + acmod(3) + lfeon(1) + bit_rate_code(5) + reserved(5) =
        // 24 bits, byte-aligned by construction.
        w.put(box_fscod(stream.sample_rate), 2);                    // fscod
        w.put(static_cast<std::uint32_t>(stream.bsid), 5);          // bsid
        w.put(static_cast<std::uint32_t>(stream.bsmod), 3);         // bsmod
        w.put(static_cast<std::uint32_t>(stream.acmod), 3);         // acmod
        w.put(stream.lfe ? 1U : 0U, 1);                             // lfeon
        w.put(static_cast<std::uint32_t>(stream.bit_rate_code), 5); // bit_rate_code
        w.put(0, 5);                                                // reserved
        return w.take();
    }

    // ETSI TS 102 366 Annex F §F.6 EC3SpecificBox. Field layout cross-checked
    // against Dolby's own Digital Plus Online Delivery Kit documentation -
    // the EC3SpecificBox derivation guide for data_rate's semantics, and
    // https://ott.dolby.com/OnDelKits/DDP/Dolby_Digital_Plus_Online_Delivery_Kit_v1.4.1/Documentation/Playback/SDM/help_files/topics/c_id_ddp_atmos_isobmff.html
    // for the exact bit layout of the trailing Atmos extension (see the
    // addbsi block below) - both fetched and read directly, not recalled.
    //
    // data_rate(13) + num_ind_sub(3) = 16 bits, byte-aligned. data_rate's own
    // semantics ("the data rate of the ... bitstream, or the maximum data
    // rate if VBR" - Dolby's own EC3SpecificBox derivation guide) are exactly
    // what ac3::eac3::frame_words() fixes per bitrate for this project's CBR
    // encoder, so the first access unit's own size is the exact rate, not an
    // estimate: kbps = bytes * 8 * sample_rate / samples_per_frame / 1000.
    //
    // Summed across programmes: §F.6's data_rate describes the BITSTREAM, and
    // a stream carrying a second independent substream spends that
    // programme's rate on top of the first one's within the same frame period.
    std::size_t first_unit_bytes = 0;
    for (const auto& programme : stream.programmes) {
        if (!programme.access_units.empty()) {
            first_unit_bytes += programme.access_units.front().size();
        }
    }
    const std::uint64_t data_rate_bps = static_cast<std::uint64_t>(first_unit_bytes) * 8 *
                                        sample_rate_hz(stream.sample_rate);
    constexpr std::uint64_t kDenominator = static_cast<std::uint64_t>(kSamplesPerFrame) * 1000;
    const std::uint64_t data_rate_kbps =
        first_unit_bytes == 0 ? 0 : (data_rate_bps + kDenominator / 2) / kDenominator;
    // §F.6's data_rate is 13 bits (max 8191); E-AC-3's own ceiling (§E1.3.1.5,
    // 6144 kbps) never reaches it, so clamping here is a defensive backstop,
    // not a real-world case.
    constexpr std::uint32_t kMaxDataRate = (1U << 13) - 1;
    w.put(static_cast<std::uint32_t>(std::min<std::uint64_t>(data_rate_kbps, kMaxDataRate)), 13);
    // num_ind_sub: one loop iteration per programme ac3::io::scan() found -
    // one independent substream plus whatever dependents follow it before the
    // next independent one (scan_eac3's close_unit). The field counts "one
    // less than" the substream count, mirroring frmsiz's own "value plus one"
    // convention elsewhere in this syntax, and §F.6 caps it at eight the same
    // way §E2.3.1.2 caps the substreams themselves.
    //
    // scan() never returns an empty programme list on success; the fallback
    // to one keeps the subtraction defined regardless.
    const ScannedProgramme absent{};
    const auto programme_count = std::max<std::size_t>(stream.programmes.size(), 1);
    w.put(static_cast<std::uint32_t>(programme_count - 1), 3);  // num_ind_sub

    for (std::size_t i = 0; i < programme_count; ++i) {
        // §F.6 repeats the whole per-substream block for each independent
        // substream, so a second programme's own layout and service type are
        // declared rather than the first one's being repeated.
        const ScannedProgramme& programme =
            i < stream.programmes.size() ? stream.programmes[i] : absent;
        w.put(box_fscod(stream.sample_rate), 2);                // fscod
        w.put(static_cast<std::uint32_t>(programme.bsid), 5);   // bsid
        w.put(0, 1);                                            // reserved
        // asvc: the associated-service flag. A/52 §5.4.2.2 puts the service
        // type in bsmod, and 2-7 are the associated services (audio
        // description, commentary, emergency and the rest) a receiver mixes
        // against a main one; 0-1 are complete main services. So this is
        // exactly "is this programme's own bsmod an associated one", read off
        // the bitstream rather than assumed - which for a single-programme
        // stream still comes out 0, as it always did.
        w.put(programme.bsmod >= 2 ? 1U : 0U, 1);                 // asvc
        w.put(static_cast<std::uint32_t>(programme.bsmod), 3);    // bsmod
        w.put(static_cast<std::uint32_t>(programme.acmod), 3);    // acmod
        w.put(programme.lfe ? 1U : 0U, 1);                        // lfeon
        w.put(0, 3);                                              // reserved
        // substreams_per_unit counts every substream of this programme's
        // first access unit, its independent one included (ScannedProgramme's
        // own comment) - so the dependent count is one less, floored at 0.
        const auto num_dep_sub = programme.substreams_per_unit > 0
                                     ? programme.substreams_per_unit - 1
                                     : std::size_t{0};
        w.put(static_cast<std::uint32_t>(num_dep_sub), 4);  // num_dep_sub
        if (num_dep_sub > 0) {
            // chan_loc: Annex F's own per-location channel bitmap - a
            // DIFFERENT vocabulary from this project's internal Table E2.5
            // chanmap locations (eac3::chanmap), and deliberately not
            // translated into it in this first cut (see this file's own PR
            // description). No audio is misdescribed by leaving it 0: the
            // sample entry's channelcount (mp4::AudioTrack::channels, Table
            // E2.5-derived and exact) is what a player actually opens the
            // file with, and the elementary stream in mdat is unaffected
            // either way - only this one informational field undercounts
            // which extra positions the dependent(s) add.
            w.put(0, 9);  // chan_loc
        } else {
            w.put(0, 1);  // reserved
        }
    }

    if (stream.oba_complexity_index) {
        // TS 103 420 §8.3.1/§8.3.2.2, echoed into the box exactly as
        // ac3::io::scan() read it out of the bitstream's own addbsi (see
        // ScannedStream::oba_complexity_index) - this is the exact signal
        // FFmpeg's E-AC-3+JOC remux path is documented to drop or mis-signal
        // (https://github.com/jellyfin/jellyfin-ffmpeg/issues/584), which is
        // the reason this box is built from the bitstream rather than copied
        // from another tool's dec3.
        w.put(0, 7);                                                        // reserved
        w.put(1, 1);                                                        // flag_ec3_extension_type_a
        w.put(static_cast<std::uint32_t>(*stream.oba_complexity_index), 8); // complexity_index_type_a
    } else {
        w.put(0, 7);  // reserved
        w.put(0, 1);  // flag_ec3_extension_type_a
    }
    return w.take();
}

}  // namespace ac3::io
