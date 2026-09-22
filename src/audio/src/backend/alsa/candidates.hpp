#pragma once

#include <alsa/asoundlib.h>

#include <fmt/format.h>
#include <string>
#include <vector>

#include "alsa_support.hpp"
#include "device_names.hpp"

// The card/device walk passthrough.cpp's enumerate_render_devices() and
// sink_capabilities.cpp's ALSA backend both need: every digital output on the
// machine, in card then device order, with both the alsa-lib device name
// (CARD=/DEV= carries a config-relative logical index - "hdmi:DEV=0" is the
// card's first HDMI PCM whatever hardware device number it happens to have)
// and the raw hardware device index device_names.hpp's own comment warns not
// to confuse it with. sink_capabilities.cpp needs the raw index because that,
// not the logical one, is what /proc/asound/card<N>/eld#<dev>.<port> is keyed
// by - see eld_proc.hpp.
//
// Header-only and libasound-light in the same spirit as alsa_support.hpp:
// only <alsa/asoundlib.h> for snd_pcm_stream_t, which for_each_pcm already
// requires.

namespace ac3::alsa {

// A digital output found by walking the cards, before it has been probed for
// passthrough support (that stays in passthrough.cpp - this file only finds
// candidates, it does not open any of them).
struct Candidate {
    int card = 0;
    int device = 0;       // the RAW hardware device index (not the logical
                           // one `name` carries) - what an eld# proc file is
                           // keyed by
    std::string card_id;  // the CARD= name, e.g. "PCH" - /proc/asound/<this>/
                           // is ALSA's own name-keyed sibling of card<N>/
    DigitalOutput kind = DigitalOutput::kNone;
    std::string name;     // "iec958:CARD=PCH,DEV=0" - no channel status yet
    std::string hw_name;  // "hw:CARD=PCH,DEV=1" - the control probe's target
    std::string friendly;  // for a device list a person reads
};

// Every digital output on the machine, in card then device order.
//
// The `hdmi:`/`iec958:` plugins take a logical index - the card's first HDMI
// PCM is hdmi:DEV=0 whatever hardware device number it happens to have - so
// the two are counted separately per card as the walk goes.
[[nodiscard]] inline std::vector<Candidate> find_candidates() {
    std::vector<Candidate> candidates;
    int counted_card = -1;
    unsigned hdmi_index = 0;
    unsigned spdif_index = 0;

    for_each_pcm(SND_PCM_STREAM_PLAYBACK, [&](const PcmEntry& entry) {
        if (entry.card != counted_card) {
            counted_card = entry.card;
            hdmi_index = 0;
            spdif_index = 0;
        }
        const DigitalOutput kind =
            classify_digital_output(entry.device_name, entry.card_id, entry.card_name);
        if (kind == DigitalOutput::kNone) {
            return;
        }
        unsigned& index = kind == DigitalOutput::kHdmi ? hdmi_index : spdif_index;
        candidates.push_back(Candidate{
            .card = entry.card,
            .device = entry.device,
            .card_id = entry.card_id,
            .kind = kind,
            .name = config_device_name(kind, entry.card_id, index),
            .hw_name = hw_device_name(entry.card_id, entry.device),
            .friendly = fmt::format("{}: {}", entry.card_name, entry.device_name),
        });
        ++index;
    });
    return candidates;
}

}  // namespace ac3::alsa
