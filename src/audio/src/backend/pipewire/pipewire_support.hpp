#pragma once

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/dict.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <charconv>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ac3/audio/passthrough.hpp"

// The handful of things capture.cpp, monitor.cpp and passthrough.cpp all need
// from libpipewire, kept in one place so the three cannot drift - the same
// role alsa_support.hpp plays for the ALSA backend.
//
// PipeWire's object graph has no single ownership convention the way ALSA's
// malloc/free-pair structs do: pw_core_disconnect() returns an int,
// pw_proxy_destroy() is void and doubles as pw_registry's own destructor
// (a registry IS a proxy), and pw_stream/pw_thread_loop/pw_context/
// pw_main_loop each have their own void …_destroy(). The RAII aliases below
// say that once, here, rather than at every call site in the three files
// that use them.

namespace ac3::pipewire {

struct ThreadLoopDeleter {
    void operator()(pw_thread_loop* loop) const { pw_thread_loop_destroy(loop); }
};
using ThreadLoop = std::unique_ptr<pw_thread_loop, ThreadLoopDeleter>;

struct MainLoopDeleter {
    void operator()(pw_main_loop* loop) const { pw_main_loop_destroy(loop); }
};
using MainLoop = std::unique_ptr<pw_main_loop, MainLoopDeleter>;

struct ContextDeleter {
    void operator()(pw_context* context) const { pw_context_destroy(context); }
};
using Context = std::unique_ptr<pw_context, ContextDeleter>;

struct CoreDeleter {
    void operator()(pw_core* core) const { pw_core_disconnect(core); }
};
using Core = std::unique_ptr<pw_core, CoreDeleter>;

struct RegistryDeleter {
    void operator()(pw_registry* registry) const {
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry));
    }
};
using Registry = std::unique_ptr<pw_registry, RegistryDeleter>;

struct StreamDeleter {
    void operator()(pw_stream* stream) const { pw_stream_destroy(stream); }
};
using Stream = std::unique_ptr<pw_stream, StreamDeleter>;

// pw_init()/pw_deinit() are process-wide, not per-object: repeated init/
// deinit cycles within one process are not a documented, relied-upon PipeWire
// usage pattern the way opening and closing an ALSA PCM repeatedly is, so
// this calls pw_init() exactly once, ever, and never pairs it with
// pw_deinit() - the same "leak it for the life of the process" choice every
// real PipeWire client (pw-cat, pw-mon, and the session manager itself)
// makes, since the alternative is repeated init/deinit cycles nothing
// upstream actually exercises.
inline void ensure_initialized() {
    static std::once_flag once;
    std::call_once(once, [] { pw_init(nullptr, nullptr); });
}

// The link rate a carrier runs at to carry `content_rate` of `format` - the
// PipeWire-side twin of ac3::alsa::carrier_rate(). Not shared with it: three
// lines duplicated beside their own backend read more clearly than a cross-
// backend helper would, and the two backends have no other reason to share
// code at all.
[[nodiscard]] constexpr std::uint32_t carrier_rate(audio::BitstreamFormat format,
                                                    std::uint32_t content_rate) {
    return format == audio::BitstreamFormat::kEac3 ? content_rate * 4 : content_rate;
}

[[nodiscard]] constexpr spa_audio_iec958_codec iec958_codec_for(audio::BitstreamFormat format) {
    return format == audio::BitstreamFormat::kEac3 ? SPA_AUDIO_IEC958_CODEC_EAC3
                                                    : SPA_AUDIO_IEC958_CODEC_AC3;
}

// The carrier is a 2-channel 16-bit stream whatever rides inside it (see
// ac3::alsa's identical constant and the comment on it), so a burst's length
// in bytes and in sample-frames differ by a constant 4 here too.
inline constexpr std::size_t kCarrierFrameBytes = 4;

