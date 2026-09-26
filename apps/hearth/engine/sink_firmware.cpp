#include "sink_firmware.hpp"

#include <httplib.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <span>
#include <utility>

#include <fmt/format.h>

#include "ac3/sendspin/base64.hpp"
#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/json.hpp"

namespace ac3::hearth {

namespace {

namespace json = ac3::sendspin::json;

// GET /firmware is about 1.3 KB, so a few hundred values; the cap only keeps a
// hostile answer from sizing the token store.
constexpr std::size_t kMaxJsonTokens = 4096;

// esp_image_format.c's process_checksum: 0xEF XORed with every byte of every
// segment's data.
constexpr std::uint8_t kChecksumSeed = 0xEF;
constexpr std::size_t kDigestBytes = 32;

// ESP-IDF's targets and their esp_chip_id_t, the image header's chip field.
struct TargetChip {
    std::string_view target;
    std::uint16_t chip_id;
};
constexpr std::array<TargetChip, 10> kTargets{{
    {"esp32", 0x0000},
    {"esp32s2", 0x0002},
    {"esp32c3", 0x0005},
    {"esp32s3", 0x0009},
    {"esp32c2", 0x000C},
    {"esp32c6", 0x000D},
    {"esp32h2", 0x0010},
    {"esp32p4", 0x0012},
    {"esp32c61", 0x0014},
    {"esp32c5", 0x0017},
}};

[[nodiscard]] std::optional<std::uint16_t> chip_id_of(std::string_view target) {
    for (const TargetChip& entry : kTargets) {
        if (entry.target == target) {
            return entry.chip_id;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::string text_at(const json::Value& value, std::string_view key) {
    return value[key].as_string().value_or(std::string());
}

[[nodiscard]] std::uint64_t number_at(const json::Value& value, std::string_view key) {
    const std::optional<std::int64_t> number = value[key].as_int();
    return number && *number > 0 ? static_cast<std::uint64_t>(*number) : 0;
}

[[nodiscard]] std::optional<ac3forge::FirmwareSlot> slot_from(const json::Value& value) {
    if (!value.is_object()) {
        return std::nullopt;
    }
    ac3forge::FirmwareSlot slot;
    slot.label = text_at(value, "label");
    slot.state = text_at(value, "state");
    slot.version = text_at(value, "version");
    slot.project = text_at(value, "project");
    slot.idf_version = text_at(value, "idf_version");
    slot.elf_sha256 = text_at(value, "elf_sha256");
    slot.image_sha256 = text_at(value, "image_sha256");
    slot.intact = value["intact"].as_bool();
    return slot;
}

[[nodiscard]] std::string hex(std::span<const std::uint8_t> bytes) {
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::uint8_t byte : bytes) {
        out += fmt::format("{:02x}", byte);
    }
    return out;
}

[[nodiscard]] std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c); });
    return out;
}

[[nodiscard]] std::string grouped(std::uint64_t value) { return grouped_number(value); }

// esp_image_flash_size_t for a flash size in bytes: 2 is 4 MB, 4 is 16 MB.
[[nodiscard]] std::optional<std::uint8_t> flash_code(std::uint64_t bytes) {
    for (std::uint8_t code = 0; code <= 7; ++code) {
        if (bytes == (std::uint64_t{1} << code) * 1024 * 1024) {
            return code;
        }
    }
    return std::nullopt;
}

// GET /hardware's "revision", "M.m", as M * 100 + m.
[[nodiscard]] std::optional<std::uint16_t> parse_revision(std::string_view text) {
    if (!text.empty() && text.front() == 'v') {
        text.remove_prefix(1);
    }
    const std::size_t dot = text.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == text.size()) {
        return std::nullopt;
    }
    unsigned major = 0;
    unsigned minor = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (i == dot) {
            continue;
        }
        const char c = text[i];
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        unsigned& part = i < dot ? major : minor;
        part = part * 10 + static_cast<unsigned>(c - '0');
        if (part > 655) {
            return std::nullopt;
        }
    }
    return static_cast<std::uint16_t>(major * 100 + minor);
}

[[nodiscard]] bool sha256_of(std::span<const std::uint8_t> bytes, std::array<std::uint8_t, 32>& out) {
    return ac3::sendspin::crypto::sha256({bytes}, out);
}

[[nodiscard]] bool is_new_image(const ac3forge::FirmwareSlot* running, const FirmwareFile& file,
                                std::string_view slot) {
    return running != nullptr && lower(running->elf_sha256) == file.elf_sha256 &&
           (slot.empty() || running->label == slot);
}

