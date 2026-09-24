#include "output_decision.hpp"

#include <algorithm>
#include <fmt/format.h>

// See output_decision.hpp for what this decides and why it is a pure
// function. The order of the rules below is the order they are applied, and
// each one is a case in tests/hearth/test_output_decision.cpp.

namespace ac3::hearth {

namespace {

using audio::BitstreamFormat;

// Whether `endpoint` will carry `stream` as it is.
[[nodiscard]] bool carries_native(const EndpointFacts& endpoint,
                                   std::optional<BitstreamFormat> stream) {
    if (!stream) {
        return false;
    }
    return *stream == BitstreamFormat::kEac3 ? endpoint.accepts_eac3 : endpoint.accepts_ac3;
}

// Whether an E-AC-3 stream could reach `endpoint` as AC-3 instead.
[[nodiscard]] bool carries_as_ac3(const EndpointFacts& endpoint,
                                   std::optional<BitstreamFormat> stream,
                                   const OutputRequest& request) {
    return stream == BitstreamFormat::kEac3 && endpoint.accepts_ac3 && request.follow_sink &&
           request.transcode_available;
}

// What a capability answer is worth saying about, and nothing when it needs
// no explanation. A probe or a descriptor answered the question; the other
// two did not, and which of them it was is gap 6's whole point.
[[nodiscard]] std::string unknown_note(const EndpointFacts& endpoint) {
    switch (endpoint.source) {
        case CapabilitySource::kNoDescriptor:
            return fmt::format(" \"{}\" reports no descriptor, so nothing is known about what it "
                               "accepts - a receiver that is off or on another input looks like "
                               "this.",
                               endpoint.name);
        case CapabilitySource::kNoReader:
            return " This platform cannot read a sink's descriptor at all, so only a probe of the "
                   "output answered - a refusal can be this machine's, such as another "
                   "application holding the output, rather than a limit of the sink.";
        case CapabilitySource::kDescriptor:
        case CapabilitySource::kProbe: return {};
    }
    return {};
}

// The endpoint a local decode should go to when nothing else has chosen one:
// the default, else the widest that says how wide it is, else the first.
[[nodiscard]] const EndpointFacts* best_for_pcm(std::span<const EndpointFacts> endpoints) {
    if (endpoints.empty()) {
        return nullptr;
    }
    for (const auto& endpoint : endpoints) {
        if (endpoint.is_default) {
            return &endpoint;
        }
    }
    const auto widest = std::max_element(endpoints.begin(), endpoints.end(),
                                         [](const EndpointFacts& a, const EndpointFacts& b) {
                                             return a.channels < b.channels;
                                         });
    return widest != endpoints.end() ? &*widest : &endpoints.front();
}

[[nodiscard]] const EndpointFacts* find_endpoint(std::span<const EndpointFacts> endpoints,
                                                  std::string_view id) {
    if (id.empty()) {
        return nullptr;
    }
    const auto found = std::find_if(endpoints.begin(), endpoints.end(),
                                    [id](const EndpointFacts& e) { return e.id == id; });
    return found != endpoints.end() ? &*found : nullptr;
}

// A local decode to `endpoint`, with whatever the caller should know about
// it: what a decode loses against bitstreaming, and how wide the device is.
[[nodiscard]] OutputChoice local_pcm(const EndpointFacts& endpoint, const OutputRequest& request,
                                      std::string_view because) {
    std::string reason = fmt::format("Decoding here and playing PCM to \"{}\"", endpoint.name);
    if (endpoint.channels != 0) {
        reason += fmt::format(" at its own {} channels", endpoint.channels);
    }
    reason += ".";
    if (!because.empty()) {
        reason += fmt::format(" {}", because);
    }
    if (request.has_objects) {
        reason +=
            " The object layer is rendered to those speakers rather than sent on, which is what "
            "a decode of a JOC stream means.";
    }
    return {.mode = OutputMode::kLocalPcm,
            .endpoint_id = endpoint.id,
            .endpoint_name = endpoint.name,
            .reason = std::move(reason)};
}

[[nodiscard]] OutputChoice bitstream(const EndpointFacts& endpoint, BitstreamFormat format,
                                      const OutputRequest& request, std::string_view because) {
    const bool as_ac3 = format == BitstreamFormat::kAc3;
    std::string reason =
        fmt::format("Bitstreaming {} to \"{}\" over IEC 61937, untouched.",
                    as_ac3 ? "AC-3" : "E-AC-3", endpoint.name);
    if (!because.empty()) {
        reason += fmt::format(" {}", because);
    }
    if (endpoint.is_default) {
        reason +=
            " It is this machine's default output, so holding it exclusively silences whatever "
            "else is playing - and can be refused outright while another application holds it.";
    }
    if (request.has_objects && format == BitstreamFormat::kEac3) {
        reason += " The object layer travels with it, for the sink to render.";
    }
    return {.mode = OutputMode::kBitstream,
            .endpoint_id = endpoint.id,
            .endpoint_name = endpoint.name,
            .reason = std::move(reason)};
}

[[nodiscard]] OutputChoice transcoded(const EndpointFacts& endpoint, const OutputRequest& request) {
    std::string reason = fmt::format(
        "\"{}\" takes AC-3 but not E-AC-3, so this is transcoded to AC-3 and bitstreamed.",
        endpoint.name);
    if (request.has_objects) {
        reason += " The object layer cannot survive that and is rendered into the 5.1 bed.";
    }
    return {.mode = OutputMode::kBitstreamAsAc3,
            .endpoint_id = endpoint.id,
            .endpoint_name = endpoint.name,
            .reason = std::move(reason)};
}

[[nodiscard]] OutputChoice nothing(std::string reason) {
    return OutputChoice{.mode = OutputMode::kNone,
                        .endpoint_id = {},
                        .endpoint_name = {},
                        .reason = std::move(reason)};
}

// The automatic choice for one endpoint, in preference order: the stream as
// it is, then transcoded, then a local decode. `nullptr` when this endpoint
// can carry nothing at all.
[[nodiscard]] std::optional<OutputChoice> best_for(const EndpointFacts& endpoint,
                                                    const OutputRequest& request) {
    if (carries_native(endpoint, request.stream)) {
        return bitstream(endpoint, *request.stream, request, {});
    }
    if (carries_as_ac3(endpoint, request.stream, request)) {
        return transcoded(endpoint, request);
    }
    if (endpoint.accepts_pcm || endpoint.channels != 0) {
        return local_pcm(endpoint, request, {});
    }
    return std::nullopt;
}

}  // namespace

std::string_view describe(OutputMode mode) {
    switch (mode) {
        case OutputMode::kBitstream: return "bitstream";
        case OutputMode::kBitstreamAsAc3: return "bitstream as AC-3";
        case OutputMode::kLocalPcm: return "local PCM";
        case OutputMode::kNetworkGroup: return "network group";
        case OutputMode::kNone: return "none";
    }
    return "unknown output mode";
}

std::string_view describe(CapabilitySource source) {
    switch (source) {
        case CapabilitySource::kDescriptor: return "the sink's own descriptor";
        case CapabilitySource::kProbe: return "a live probe of the endpoint";
        case CapabilitySource::kNoDescriptor: return "no descriptor reported";
        case CapabilitySource::kNoReader: return "no descriptor reader on this platform";
    }
    return "unknown capability source";
}

OutputChoice choose_output(const OutputRequest& request) {
    // A group of network sinks is not an endpoint of this machine and does
    // not compete with one: it is chosen when the user has chosen it, and
    // its readiness is the only question. Taken first so that a group the
    // user selected is never quietly replaced by a local output.
    const bool wants_group = request.pinned == OutputMode::kNetworkGroup ||
                              (!request.group_name.empty() && !request.pinned.has_value());
    if (wants_group) {
        if (request.group_ready) {
            return OutputChoice{
                .mode = OutputMode::kNetworkGroup,
                .endpoint_id = {},
                .endpoint_name = {},
                .group_name = request.group_name,
                .reason = fmt::format("Playing to the group \"{}\". Each sink decodes for "
                                      "itself, so what travels is the stream, not PCM.",
                                      request.group_name)};
        }
        if (request.group_name.empty()) {
            return nothing(
                "No group is selected, so there is nothing to play to. Pick a group, or an output "
                "of this machine.");
        }
        // Not ready: say so, then carry on to the local choice unless the
        // user asked to be refused instead.
        const std::string held = fmt::format(
            "The group \"{}\" is not ready - a sink is unpaired, or another server is holding it.",
            request.group_name);
        if (!request.follow_sink || request.pinned == OutputMode::kNetworkGroup) {
            return nothing(held);
        }
        if (const auto* fallback = best_for_pcm(request.endpoints); fallback != nullptr) {
            if (auto choice = best_for(*fallback, request)) {
                choice->reason = fmt::format("{} {}", held, choice->reason);
                return *choice;
            }
        }
        return nothing(held);
    }

    if (request.endpoints.empty()) {
        return nothing(
            "This machine reports no output at all. Nothing can be played until one appears, or a "
            "group of network sinks is selected.");
    }

    // An endpoint the user named is taken with the best mode it can carry -
    // their pinned mode first if it can, and the reason says what it got
    // instead if it cannot.
    const EndpointFacts* preferred = find_endpoint(request.endpoints, request.preferred_endpoint_id);
    const std::span<const EndpointFacts> pool =
        preferred != nullptr ? std::span<const EndpointFacts>{preferred, 1} : request.endpoints;

    if (request.pinned == OutputMode::kBitstream || request.pinned == OutputMode::kBitstreamAsAc3) {
        if (!request.stream) {
            return nothing(
                "This item carries nothing IEC 61937 can wrap - a bitstream output has no format "
                "to send. Decode it instead.");
        }
        const bool want_ac3 = request.pinned == OutputMode::kBitstreamAsAc3;
        // AC-3 already is AC-3: only E-AC-3 needs the transcode.
        const bool needs_transcode = want_ac3 && *request.stream == BitstreamFormat::kEac3;
        bool takes_ac3 = false;
        for (const auto& endpoint : pool) {
            if (!want_ac3 && carries_native(endpoint, request.stream)) {
                return bitstream(endpoint, *request.stream, request, {});
            }
            takes_ac3 = takes_ac3 || endpoint.accepts_ac3;
            if (want_ac3 && endpoint.accepts_ac3 &&
                (!needs_transcode || request.transcode_available)) {
                return needs_transcode ? transcoded(endpoint, request)
                                       : bitstream(endpoint, BitstreamFormat::kAc3, request, {});
            }
        }
        const std::string note = preferred != nullptr ? unknown_note(*preferred) : std::string{};
        // An output that takes AC-3 was there; what was missing is the
        // transcode to reach it.
        const std::string why =
            needs_transcode && takes_ac3
                ? fmt::format("{} takes AC-3, but E-AC-3 cannot be transcoded to AC-3 here.{}",
                              preferred != nullptr ? "The chosen output" : "An output", note)
                : fmt::format("No {} output takes {} over IEC 61937.{}",
                              preferred != nullptr ? "chosen" : "available",
                              *request.stream == BitstreamFormat::kEac3 && !want_ac3 ? "E-AC-3"
                                                                                     : "AC-3",
                              note);
        if (!request.follow_sink) {
            return nothing(fmt::format("{} follow=off, so this is a refusal rather than a decode.",
                                       why));
        }
        const auto* fallback = preferred != nullptr ? preferred : best_for_pcm(request.endpoints);
        if (fallback != nullptr) {
            return local_pcm(*fallback, request, why);
        }
        return nothing(why);
    }

    if (request.pinned == OutputMode::kLocalPcm) {
        const auto* endpoint = preferred != nullptr ? preferred : best_for_pcm(request.endpoints);
        if (endpoint == nullptr) {
            return nothing("No output of this machine can be played to.");
        }
        return local_pcm(*endpoint, request, {});
    }

    // Automatic. The stream as it is beats a transcode, which beats a decode,
    // and an endpoint the user named beats one they did not.
    for (const auto& endpoint : pool) {
        if (carries_native(endpoint, request.stream)) {
            return bitstream(endpoint, *request.stream, request, {});
        }
    }
    for (const auto& endpoint : pool) {
        if (carries_as_ac3(endpoint, request.stream, request)) {
            return transcoded(endpoint, request);
        }
    }
    if (!request.follow_sink && request.stream) {
        return nothing(fmt::format(
            "No output takes {} over IEC 61937, and follow=off refuses a decode in its place.{}",
            *request.stream == BitstreamFormat::kEac3 ? "E-AC-3" : "AC-3",
            preferred != nullptr ? unknown_note(*preferred) : std::string{}));
    }
    const auto* endpoint = preferred != nullptr ? preferred : best_for_pcm(request.endpoints);
    if (endpoint == nullptr) {
        return nothing("No output of this machine can be played to.");
    }
    std::string because;
    if (request.stream) {
        because = fmt::format("No output takes {} over IEC 61937.{}",
                              *request.stream == BitstreamFormat::kEac3 ? "E-AC-3" : "AC-3",
                              unknown_note(*endpoint));
    }
    return local_pcm(*endpoint, request, because);
}

}  // namespace ac3::hearth