// Every Audio/Source or Audio/Sink node currently in the graph, visited once
// each with its global id and property dictionary.
//
// This is a full connect - list - disconnect round trip against a fresh,
// scoped pw_main_loop: attach to the session, register a registry listener,
// ask the server to confirm it has sent every global that existed at attach
// time (pw_core_sync's `done` event, matched by sequence number so a global
// added mid-walk cannot be mistaken for the boundary), and return once that
// confirmation arrives. The same shape PipeWire's own listing tools use
// (`pw-cli ls`, the `list-inputs.c` example) as a synchronous call, which is
// what ac3::audio::enumerate_devices() and ac3::audio::enumerate_render_
// devices() both need to stay synchronous themselves.
//
// A machine with no PipeWire session running - a container, a CI runner, a
// desktop where the daemon simply is not started - fails at
// pw_context_connect() and `visit` is never called, exactly like ALSA's own
// for_each_pcm() silently visiting nothing on a machine with no sound card.
// Returns false only for that "no session to ask" case; the caller decides
// what an empty result means from context, the same way an empty ALSA
// enumeration is a success rather than an error.
template <typename Visitor>
bool for_each_global(Visitor&& visit) {
    ensure_initialized();

    MainLoop loop{pw_main_loop_new(nullptr)};
    if (!loop) {
        return false;
    }
    Context context{pw_context_new(pw_main_loop_get_loop(loop.get()), nullptr, 0)};
    if (!context) {
        return false;
    }
    Core core{pw_context_connect(context.get(), nullptr, 0)};
    if (!core) {
        return false;
    }
    Registry registry{pw_core_get_registry(core.get(), PW_VERSION_REGISTRY, 0)};
    if (!registry) {
        return false;
    }

    struct State {
        pw_main_loop* loop;
        Visitor* visit;
        int pending_seq = -1;
        bool done = false;
    } state{loop.get(), &visit, -1, false};

    spa_hook registry_listener{};
    pw_registry_events registry_events{};
    registry_events.version = PW_VERSION_REGISTRY_EVENTS;
    registry_events.global = [](void* data, uint32_t id, uint32_t /*permissions*/,
                                 const char* type, uint32_t /*version*/,
                                 const spa_dict* props) {
        auto& s = *static_cast<State*>(data);
        if (props != nullptr && type != nullptr) {
            (*s.visit)(id, std::string_view{type}, *props);
        }
    };
    pw_registry_add_listener(registry.get(), &registry_listener, &registry_events, &state);

    spa_hook core_listener{};
    pw_core_events core_events{};
    core_events.version = PW_VERSION_CORE_EVENTS;
    core_events.done = [](void* data, uint32_t id, int seq) {
        auto& s = *static_cast<State*>(data);
        if (id == PW_ID_CORE && seq == s.pending_seq) {
            s.done = true;
            pw_main_loop_quit(s.loop);
        }
    };
    pw_core_add_listener(core.get(), &core_listener, &core_events, &state);

    state.pending_seq = pw_core_sync(core.get(), PW_ID_CORE, 0);
    pw_main_loop_run(loop.get());

    return state.done;
}

// Every Node in the graph, which is what this backend's device enumeration
// wants. Written over for_each_global() so there is still exactly one
// connect-list-disconnect round trip in this file.
template <typename Visitor>
bool for_each_audio_node(Visitor&& visit) {
    return for_each_global([&visit](std::uint32_t id, std::string_view type, const spa_dict& props) {
        if (type == PW_TYPE_INTERFACE_Node) {
            visit(id, props);
        }
    });
}

// "Audio/Source" (a real input) or "Audio/Sink" (a render endpoint, whose
// monitor a capture stream can loop back - see capture.cpp) - the two
// media.class values this backend does anything with. Everything else in
// the graph (video nodes, other clients' own streams, virtual devices with
// no media.class at all) is silently not visited by for_each_audio_node's
// caller-side filtering below.
[[nodiscard]] inline bool is_audio_source(const spa_dict& props) {
    const char* class_name = spa_dict_lookup(&props, PW_KEY_MEDIA_CLASS);
    return class_name != nullptr && std::string_view{class_name} == "Audio/Source";
}

[[nodiscard]] inline bool is_audio_sink(const spa_dict& props) {
    const char* class_name = spa_dict_lookup(&props, PW_KEY_MEDIA_CLASS);
    return class_name != nullptr && std::string_view{class_name} == "Audio/Sink";
}