[[nodiscard]] std::string updated_text(const ac3forge::FirmwareSlot& running, const FirmwareFile& file) {
    std::string text = fmt::format("updated: runs {} from {}, accepted", running.version, running.label);
    if (running.intact == false) {
        return text + "; but the board's own check of the running slot does not check out";
    }
    const std::string reported = lower(running.image_sha256);
    if (reported.empty()) {
        return text + "; the board has not reported the image's SHA-256 yet";
    }
    if (reported == file.image_sha256) {
        return text + "; the image's SHA-256 on the board matches the file's";
    }
    return text + fmt::format("; but the board reports the image's SHA-256 as {}, and the file's is {}", reported,
                              file.image_sha256);
}

[[nodiscard]] bool same_last_update(const std::optional<ac3forge::FirmwareLastUpdate>& a,
                                    const std::optional<ac3forge::FirmwareLastUpdate>& b) {
    if (a.has_value() != b.has_value()) {
        return false;
    }
    return !a || (a->version == b->version && a->result == b->result && a->reason == b->reason);
}

[[nodiscard]] std::string http_hint(int status) {
    if (status == 403) {
        return " (name the board by its IP address or its .local name)";
    }
    if (status == 404) {
        return " (the image it runs takes no updates over the network)";
    }
    return {};
}

// A request's Host header: the board takes its own address or name there
// (firmware_image.hpp's host_is_the_board), and an IPv6 address only in
// brackets, which cpp-httplib would not add.
[[nodiscard]] std::string host_header(std::string_view host, std::uint16_t port) {
    std::string out = host.find(':') != std::string_view::npos ? fmt::format("[{}]", host) : std::string(host);
    if (port != 80) {
        out += fmt::format(":{}", port);
    }
    return out;
}

}  // namespace

std::string grouped_number(std::uint64_t value) {
    std::string digits = std::to_string(value);
    for (std::ptrdiff_t at = static_cast<std::ptrdiff_t>(digits.size()) - 3; at > 0; at -= 3) {
        digits.insert(static_cast<std::size_t>(at), 1, ',');
    }
    return digits;
}

std::string trial_words(const ac3forge::FirmwareTrial& trial) {
    std::string text = fmt::format("healthy for {}s of {}s", trial.healthy_for_ms / 1000, trial.hold_ms / 1000);
    if (!trial.waiting_for.empty()) {
        text += ", waiting for ";
        for (std::size_t i = 0; i < trial.waiting_for.size(); ++i) {
            text += (i > 0 ? ", " : "") + trial.waiting_for[i];
        }
    }
    return text + fmt::format(" ({}s left)", trial.remaining_ms / 1000);
}

std::optional<SinkHardware> parse_sink_hardware(std::string_view text) {
    std::vector<json::Token> tokens;
    json::Document document;
    if (!document.parse(text, tokens, kMaxJsonTokens)) {
        return std::nullopt;
    }
    const json::Value root = document.root();
    if (!root.is_object() || !root["target"].is_string()) {
        return std::nullopt;
    }
    return SinkHardware{
        .target = text_at(root, "target"),
        .chip = text_at(root, "chip"),
        .revision = text_at(root, "revision"),
        .project = text_at(root, "project"),
        .version = text_at(root, "version"),
    };
}

