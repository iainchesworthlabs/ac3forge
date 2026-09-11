// The REST control surface, and the web UI it serves. See
// ../include/ac3forge/control.hpp and planning/esp32-device-ui.md.

#include "ac3forge/control.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>

#include "esp_http_server.h"

// The web UI's two files. CMakeLists.txt embeds them (EMBED_FILES) and they stay
// in flash. ESP-IDF names each symbol after the file's base name, which is why
// the files carry the component's name: a firmware that embeds an index.html of
// its own would otherwise have two definitions of one symbol.
extern const char ac3forge_ui_html_start[] asm("_binary_ac3forge_ui_html_start");
extern const char ac3forge_ui_html_end[] asm("_binary_ac3forge_ui_html_end");
extern const char ac3forge_ui_js_start[] asm("_binary_ac3forge_ui_js_start");
extern const char ac3forge_ui_js_end[] asm("_binary_ac3forge_ui_js_end");

namespace ac3forge {
namespace {

// The body of a small POST or PUT, as a string. Empty on a read error or an
// empty body - the two are the same to a handler that needs a location.
std::string read_body(httpd_req_t* req) {
    std::string body;
    if (req->content_len <= 0 || req->content_len > 2048) {
        return body;
    }
    body.resize(static_cast<std::size_t>(req->content_len));
    std::size_t got = 0;
    while (got < body.size()) {
        const int n = httpd_req_recv(req, body.data() + got, body.size() - got);
        if (n <= 0) {
            return std::string{};
        }
        got += static_cast<std::size_t>(n);
    }
    // Trim the whitespace a shell puts on the end of `-d`.
    while (!body.empty() && (body.back() == '\n' || body.back() == '\r' || body.back() == ' ')) {
        body.pop_back();
    }
    return body;
}

// JSON string escaping for the two characters that can break a document; the
// strings here are URLs, paths, layouts and this code's own constants.
void append_json_string(std::string& out, std::string_view s) {
    out += '"';
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        if (static_cast<unsigned char>(c) < 0x20) {
            continue;
        }
        out += c;
    }
    out += '"';
}

void append_key(std::string& out, const char* key) {
    if (out.back() != '{') {
        out += ',';
    }
    append_json_string(out, key);
    out += ':';
}

void append_number(std::string& out, const char* key, unsigned long long value) {
    append_key(out, key);
    out += std::to_string(value);
}

void append_bool(std::string& out, const char* key, bool value) {
    append_key(out, key);
    out += value ? "true" : "false";
}

esp_err_t send_text(httpd_req_t* req, const char* status, const char* text) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
}

// A file of the web UI, sent from where the linker put it: httpd_resp_send
// builds the headers in a small buffer of its own and sends the body from this
// pointer, so no part of the file is copied to the heap. no-cache has a browser
// ask again rather than keep a script from before a firmware update. The
// policy lets the page run script from the device alone and style from its own
// <style> element, and allows the empty data: icon the page declares so that a
// browser does not ask for /favicon.ico.
esp_err_t send_file(httpd_req_t* req, const char* type, const char* begin, const char* end) {
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Content-Security-Policy",
                       "default-src 'self'; style-src 'self' 'unsafe-inline'; img-src data:; "
                       "frame-ancestors 'none'");
    return httpd_resp_send(req, begin, static_cast<ssize_t>(end - begin));
}

}  // namespace

struct Control::Impl {
    ControlHandlers handlers;
    httpd_handle_t server = nullptr;

    static Impl* self(httpd_req_t* req) { return static_cast<Impl*>(req->user_ctx); }

    // The web UI: a page and its script, which read /status and drive the
    // routes below like any other client.
    static esp_err_t on_page(httpd_req_t* req) {
        return send_file(req, "text/html; charset=utf-8", ac3forge_ui_html_start,
                         ac3forge_ui_html_end);
    }

    static esp_err_t on_script(httpd_req_t* req) {
        return send_file(req, "text/javascript; charset=utf-8", ac3forge_ui_js_start,
                         ac3forge_ui_js_end);
    }

    static esp_err_t on_api(httpd_req_t* req) {
        return send_text(req, "200 OK",
                         "ac3forge player\n"
                         "GET  /              a web page that shows and drives the player\n"
                         "GET  /api           this list\n"
                         "GET  /status        what is playing, as JSON\n"
                         "POST /play          body: a URL or path to play\n"
                         "POST /stop\n"
                         "POST /volume        body: 0.0 to 1.0\n"
                         "GET  /layout        the output layout\n"
                         "PUT  /layout        body: a name (5.1.4) or a speaker list; next play\n");
    }

