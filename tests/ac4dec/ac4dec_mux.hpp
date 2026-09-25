#pragma once

// The test multiplexer of phase D7 (planning/ac4.md): it builds presentations
// of several substreams (ETSI TS 103 190-2 V1.3.1 clause 4.8) from the
// substreams of separate streams - DEE's, and the encoder's mono and stereo
// ones, which DEE does not write - frame by frame.
//
// Each group of the new stream takes the audio substream of one source,
// rewritten where the layout asks: its extended_metadata() with the dialogue
// fields (b_dialog, dialog_max_gain, pan_dialog), and its
// dialog_enhancement() with a hybrid method's configuration and parameters.
// Each presentation takes the presentation substream of one source (its
// dialnorm, DRC and downmix), with the group gains and the associated audio's
// mixing values in place of the source's. The rest of each substream is the
// source's, bit for bit: the multiplexer finds the fields it replaces by the
// offsets the decoder's syntax trace gives them. The new fields and the table
// of contents are written with the encoder's writer (src/ac4enc/src/frame/),
// which phase E6 extends to the encoder's own presentations.
//
// multiplex() writes bitstream_version 2 with version 1 presentations, at the
// sources' frame rate, each group one channel-coded substream.
// multiplex_v0() writes bitstream_version 1 with version 0 presentations
// (Part 1 clause 4.2.3), which name their substreams directly and have no
// presentation substream: each substream's metadata() is rewritten at sus_ver
// 0, carrying its own dialnorm and, where the table of contents makes it
// associated audio or dialogue, the mixing fields; its table of contents is
// written with the tests' own writer (tests/ac4/ac4_toc_writer.hpp), the
// encoder writing no version 0 presentations.

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "frame/metadata.hpp"

namespace ac4dec_test {

// A hybrid dialogue enhancement method to write into a group's substream in
// place of what the source sends: its de_config() and every frame's
// parameters (constant), with de_signal_contribution.
struct MuxDe {
    ac4::detail::DeConfigCodes config{};
    ac4::detail::DeFrameParameters parameters{};
};

struct MuxGroup {
    std::size_t source = 0;                 // whose audio substream
    std::optional<int> content_classifier;  // content_type(), Part 1 Table 91
    std::string language;                   // language_tag_bytes
    // extended_metadata()'s dialogue fields; none writes b_dialog 0.
    std::optional<ac4::detail::DialogueMixCodes> dialogue;
    std::optional<MuxDe> de;
};

struct MuxPresentation {
    std::optional<int> presentation_config;  // unset: a single substream group
    std::vector<int> groups;
    int md_compat = 1;
    std::optional<int> presentation_id;
    std::optional<bool> enable;
    bool pre_virtualized = false;
    std::size_t source = 0;  // whose presentation substream it is spliced from
    // The group gains and the associated audio's values; n_substream_groups
    // is filled in from the configuration.
    ac4::detail::PresentationMixCodes mix{};
};

struct MuxLayout {
    std::vector<MuxGroup> groups;
    std::vector<MuxPresentation> presentations;
};

// A substream of a version 0 layout: one source's audio substream with its
// own dialnorm (basic_metadata(), Part 1 clause 4.3.12.2.1) and content type.
// Part 1 clauses 4.3.12.4.1 and 4.3.12.4.2 make it associated audio or
// dialogue by its content_classifier or its place in a presentation
// (src/ac4dec/ERRATA.md, "b_associated and b_dialog are parameters at sus_ver
// 0"), and then extended_metadata() carries these fields, none sent where
// they are unset; the multiplexer refuses a substream those make one thing in
// one presentation and another in the next.
struct MuxSubstreamV0 {
    std::size_t source = 0;
    int dialnorm_bits = 124;  // -31 dBFS
    std::optional<int> content_classifier;
    std::string language;
    std::optional<ac4::detail::AssociatedMixCodes> associated;
    std::optional<ac4::detail::DialogueMixCodes> dialogue;
};

struct MuxPresentationV0 {
    std::optional<int> presentation_config;  // unset: a single substream
    std::vector<int> substreams;             // Part 1 Table 85's order
    int md_compat = 0;
    std::optional<int> presentation_id;
};

struct MuxLayoutV0 {
    std::vector<MuxSubstreamV0> substreams;
    std::vector<MuxPresentationV0> presentations;
};

// A source: one stream's raw_ac4_frame()s, one presentation of one group of
// one channel-coded substream each, as DEE and the encoder write them.
struct MuxSource {
    std::vector<std::vector<std::byte>> frames;
};

// The sync-framed file's raw_ac4_frame()s.
[[nodiscard]] MuxSource mux_source(std::span<const std::byte> file);

// `frames` frames of the layout over `sources`, each source's frames taken
// from its first. The presentation substreams come first in the substream
// index table, in the presentations' order, then the groups' audio
// substreams. Throws std::runtime_error where a source cannot be spliced.
[[nodiscard]] std::vector<std::vector<std::byte>> multiplex(std::span<const MuxSource> sources,
                                                            const MuxLayout& layout, std::size_t frames);

// The same for version 0 presentations: the substreams in the layout's order,
// every one an audio substream.
[[nodiscard]] std::vector<std::vector<std::byte>> multiplex_v0(std::span<const MuxSource> sources,
                                                               const MuxLayoutV0& layout, std::size_t frames);

// The frames sync-framed with a CRC (Part 1 Annex G), as a file holds them.
[[nodiscard]] std::vector<std::byte> mux_sync_framed(std::span<const std::vector<std::byte>> frames);

}  // namespace ac4dec_test