std::optional<ac3forge::FirmwareStatus> parse_firmware_status(std::string_view text) {
    std::vector<json::Token> tokens;
    json::Document document;
    if (!document.parse(text, tokens, kMaxJsonTokens)) {
        return std::nullopt;
    }
    const json::Value root = document.root();
    if (!root.is_object() || !root["mode"].is_string()) {
        return std::nullopt;
    }
    ac3forge::FirmwareStatus status;
    status.mode = text_at(root, "mode");
    status.running = slot_from(root["running"]);
    status.other = slot_from(root["other"]);
    if (const json::Value value = root["trial"]; value.is_object()) {
        ac3forge::FirmwareTrial trial;
        trial.healthy_for_ms = static_cast<std::uint32_t>(number_at(value, "healthy_for_ms"));
        trial.hold_ms = static_cast<std::uint32_t>(number_at(value, "hold_ms"));
        trial.remaining_ms = static_cast<std::uint32_t>(number_at(value, "remaining_ms"));
        for (const json::Value condition : value["waiting_for"].elements()) {
            if (std::optional<std::string> words = condition.as_string()) {
                trial.waiting_for.push_back(std::move(*words));
            }
        }
        status.trial = std::move(trial);
    }
    if (const json::Value value = root["upload"]; value.is_object()) {
        status.upload = ac3forge::FirmwareUpload{
            .received = static_cast<std::size_t>(number_at(value, "received")),
            .total = static_cast<std::size_t>(number_at(value, "total")),
            .stage = text_at(value, "stage"),
        };
    }
    if (const json::Value value = root["last_update"]; value.is_object()) {
        status.last_update = ac3forge::FirmwareLastUpdate{
            .version = text_at(value, "version"),
            .result = text_at(value, "result"),
            .reason = text_at(value, "reason"),
        };
    }
    if (const json::Value value = root["coredump"]; value.is_object()) {
        status.coredump = ac3forge::FirmwareCoredump{
            .bytes = static_cast<std::size_t>(number_at(value, "bytes")),
            .intact = value["intact"].as_bool().value_or(false),
            .task = text_at(value, "task"),
            .pc = text_at(value, "pc"),
            .reason = text_at(value, "reason"),
            .elf_sha256 = text_at(value, "elf_sha256"),
        };
    }
    status.network = root["network"].as_string().value_or(std::string("none"));
    status.slot_bytes = static_cast<std::size_t>(number_at(root, "slot_bytes"));
    status.flash_bytes = static_cast<std::size_t>(number_at(root, "flash_bytes"));
    for (const json::Value entry : root["partitions"].elements()) {
        status.partitions.push_back(ac3forge::FirmwarePartition{
            .label = text_at(entry, "label"),
            .type = static_cast<unsigned>(number_at(entry, "type")),
            .subtype = static_cast<unsigned>(number_at(entry, "subtype")),
            .offset = static_cast<std::uint32_t>(number_at(entry, "offset")),
            .size = static_cast<std::uint32_t>(number_at(entry, "size")),
        });
    }
    status.bootloader_version = text_at(root, "bootloader_version");
    return status;
}

ReadFirmwareFile read_firmware_file(std::vector<std::uint8_t> bytes) {
    if (bytes.size() < ac3forge::kImageHeadBytes) {
        return {std::nullopt, fmt::format("it is {} bytes, too short to be an application image", bytes.size())};
    }
    ac3forge::ParsedHead parsed = ac3forge::parse_image_head(bytes);
    if (!parsed.head) {
        return {std::nullopt, std::move(parsed.why)};
    }
    const std::span<const std::uint8_t> data(bytes);
    std::size_t at = ac3forge::kImageHeaderBytes;
    std::uint8_t checksum = kChecksumSeed;
    const std::uint8_t segments = data[1];
    for (std::uint8_t index = 0; index < segments; ++index) {
        if (data.size() - at < ac3forge::kSegmentHeaderBytes) {
            return {std::nullopt,
                    fmt::format("it ends before segment {}'s header: it is cut short or damaged", index)};
        }
        const std::uint32_t length = ac3forge::detail::u32_at(data, at + 4);
        at += ac3forge::kSegmentHeaderBytes;
        if (length % 4 != 0) {
            return {std::nullopt, fmt::format("segment {} is {} bytes, not a whole number of words", index, length)};
        }
        if (data.size() - at < length) {
            return {std::nullopt, fmt::format("it ends inside segment {}: it is cut short or damaged", index)};
        }
        for (const std::uint8_t byte : data.subspan(at, length)) {
            checksum ^= byte;
        }
        at += length;
    }
    const std::size_t padded = (at + 1 + 15) & ~std::size_t{15};  // the checksum byte ends a 16-byte block
    if (padded > data.size()) {
        return {std::nullopt, "it ends before its checksum: it is cut short"};
    }
    if (!parsed.head->hash_appended) {
        return {std::nullopt,
                "it carries no SHA-256 of itself (hash_appended is not set), so damage to it could not be "
                "found; the board refuses such an image too"};
    }
    if (data.size() - padded < kDigestBytes) {
        return {std::nullopt, "it ends before the SHA-256 the build appended: it is cut short"};
    }
    std::array<std::uint8_t, 32> digest{};
    if (!sha256_of(data.first(padded), digest)) {
        return {std::nullopt, "this computer could not compute a SHA-256"};
    }
    const std::span<const std::uint8_t> appended = data.subspan(padded, kDigestBytes);
    if (!std::equal(digest.begin(), digest.end(), appended.begin())) {
        return {std::nullopt, "its SHA-256 does not match the one the build appended to it: the file is damaged"};
    }
    if (data[padded - 1] != checksum) {
        return {std::nullopt, fmt::format("its checksum byte is {:#04x}, and its segments add up to {:#04x}: the "
                                          "image is damaged",
                                          data[padded - 1], checksum)};
    }
    FirmwareFile file;
    if (!sha256_of(data, file.file_sha256)) {
        return {std::nullopt, "this computer could not compute a SHA-256"};
    }
    file.image_sha256 = hex(appended);
    file.head = std::move(*parsed.head);
    file.elf_sha256 = hex(file.head.elf_sha256);
    file.data = std::move(bytes);
    return {std::move(file), {}};
}

