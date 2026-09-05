#include <xcb/xcb.h>
#include <xcb/xproto.h>

#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "x11_foreground.hpp"

// The real X11WindowReader (x11_foreground.hpp), over libxcb. Compiled only
// when the build found libxcb (apps/crucible/CMakeLists.txt); the no-xcb twin
// stands in otherwise.
//
// libxcb rather than libX11: it is thread-safe without XInitThreads, it
// returns protocol errors (BadWindow, when the active window vanished between
// two reads) as per-request replies rather than through a process-global
// handler whose default exits the process, and it is already on every
// machine that runs Qt's xcb platform plugin, so the package's dependencies
// gain nothing new.
//
// What is read is EWMH's: _NET_SUPPORTING_WM_CHECK on the root says a
// compliant window manager is running (Xephyr on its own maintains none of
// these properties; openbox or any current desktop's manager does),
// _NET_ACTIVE_WINDOW names the window with focus, _NET_WM_STATE on that
// window lists its states, and _NET_WM_PID is the client's own claim about
// its process, which EWMH says is meaningful only together with
// WM_CLIENT_MACHINE naming this host. Two round trips on a local socket,
// well under a millisecond, every 500 ms, on the engine's session thread.
//
// Known gaps, stated rather than hidden. A client in a pid namespace
// (Flatpak, Snap) reports a namespace pid, which matches nothing here: a
// false negative, the same as no detection at all, and the XRes extension
// (xcb-res, XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID) is the named follow-up
// for it. An override-redirect full-screen window (SDL 1, some Wine modes)
// never becomes _NET_ACTIVE_WINDOW and is not seen. And a window manager that
// exits mid-session leaves _NET_ACTIVE_WINDOW stale or absent, which reads as
// "no window" rather than as a lost display, so the rule is quietly off until
// the manager is back.

namespace ac3::crucible {

namespace {

constexpr const char* kNoServer =
    "DISPLAY is set but no X server answered on it; the full-screen rule is off";
constexpr const char* kNoWindowManager =
    "no EWMH window manager is running on this X display, so the active window cannot be "
    "read; the full-screen rule is off";

// libxcb hands back malloc'd replies and expects free().
struct FreeReply {
    void operator()(void* reply) const { std::free(reply); }
};
template <class Reply>
using Owned = std::unique_ptr<Reply, FreeReply>;

// The reply, or null for a protocol error (the window went away) and for a
// dead connection alike; the caller tells those apart with
// xcb_connection_has_error.
[[nodiscard]] Owned<xcb_get_property_reply_t> property_reply(xcb_connection_t* conn,
                                                            xcb_get_property_cookie_t cookie) {
    xcb_generic_error_t* error = nullptr;
    Owned<xcb_get_property_reply_t> reply{xcb_get_property_reply(conn, cookie, &error)};
    std::free(error);
    return reply;
}

// The index-th 32-bit word of a reply's value, when the reply has that
// format and that many. memcpy rather than a pointer cast: the payload's
// alignment is libxcb's business, and -Wcast-align is on.
[[nodiscard]] std::optional<std::uint32_t> word_at(const xcb_get_property_reply_t* reply,
                                                   std::uint32_t index) {
    if (reply == nullptr || reply->format != 32 || reply->value_len <= index) {
        return std::nullopt;
    }
    std::uint32_t word = 0;
    const auto* bytes = static_cast<const std::uint8_t*>(xcb_get_property_value(reply));
    std::memcpy(&word, bytes + static_cast<std::size_t>(index) * sizeof(word), sizeof(word));
    return word;
}

// A reply's value as text, up to its first NUL if it carries one.
[[nodiscard]] std::string_view text_of(const xcb_get_property_reply_t* reply) {
    if (reply == nullptr || reply->format != 8) {
        return {};
    }
    const int length = xcb_get_property_value_length(reply);
    if (length <= 0) {
        return {};
    }
    const std::string_view text{static_cast<const char*>(xcb_get_property_value(reply)),
                                static_cast<std::size_t>(length)};
    return text.substr(0, text.find('\0'));
}

// The host part of a name, before any domain: WM_CLIENT_MACHINE is whatever
// the client's gethostname() said, which may or may not be qualified.
[[nodiscard]] std::string_view short_host(std::string_view host) {
    return host.substr(0, host.find('.'));
}

[[nodiscard]] std::string this_host() {
    std::array<char, 256> buffer{};
    if (::gethostname(buffer.data(), buffer.size() - 1) != 0) {
        return {};
    }
    return std::string{buffer.data()};
}

class XcbWindowReader final : public X11WindowReader {
public:
    XcbWindowReader() = default;
    ~XcbWindowReader() override { disconnect(); }
    XcbWindowReader(const XcbWindowReader&) = delete;
    XcbWindowReader& operator=(const XcbWindowReader&) = delete;
    XcbWindowReader(XcbWindowReader&&) = delete;
    XcbWindowReader& operator=(XcbWindowReader&&) = delete;

    const char* connect() override;
    std::optional<X11ActiveWindow> active_window() override;

private:
    void disconnect() {
        if (conn_ != nullptr) {
            xcb_disconnect(conn_);
            conn_ = nullptr;
        }
    }

    // A property request from the start of the value, `longs` 32-bit units
    // of it; the reply is awaited separately so several can be in flight.
    [[nodiscard]] xcb_get_property_cookie_t ask(xcb_window_t window, xcb_atom_t property,
                                                xcb_atom_t type, std::uint32_t longs) const {
        return xcb_get_property(conn_, 0, window, property, type, 0, longs);
    }

