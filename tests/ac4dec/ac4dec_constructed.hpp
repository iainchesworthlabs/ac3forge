#pragma once

// Streams of the channel elements DEE's streams do not reach, built for the
// decoder's tests: the 3.0 element, the 5.X element in every coding_config
// and 2ch_mode, and the 7.X element in its three channel modes, in the SIMPLE
// and ASPX codec modes (ETSI TS 103 190-1 V1.4.1 clause 4.2.6); and the
// channel pair, 5.X and 7.X elements in the A-CPL modes. Each output channel
// carries a tone of its own, so a channel coded in the wrong place shows as
// its tone in the wrong channel.
//
// In the A-CPL modes the parameters are constant: alpha 1 in every band, which
// sends each module's downmix to its first output and leaves the second
// silent, or -1, the other way round, with beta 0; ASPX_ACPL_1's residuals
// carry the second outputs below acpl_qmf_band 8 (3 kHz), above every tone.
// ASPX_ACPL_3 routes L and R's downmixes to L and R, or Ls and Rs, by gamma1
// and gamma4 (and alpha 1 or -1). The tracks are the tones worked back through
// Pseudocodes 115 to 120 as printed, with Table 202's pairs for the 7.X
// element, so the decoder must read those as printed for the tones to come
// back where they started.
//
// They are written with the encoder's writer (src/ac4enc/src): its audio
// spectral frontend coder, chparam_info(), companding_control() and A-SPX
// writers, and its frame writer. The element syntax around those, and where
// each channel goes (Tables 180, 182 and 183), are written here, a
// transcription separate from the decoder's routing (src/ac4dec/src/pcm/
// routing.cpp). Where stereo processing mixes tracks, the tracks are the
// channels through the inverse of the printed matrix (ac4dec_printed_matrices.
// hpp), so the decoder's matrix must be the printed one for the tones to come
// back where they started. A-SPX data are one FIXFIX envelope per channel,
// loud with noise for one aspx_data element and silent for the others, so the
// channels A-SPX fills show which element the decoder gave them.

#include <cstddef>
#include <string>
#include <vector>

#include "ac4/syntax.hpp"
#include "ac4dec/decoder.hpp"

namespace ac4dec_test {

struct ElementCase {
    std::string name{};
    int ch_mode = 4;               // Part 1 Table 88: 2 (3.0) to 10
    bool aspx = false;             // the ASPX codec mode, else SIMPLE
    int coding_config = 0;         // 3_0_coding_config (0 or 1), or coding_config (0 to 3)
    bool two_ch_mode = false;      // 2ch_mode, in coding_config 0 of the 5.X and 7.X elements
    int chel_matsel = 0;           // of three_channel_data() and five_channel_data()
    int sap_mode = 0;              // every chparam_info()'s: 0, left and right, or 2, M/S
    bool stereo_proc = true;       // every pair's b_enable_mdct_stereo_proc
    bool use_sap_add_ch = false;   // the 7.X element's b_use_sap_add_ch
    int sap_add_mode = 2;          // the sap_mode of the two chparam_info() it sends
    int loud_unit = -1;            // ASPX: the aspx_data element, in syntax order, sent loud
    int companded = -1;            // ASPX: the companding_control() channel with b_compand_on
    // An A-CPL codec mode, 2 to 4 (ASPX_ACPL_1 to 3), in place of `aspx`.
    int acpl = 0;
    bool acpl_second = false;      // route each module's downmix to its second output
    bool add_ch_base = false;      // the 7.X element's, in 5/2/0 and 3/2/2
    int acpl_bands_id = 0;         // acpl_num_param_bands_id
    int acpl_quant = 0;            // acpl_quant_mode (and both of acpl_config_2ch())
    // The channel pair in ASPX_ACPL_2 with alpha 0 and this beta_q: L and R
    // each the downmix and half its decorrelated copy, of opposite signs.
    int acpl_beta_q = 0;
};

struct BuiltStream {
    std::vector<std::vector<std::byte>> frames;          // raw_ac4_frame()s
    std::vector<std::vector<ac4::SyntaxRecord>> traces;  // the writer's records, frame by frame
    std::vector<ac4::Speaker> speakers;                  // the decoder's channels, in its order
    std::vector<double> tone_hz;                         // each of those channels' tone; 0 when silent
};

// `frames` frames of the case, an I-frame every fourth.
[[nodiscard]] BuiltStream build_stream(const ElementCase& c, int frames);

// A stream's frames, sync-framed with a CRC (Part 1 Annex G), as a file holds
// them.
[[nodiscard]] std::vector<std::byte> sync_framed(const BuiltStream& stream);

// The builder's reading of Table 213: the aspx_data elements of an element in
// ASPX (codec mode 1) or an A-CPL mode in syntax order, each the channels it
// carries.
[[nodiscard]] std::vector<std::vector<ac4::Speaker>> aspx_elements(int ch_mode, int codec_mode = 1);

// The cases committed as tests/golden/ac4dec/constructed/<name>.ac4, with
// kCommittedFrames frames each, whose digests tools/references/ac4_syntax.py
// wrote beside the other digests in tests/golden/ac4dec/.
inline constexpr int kCommittedFrames = 4;
[[nodiscard]] std::vector<ElementCase> committed_cases();

}  // namespace ac4dec_test