std::optional<std::string> refuse_update(const FirmwareFile& file, const SinkHardware& hardware,
                                         const ac3forge::FirmwareStatus& firmware) {
    const std::optional<std::uint16_t> board_chip = chip_id_of(hardware.target);
    if (!board_chip) {
        return fmt::format("the board reports its chip as {}, which this app does not know",
                           hardware.chip.empty() ? hardware.target : hardware.chip);
    }
    const std::optional<std::uint16_t> revision = parse_revision(hardware.revision);
    if (!revision) {
        return fmt::format("the board reports its chip revision as '{}', not as M.m", hardware.revision);
    }
    ac3forge::BoardFacts board;
    board.chip_id = *board_chip;
    board.revision_full = *revision;
    // A board that does not report its flash size is not held to one, as
    // ota.py does not hold it.
    board.flash_size = firmware.flash_bytes != 0 ? flash_code(firmware.flash_bytes).value_or(0xFF)
                                                 : file.head.flash_size;
    // An older board that does not name its project is not held to one either.
    board.project = hardware.project.empty() ? file.head.project : hardware.project;
    if (std::optional<std::string> why = ac3forge::refuse_image(file.head, board)) {
        return why;
    }
    if (firmware.slot_bytes != 0 && file.data.size() > firmware.slot_bytes) {
        return fmt::format("the image is {} bytes, and the board's slot holds {}", grouped(file.data.size()),
                           grouped(firmware.slot_bytes));
    }
    if ((firmware.running && firmware.running->state == "trial") || firmware.trial) {
        return "the running image is still on trial; wait until the board has accepted it, then send the image "
               "again";
    }
    if (firmware.upload) {
        return "an update is already under way on this board";
    }
    if (!firmware.other) {
        return "this board's partition table has one app slot: move it to the two-slot table with one USB flash "
               "first";
    }
    if (firmware.network == "built-in") {
        return "the board's only network is built into the image it runs, and an image file cannot say whether it "
               "has one: store the network on the board first (its page, under Network), then send the image";
    }
    return std::nullopt;
}

WaitVerdict judge_wait(const ac3forge::FirmwareStatus* firmware, const FirmwareFile& file,
                       const WaitContext& context) {
    if (firmware == nullptr) {
        return {UpdateOutcome::kNone, "no answer yet: restarting"};
    }
    const ac3forge::FirmwareSlot* running = firmware->running ? &*firmware->running : nullptr;
    const std::optional<ac3forge::FirmwareLastUpdate>& last = firmware->last_update;
    if (firmware->mode == "flash") {
        if (context.reply_lost && !firmware->upload) {
            const std::string reason = last && !last->reason.empty() ? last->reason : "the board did not say why";
            return {UpdateOutcome::kRefused, "refused: " + reason};
        }
        return {UpdateOutcome::kNone, "still in flash mode; it restarts next"};
    }
    if (is_new_image(running, file, context.slot)) {
        if (running->state == "trial" || firmware->trial) {
            return {UpdateOutcome::kNone,
                    "on trial: " + (firmware->trial ? trial_words(*firmware->trial) : std::string("starting"))};
        }
        if (running->state == "valid") {
            if (!running->image_sha256.empty() || running->intact == false || context.sha_wait_over) {
                return {UpdateOutcome::kUpdated, updated_text(*running, file)};
            }
            return {UpdateOutcome::kNone, "accepted; waiting for the board's own check of the slot", true};
        }
        return {UpdateOutcome::kNone, fmt::format("runs the new image ({})", running->state)};
    }
    if (context.reply_lost && !firmware->upload && same_last_update(last, context.last_before)) {
        return {UpdateOutcome::kFailed,
                "failed: the board runs the image it ran before and says nothing of this upload, so it did not take "
                "it"};
    }
    if (last && last->result == "rolled back") {
        return {UpdateOutcome::kRolledBack,
                fmt::format("rolled back: {} did not last, and the board runs {} from {} again. The board says: {}",
                            last->version.empty() ? file.head.version : last->version,
                            running != nullptr ? running->version : std::string("?"),
                            running != nullptr ? running->label : std::string("?"),
                            last->reason.empty() ? std::string("no reason given") : last->reason)};
    }
    return {UpdateOutcome::kNone, "restarting"};
}

