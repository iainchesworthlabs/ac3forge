#pragma once

// Object audio streams for the decoder's tests (phase D10 of planning/ac4.md):
// ETSI TS 103 190-2 V1.3.1's A-JOC substreams (audio_data_ajoc(), clause
// 6.2.3.4) and direct-coded object substreams (audio_data_objs(), 6.2.3.2) with
// their object audio metadata (6.2.8), in a presentation of one object group
// with or without an OAMD substream (6.2.2.4). DEE writes no A-JOC from this
// project's masters and nothing here writes direct-coded objects, so these are
// the streams that reach that syntax besides Chromium's ac4-ajoc.ac4.
//
// Every downmix signal and every direct-coded object carries a tone of its own,
// each at the middle of a QMF subband (375 Hz wide at 48 kHz), every second
// one, under 6 kHz, the top of the coded band and below A-SPX's crossover.
// A-JOC's parameters are constant in time and each
// object's coefficient on a downmix signal is a whole number of dequantisation
// steps, so each object's output is a known sum of those tones: the builder
// says which tones at which amplitudes each object should carry, in full
// decoding and in core decoding, and where its metadata puts it in each frame.
//
// They are written with the encoder's writers (src/ac4enc/src): the audio
// spectral frontend coder, A-SPX and companding, the A-JOC and object audio
// metadata syntax (ajoc/, oamd/) and the table of contents (frame/
// toc_writer.hpp). The element syntax around those, the metadata() of each
// substream and the presentation substream are written here. Where the builder
// needs a reading - which downmix signal Pseudocode 14a makes each A-JOC
// input, which object each tone lands in - it is its own transcription, not
// the decoder's.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ac4/syntax.hpp"

namespace ac4dec_test {

struct ObjectCase {
    std::string name{};
    enum class Kind : std::uint8_t {
        kAjoc,        // an A-JOC substream, a var_channel_element() downmix
        kAjocStatic,  // an A-JOC substream over a static 5.X downmix
        kDynamic,     // direct-coded dynamic objects over two substreams, with an OAMD substream
        kBed,         // a direct-coded 5.1 bed, with an OAMD substream
        kIsf,  // a direct-coded SR3.1.0.0 intermediate spatial format, with an OAMD substream
    };
    Kind kind = Kind::kAjoc;

    // A-JOC: the downmix's fullband signals (1 to 7), var_codec_mode ASPX, the
    // LFE, and var_coding_config for an odd count above 1.
    int dmx = 2;
    bool aspx = false;
    bool lfe = false;
    int var_coding_config = 0;
    // The upmix objects, their parameters and the decorrelators.
    int umx = 4;
    int decorr = 0;          // ajoc_num_decorr, every one enabled
    int bands_code = 7;      // ajoc_num_bands_code of every object: 7 is one band
    int quant = 1;           // ajoc_quant_select: 1 coarse, 0 fine
    bool sparse = false;     // every present object sparse, sending the coefficients that are not 0
    int dpoints = 1;         // ajoc_num_dpoints in every frame
    int ramp = 1;            // ajoc_ramp_len
    bool diff_time = false;  // DIFF_TIME in the frames that are not I-frames
    bool absent = false;     // the last object not present (ajoc_object_present 0)
    // Dialogue: A-JOC's object 0 is a dialogue object of de_max_gain 2, with
    // its downmix coefficients; direct-coded substreams are dialogue
    // substreams (b_dialog) of dialog_max_gain 2.
    bool dialogue = false;
    // OAMD: blocks per frame, the substream sending the timing (A-JOC's own
    // or the group's OAMD substream), a moving object 0, and the extras:
    // common data with trim, bed render and headphone data, and
    // add_per_object_md() with extended precision.
    int blocks = 1;
    bool oamd_substream = false;
    bool common = false;
    bool extras = false;
    bool bed_info = false;  // b_oamd_extension_present with ajoc_bed_info()
};

struct ExpectedObject {
    bool lfe = false;
    // The tones the object carries once it has settled, and their amplitudes
    // (full scale 1.0); in core decoding the downmix's objects. With
    // `decorrelated`, the object also takes a decorrelator's output, which
    // adds to its tones at a phase the builder does not work out.
    std::vector<std::array<double, 2>> tones;
    bool decorrelated = false;
    // pos3D_X, pos3D_Y, pos3D_Z_sign and pos3D_Z of the metadata of each frame
    // (its last block), for a dynamic object; empty for the others.
    std::vector<std::array<int, 4>> positions;
};

struct BuiltObjectStream {
    std::vector<std::vector<std::byte>> frames;  // raw_ac4_frame()s
    // The writer's records of each frame, in the order the decoder reads them.
    std::vector<std::vector<ac4::SyntaxRecord>> traces;
    std::vector<ExpectedObject> full;  // the objects of full decoding, LFE first
    std::vector<ExpectedObject> core;  // and of core decoding
    // Full decoding's metadata timing: per frame, each block's update sample
    // from the codec frame's first sample (sample_offset + 32 x
    // block_offset_factor, clause 5.9.2), of the timing that applies to it.
    std::vector<std::vector<int>> update_samples;
};

// `frames` frames of the case, an I-frame every fourth.
[[nodiscard]] BuiltObjectStream build_objects(const ObjectCase& c, int frames);

// A stream's frames, sync-framed with a CRC, as a file holds them.
[[nodiscard]] std::vector<std::byte> sync_framed(const BuiltObjectStream& stream);

// The cases committed as tests/golden/ac4dec/objects/<name>.ac4 with
// kCommittedObjectFrames frames each, whose digests tools/references/
// ac4_syntax.py wrote beside the others in tests/golden/ac4dec/.
inline constexpr int kCommittedObjectFrames = 8;
[[nodiscard]] std::vector<ObjectCase> committed_object_cases();

// A-JOC: object `o`'s dry coefficient on the downmix signal A-JOC takes as
// input `ch` (Pseudocode 14a's order), in the band of that signal's tone, as
// the builder quantises it; that signal's tone; and, for a case with
// `dialogue`, the dialogue object's downmix coefficient on it (Table 82).
[[nodiscard]] double ajoc_dry_coefficient(const ObjectCase& c, int o, int ch);
[[nodiscard]] double ajoc_input_tone_hz(const ObjectCase& c, int ch);
[[nodiscard]] double ajoc_dialogue_dmx_coefficient(const ObjectCase& c, int ch);

// The tone of downmix signal or direct-coded object `k`, in Hz: the middle of
// QMF subband 2k + 1. The LFE's is 47 Hz.
[[nodiscard]] double object_tone_hz(int k);
inline constexpr double kLfeToneHz = 47.0;

}  // namespace ac4dec_test