// What PW_KEY_TARGET_OBJECT accepts for this node - see pw_stream_connect()'s
// own documentation ("the PW_KEY_NODE_NAME value of the target node"). Used
// both as the DeviceInfo/RenderDeviceInfo id this library hands back to a
// caller and as the value that caller's id is later handed back to
// PW_KEY_TARGET_OBJECT, so the two must stay the same key.
[[nodiscard]] inline std::string node_id(const spa_dict& props) {
    const char* name = spa_dict_lookup(&props, PW_KEY_NODE_NAME);
    return name != nullptr ? name : "";
}

[[nodiscard]] inline std::string node_friendly_name(const spa_dict& props) {
    if (const char* description = spa_dict_lookup(&props, PW_KEY_NODE_DESCRIPTION);
        description != nullptr && description[0] != '\0') {
        return description;
    }
    return node_id(props);
}

// An application's own playback stream, as opposed to a device node.
// PipeWire gives every client stream a media.class of Stream/Output/Audio
// and tags it with the client's process id and name, which is what makes a
// per-process tap possible here without any of the machinery Windows needs
// (roadmap UX12). A sink's monitor carries the whole mix; this is one
// application on its own.
[[nodiscard]] inline bool is_output_stream(const spa_dict& props) {
    const char* class_name = spa_dict_lookup(&props, PW_KEY_MEDIA_CLASS);
    return class_name != nullptr && std::string_view{class_name} == "Stream/Output/Audio";
}

// A numeric property, or 0 when it is absent or unparsable.
[[nodiscard]] inline std::uint32_t dict_u32(const spa_dict& props, const char* key) {
    const char* text = spa_dict_lookup(&props, key);
    if (text == nullptr) {
        return 0;
    }
    std::uint32_t value = 0;
    const auto* first = text;
    const auto* last = text + std::char_traits<char>::length(text);
    const auto result = std::from_chars(first, last, value);
    return result.ec == std::errc{} ? value : 0;
}

// The application.* keys a client sets on itself, as one record: what the
// window needs to find an icon for the process behind a stream.
//
// These live on an object's **info**, not on its registry dictionary. What
// a registry listener is handed for a node is the server's global-key
// subset of its properties - object.path, the client and device ids,
// priorities, media.class and application.name among them - and
// application.icon-name and application.process.binary are not in that
// subset. They reach a client only by binding the object and listening for
// its info event, the same fact audio_sinks_with_info() records for
// iec958.codecs. The server copies a client's application.* properties onto
// the nodes that client creates, so a stream node's info normally carries
// them; a Flatpak client's pipewire.access.portal.app_id is on the Client
// alone. output_stream_nodes() therefore binds both, prefers the node's
// values and lets the client's fill what the node left empty.
struct StreamIdentity {
    std::string application;   // application.name
    std::string icon_name;     // application.icon-name: a freedesktop icon-theme name
    std::string binary;        // application.process.binary: the executable's basename
    std::string app_id;        // pipewire.access.portal.app_id (a Flatpak id), else application.id
    std::uint32_t claimed_pid = 0;   // application.process.id: the pid the client says it has
};

// Fills `into` from an info dictionary, keeping what is already there
// wherever a key is absent or empty.
inline void read_stream_identity(const spa_dict& props, StreamIdentity& into) {
    const auto take = [&props](const char* key, std::string& field) {
        if (const char* value = spa_dict_lookup(&props, key);
            value != nullptr && value[0] != '\0') {
            field = value;
        }
    };
    take(PW_KEY_APP_NAME, into.application);
    take(PW_KEY_APP_ICON_NAME, into.icon_name);
    take(PW_KEY_APP_PROCESS_BINARY, into.binary);
    take("pipewire.access.portal.app_id", into.app_id);
    if (into.app_id.empty()) {
        take(PW_KEY_APP_ID, into.app_id);
    }
    if (const std::uint32_t claimed = dict_u32(props, PW_KEY_APP_PROCESS_ID); claimed != 0) {
        into.claimed_pid = claimed;
    }
}