std::string silent_text(const ac3forge::FirmwareStatus* last, const FirmwareFile& file, const WaitContext& context,
                        std::chrono::seconds waited) {
    const long long seconds = waited.count();
    const ac3forge::FirmwareSlot* running = last != nullptr && last->running ? &*last->running : nullptr;
    std::string head;
    if (last == nullptr) {
        head = fmt::format("did not come back within {} s.", seconds);
    } else if (last->mode == "flash") {
        head = fmt::format("still in flash mode after {} s: it has not restarted into the new image.", seconds);
    } else if (is_new_image(running, file, context.slot)) {
        head = running->state == "trial" || last->trial
                   ? fmt::format("still runs the new image on trial after {} s.", seconds)
                   : fmt::format("runs the new image after {} s ({}), and has not accepted it.", seconds,
                                 running->state);
    } else {
        head = fmt::format("runs {} from {} after {} s, and has not said how the update to {} ended.",
                           running != nullptr ? running->version : std::string("?"),
                           running != nullptr ? running->label : std::string("?"), seconds,
                           context.slot.empty() ? std::string("the other slot") : context.slot);
    }
    return head +
           " Cycling the board's power while the new image is on trial boots the previous one. Failing that, "
           "flash it over USB.";
}

// --- the client ------------------------------------------------------------------

class SinkFirmware::Worker {
   public:
    Worker(std::string host, std::uint16_t port, SinkFirmwareTiming timing)
        : host_(std::move(host)), port_(port), timing_(timing) {
        snapshot_.host = host_;
        snapshot_.port = port_;
    }

    enum class Job : std::uint8_t { kNone, kUpdate, kRollback, kRestart };

    // The thread's body; then says it has finished, which the destructor
    // waits a little for.
    void run();
    void finish_thread() {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            finished_ = true;
        }
        wake_.notify_all();
    }

    // Stops the thread, and cuts its request short where the platform lets
    // it; true when it has finished within `grace`.
    bool stop(std::chrono::milliseconds grace) {
        std::unique_lock<std::mutex> lock(mutex_);
        stop_ = true;
        if (active_client_ != nullptr) {
            active_client_->stop();
        }
        wake_.notify_all();
        return wake_.wait_for(lock, grace, [this] { return finished_; });
    }

    Snapshot snapshot() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_;
    }

    void set_watching(bool watching) {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (watching_ == watching) {
                return;
            }
            watching_ = watching;
            next_poll_ = {};
        }
        wake_.notify_all();
    }

    bool start(Job job, std::optional<FirmwareFile> file) {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (job_ != Job::kNone) {
                return false;
            }
            job_ = job;
            if (job == Job::kUpdate && file) {
                snapshot_.action.clear();
                snapshot_.update =
                    Update{.version = file->head.version, .stage = "checking", .text = "checking the board"};
                ++snapshot_.generation;
            }
            job_file_ = std::move(file);
        }
        wake_.notify_all();
        return true;
    }

    bool busy() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return job_ != Job::kNone;
    }

   private:
    // One request's outcome: status 0 when nothing answered, and why.
    struct Answer {
        int status = 0;
        std::string body;
        std::string error;
    };

    void poll_once();
    void run_update(FirmwareFile file);
    void run_action(Job job);
    // A request through `client`, which stop() can reach while it runs.
    template <class Request>
    [[nodiscard]] Answer request(httplib::Client& client, Request&& send);
    [[nodiscard]] Answer get(const char* path);
    // `method` with an empty body: PUT /firmware/rollback, POST /restart.
    [[nodiscard]] Answer send_empty(bool post, const char* path);
    // GET /firmware, published to the snapshot as a poll would be; nothing
    // when it was not answered with one.
    std::optional<ac3forge::FirmwareStatus> read_firmware();
    [[nodiscard]] std::string refusal_after_break();
    void set_update(const Update& update);
    void publish_poll(std::optional<SinkHardware> hardware, std::optional<ac3forge::FirmwareStatus> firmware,
                      bool answered, std::string error);
    // Waits `delay`, or less if the thread is to stop; true when it is.
    bool sleep_for(std::chrono::milliseconds delay) {
        std::unique_lock<std::mutex> lock(mutex_);
        return wake_.wait_for(lock, delay, [this] { return stop_; });
    }
    [[nodiscard]] bool stopping() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return stop_;
    }
    [[nodiscard]] httplib::Client client(std::chrono::seconds read) const {
        httplib::Client out(host_, port_);
        out.set_connection_timeout(static_cast<time_t>(timing_.request.count()));
        out.set_read_timeout(static_cast<time_t>(read.count()));
        out.set_write_timeout(static_cast<time_t>(read.count()));
        return out;
    }

    const std::string host_;
    const std::uint16_t port_;
    const SinkFirmwareTiming timing_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    Snapshot snapshot_;
    bool watching_ = false;
    bool stop_ = false;
    bool finished_ = false;
    Job job_ = Job::kNone;
    bool job_running_ = false;
    std::optional<FirmwareFile> job_file_;
    std::chrono::steady_clock::time_point next_poll_{};
    // The request under way, for stop(). On Linux and macOS that ends it at
    // once; on Windows the socket's wait goes on to its timeout.
    httplib::Client* active_client_ = nullptr;
};

