#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "bit_reader.hpp"
#include "syntax/acpl.hpp"
#include "syntax/asf.hpp"
#include "syntax/aspx.hpp"
#include "syntax/context.hpp"

// audio_data_chan() (ETSI TS 103 190-2 V1.3.1 clause 6.2.3.1) for the Part 1
// channel elements: single_channel_element, channel_pair_element,
// 3_0_channel_element, 5_X_channel_element and 7_X_channel_element (ETSI TS
// 103 190-1 V1.4.1 clauses 4.2.5 and 4.2.6), and companding_control() (4.2.11).
// The immersive and 22.2 elements are refused.

namespace ac4::detail {

// Codec mode values: Part 1 Tables 93 and 95 to 98.
namespace codec_mode {
inline constexpr int kSimple = 0;
inline constexpr int kAspx = 1;
inline constexpr int kAspxAcpl1 = 2;
inline constexpr int kAspxAcpl2 = 3;
inline constexpr int kAspxAcpl3 = 4;
}  // namespace codec_mode

// 4.2.11 companding_control(num_chan).
struct CompandingControl {
    int num_chan = 0;
    bool sync_flag = false;
    std::array<bool, 5> b_compand_on{};  // one entry, or num_chan when !sync_flag
    bool b_compand_avg = false;
};

// Which kind of channel element a substream's channel_mode selects.
enum class ElementKind : std::uint8_t { kSingle, kPair, k3_0, k5X, k7X };

// One sf_data() and the sf_info() that governs it, in syntax order.
struct Track {
    int info = 0;               // index into ChannelElement::infos
    bool side_channel = false;  // Pseudocode 5's b_side_channel
    bool lfe = false;
    SfData data;
    // Populated alongside `data` only where this substream's HSF extension
    // is active (see parse_audio_data_chan's `hsf_reader` parameter); a
    // default-constructed HsfSfData otherwise.
    HsfSfData hsf;
};

// Everything an audio substream's channel element carried in one frame.
struct ChannelElement {
    ElementKind kind = ElementKind::kPair;
    int codec_mode = codec_mode::kSimple;
    std::optional<int> coding_config;         // 3_0_coding_config or coding_config, when read
    std::optional<bool> two_ch_mode;          // 2ch_mode
    std::optional<bool> b_use_sap_add_ch;     // 7_X only
    std::vector<bool> b_enable_mdct_stereo_proc;  // one per stereo_data / two_channel_data / ACPL_1 pair, in order
    std::vector<int> chel_matsel;             // one per three_channel_info / five_channel_info, in order
    std::optional<int> max_sfb_master;
    std::optional<CompandingControl> companding;

    std::vector<SfInfo> infos;
    std::vector<Track> tracks;
    std::vector<ChparamInfo> chparams;        // in syntax order

    std::vector<AspxData1ch> aspx_1ch;
    std::vector<AspxData2ch> aspx_2ch;
    std::vector<AcplData1ch> acpl_1ch;
    std::optional<AcplData2ch> acpl_2ch;
};

// The I-frame configuration a channel element's later frames depend on, kept
// per audio substream.
struct ChannelElementState {
    // The codec mode the configuration below was sent for. A later frame in a
    // different mode cannot use it.
    std::optional<int> configured_codec_mode;
    std::optional<ElementKind> configured_kind;
    std::optional<AspxConfig> aspx_config;
    std::optional<AcplConfig1ch> acpl_config_1ch;
    std::optional<AcplConfig2ch> acpl_config_2ch;
    // One per aspx_data_1ch()/aspx_data_2ch() position in the element, in
    // syntax order: 7_X in ASPX mode has the most, four.
    std::array<AspxElementState, 4> aspx{};
};

// `hsf_reader` is the owning substream's HSF extension reader, positioned at
// the start of its ac4_hsf_ext_substream() (Table 17), or nullptr where no
// extension is linked. When set, this peeks the extension's own header
// (max_sfb_ext_hsf[]) once, before the element's first track's sf_data(),
// using that track's own b_different_framing (see HsfExtHeader) - the reader
// is left positioned at sf_hsf_data()'s first bit for parse_sf_hsf_data() to
// continue with, once every track here has been read.
[[nodiscard]] ParseResult parse_audio_data_chan(BitReader& r, const SubstreamContext& ctx,
                                                ChannelElementState& state, ChannelElement& out,
                                                BitReader* hsf_reader = nullptr);

}  // namespace ac4::detail