// Whether the client that made a node speaks to the daemon through a relay,
// from the node's client.api property.
//
// A client talking to the daemon itself sets nothing here, or "pipewire". A
// PulseAudio client reaches it through pipewire-pulse, which sets
// "pipewire-pulse"; that process owns the socket, so the Client's
// credentials describe pipewire-pulse and not the application. Anything else
// unrecognised is treated as a relay too, which costs a bind and is the safe
// way round: believing a relay's pid mixes every application it carries into
// one, while binding a direct client's node changes nothing about the answer.
[[nodiscard]] inline bool client_api_is_relay(const char* api) {
    return api != nullptr && std::string_view{api} != "pipewire";
}

// Which process a stream belongs to, from the two pids that describe it:
// what the daemon read from the socket credentials, and what the client said
// about itself in application.process.id.
//
// The credentials win, because they cannot be forged - except where they
// name a relay carrying many applications, where they are the same pid for
// all of them and therefore useless, and except where there are none. The
// claimed pid is the answer in both of those cases, and 0 when there is
// nothing to go on: a stream nobody can attribute is one nothing can tap.
[[nodiscard]] inline std::uint32_t stream_owner_pid(std::uint32_t credentials_pid,
                                                    std::uint32_t claimed_pid, bool relayed) {
    if ((relayed || credentials_pid == 0) && claimed_pid != 0) {
        return claimed_pid;
    }
    return credentials_pid;
}

// One application's playback stream, with the process behind it.
struct OutputStreamNode {
    std::uint32_t id = 0;      // the registry global id
    std::string target;        // what PW_KEY_TARGET_OBJECT accepts for it
    std::string application;   // application.name, for a person to read
    std::uint32_t pid = 0;     // the process that plays the sound (see the relay note below)
    std::string icon_name;     // application.icon-name, or empty (see StreamIdentity)
    std::string binary;        // application.process.binary, or empty
    std::string app_id;        // the portal app id (Flatpak) or application.id, or empty
};

// Every application playback stream in the graph, each with the process id
// behind it and what the application says about itself for an icon.
//
// The pid is **not on the node**, which is the thing worth writing down: a
// Stream/Output/Audio node carries application.name, media.class and a
// client.id, and nothing else about who owns it. The process lives on the
// **Client** object that client.id names. Reading application.process.id off
// the node - the obvious thing, and the wrong thing - matches nothing at all,
// so a tap keyed on it never finds a process and a session list is always
// empty. Both happened, and neither shows up without a real session to walk.
//
// Of the two pids the Client carries, pipewire.sec.pid is the one the daemon
// took from the socket credentials, while application.process.id is whatever
// the client said about itself. The credentials are the better answer right
// up until the socket belongs to a **relay**, and on a desktop it usually
// does: pipewire-pulse speaks the PulseAudio protocol on one side and the
// native one on the other, so every application that uses the Pulse API -
// VLC, Firefox, Chromium, Spotify, most of a Debian desktop - reaches the
// daemon through that one process, and every one of their Clients carries
// pipewire-pulse's pid. Taking it at face value gives a session list with a
// single entry called pipewire-pulse however many applications are playing,
// and a per-process tap that matches none of them. Measured on a Raspberry
// Pi 4B, 2026-09-05: VLC's Client said pipewire.sec.pid 32005, which was
// pipewire-pulse; VLC itself was 49692.
//
// The relay says so on each node it creates: client.api, a registry key, is
// "pipewire-pulse" there and absent or "pipewire" for a native client. Where
// it names a relay the credentials describe the relay rather than the
// application, and application.process.id - which pipewire-pulse copies from
// the Pulse client's own proplist - is the pid that means something. That
// key is not in the registry subset either, so those nodes are bound for
// their info whatever identity depth the caller asked for: a wrong pid is
// not an identity nicety, it is the difference between a tap that works and
// one that captures silence.
//
// The icon identity is the second thing the registry dictionary lacks (see
// StreamIdentity), so this is a bind-for-info walk in the shape of
// audio_sinks_with_info(): the registry pass records the facts it always
// did - target, application.name, client.id, the Client's pid - and
// additionally binds each stream node and each Client; a second sync waits
// for the info events those binds provoke. The registry facts never depend
// on a bind: a machine where a bind fails, or a server that sends no info,
// yields the list this function returned before, with the three identity
// fields empty. A core error, or a first sync that never comes back, is the
// other case: nothing is returned at all, as the registry-only walk did.
//
// Cost, measured on a Raspberry Pi 4B on 2026-09-05: the binds and the
// second round trip take about a second there with a graph of a few dozen
// objects. The Linux session monitor pays it twice a second on a thread of
// its own and does not care; the tap does, because Capture::
// start_process_loopback calls this from the engine's frame thread, where a
// second is thirty dropped frames. So the identity is asked for rather than
// assumed: kRegistryOnly, the default, binds nothing it does not have to -
// only the relayed nodes above, and only because their pid is wrong without
// it - and the session monitor, which needs an icon name for every stream,
// asks for kWithInfo. A graph with no Pulse application in it costs
// kRegistryOnly exactly what it always did: one round trip, no binds.
enum class StreamIdentityDepth : std::uint8_t {
    kRegistryOnly,  // names, target and pid; the identity fields are filled only
                    // for relayed streams, which are bound anyway for their pid
    kWithInfo,      // additionally bind each node and client for the identity on their info
};