void SinkFirmware::Worker::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_) {
        if (job_ != Job::kNone && !job_running_) {
            const Job job = job_;
            std::optional<FirmwareFile> file = std::move(job_file_);
            job_file_.reset();
            job_running_ = true;
            lock.unlock();
            if (job == Job::kUpdate && file) {
                run_update(std::move(*file));
            } else {
                run_action(job);
            }
            lock.lock();
            job_ = Job::kNone;
            job_running_ = false;
            next_poll_ = {};  // what the board says now, at once
            ++snapshot_.generation;
            continue;
        }
        const auto now = std::chrono::steady_clock::now();
        if (watching_ && now >= next_poll_) {
            next_poll_ = now + timing_.poll;
            lock.unlock();
            poll_once();
            lock.lock();
            continue;
        }
        if (watching_) {
            wake_.wait_until(lock, next_poll_);
        } else {
            wake_.wait(lock);
        }
    }
}

template <class Request>
SinkFirmware::Worker::Answer SinkFirmware::Worker::request(httplib::Client& client, Request&& send) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) {
            return {0, {}, "stopped"};
        }
        active_client_ = &client;
    }
    const httplib::Result result = send(client);
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        active_client_ = nullptr;
    }
    if (!result) {
        return {0, {}, httplib::to_string(result.error())};
    }
    std::string body = result->body;
    while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) {
        body.pop_back();
    }
    return {result->status, std::move(body), {}};
}

SinkFirmware::Worker::Answer SinkFirmware::Worker::get(const char* path) {
    httplib::Client http = client(timing_.request);
    return request(http, [&](httplib::Client& c) {
        return c.Get(path, httplib::Headers{{"Host", host_header(host_, port_)}});
    });
}

SinkFirmware::Worker::Answer SinkFirmware::Worker::send_empty(bool post, const char* path) {
    httplib::Client http = client(timing_.request);
    const httplib::Headers headers{{"Host", host_header(host_, port_)}};
    return request(http, [&](httplib::Client& c) {
        return post ? c.Post(path, headers, std::string(), "text/plain")
                    : c.Put(path, headers, std::string(), "text/plain");
    });
}

void SinkFirmware::Worker::publish_poll(std::optional<SinkHardware> hardware,
                                        std::optional<ac3forge::FirmwareStatus> firmware, bool answered,
                                        std::string error) {
    const std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.asked = true;
    snapshot_.answering = answered;
    snapshot_.error = std::move(error);
    if (hardware) {
        snapshot_.hardware = std::move(hardware);
    }
    if (firmware) {
        snapshot_.firmware = std::move(firmware);
    }
    ++snapshot_.generation;
}

std::optional<ac3forge::FirmwareStatus> SinkFirmware::Worker::read_firmware() {
    const Answer answer = get("/firmware");
    if (answer.status == 0) {
        publish_poll(std::nullopt, std::nullopt, false, fmt::format("no answer: {}", answer.error));
        return std::nullopt;
    }
    if (answer.status == 404) {
        publish_poll(std::nullopt, std::nullopt, false,
                     "the firmware it runs takes no updates over the network: it has no GET /firmware");
        return std::nullopt;
    }
    std::optional<ac3forge::FirmwareStatus> status =
        answer.status == 200 ? parse_firmware_status(answer.body) : std::nullopt;
    if (!status) {
        publish_poll(std::nullopt, std::nullopt, false,
                     fmt::format("GET /firmware answered {} with something this app cannot read", answer.status));
        return std::nullopt;
    }
    publish_poll(std::nullopt, status, true, {});
    return status;
}