    static esp_err_t on_status(httpd_req_t* req) {
        auto& h = self(req)->handlers;
        std::string out = "{";
        append_key(out, "state");
        append_json_string(out, h.state ? h.state() : "unknown");
        if (h.location) {
            append_key(out, "location");
            append_json_string(out, h.location());
        }
        if (h.source_name) {
            append_key(out, "source");
            append_json_string(out, h.source_name());
        }
        if (h.sink_name) {
            append_key(out, "sink");
            append_json_string(out, h.sink_name());
        }
        if (h.sink_slots) {
            append_number(out, "sink_slots", static_cast<unsigned long long>(h.sink_slots()));
        }
        if (h.layout) {
            append_key(out, "layout");
            append_json_string(out, h.layout());
        }
        if (h.volume) {
            append_key(out, "volume");
            std::array<char, 16> buf{};
            std::snprintf(buf.data(), buf.size(), "%.3f", static_cast<double>(h.volume()));
            out += buf.data();
        }
        if (h.stream) {
            append_key(out, "stream");
            if (const auto info = h.stream()) {
                out += '{';
                append_key(out, "codec");
                append_json_string(out, info->eac3 ? "E-AC-3" : "AC-3");
                append_number(out, "acmod", static_cast<unsigned long long>(info->acmod));
                append_number(out, "channels", static_cast<unsigned long long>(info->channels));
                append_number(out, "substreams", static_cast<unsigned long long>(info->substreams));
                append_key(out, "dialnorm");
                out += '-';
                out += std::to_string(info->dialnorm);
                append_bool(out, "objects", info->objects);
                append_bool(out, "objects_rendered", info->objects_rendered);
                append_number(out, "slots", static_cast<unsigned long long>(info->slots));
                // How this play serves the layout (planning/esp32-device-ui.md,
                // "The output layout").
                append_key(out, "layout");
                append_json_string(out, info->layout.data());
                append_key(out, "render");
                append_json_string(out, info->render);
                append_key(out, "coded");
                append_json_string(out, info->coded.data());
                append_key(out, "silent");
                append_json_string(out, info->silent.data());
                out += '}';
            } else {
                out += "null";
            }
        }
        if (h.stats) {
            const PlayerStats s = h.stats();
            append_number(out, "frames", s.frames_played);
            append_number(out, "held", s.frames_held);
            append_number(out, "us_per_frame",
                          s.frames_played > 0 ? s.decode_us / s.frames_played : 0);
            append_number(out, "worst_frame_us", s.worst_frame_us);
            append_number(out, "render_us_per_frame",
                          s.frames_played > 0 ? s.render_us / s.frames_played : 0);
            append_number(out, "sink_us_per_frame",
                          s.frames_played > 0 ? s.sink_us / s.frames_played : 0);
            append_number(out, "realtime_permille",
                          s.frames_played > 0 ? (s.decode_us * 1000) / (32000ULL * s.frames_played)
                                              : 0);
            append_number(out, "resync_bytes", s.resync_bytes);
            append_number(out, "fetched_bytes", s.fetched_bytes);
            append_key(out, "ring_low");
            out += s.ring_low_valid ? std::to_string(s.ring_low_water) : "null";
            append_number(out, "passes", s.passes);
            append_number(out, "layout_mismatches", s.layout_mismatches);
            append_bool(out, "finished", s.finished);
            append_bool(out, "failed", s.failed);
            append_key(out, "why");
            append_json_string(out, s.failure);
            append_number(out, "error", static_cast<unsigned long long>(s.error));
        }
        out += "}\n";
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, out.c_str(), static_cast<ssize_t>(out.size()));
    }

    static esp_err_t on_play(httpd_req_t* req) {
        auto& h = self(req)->handlers;
        const std::string location = read_body(req);
        if (location.empty()) {
            return send_text(req, "400 Bad Request", "POST /play wants the location as the body\n");
        }
        if (!h.play || !h.play(location)) {
            return send_text(req, "409 Conflict", "this source cannot play that\n");
        }
        // Accepted, not yet playing: the owner opens the source on its own
        // task, and GET /status says how that went.
        return send_text(req, "202 Accepted", "accepted\n");
    }

    static esp_err_t on_stop(httpd_req_t* req) {
        auto& h = self(req)->handlers;
        if (h.stop) {
            h.stop();
        }
        return send_text(req, "200 OK", "stopped\n");
    }

    static esp_err_t on_volume(httpd_req_t* req) {
        auto& h = self(req)->handlers;
        const std::string body = read_body(req);
        char* end = nullptr;
        const double value = body.empty() ? -1.0 : std::strtod(body.c_str(), &end);
        if (body.empty() || end == body.c_str() || value < 0.0 || value > 1.0) {
            return send_text(req, "400 Bad Request", "POST /volume wants a number 0.0 to 1.0\n");
        }
        if (!h.set_volume || !h.set_volume(static_cast<float>(value))) {
            return send_text(req, "409 Conflict", "this sink has no volume to set\n");
        }
        return send_text(req, "200 OK", "ok\n");
    }

    static esp_err_t on_layout_get(httpd_req_t* req) {
        auto& h = self(req)->handlers;
        if (!h.layout) {
            return send_text(req, "404 Not Found", "this player has no layout to report\n");
        }
        std::string text = h.layout();
        text += '\n';
        return send_text(req, "200 OK", text.c_str());
    }

    static esp_err_t on_layout_put(httpd_req_t* req) {
        auto& h = self(req)->handlers;
        const std::string body = read_body(req);
        if (body.empty()) {
            return send_text(req, "400 Bad Request",
                             "PUT /layout wants a name (5.1.4) or a speaker list (L,R,C,LFE,Ls,Rs)\n");
        }
        if (!h.set_layout) {
            return send_text(req, "409 Conflict", "this player's layout is fixed\n");
        }
        if (!h.set_layout(body)) {
            return send_text(req, "409 Conflict",
                             "not a layout this player can play: check the name or the list, and "
                             "that it has no more slots than the sink\n");
        }
        return send_text(req, "200 OK", "ok; takes effect at the next play\n");
    }
};