[[nodiscard]] inline std::vector<OutputStreamNode> output_stream_nodes(
    StreamIdentityDepth depth = StreamIdentityDepth::kRegistryOnly) {
    ensure_initialized();

    MainLoop loop{pw_main_loop_new(nullptr)};
    if (!loop) {
        return {};
    }
    Context context{pw_context_new(pw_main_loop_get_loop(loop.get()), nullptr, 0)};
    if (!context) {
        return {};
    }
    Core core{pw_context_connect(context.get(), nullptr, 0)};
    if (!core) {
        return {};
    }
    Registry registry{pw_core_get_registry(core.get(), PW_VERSION_REGISTRY, 0)};
    if (!registry) {
        return {};
    }

    struct BoundNode {
        pw_proxy* proxy = nullptr;
        spa_hook listener{};
        pw_node_events events{};
        StreamIdentity identity;
    };
    struct BoundClient {
        pw_proxy* proxy = nullptr;
        spa_hook listener{};
        pw_client_events events{};
        StreamIdentity identity;
    };
    struct Pending {
        std::uint32_t id = 0;
        std::string target;
        std::string application;
        std::uint32_t client_id = 0;
        bool relayed = false;         // client.api names a relay: trust the claimed pid
        BoundNode* bound = nullptr;   // null when the bind failed
    };
    struct State {
        pw_registry* registry = nullptr;
        pw_main_loop* loop = nullptr;
        StreamIdentityDepth depth = StreamIdentityDepth::kRegistryOnly;
        std::vector<Pending> streams;
        std::unordered_map<std::uint32_t, std::uint32_t> client_pids;
        std::vector<std::unique_ptr<BoundNode>> nodes;
        std::unordered_map<std::uint32_t, std::unique_ptr<BoundClient>> clients;
        int pending = 0;
        bool bound_any = false;   // something was bound, so info events are coming
        bool completed = false;   // the sync this loop waits for came back
        bool failed = false;      // the core reported an error: the walk is over
    } state{registry.get(), loop.get(), depth, {}, {}, {}, {}, 0, false, false, false};

    spa_hook registry_listener{};
    pw_registry_events registry_events{};
    registry_events.version = PW_VERSION_REGISTRY_EVENTS;
    registry_events.global = [](void* data, std::uint32_t global_id, std::uint32_t,
                                const char* type, std::uint32_t, const spa_dict* props) {
        auto* self = static_cast<State*>(data);
        if (type == nullptr || props == nullptr) {
            return;
        }
        const std::string_view kind{type};
        if (kind == PW_TYPE_INTERFACE_Client) {
            // The registry fact, as before: which process owns this client.
            std::uint32_t pid = dict_u32(*props, "pipewire.sec.pid");
            if (pid == 0) {
                pid = dict_u32(*props, PW_KEY_APP_PROCESS_ID);
            }
            if (pid != 0) {
                self->client_pids.emplace(global_id, pid);
            }
            if (self->depth == StreamIdentityDepth::kRegistryOnly) {
                return;
            }
            // And a bind for the identity only its info carries.
            auto* client_proxy = static_cast<pw_proxy*>(
                pw_registry_bind(self->registry, global_id, type, PW_VERSION_CLIENT, 0));
            if (client_proxy == nullptr) {
                return;
            }
            auto client_bound = std::make_unique<BoundClient>();
            client_bound->proxy = client_proxy;
            client_bound->events.version = PW_VERSION_CLIENT_EVENTS;
            client_bound->events.info = [](void* bound_data, const pw_client_info* client_info) {
                auto* b = static_cast<BoundClient*>(bound_data);
                if (client_info == nullptr || client_info->props == nullptr) {
                    return;
                }
                read_stream_identity(*client_info->props, b->identity);
            };
            pw_proxy_add_object_listener(client_proxy, &client_bound->listener,
                                         &client_bound->events, client_bound.get());
            self->bound_any = true;
            self->clients.emplace(global_id, std::move(client_bound));
            return;
        }
        if (kind != PW_TYPE_INTERFACE_Node || !is_output_stream(*props)) {
            return;
        }
        // The registry facts, as before.
        const char* name = spa_dict_lookup(props, PW_KEY_APP_NAME);
        const char* serial = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
        const char* node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
        // Which protocol the owning client speaks. Absent, or "pipewire",
        // for one talking to the daemon itself; anything else - in practice
        // "pipewire-pulse" - is a relay, and its Client's credentials name
        // the relay rather than the application.
        const bool relayed = client_api_is_relay(spa_dict_lookup(props, "client.api"));
        Pending stream{.id = global_id,
                       .target = serial != nullptr    ? serial
                                 : node_name != nullptr ? node_name
                                                        : std::string{},
                       .application = name != nullptr ? name
                                      : node_name != nullptr ? node_name
                                                             : std::string{},
                       .client_id = dict_u32(*props, PW_KEY_CLIENT_ID),
                       .relayed = relayed,
                       .bound = nullptr};
        // And the bind, whose failure costs the identity - and, for a
        // relayed stream, the only pid that names the application.
        auto* node_proxy =
            self->depth == StreamIdentityDepth::kRegistryOnly && !relayed
                ? nullptr
                : static_cast<pw_proxy*>(
                      pw_registry_bind(self->registry, global_id, type, PW_VERSION_NODE, 0));
        if (node_proxy != nullptr) {
            auto node_bound = std::make_unique<BoundNode>();
            node_bound->proxy = node_proxy;
            node_bound->events.version = PW_VERSION_NODE_EVENTS;
            node_bound->events.info = [](void* bound_data, const pw_node_info* node_info) {
                auto* b = static_cast<BoundNode*>(bound_data);
                if (node_info == nullptr || node_info->props == nullptr) {
                    return;
                }
                read_stream_identity(*node_info->props, b->identity);
            };
            pw_proxy_add_object_listener(node_proxy, &node_bound->listener,
                                         &node_bound->events, node_bound.get());
            stream.bound = node_bound.get();
            self->bound_any = true;
            self->nodes.push_back(std::move(node_bound));
        }
        self->streams.push_back(std::move(stream));
    };
    pw_registry_add_listener(registry.get(), &registry_listener, &registry_events, &state);

    spa_hook core_listener{};
    pw_core_events core_events{};
    core_events.version = PW_VERSION_CORE_EVENTS;
    core_events.done = [](void* data, std::uint32_t done_id, int seq) {
        auto* self = static_cast<State*>(data);
        if (done_id == PW_ID_CORE && seq == self->pending) {
            self->completed = true;
            pw_main_loop_quit(self->loop);
        }
    };
    // A core error (the daemon going away mid-walk) ends the wait rather
    // than leaving the monitor thread inside pw_main_loop_run for ever, and
    // records that it happened: a second sync on a dead core never comes
    // back either, so it is never issued, and a walk that did not finish
    // returns nothing rather than a short list that reads as the graph.
    core_events.error = [](void* data, std::uint32_t error_id, int, int, const char*) {
        auto* self = static_cast<State*>(data);
        if (error_id == PW_ID_CORE) {
            self->failed = true;
            pw_main_loop_quit(self->loop);
        }
    };
    pw_core_add_listener(core.get(), &core_listener, &core_events, &state);

    // First: every existing global, binding streams and clients as they arrive.
    state.pending = pw_core_sync(core.get(), PW_ID_CORE, 0);
    pw_main_loop_run(loop.get());
    const bool listed = state.completed && !state.failed;
    if (listed && state.bound_any) {
        // Second: the info events those binds asked for. There are none to
        // wait for when nothing was bound - a kRegistryOnly walk of a graph
        // with no relayed stream in it - and the round trip is the cost.
        state.completed = false;
        state.pending = pw_core_sync(core.get(), PW_ID_CORE, 0);
        pw_main_loop_run(loop.get());
    }

    std::vector<OutputStreamNode> out;
    if (listed) {
        out.reserve(state.streams.size());
        for (auto& stream : state.streams) {
            const auto found = state.client_pids.find(stream.client_id);
            StreamIdentity identity;
            if (stream.bound != nullptr) {
                identity = stream.bound->identity;
            }
            if (const auto client = state.clients.find(stream.client_id);
                client != state.clients.end()) {
                // The node's info first; the client's fills what it left empty.
                const StreamIdentity& theirs = client->second->identity;
                if (identity.icon_name.empty()) {
                    identity.icon_name = theirs.icon_name;
                }
                if (identity.binary.empty()) {
                    identity.binary = theirs.binary;
                }
                if (identity.app_id.empty()) {
                    identity.app_id = theirs.app_id;
                }
                if (identity.claimed_pid == 0) {
                    identity.claimed_pid = theirs.claimed_pid;
                }
            }
            const std::uint32_t pid = stream_owner_pid(
                found == state.client_pids.end() ? 0 : found->second, identity.claimed_pid,
                stream.relayed);
            out.push_back({.id = stream.id,
                           .target = std::move(stream.target),
                           .application = identity.application.empty()
                                              ? std::move(stream.application)
                                              : std::move(identity.application),
                           .pid = pid,
                           .icon_name = std::move(identity.icon_name),
                           .binary = std::move(identity.binary),
                           .app_id = std::move(identity.app_id)});
        }
    }
    for (auto& node : state.nodes) {
        spa_hook_remove(&node->listener);
        pw_proxy_destroy(node->proxy);
    }
    for (auto& entry : state.clients) {
        spa_hook_remove(&entry.second->listener);
        pw_proxy_destroy(entry.second->proxy);
    }
    spa_hook_remove(&core_listener);
    spa_hook_remove(&registry_listener);
    return out;
}