void SinkFirmware::Worker::poll_once() {
    bool need_hardware = false;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        need_hardware = !snapshot_.hardware.has_value();
    }
    if (need_hardware) {
        const Answer answer = get("/hardware");
        if (answer.status == 200) {
            if (std::optional<SinkHardware> hardware = parse_sink_hardware(answer.body)) {
                publish_poll(std::move(hardware), std::nullopt, true, {});
            }
        }
    }
    (void)read_firmware();
}

void SinkFirmware::Worker::set_update(const Update& update) {
    const std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.update = update;
    ++snapshot_.generation;
}

std::string SinkFirmware::Worker::refusal_after_break() {
    // An upload that reached the board put it in flash mode, so a board in
    // normal mode never had this one, and its last update is an older one.
    const auto deadline = std::chrono::steady_clock::now() + timing_.refusal_wait;
    while (true) {
        const std::optional<ac3forge::FirmwareStatus> firmware = read_firmware();
        if (firmware && !firmware->upload) {
            const bool in_flash_mode = firmware->mode == "flash";
            const std::optional<ac3forge::FirmwareLastUpdate>& last = firmware->last_update;
            if (in_flash_mode && last && (last->result == "refused" || last->result == "failed")) {
                return last->reason.empty() ? last->result : last->reason;
            }
            return {};
        }
        if (std::chrono::steady_clock::now() >= deadline || sleep_for(timing_.wait_poll)) {
            return {};
        }
    }
}

void SinkFirmware::Worker::run_update(FirmwareFile file) {
    Update update{.version = file.head.version, .stage = "checking", .text = "checking the board"};
    set_update(update);
    const auto finish = [&](UpdateOutcome outcome, std::string text) {
        update.stage = "done";
        update.outcome = outcome;
        update.text = std::move(text);
        set_update(update);
    };

    // The pre-flight: what the board is now, not what the page last showed.
    std::optional<SinkHardware> hardware;
    {
        const Answer answer = get("/hardware");
        if (answer.status == 0) {
            finish(UpdateOutcome::kRefused, fmt::format("refused: GET /hardware got no answer: {}", answer.error));
            return;
        }
        hardware = answer.status == 200 ? parse_sink_hardware(answer.body) : std::nullopt;
        if (!hardware) {
            finish(UpdateOutcome::kRefused,
                   fmt::format("refused: GET /hardware answered {} with something this app cannot read",
                               answer.status));
            return;
        }
        publish_poll(hardware, std::nullopt, true, {});
    }
    const std::optional<ac3forge::FirmwareStatus> before = read_firmware();
    if (!before) {
        finish(UpdateOutcome::kRefused, "refused: " + snapshot().error);
        return;
    }
    if (std::optional<std::string> why = refuse_update(file, *hardware, *before)) {
        finish(UpdateOutcome::kRefused, "refused: " + *why);
        return;
    }
    update.target = hardware->target;

    // The upload, with the whole file's SHA-256 for the board to check what
    // it received against.
    WaitContext context;
    context.slot = before->other ? before->other->label : std::string();
    context.last_before = before->last_update;
    update.stage = "sending";
    update.total = file.data.size();
    update.text = fmt::format("sending {} bytes to {}; the board erases what the image needs first",
                              grouped(file.data.size()), context.slot.empty() ? "the other slot" : context.slot);
    set_update(update);

    httplib::Client http = client(timing_.upload_answer);
    const httplib::Headers headers{
        {"Host", host_header(host_, port_)},
        {"Content-Digest", "sha-256=:" + ac3::sendspin::base64::encode(file.file_sha256) + ":"},
    };
    std::size_t sent = 0;
    const auto progress = [&](std::size_t current, std::size_t total) {
        sent = current;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (snapshot_.update) {
                snapshot_.update->sent = current;
                if (current >= total) {
                    snapshot_.update->stage = "answering";
                    snapshot_.update->text = "sent; the board reads the slot back and checks it before it answers";
                }
                ++snapshot_.generation;
            }
        }
        return !stopping();
    };
    const Answer answer = request(http, [&](httplib::Client& c) {
        return c.Put("/firmware", headers, reinterpret_cast<const char*>(file.data.data()), file.data.size(),
                     "application/octet-stream", progress);
    });
    if (stopping()) {
        return;
    }
    update.sent = sent;
    if (answer.status == 0 && sent < file.data.size()) {
        update.text = fmt::format("the upload broke off after {} bytes: {}", grouped(sent), answer.error);
        set_update(update);
        const std::string reason = refusal_after_break();
        finish(reason.empty() ? UpdateOutcome::kFailed : UpdateOutcome::kRefused,
               reason.empty() ? "failed: the board gave no reason. If it is in flash mode, another update, or ten "
                                "minutes, restarts it into the image it runs"
                              : "refused: " + reason);
        return;
    }
    if (answer.status == 0) {
        context.reply_lost = true;
        update.text = fmt::format("no answer to the upload ({}); looking for the board", answer.error);
    } else if (answer.status != 200) {
        finish(UpdateOutcome::kRefused,
               fmt::format("refused ({}): {}{}", answer.status, answer.body, http_hint(answer.status)));
        return;
    } else {
        std::vector<json::Token> tokens;
        json::Document document;
        std::string written = "written and checked";
        if (document.parse(answer.body, tokens, kMaxJsonTokens) && document.root().is_object()) {
            const json::Value root = document.root();
            if (const std::string slot = text_at(root, "slot"); !slot.empty()) {
                context.slot = slot;
            }
            written = fmt::format("written to {} and checked", context.slot.empty() ? "the other slot" : context.slot);
            const std::string reported = lower(text_at(root, "sha256"));
            if (!reported.empty() && reported != hex(file.file_sha256)) {
                written += "; the board's SHA-256 of what it received is " + reported + ", not the file's";
            }
        }
        update.text = written + "; the board restarts into it, on trial";
    }

    // The restart and the trial.
    update.stage = "waiting";
    set_update(update);
    const auto deadline = std::chrono::steady_clock::now() + timing_.wait;
    std::optional<std::chrono::steady_clock::time_point> sha_deadline;
    std::optional<ac3forge::FirmwareStatus> last;
    while (true) {
        const std::optional<ac3forge::FirmwareStatus> firmware = read_firmware();
        if (stopping()) {
            return;
        }
        if (firmware) {
            last = firmware;
        }
        const auto now = std::chrono::steady_clock::now();
        if (sha_deadline && now >= *sha_deadline) {
            context.sha_wait_over = true;
        }
        const WaitVerdict verdict = judge_wait(firmware ? &*firmware : nullptr, file, context);
        if (verdict.outcome != UpdateOutcome::kNone) {
            finish(verdict.outcome, verdict.text);
            return;
        }
        if (verdict.accepted && !sha_deadline) {
            sha_deadline = now + timing_.sha_wait;
        }
        if (now >= deadline) {
            finish(UpdateOutcome::kSilent, silent_text(last ? &*last : nullptr, file, context, timing_.wait));
            return;
        }
        if (update.text != verdict.text) {
            update.text = verdict.text;
            set_update(update);
        }
        if (sleep_for(timing_.wait_poll)) {
            return;
        }
    }
}