    xcb_connection_t* conn_ = nullptr;
    xcb_window_t root_ = 0;
    xcb_atom_t wm_check_ = 0;
    xcb_atom_t active_ = 0;
    xcb_atom_t state_ = 0;
    xcb_atom_t fullscreen_ = 0;
    xcb_atom_t pid_ = 0;
    xcb_atom_t client_machine_ = 0;
    std::string host_;
};

const char* XcbWindowReader::connect() {
    disconnect();
    int screen = 0;
    conn_ = xcb_connect(nullptr, &screen);  // nullptr: $DISPLAY
    if (xcb_connection_has_error(conn_) != 0) {
        disconnect();
        return kNoServer;
    }

    // The root of the screen DISPLAY names.
    xcb_screen_iterator_t screens = xcb_setup_roots_iterator(xcb_get_setup(conn_));
    for (int i = 0; i < screen && screens.rem > 0; ++i) {
        xcb_screen_next(&screens);
    }
    if (screens.rem <= 0 || screens.data == nullptr) {
        disconnect();
        return kNoServer;
    }
    root_ = screens.data->root;

    // Six atoms in one round trip: every request first, then every reply.
    constexpr std::array<const char*, 6> names = {"_NET_SUPPORTING_WM_CHECK", "_NET_ACTIVE_WINDOW",
                                                  "_NET_WM_STATE",  "_NET_WM_STATE_FULLSCREEN",
                                                  "_NET_WM_PID",    "WM_CLIENT_MACHINE"};
    std::array<xcb_intern_atom_cookie_t, names.size()> cookies{};
    for (std::size_t i = 0; i < names.size(); ++i) {
        cookies[i] = xcb_intern_atom(conn_, 0, static_cast<std::uint16_t>(std::strlen(names[i])),
                                     names[i]);
    }
    std::array<xcb_atom_t, names.size()> atoms{};
    for (std::size_t i = 0; i < names.size(); ++i) {
        xcb_generic_error_t* error = nullptr;
        const Owned<xcb_intern_atom_reply_t> reply{
            xcb_intern_atom_reply(conn_, cookies[i], &error)};
        std::free(error);
        if (!reply) {
            disconnect();
            return kNoServer;
        }
        atoms[i] = reply->atom;
    }
    wm_check_ = atoms[0];
    active_ = atoms[1];
    state_ = atoms[2];
    fullscreen_ = atoms[3];
    pid_ = atoms[4];
    client_machine_ = atoms[5];
    host_ = this_host();

    // An EWMH window manager announces itself on the root; without one there
    // is no _NET_ACTIVE_WINDOW to read, and asking would report "no window"
    // for ever without a word about why.
    const auto check =
        property_reply(conn_, ask(root_, wm_check_, static_cast<xcb_atom_t>(XCB_ATOM_WINDOW), 1));
    if (!word_at(check.get(), 0)) {
        const bool lost = xcb_connection_has_error(conn_) != 0;
        disconnect();
        return lost ? kNoServer : kNoWindowManager;
    }
    return nullptr;
}

std::optional<X11ActiveWindow> XcbWindowReader::active_window() {
    if (conn_ == nullptr || xcb_connection_has_error(conn_) != 0) {
        return std::nullopt;
    }
    const auto active =
        property_reply(conn_, ask(root_, active_, static_cast<xcb_atom_t>(XCB_ATOM_WINDOW), 1));
    if (xcb_connection_has_error(conn_) != 0) {
        return std::nullopt;
    }
    const std::optional<std::uint32_t> xid = word_at(active.get(), 0);
    if (!xid || *xid == 0) {
        return X11ActiveWindow{.window = 0, .fullscreen = false, .pid = std::nullopt};
    }
    const xcb_window_t window = *xid;

    // Three requests on the window before any reply is awaited: one round
    // trip for the lot.
    const auto state_cookie = ask(window, state_, static_cast<xcb_atom_t>(XCB_ATOM_ATOM), 32);
    const auto pid_cookie = ask(window, pid_, static_cast<xcb_atom_t>(XCB_ATOM_CARDINAL), 1);
    const auto machine_cookie =
        ask(window, client_machine_, static_cast<xcb_atom_t>(XCB_GET_PROPERTY_TYPE_ANY), 64);
    const auto state = property_reply(conn_, state_cookie);
    const auto pid = property_reply(conn_, pid_cookie);
    const auto machine = property_reply(conn_, machine_cookie);
    if (xcb_connection_has_error(conn_) != 0) {
        return std::nullopt;
    }
    if (!state || !pid || !machine) {
        // BadWindow: it vanished between the two round trips. Nothing is in
        // front until the next read says otherwise.
        return X11ActiveWindow{.window = 0, .fullscreen = false, .pid = std::nullopt};
    }

    X11ActiveWindow out{.window = window, .fullscreen = false, .pid = word_at(pid.get(), 0)};
    for (std::uint32_t i = 0;; ++i) {
        const std::optional<std::uint32_t> atom = word_at(state.get(), i);
        if (!atom) {
            break;
        }
        if (*atom == fullscreen_) {
            out.fullscreen = true;
            break;
        }
    }
    // EWMH: _NET_WM_PID is meaningful only with WM_CLIENT_MACHINE naming this
    // host. A pid from another machine (an ssh -X client) matches nothing here
    // and could collide with a local one, so it is dropped.
    // Only when this host has a name to compare: a failed gethostname() would
    // otherwise drop every pid while support() still said the rule was on,
    // which is the silent-wrong-answer foreground.hpp warns against.
    if (out.pid && !host_.empty() && machine->value_len > 0 &&
        short_host(text_of(machine.get())) != short_host(host_)) {
        out.pid = std::nullopt;
    }
    return out;
}

}  // namespace

std::unique_ptr<X11WindowReader> make_x11_window_reader() {
    return std::make_unique<XcbWindowReader>();
}

}  // namespace ac3::crucible