// What a human would call the application behind a stream node.
[[nodiscard]] inline std::string node_application_name(const spa_dict& props) {
    if (const char* name = spa_dict_lookup(&props, PW_KEY_APP_NAME);
        name != nullptr && name[0] != '\0') {
        return name;
    }
    return node_id(props);
}

// What to hand PW_KEY_TARGET_OBJECT to link to this node. object.serial is
// preferred over node.name for an application's stream: two instances of the
// same program share a node.name, and the serial is unique for the life of
// the graph. Falls back to node.name, which is what pw_stream_connect()
// documents and what the device paths in this backend already use.
[[nodiscard]] inline std::string node_target(const spa_dict& props) {
    if (const char* serial = spa_dict_lookup(&props, PW_KEY_OBJECT_SERIAL);
        serial != nullptr && serial[0] != '\0') {
        return serial;
    }
    return node_id(props);
}

// An Audio/Sink node with the properties only its *info* carries.
//
// What a registry listener is handed for a node is the global's property
// dictionary, and that is a subset - eleven keys on a real sink: names,
// class, serial, the owning client and device, priorities. Everything the
// session manager sets on the node afterwards, iec958.codecs among them,
// lives on the node's info and reaches a client only by binding the node
// and listening for its info event. pw-dump and wpctl inspect show the
// info; a registry walk never sees it, which is how a gate on
// iec958.codecs came to reject every sink on the first machine with one.
struct SinkInfo {
    std::uint32_t id = 0;      // registry global id
    std::string name;          // node.name, this backend's device id
    std::string description;   // node.description, else node.name
    std::string codecs;        // iec958.codecs, verbatim, or empty
};