Control::~Control() { stop(); }

bool Control::start(const ControlHandlers& handlers, std::uint16_t port, std::size_t stack_bytes) {
    if (impl_ != nullptr) {
        return true;
    }
    impl_ = new Impl{};
    impl_->handlers = handlers;

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = &Impl::on_page, .user_ctx = impl_},
        {.uri = "/ui.js", .method = HTTP_GET, .handler = &Impl::on_script, .user_ctx = impl_},
        {.uri = "/api", .method = HTTP_GET, .handler = &Impl::on_api, .user_ctx = impl_},
        {.uri = "/status", .method = HTTP_GET, .handler = &Impl::on_status, .user_ctx = impl_},
        {.uri = "/play", .method = HTTP_POST, .handler = &Impl::on_play, .user_ctx = impl_},
        {.uri = "/stop", .method = HTTP_POST, .handler = &Impl::on_stop, .user_ctx = impl_},
        {.uri = "/volume", .method = HTTP_POST, .handler = &Impl::on_volume, .user_ctx = impl_},
        {.uri = "/layout", .method = HTTP_GET, .handler = &Impl::on_layout_get, .user_ctx = impl_},
        {.uri = "/layout", .method = HTTP_PUT, .handler = &Impl::on_layout_put, .user_ctx = impl_},
    };

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    // The routes above and one or two clients at a time are the whole job;
    // the defaults size the server for more than that and this part has less.
    // Each handler slot is a pointer the server allocates when it starts, so
    // there is one per route and none spare.
    config.max_uri_handlers = static_cast<decltype(config.max_uri_handlers)>(std::size(routes));
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;
    // The owner's callbacks run on this task too: see kDefaultStackBytes.
    config.stack_size = stack_bytes;
    if (httpd_start(&impl_->server, &config) != ESP_OK) {
        std::printf("control: could not start the HTTP server on port %u\n",
                    static_cast<unsigned>(port));
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    for (const auto& route : routes) {
        if (httpd_register_uri_handler(impl_->server, &route) != ESP_OK) {
            std::printf("control: could not register %s\n", route.uri);
        }
    }
    std::printf("control: http on port %u - a web page at /, the REST routes listed at /api\n",
                static_cast<unsigned>(port));
    return true;
}

void Control::stop() {
    if (impl_ == nullptr) {
        return;
    }
    if (impl_->server != nullptr) {
        httpd_stop(impl_->server);
    }
    delete impl_;
    impl_ = nullptr;
}

}  // namespace ac3forge
