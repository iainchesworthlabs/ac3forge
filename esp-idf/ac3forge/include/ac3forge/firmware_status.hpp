#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// GET /firmware's body (planning/esp32-ota.md, "Routes"): what the board runs,
// what it would go back to, how an update or a trial is going and how the last
// one ended, and the facts a tool checks before it sends an image - the slot
// size, the flash size and the partition table as the board has it.
//
// Plain values, which firmware.cpp fills in from ESP-IDF, rendered here so the
// shape can be tested on a laptop (tests/io/test_firmware_status.cpp) and read
// by tools/hearth/ota.py and the device page without either guessing at it.
// Every key is always present; a part that does not apply is null.

namespace ac3forge {

// One application slot.
struct FirmwareSlot {
    std::string label;  // "ota_0" or "ota_1"
    // "valid", "trial" (the running image, before it has accepted itself),
    // "new" (written, not yet booted), "invalid", "aborted", "undefined" (a
    // USB flash that has not booted yet), or "empty" (no image in the slot).
    std::string state;
    std::string version;
    std::string project;
    std::string idf_version;
    std::string elf_sha256;    // hex, esp_app_desc_t's app_elf_sha256
    std::string image_sha256;  // hex, the SHA-256 the build appended to the image
    // Whether the image checked out the last time the board read it through;
    // nothing before the first check.
    std::optional<bool> intact;
};

// The trial of an image an update wrote, while it runs.
struct FirmwareTrial {
    std::uint32_t healthy_for_ms = 0;
    std::uint32_t hold_ms = 0;
    std::uint32_t remaining_ms = 0;
    // What has not held yet, in the owner's words ("a network address").
    std::vector<std::string> waiting_for;
};

// An upload in progress.
struct FirmwareUpload {
    std::size_t received = 0;
    std::size_t total = 0;
    std::string stage;  // "waiting", "erasing", "writing", "checking" or "restarting"
};

// How the last update this board knows of ended.
struct FirmwareLastUpdate {
    std::string version;
    // "on trial", "accepted", "rolled back", "rollback requested", "refused"
    // (the image or the request was wrong) or "failed" (the board was).
    std::string result;
    std::string reason;
};

struct FirmwarePartition {
    std::string label;
    unsigned type = 0;
    unsigned subtype = 0;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
};

struct FirmwareStatus {
    std::string mode = "normal";  // "normal" or "flash"
    std::optional<FirmwareSlot> running;
    std::optional<FirmwareSlot> other;  // nothing on a board with one app slot
    std::optional<FirmwareTrial> trial;
    std::optional<FirmwareUpload> upload;
    std::optional<FirmwareLastUpdate> last_update;
    // Where the board's network comes from: "stored" in NVS, "built-in" to the
    // image alone, "wired" (a network that needs nothing stored), or "none".
    std::string network = "none";
    std::size_t slot_bytes = 0;
    std::size_t flash_bytes = 0;
    std::vector<FirmwarePartition> partitions;
    std::string bootloader_version;
};

namespace detail {

// JSON string escaping: the two characters that end or escape a string, and
// every control character as \u00XX, since a version or a reason is text a
// build or a failure chose.
inline void append_json_text(std::string& out, std::string_view text) {
    out += '"';
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (byte < 0x20) {
            constexpr std::string_view kHex = "0123456789abcdef";
            out += "\\u00";
            out += kHex[static_cast<std::size_t>(byte >> 4)];
            out += kHex[static_cast<std::size_t>(byte & 0x0F)];
        } else {
            out += c;
        }
    }
    out += '"';
}

class JsonObject {
   public:
    explicit JsonObject(std::string& out) : out_(out) { out_ += '{'; }
    ~JsonObject() { out_ += '}'; }
    JsonObject(const JsonObject&) = delete;
    JsonObject& operator=(const JsonObject&) = delete;

    std::string& key(std::string_view name) {
        if (!first_) {
            out_ += ',';
        }
        first_ = false;
        append_json_text(out_, name);
        out_ += ':';
        return out_;
    }
    void text(std::string_view name, std::string_view value) { append_json_text(key(name), value); }
    void number(std::string_view name, std::uint64_t value) { key(name) += std::to_string(value); }
    void null(std::string_view name) { key(name) += "null"; }

   private:
    std::string& out_;
    bool first_ = true;
};

inline void append_slot(std::string& out, const FirmwareSlot& slot) {
    JsonObject object(out);
    object.text("label", slot.label);
    object.text("state", slot.state);
    object.text("version", slot.version);
    object.text("project", slot.project);
    object.text("idf_version", slot.idf_version);
    object.text("elf_sha256", slot.elf_sha256);
    object.text("image_sha256", slot.image_sha256);
    if (slot.intact) {
        object.key("intact") += *slot.intact ? "true" : "false";
    } else {
        object.null("intact");
    }
}

}  // namespace detail

[[nodiscard]] inline std::string render_firmware_status(const FirmwareStatus& status) {
    std::string out;
    {
        detail::JsonObject object(out);
        object.text("mode", status.mode);
        if (status.running) {
            detail::append_slot(object.key("running"), *status.running);
        } else {
            object.null("running");
        }
        if (status.other) {
            detail::append_slot(object.key("other"), *status.other);
        } else {
            object.null("other");
        }
        if (status.trial) {
            detail::JsonObject trial(object.key("trial"));
            trial.number("healthy_for_ms", status.trial->healthy_for_ms);
            trial.number("hold_ms", status.trial->hold_ms);
            trial.number("remaining_ms", status.trial->remaining_ms);
            std::string& list = trial.key("waiting_for");
            list += '[';
            for (std::size_t i = 0; i < status.trial->waiting_for.size(); ++i) {
                if (i > 0) {
                    list += ',';
                }
                detail::append_json_text(list, status.trial->waiting_for[i]);
            }
            list += ']';
        } else {
            object.null("trial");
        }
        if (status.upload) {
            detail::JsonObject upload(object.key("upload"));
            upload.number("received", status.upload->received);
            upload.number("total", status.upload->total);
            upload.text("stage", status.upload->stage);
        } else {
            object.null("upload");
        }
        if (status.last_update) {
            detail::JsonObject last(object.key("last_update"));
            last.text("version", status.last_update->version);
            last.text("result", status.last_update->result);
            last.text("reason", status.last_update->reason);
        } else {
            object.null("last_update");
        }
        object.text("network", status.network);
        object.number("slot_bytes", status.slot_bytes);
        object.number("flash_bytes", status.flash_bytes);
        std::string& list = object.key("partitions");
        list += '[';
        for (std::size_t i = 0; i < status.partitions.size(); ++i) {
            if (i > 0) {
                list += ',';
            }
            const FirmwarePartition& p = status.partitions[i];
            detail::JsonObject part(list);
            part.text("label", p.label);
            part.number("type", p.type);
            part.number("subtype", p.subtype);
            part.number("offset", p.offset);
            part.number("size", p.size);
        }
        list += ']';
        object.text("bootloader_version", status.bootloader_version);
    }
    out += '\n';
    return out;
}

}  // namespace ac3forge
