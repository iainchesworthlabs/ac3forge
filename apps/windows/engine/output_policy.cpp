#include "output_policy.hpp"

#include <algorithm>

namespace ac3::windemo {

std::string_view describe(OutputMode mode) {
    switch (mode) {
        case OutputMode::kAtmos: return "Atmos (E-AC-3 JOC over HDMI)";
        case OutputMode::kDdPlus51: return "Dolby Digital Plus 5.1 over HDMI";
        case OutputMode::kDd51: return "Dolby Digital 5.1 over HDMI/S/PDIF";
        case OutputMode::kPcmSurround: return "PCM surround (decoded)";
        case OutputMode::kHeadphones: return "headphones (decoded objects, Windows Spatial Sound)";
        case OutputMode::kStereo: return "stereo (decoded Lo/Ro)";
        case OutputMode::kNone: return "no usable output";
    }
    return "unknown output mode";
}

namespace {

bool is_bitstream(OutputMode mode) {
    return mode == OutputMode::kAtmos || mode == OutputMode::kDdPlus51 || mode == OutputMode::kDd51;
}

// Whether `endpoint` can carry `mode` at all, ignoring the default-endpoint
// rule (handled by the caller so the reason can say so).
bool carries(const EndpointFacts& endpoint, OutputMode mode, bool key) {
    switch (mode) {
        case OutputMode::kAtmos: return endpoint.accepts_eac3 && key;
        case OutputMode::kDdPlus51: return endpoint.accepts_eac3 && !key;
        case OutputMode::kDd51: return endpoint.accepts_ac3;
        case OutputMode::kPcmSurround: return endpoint.shared_channels >= 6;
        case OutputMode::kHeadphones: return endpoint.spatial && endpoint.spatial_max_objects > 0;
        case OutputMode::kStereo: return endpoint.shared_channels >= 2;
        case OutputMode::kNone: return false;
    }
    return false;
}

struct Candidate {
    const EndpointFacts* endpoint = nullptr;
    // A bitstream mode that only the default endpoint could carry: not
    // chosen, but worth telling the user about.
    const EndpointFacts* blocked_by_default = nullptr;
};

// The best endpoint for `mode`: any that is not the null sink and, for a
// bitstream mode, not the default. With `allow_default` false the default is
// skipped for every mode, which is how the caller keeps shared-mode output
// off the endpoint applications render to (hearing the direct mix alongside
// ours is the worst outcome of all) until nothing else is left.
Candidate best_for(std::span<const EndpointFacts> endpoints, OutputMode mode, bool key,
                   bool allow_default) {
    Candidate best;
    for (const auto& endpoint : endpoints) {
        if (endpoint.is_null_sink || !carries(endpoint, mode, key)) {
            continue;
        }
        if (endpoint.is_default) {
            if (is_bitstream(mode)) {
                if (best.blocked_by_default == nullptr) {
                    best.blocked_by_default = &endpoint;
                }
                continue;
            }
            if (!allow_default) {
                continue;
            }
        }
        if (best.endpoint == nullptr || (best.endpoint->is_default && !endpoint.is_default)) {
            best.endpoint = &endpoint;
        }
    }
    return best;
}

OutputChoice choose(const EndpointFacts& endpoint, OutputMode mode, std::string reason) {
    return {.mode = mode,
            .endpoint_id = endpoint.id,
            .endpoint_name = endpoint.name,
            .reason = std::move(reason)};
}

constexpr OutputMode kPreference[] = {OutputMode::kAtmos,       OutputMode::kDdPlus51,
                                      OutputMode::kDd51,        OutputMode::kPcmSurround,
                                      OutputMode::kHeadphones,  OutputMode::kStereo};

}  // namespace

OutputChoice choose_output(const OutputPolicyInput& input) {
    const bool key = input.signing_key_loaded;
    std::string preamble;

    if (input.pinned && *input.pinned != OutputMode::kNone) {
        auto pinned = best_for(input.endpoints, *input.pinned, key, /*allow_default=*/false);
        if (pinned.endpoint == nullptr) {
            pinned = best_for(input.endpoints, *input.pinned, key, /*allow_default=*/true);
        }
        if (pinned.endpoint != nullptr) {
            return choose(*pinned.endpoint, *input.pinned,
                          "pinned by the user on \"" + pinned.endpoint->name + "\"");
        }
        preamble = "pinned mode " + std::string(describe(*input.pinned)) +
                   " has no endpoint that can carry it" +
                   (pinned.blocked_by_default != nullptr
                        ? " except the default (\"" + pinned.blocked_by_default->name +
                              "\"), which applications are rendering to; move the default to "
                              "the null sink first"
                        : std::string{}) +
                   "; falling back. ";
    }

    // Two passes over the preference order: first with the default endpoint
    // off the table entirely, then, only if that found nothing, with it
    // allowed for the shared-mode modes.
    const EndpointFacts* blocked = nullptr;
    OutputMode blocked_mode = OutputMode::kNone;
    for (const bool allow_default : {false, true}) {
        for (const OutputMode mode : kPreference) {
            const auto candidate = best_for(input.endpoints, mode, key, allow_default);
            if (candidate.blocked_by_default != nullptr && blocked == nullptr) {
                blocked = candidate.blocked_by_default;
                blocked_mode = mode;
            }
            if (candidate.endpoint == nullptr) {
                continue;
            }
            std::string reason = preamble;
            switch (mode) {
                case OutputMode::kAtmos:
                    reason += "\"" + candidate.endpoint->name +
                              "\" accepts E-AC-3 and a signing key is loaded: objects go out intact";
                    break;
                case OutputMode::kDdPlus51:
                    reason += "\"" + candidate.endpoint->name +
                              "\" accepts E-AC-3 but no signing key is loaded: 5.1 bed only, "
                              "positions panned (load a key for objects)";
                    break;
                case OutputMode::kDd51:
                    reason += "\"" + candidate.endpoint->name +
                              "\" accepts AC-3 but not E-AC-3: 5.1 bed, positions panned";
                    break;
                case OutputMode::kPcmSurround:
                    reason += "no endpoint bitstreams; \"" + candidate.endpoint->name + "\" takes " +
                              std::to_string(candidate.endpoint->shared_channels) +
                              " PCM channels, so the stream is decoded and played as surround";
                    break;
                case OutputMode::kHeadphones:
                    reason += "no endpoint bitstreams or takes surround PCM; \"" +
                              candidate.endpoint->name +
                              "\" has a spatial sound format enabled, so decoded objects go through "
                              "Windows Spatial Sound";
                    break;
                case OutputMode::kStereo:
                    reason += "nothing better than stereo on \"" + candidate.endpoint->name +
                              "\": the stream is decoded to Lo/Ro";
                    break;
                case OutputMode::kNone: break;
            }
            if (blocked != nullptr && is_bitstream(blocked_mode) && !is_bitstream(mode)) {
                reason += ". \"" + blocked->name + "\" could carry " + std::string(describe(blocked_mode)) +
                          " but is the default endpoint applications render to; move the default "
                          "to the null sink to use it";
            }
            if (candidate.endpoint->is_default && !is_bitstream(mode)) {
                reason += ". This is also the endpoint applications render to, so their direct "
                          "audio is audible alongside";
            }
            return choose(*candidate.endpoint, mode, std::move(reason));
        }
    }

    OutputChoice none;
    none.reason = preamble + "no render endpoint can carry any mode";
    if (blocked != nullptr) {
        none.reason += "; \"" + blocked->name + "\" could carry " +
                       std::string(describe(blocked_mode)) +
                       " but is the default endpoint applications render to";
    }
    return none;
}

}  // namespace ac3::windemo