void SinkFirmware::Worker::run_action(Job job) {
    const bool rollback = job == Job::kRollback;
    const Answer answer = rollback ? send_empty(false, "/firmware/rollback") : send_empty(true, "/restart");
    std::string text = answer.status == 0
                           ? fmt::format("{}: no answer ({})", rollback ? "roll back" : "restart", answer.error)
                           : fmt::format("{}: {} {}{}", rollback ? "roll back" : "restart", answer.status,
                                         answer.body, http_hint(answer.status));
    const std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.action = std::move(text);
    ++snapshot_.generation;
}

SinkFirmware::SinkFirmware(std::string host, std::uint16_t port, SinkFirmwareTiming timing)
    : worker_(std::make_shared<Worker>(std::move(host), port, timing)) {
    thread_ = std::thread([worker = worker_] {
        worker->run();
        worker->finish_thread();
    });
}

SinkFirmware::~SinkFirmware() {
    if (worker_->stop(std::chrono::milliseconds(1000))) {
        thread_.join();
    } else {
        // Inside a request the platform would not cut short: the thread
        // holds its own share of the worker, and ends with the request.
        thread_.detach();
    }
}

SinkFirmware::Snapshot SinkFirmware::snapshot() const { return worker_->snapshot(); }

void SinkFirmware::set_watching(bool watching) { worker_->set_watching(watching); }

bool SinkFirmware::start_update(FirmwareFile file) { return worker_->start(Worker::Job::kUpdate, std::move(file)); }

bool SinkFirmware::rollback() { return worker_->start(Worker::Job::kRollback, std::nullopt); }

bool SinkFirmware::restart() { return worker_->start(Worker::Job::kRestart, std::nullopt); }

bool SinkFirmware::busy() const { return worker_->busy(); }

}  // namespace ac3::hearth