// Every Audio/Sink in the graph with its info properties. Two round trips:
// the registry walk binds each sink as it appears, and a second sync waits
// for the info events those binds provoke. Returns an empty list when there
// is no session to ask.
[[nodiscard]] inline std::vector<SinkInfo> audio_sinks_with_info() {
    ensure_initialized();

    MainLoop loop{pw_main_loop_new(nullptr)};
    if (!loop) {
        return {};
    }
    Context context{pw_context_new(pw_main_loop_get_loop(loop.get()), nullptr, 0)};
    if (!context) {
        return {};
    }
    Core core{pw_context_connect(context.get(), nullptr, 0)};
    if (!core) {
        return {};
    }
    Registry registry{pw_core_get_registry(core.get(), PW_VERSION_REGISTRY, 0)};
    if (!registry) {
        return {};
    }

    struct Bound {
        pw_proxy* proxy = nullptr;
        spa_hook listener{};
        pw_node_events events{};
        SinkInfo info;
    };
    struct State {
        pw_registry* registry = nullptr;
        pw_main_loop* loop = nullptr;
        std::vector<std::unique_ptr<Bound>> bound;
        int pending = 0;
    } state{registry.get(), loop.get(), {}, 0};

    spa_hook registry_listener{};
    pw_registry_events registry_events{};
    registry_events.version = PW_VERSION_REGISTRY_EVENTS;
    registry_events.global = [](void* data, std::uint32_t id, std::uint32_t, const char* type,
                                std::uint32_t, const spa_dict* props) {
        auto* self = static_cast<State*>(data);
        if (type == nullptr || props == nullptr || std::string_view{type} != PW_TYPE_INTERFACE_Node) {
            return;
        }
        if (!is_audio_sink(*props)) {
            return;
        }
        auto* proxy = static_cast<pw_proxy*>(
            pw_registry_bind(self->registry, id, type, PW_VERSION_NODE, 0));
        if (proxy == nullptr) {
            return;
        }
        auto bound = std::make_unique<Bound>();
        bound->proxy = proxy;
        bound->info.id = id;
        bound->info.name = node_id(*props);
        bound->info.description = node_friendly_name(*props);
        bound->events.version = PW_VERSION_NODE_EVENTS;
        bound->events.info = [](void* bound_data, const pw_node_info* info) {
            auto* b = static_cast<Bound*>(bound_data);
            if (info == nullptr || info->props == nullptr) {
                return;
            }
            if (const char* codecs = spa_dict_lookup(info->props, "iec958.codecs");
                codecs != nullptr) {
                b->info.codecs = codecs;
            }
        };
        pw_proxy_add_object_listener(proxy, &bound->listener, &bound->events, bound.get());
        self->bound.push_back(std::move(bound));
    };
    pw_registry_add_listener(registry.get(), &registry_listener, &registry_events, &state);

    spa_hook core_listener{};
    pw_core_events core_events{};
    core_events.version = PW_VERSION_CORE_EVENTS;
    core_events.done = [](void* data, std::uint32_t id, int seq) {
        auto* self = static_cast<State*>(data);
        if (id == PW_ID_CORE && seq == self->pending) {
            pw_main_loop_quit(self->loop);
        }
    };
    pw_core_add_listener(core.get(), &core_listener, &core_events, &state);

    // First: every existing global, binding the sinks as they arrive.
    state.pending = pw_core_sync(core.get(), PW_ID_CORE, 0);
    pw_main_loop_run(loop.get());
    // Second: the info events those binds asked for.
    state.pending = pw_core_sync(core.get(), PW_ID_CORE, 0);
    pw_main_loop_run(loop.get());

    std::vector<SinkInfo> out;
    out.reserve(state.bound.size());
    for (auto& b : state.bound) {
        spa_hook_remove(&b->listener);
        pw_proxy_destroy(b->proxy);
        out.push_back(std::move(b->info));
    }
    spa_hook_remove(&core_listener);
    spa_hook_remove(&registry_listener);
    return out;
}

}  // namespace ac3::pipewire
