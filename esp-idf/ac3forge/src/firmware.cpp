// Updates over the network. See ../include/ac3forge/firmware.hpp and
// planning/esp32-ota.md.

#include "ac3forge/firmware.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sdkconfig.h"

#include "esp_app_desc.h"
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
// Exported only when the project keeps core dumps.
#include "esp_core_dump.h"
#endif
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/efuse_hal.h"
#include "nvs.h"
#include "psa/crypto.h"

#include "ac3forge/firmware_image.hpp"
#include "ac3forge/firmware_status.hpp"

namespace ac3forge {
namespace {

// How the last update ended, and the image an update wrote until its trial
// decides, in NVS: both images share the partition, so the image that runs
// after a rollback can say what happened to the one that did not last.
constexpr const char* kNamespace = "firmware";
constexpr const char* kKeyPending = "pending";       // ELF SHA-256 (hex) of an image written, undecided
constexpr const char* kKeyPendingVersion = "pend_ver";
constexpr const char* kKeyWhy = "why";               // why a trial gave up, from the image that gave up
constexpr const char* kKeyLastVersion = "last_ver";
constexpr const char* kKeyLastResult = "last_res";
constexpr const char* kKeyLastReason = "last_why";

// httpd_req_recv's timeout is 5 s a call; this many in a row is a stalled
// upload (planning/esp32-ota.md: "A stalled connection gives up after 30 s").
constexpr int kStalledReceives = 6;

std::string hex(std::span<const std::uint8_t> bytes) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::uint8_t b : bytes) {
        out += kDigits[static_cast<std::size_t>(b >> 4)];
        out += kDigits[static_cast<std::size_t>(b & 0x0F)];
    }
    return out;
}

template <std::size_t N>
std::string field_text(const char (&field)[N]) {
    return {field, static_cast<std::size_t>(std::find(field, field + N, '\0') - field)};
}

std::string nvs_text(const char* key) {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return {};
    }
    std::size_t length = 0;
    std::string value;
    if (nvs_get_str(handle, key, nullptr, &length) == ESP_OK && length > 0) {
        value.resize(length);
        if (nvs_get_str(handle, key, value.data(), &length) == ESP_OK) {
            value.resize(length - 1);  // the terminator nvs_get_str counts
        } else {
            value.clear();
        }
    }
    nvs_close(handle);
    return value;
}

// Each write is its own open and commit: these happen a few times an update.
void nvs_set_texts(std::initializer_list<std::pair<const char*, std::string_view>> values) {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        std::printf("firmware: could not open NVS to record the update\n");
        return;
    }
    for (const auto& [key, value] : values) {
        const std::string text(value);
        if (text.empty()) {
            (void)nvs_erase_key(handle, key);
        } else if (nvs_set_str(handle, key, text.c_str()) != ESP_OK) {
            std::printf("firmware: NVS refused %s\n", key);
        }
    }
    (void)nvs_commit(handle);
    nvs_close(handle);
}

void record_last(std::string_view version, std::string_view result, std::string_view reason) {
    nvs_set_texts({{kKeyLastVersion, version}, {kKeyLastResult, result}, {kKeyLastReason, reason}});
}

const char* state_name(esp_ota_img_states_t state) {
    switch (state) {
        case ESP_OTA_IMG_NEW:
            return "new";
        case ESP_OTA_IMG_PENDING_VERIFY:
            return "trial";
        case ESP_OTA_IMG_VALID:
            return "valid";
        case ESP_OTA_IMG_INVALID:
            return "invalid";
        case ESP_OTA_IMG_ABORTED:
            return "aborted";
        default:
            return "undefined";
    }
}

// Why the image before this one did not last, from the reset that ended it,
// for a trial that gave up without saying why first.
const char* reset_reason_text(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_PANIC:
            return "it panicked";
        case ESP_RST_INT_WDT:
            return "the interrupt watchdog reset it";
        case ESP_RST_TASK_WDT:
            return "the task watchdog reset it";
        case ESP_RST_WDT:
            return "a watchdog reset it";
        case ESP_RST_BROWNOUT:
            return "its supply dipped (brownout)";
        case ESP_RST_POWERON:
            return "the power went off before it had proved itself";
        case ESP_RST_EXT:
            return "it was reset before it had proved itself";
        case ESP_RST_SW:
            return "it restarted before it had proved itself";
        default:
            return "it reset before it had proved itself";
    }
}

std::string request_header(httpd_req_t* req, const char* name) {
    const std::size_t length = httpd_req_get_hdr_value_len(req, name);
    if (length == 0) {
        return {};
    }
    std::string value(length + 1, '\0');
    if (httpd_req_get_hdr_value_str(req, name, value.data(), value.size()) != ESP_OK) {
        return {};
    }
    value.resize(length);
    return value;
}

// A short body, whitespace trimmed; empty on anything longer than `limit`.
std::string short_body(httpd_req_t* req, std::size_t limit = 32) {
    if (req->content_len <= 0 || req->content_len > limit) {
        return {};
    }
    std::string body(req->content_len, '\0');
    std::size_t got = 0;
    while (got < body.size()) {
        const int n = httpd_req_recv(req, body.data() + got, body.size() - got);
        if (n <= 0) {
            return {};
        }
        got += static_cast<std::size_t>(n);
    }
    while (!body.empty() && (body.back() == '\n' || body.back() == '\r' || body.back() == ' ')) {
        body.pop_back();
    }
    return body;
}

esp_err_t reply(httpd_req_t* req, const char* status, const char* type, std::string_view body) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, type);
    return httpd_resp_send(req, body.data(), static_cast<ssize_t>(body.size()));
}

esp_err_t reply_text(httpd_req_t* req, const char* status, std::string_view text) {
    std::string line(text);
    line += '\n';
    return reply(req, status, "text/plain", line);
}

// A slot's image hashed as it lies in flash, over its first `length` bytes:
// the upload's read-back.
std::optional<std::array<std::uint8_t, 32>> hash_slot(const esp_partition_t* slot, std::size_t length,
                                                      std::span<std::uint8_t> buffer) {
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        return std::nullopt;
    }
    for (std::size_t at = 0; at < length;) {
        const std::size_t n = std::min(buffer.size(), length - at);
        if (esp_partition_read(slot, at, buffer.data(), n) != ESP_OK ||
            psa_hash_update(&op, buffer.data(), n) != PSA_SUCCESS) {
            (void)psa_hash_abort(&op);
            return std::nullopt;
        }
        at += n;
    }
    std::array<std::uint8_t, 32> digest{};
    std::size_t written = 0;
    if (psa_hash_finish(&op, digest.data(), digest.size(), &written) != PSA_SUCCESS) {
        return std::nullopt;
    }
    return digest;
}

// The background check's work on one slot: its image's SHA-256 recomputed
// over what the build hashed (walk_image) and compared with the one appended,
// as the bootloader checks an image at boot. Read with esp_partition_read
// rather than esp_partition_get_sha256, which returns the appended hash
// without checking it, and maps the image to find it; Espressif's QEMU for
// the S3 does not survive a second mapping of the running image's flash being
// unmapped.
struct SlotHashing {
    const esp_partition_t* slot = nullptr;
    bool is_running = false;
    unsigned generation = 0;  // the other slot's rewrite count when it began
    std::size_t at = 0;
    std::size_t end = 0;
    std::array<std::uint8_t, 32> stored{};
    std::array<std::uint8_t, 256> buffer{};
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
};

// Hashing `slot` set up to begin, or why its image cannot be checked.
std::unique_ptr<SlotHashing> begin_hashing(const esp_partition_t* slot, std::string& why) {
    const WalkedImage walked = walk_image(
        [slot](std::size_t offset, std::span<std::uint8_t> out) {
            return esp_partition_read(slot, offset, out.data(), out.size()) == ESP_OK;
        },
        slot->size);
    if (!walked.extent) {
        why = walked.why;
        return nullptr;
    }
    if (!walked.extent->hash_appended) {
        why = "its image carries no SHA-256 of itself";
        return nullptr;
    }
    auto hashing = std::make_unique<SlotHashing>();
    hashing->slot = slot;
    hashing->end = walked.extent->hashed_bytes;
    if (esp_partition_read(slot, hashing->end, hashing->stored.data(), hashing->stored.size()) != ESP_OK) {
        why = "its SHA-256 could not be read";
        return nullptr;
    }
    if (psa_hash_setup(&hashing->op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        why = "the board could not start a SHA-256";
        return nullptr;
    }
    return hashing;
}

// What GET /firmware says about a slot, read from flash once (read_slot) and
// kept: the fields of the image's esp_app_desc_t that the report carries, and
// the slot's state. Arrays rather than strings, so it holds no heap.
struct SlotFacts {
    const char* state = "empty";
    std::array<char, 32> version{};
    std::array<char, 32> project{};
    std::array<char, 32> idf_version{};
    std::array<std::uint8_t, 32> elf_sha256{};
};

template <std::size_t N>
std::string field_text(const std::array<char, N>& field) {
    return {field.data(), static_cast<std::size_t>(std::find(field.begin(), field.end(), '\0') - field.begin())};
}

SlotFacts read_slot(const esp_partition_t* slot, bool is_running) {
    SlotFacts facts;
    esp_app_desc_t desc{};
    if (esp_ota_get_partition_description(slot, &desc) != ESP_OK) {
        return facts;
    }
    std::copy_n(std::begin(desc.version), facts.version.size(), facts.version.begin());
    std::copy_n(std::begin(desc.project_name), facts.project.size(), facts.project.begin());
    std::copy_n(std::begin(desc.idf_ver), facts.idf_version.size(), facts.idf_version.begin());
    std::copy_n(std::begin(desc.app_elf_sha256), facts.elf_sha256.size(), facts.elf_sha256.begin());
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    facts.state = esp_ota_get_state_partition(slot, &state) == ESP_OK ? state_name(state)
                  : is_running                                          ? "valid"
                                                                        : "undefined";
    return facts;
}

// The report GET /firmware gives of a slot, without reading flash. The
// image's SHA-256 and whether it checked out come from the background check,
// and status() adds them under the lock.
FirmwareSlot report_slot(const esp_partition_t* slot, const SlotFacts& facts) {
    FirmwareSlot report;
    report.label = slot->label;
    report.state = facts.state;
    if (report.state == "empty") {
        return report;
    }
    report.version = field_text(facts.version);
    report.project = field_text(facts.project);
    report.idf_version = field_text(facts.idf_version);
    report.elf_sha256 = hex(facts.elf_sha256);
    return report;
}

std::uint32_t now_ms() { return static_cast<std::uint32_t>(esp_timer_get_time() / 1000); }

// The core dump a crash left in the coredump partition, read once at boot so
// that GET /firmware reads no flash (as with SlotFacts). A board that keeps
// no core dumps, or whose partition holds none, has nothing to report.
std::optional<FirmwareCoredump> read_coredump() {
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    std::size_t address = 0;
    std::size_t size = 0;
    if (esp_core_dump_image_get(&address, &size) != ESP_OK) {
        return std::nullopt;
    }
    FirmwareCoredump dump;
    dump.bytes = size;
    dump.intact = esp_core_dump_image_check() == ESP_OK;
    if (dump.intact) {
        // Some hundreds of bytes: on the heap, and only while this runs.
        auto summary = std::make_unique<esp_core_dump_summary_t>();
        if (esp_core_dump_get_summary(summary.get()) == ESP_OK) {
            dump.task = field_text(summary->exc_task);
            std::array<char, 16> pc{};
            (void)std::snprintf(pc.data(), pc.size(), "0x%08lx", static_cast<unsigned long>(summary->exc_pc));
            dump.pc = pc.data();
            const auto* sha = reinterpret_cast<const char*>(summary->app_elf_sha256);
            dump.elf_sha256.assign(sha, std::find(sha, sha + sizeof(summary->app_elf_sha256), '\0'));
        }
        std::array<char, 160> reason{};
        if (esp_core_dump_get_panic_reason(reason.data(), reason.size()) == ESP_OK) {
            dump.reason = reason.data();
        }
    }
    std::printf("firmware: a core dump of %u bytes from the last crash%s%s%s; GET /firmware/coredump has it\n",
                static_cast<unsigned>(dump.bytes), dump.task.empty() ? "" : ", in ", dump.task.c_str(),
                dump.intact ? "" : " (it does not check out)");
    return dump;
#else
    return std::nullopt;
#endif
}

}  // namespace

struct Firmware::Impl {
    FirmwareHooks hooks;
    FirmwareConfig config;

    const esp_partition_t* running = nullptr;
    const esp_partition_t* other = nullptr;  // nothing on a table with one app slot
    std::uint8_t flash_size_code = 0;        // the running image's header's
    std::size_t flash_bytes = 0;
    std::string bootloader_version;

    mutable std::mutex mutex;  // guards what follows
    bool flash_mode = false;
    bool busy = false;  // an upload, a rollback or a mode change is under way
    std::optional<FirmwareUpload> upload;
    std::optional<FirmwareLastUpdate> last_update;
    std::optional<FirmwareCoredump> coredump;  // read at boot (read_coredump)
    std::optional<Trial> trial;
    std::vector<std::string> waiting_for;
    // Each slot as read_slot() read it at boot, and again after whatever this
    // code changes about it: GET /firmware answers from these, so that a
    // client polling it never has the board read flash.
    SlotFacts running_facts;
    SlotFacts other_facts;
    // What the background check found: each slot's appended SHA-256 and
    // whether its image checked out. Until it has run, nothing. An upload
    // that erases the other slot makes it "not intact" and counts a rewrite,
    // so that a check already reading the slot drops what it found.
    std::optional<std::array<std::uint8_t, 32>> running_sha;
    std::optional<std::array<std::uint8_t, 32>> other_sha;
    std::optional<bool> running_intact;
    std::optional<bool> other_intact;
    unsigned other_generation = 0;

    // The background check reads both slots through, one at a time, and only
    // while nothing else writes flash: it starts once no trial is left to
    // decide, and stop_check() ends it before an upload, flash mode or a
    // rollback writes anything. Under QEMU, its reads beside other flash
    // work hung the emulated board. It runs a slice at a time from
    // esp_timer's task (check_slice), so it has no task or stack of its own:
    // a task's stack and buffer, 8 KiB, took the widest shape CI plays
    // (sdkconfig.ci-http714) under its free-heap floor.
    std::atomic<bool> check_running{false};
    std::atomic<bool> check_stop{false};
    std::unique_ptr<SlotHashing> hashing;  // touched only by the check's own timer once it runs
    esp_timer_handle_t check_timer = nullptr;

    esp_timer_handle_t idle_timer = nullptr;
    esp_timer_handle_t guard_timer = nullptr;
    // The trial is read once a second from esp_timer's task (on_trial), with
    // no task of its own until it has decided: then decide_task writes what
    // it decided. A task made at boot for the whole trial took 6 KiB of
    // internal RAM as the board started, and on the S3 board that left the
    // Sendspin player without the 32 KiB block its burst player starts with,
    // so no updated image could pass its trial there.
    esp_timer_handle_t trial_timer = nullptr;
    TrialStep decided = TrialStep::kWait;

    [[nodiscard]] FirmwareStatus status() const;
    [[nodiscard]] BoardFacts board() const;
    [[nodiscard]] bool host_allowed(httpd_req_t* req) const;
    void read_last_update();
    void enter_flash_mode();
    void arm_idle_timer();
    [[noreturn]] void restart_now(const char* why);

    struct UploadJob {
        Impl* im = nullptr;
        httpd_req_t* req = nullptr;  // the async copy
        std::size_t length = 0;
        ContentDigest digest;
    };
    struct Outcome {
        const char* status = "500 Internal Server Error";
        std::string body;
        bool json = false;
        bool restart = false;
        bool erased = false;  // the other slot was erased, so its report is out of date
        std::string version;
    };
    Outcome run_upload(UploadJob& job);
    static void upload_task(void* arg);
    static void on_trial(void* arg);
    static void decide_task(void* arg);
    void start_check();
    void stop_check();
    [[nodiscard]] std::unique_ptr<SlotHashing> begin_other();
    void check_slice();
    void finish_slot(bool intact, const char* why);
    void end_check();
    static void on_check(void* arg);
    static void on_idle(void* arg);
    static void on_guard(void* arg);
    void accept_trial();
    [[noreturn]] void give_up_trial(const std::vector<std::string>& waiting);
};

// --- reports ---------------------------------------------------------------------

FirmwareStatus Firmware::Impl::status() const {
    FirmwareStatus s;
    s.network = hooks.network_source ? hooks.network_source() : "none";
    s.slot_bytes = other != nullptr ? other->size : running->size;
    s.flash_bytes = flash_bytes;
    // The partition table as esp_partition read it at boot, from RAM.
    for (esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
         it != nullptr; it = esp_partition_next(it)) {
        const esp_partition_t* p = esp_partition_get(it);
        s.partitions.push_back(FirmwarePartition{p->label, static_cast<unsigned>(p->type),
                                                 static_cast<unsigned>(p->subtype), p->address, p->size});
    }
    s.bootloader_version = bootloader_version;
    const std::lock_guard lock(mutex);
    s.mode = flash_mode ? "flash" : "normal";
    s.running = report_slot(running, running_facts);
    s.running->image_sha256 = running_sha ? hex(*running_sha) : std::string{};
    s.running->intact = running_intact;
    if (other != nullptr) {
        s.other = report_slot(other, other_facts);
        if (s.other->state != "empty") {
            s.other->image_sha256 = other_sha ? hex(*other_sha) : std::string{};
            s.other->intact = other_intact;
        }
    }
    if (trial) {
        const std::uint32_t now = now_ms();
        s.trial = FirmwareTrial{trial->healthy_for_ms(now), trial->policy().hold_ms, trial->remaining_ms(now),
                                waiting_for};
    }
    s.upload = upload;
    s.last_update = last_update;
    s.coredump = coredump;
    return s;
}

BoardFacts Firmware::Impl::board() const {
    BoardFacts facts;
    facts.chip_id = static_cast<std::uint16_t>(CONFIG_IDF_FIRMWARE_CHIP_ID);
    facts.revision_full = static_cast<std::uint16_t>(efuse_hal_chip_revision());
    facts.ignore_max_revision = efuse_hal_get_disable_wafer_version_major();
    facts.flash_size = flash_size_code;
    facts.project = field_text(esp_app_get_description()->project_name);
    return facts;
}

bool Firmware::Impl::host_allowed(httpd_req_t* req) const {
    const std::vector<std::string> names = hooks.host_names ? hooks.host_names() : std::vector<std::string>{};
    return host_is_the_board(request_header(req, "Host"), names);
}

// How the last update ended: settled here, at boot, when the image an update
// wrote has either been accepted or gone back.
void Firmware::Impl::read_last_update() {
    const std::string pending = nvs_text(kKeyPending);
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    const bool on_trial = esp_ota_get_state_partition(running, &state) == ESP_OK &&
                          state == ESP_OTA_IMG_PENDING_VERIFY;
    if (!pending.empty() && !on_trial) {
        const std::string version = nvs_text(kKeyPendingVersion);
        if (pending == hex(esp_app_get_description()->app_elf_sha256)) {
            // Accepted, and the record of it lost to a reset in between.
            record_last(version, "accepted", "");
        } else {
            std::string why = nvs_text(kKeyWhy);
            if (why.empty()) {
                why = reset_reason_text(esp_reset_reason());
            }
            std::printf("firmware: the update to %s went back to this image: %s\n", version.c_str(), why.c_str());
            record_last(version, "rolled back", why);
        }
        nvs_set_texts({{kKeyPending, ""}, {kKeyPendingVersion, ""}, {kKeyWhy, ""}});
    }
    FirmwareLastUpdate last;
    last.version = nvs_text(kKeyLastVersion);
    last.result = nvs_text(kKeyLastResult);
    last.reason = nvs_text(kKeyLastReason);
    if (on_trial) {
        last.version = field_text(esp_app_get_description()->version);
        last.result = "on trial";
        last.reason.clear();
    }
    const std::lock_guard lock(mutex);
    if (!last.result.empty()) {
        last_update = last;
    }
}

// --- flash mode and restarts -----------------------------------------------------

void Firmware::Impl::enter_flash_mode() {
    {
        const std::lock_guard lock(mutex);
        if (flash_mode) {
            return;
        }
    }
    std::printf("firmware: flash mode - stopping everything that plays; the board restarts to leave it\n");
    stop_check();
    if (hooks.enter_flash_mode) {
        hooks.enter_flash_mode();
    }
    {
        const std::lock_guard lock(mutex);
        flash_mode = true;
    }
    arm_idle_timer();
    std::printf("firmware: flash mode; internal heap %u free, largest block %u\n",
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
}

void Firmware::Impl::arm_idle_timer() {
    if (idle_timer == nullptr) {
        return;
    }
    (void)esp_timer_stop(idle_timer);
    (void)esp_timer_start_once(idle_timer, static_cast<std::uint64_t>(config.flash_mode_idle_ms) * 1000);
}

void Firmware::Impl::on_idle(void* arg) {
    auto* im = static_cast<Impl*>(arg);
    {
        const std::lock_guard lock(im->mutex);
        if (!im->flash_mode || im->busy) {
            return;
        }
    }
    // Everything that could be told was told on the way in.
    std::printf("firmware: flash mode with no upload for %u s; restarting into this image\n",
                static_cast<unsigned>(im->config.flash_mode_idle_ms / 1000));
    esp_restart();
}

void Firmware::Impl::restart_now(const char* why) {
    std::printf("firmware: restarting %s\n", why);
    if (hooks.before_restart) {
        hooks.before_restart();
    }
    // The reply, and whatever the hook sent, leave before the sockets close.
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

// --- the trial -------------------------------------------------------------------

// Once a second from esp_timer's task while an image is on trial.
void Firmware::Impl::on_trial(void* arg) {
    auto* im = static_cast<Impl*>(arg);
    std::vector<std::string> waiting;
    if (im->hooks.trial_conditions) {
        for (const auto& [name, holds] : im->hooks.trial_conditions()) {
            if (!holds) {
                waiting.push_back(name);
            }
        }
    }
    if (im->config.test_unhealthy) {
        waiting.emplace_back("nothing (AC3FORGE_FIRMWARE_TEST_UNHEALTHY)");
    }
    TrialStep step = TrialStep::kWait;
    {
        const std::lock_guard lock(im->mutex);
        step = im->trial->step(now_ms(), waiting.empty());
        im->waiting_for = std::move(waiting);
    }
    if (step == TrialStep::kWait) {
        return;
    }
    // Either way the decision writes otadata and NVS, which takes more stack
    // than esp_timer's task has: a task of its own does it, made now, long
    // after the board has started. Accepting used 1,812 bytes of its stack on
    // the S3 board.
    (void)esp_timer_stop(im->trial_timer);
    im->decided = step;
    if (xTaskCreate(&Impl::decide_task, "fw_trial", 4096, im, tskIDLE_PRIORITY + 5, nullptr) == pdPASS) {
        return;
    }
    if (step == TrialStep::kAccept) {
        // Tried again in a second, when there may be room: the Trial keeps
        // its answer, and the guard still goes back past the deadline.
        (void)esp_timer_start_periodic(im->trial_timer, 1'000'000);
        return;
    }
    // Nothing can record why, and a restart on trial still goes back.
    std::printf("firmware: this image gives up its trial; going back\n");
    esp_restart();
}

void Firmware::Impl::decide_task(void* arg) {
    auto* im = static_cast<Impl*>(arg);
    if (im->decided == TrialStep::kAccept) {
        im->accept_trial();
    } else {
        std::vector<std::string> waiting;
        {
            const std::lock_guard lock(im->mutex);
            waiting = im->waiting_for;
        }
        im->give_up_trial(waiting);
    }
    vTaskDelete(nullptr);
}

void Firmware::Impl::accept_trial() {
    if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
        // The state could not be written. The bootloader then goes back at the
        // next reset, which is the safe way round to be wrong.
        std::printf("firmware: could not mark this image valid\n");
        return;
    }
    if (guard_timer != nullptr) {
        (void)esp_timer_stop(guard_timer);
    }
    const std::string version = field_text(esp_app_get_description()->version);
    record_last(version, "accepted", "");
    nvs_set_texts({{kKeyPending, ""}, {kKeyPendingVersion, ""}, {kKeyWhy, ""}});
    {
        const std::lock_guard lock(mutex);
        trial.reset();
        waiting_for.clear();
        last_update = FirmwareLastUpdate{version, "accepted", ""};
        running_facts.state = "valid";
    }
    std::printf("firmware: %s accepted after its trial (stack %u spare)\n", version.c_str(),
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    // Nothing is left to decide, so the slots can be read through now.
    start_check();
}

void Firmware::Impl::give_up_trial(const std::vector<std::string>& waiting) {
    std::string why = "not healthy within " + std::to_string(config.trial.deadline_ms / 1000) + " s";
    if (!waiting.empty()) {
        why += "; still waiting for ";
        for (std::size_t i = 0; i < waiting.size(); ++i) {
            why += (i == 0 ? "" : ", ") + waiting[i];
        }
    }
    nvs_set_texts({{kKeyWhy, why}});
    std::printf("firmware: this image gives up its trial (%s); going back\n", why.c_str());
    if (hooks.before_restart) {
        hooks.before_restart();
    }
    (void)esp_ota_mark_app_invalid_rollback_and_reboot();
    // Only when there was nothing to go back to. A restart on trial is a
    // rollback in the bootloader's eyes too.
    esp_restart();
}

// The deadline's backstop: a trial that has not acted 30 s past the deadline
// is starved or stuck, and a restart while on trial boots the image before
// this one.
void Firmware::Impl::on_guard(void* arg) {
    (void)arg;
    std::printf("firmware: the trial did not finish; restarting, which goes back\n");
    esp_restart();
}

// --- the background check of both slots -------------------------------------------
//
// A slice at a time from esp_timer's task: 2 KiB every 10 ms, through the 256
// bytes SlotHashing carries, so 5 s for each MiB of image. The running slot
// first, then the other if it holds an image.

void Firmware::Impl::start_check() {
    if (check_running.exchange(true)) {
        return;
    }
    check_stop = false;
    std::string why;
    hashing = begin_hashing(running, why);
    if (hashing) {
        hashing->is_running = true;
    } else {
        std::printf("firmware: the running image in %s does not check out: %s\n", running->label, why.c_str());
        {
            const std::lock_guard lock(mutex);
            running_intact = false;
        }
        hashing = begin_other();
    }
    if (!hashing || check_timer == nullptr || esp_timer_start_periodic(check_timer, 10'000) != ESP_OK) {
        hashing.reset();
        check_running = false;
    }
}

// Before anything writes flash. The check gives up at its next slice, 10 ms
// away, and this waits until it has.
void Firmware::Impl::stop_check() {
    check_stop = true;
    for (int i = 0; i < 500 && check_running.load(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// The other slot's hashing set up to begin, or nothing when it is empty or
// cannot be checked (and then says why).
std::unique_ptr<SlotHashing> Firmware::Impl::begin_other() {
    esp_app_desc_t desc{};
    if (other == nullptr || esp_ota_get_partition_description(other, &desc) != ESP_OK) {
        return nullptr;  // empty, which GET /firmware says without a check
    }
    unsigned generation = 0;
    {
        const std::lock_guard lock(mutex);
        generation = other_generation;
    }
    std::string why;
    std::unique_ptr<SlotHashing> next = begin_hashing(other, why);
    if (!next) {
        const std::lock_guard lock(mutex);
        if (generation == other_generation) {
            other_intact = false;
            std::printf("firmware: the image in %s does not check out (%s); it cannot be gone back to\n",
                        other->label, why.c_str());
        }
        return nullptr;
    }
    next->generation = generation;
    return next;
}

void Firmware::Impl::on_check(void* arg) { static_cast<Impl*>(arg)->check_slice(); }

void Firmware::Impl::check_slice() {
    if (check_stop.load() || !hashing) {
        end_check();
        return;
    }
    SlotHashing& h = *hashing;
    for (int i = 0; i < 8 && h.at < h.end; ++i) {
        const std::size_t n = std::min(h.buffer.size(), h.end - h.at);
        if (esp_partition_read(h.slot, h.at, h.buffer.data(), n) != ESP_OK ||
            psa_hash_update(&h.op, h.buffer.data(), n) != PSA_SUCCESS) {
            finish_slot(false, "it could not be read to the end");
            return;
        }
        h.at += n;
    }
    if (h.at < h.end) {
        return;
    }
    std::array<std::uint8_t, 32> digest{};
    std::size_t written = 0;
    const bool hashed = psa_hash_finish(&h.op, digest.data(), digest.size(), &written) == PSA_SUCCESS;
    const bool intact = hashed && digest == h.stored;
    finish_slot(intact, intact   ? ""
                        : hashed ? "its bytes no longer match its SHA-256"
                                 : "the board could not finish a SHA-256");
}

// Records what the check found about the slot it has just read, and moves on
// to the other slot or ends.
void Firmware::Impl::finish_slot(bool intact, const char* why) {
    const SlotHashing& h = *hashing;
    bool recorded = true;
    {
        const std::lock_guard lock(mutex);
        if (h.is_running) {
            running_intact = intact;
            running_sha = h.stored;
        } else if (h.generation == other_generation) {
            other_intact = intact;
            other_sha = h.stored;
        } else {
            recorded = false;  // an upload has erased the slot since
        }
    }
    if (recorded && !intact) {
        std::printf("firmware: the image in %s does not check out (%s)%s\n", h.slot->label, why,
                    h.is_running ? "" : "; it cannot be gone back to");
    }
    if (h.is_running) {
        (void)psa_hash_abort(&hashing->op);
        hashing = begin_other();
        if (hashing) {
            return;
        }
    }
    end_check();
}

void Firmware::Impl::end_check() {
    if (hashing) {
        (void)psa_hash_abort(&hashing->op);
        hashing.reset();
    }
    (void)esp_timer_stop(check_timer);
    check_running = false;
}

// --- the upload --------------------------------------------------------------------

Firmware::Impl::Outcome Firmware::Impl::run_upload(UploadJob& job) {
    Outcome out;
    {
        const std::lock_guard lock(mutex);
        upload = FirmwareUpload{0, job.length, "waiting"};
    }
    enter_flash_mode();
    if (idle_timer != nullptr) {
        (void)esp_timer_stop(idle_timer);
    }
    // The least free internal heap from here, with nothing playing, to the
    // upload's end: upload_task reads it and stops the monitor, which the
    // player uses for a stream and has let go of by now.
    (void)heap_caps_monitor_local_minimum_free_size_start();

    auto* buffer = static_cast<std::uint8_t*>(
        heap_caps_malloc(config.buffer_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
        out.body = "the board could not allocate a receive buffer";
        return out;
    }
    const std::span<std::uint8_t> buf(buffer, config.buffer_bytes);
    struct Freer {
        std::uint8_t* p;
        ~Freer() { heap_caps_free(p); }
    } freer{buffer};

    // Reads exactly `want` bytes into `into`, riding out a few timeouts.
    std::size_t received = 0;
    const auto receive = [&](std::span<std::uint8_t> into) -> bool {
        std::size_t got = 0;
        int stalls = 0;
        while (got < into.size()) {
            const int n = httpd_req_recv(job.req, reinterpret_cast<char*>(into.data() + got), into.size() - got);
            if (n == HTTPD_SOCK_ERR_TIMEOUT && ++stalls < kStalledReceives) {
                continue;
            }
            if (n <= 0) {
                return false;
            }
            stalls = 0;
            got += static_cast<std::size_t>(n);
        }
        received += got;
        return true;
    };

    // A refusal made on the head alone still reads the rest of the body, so
    // that a client still sending hears the answer rather than a reset:
    // esp_http_server purges what a handler leaves unread, but not what an
    // asynchronous request leaves (httpd_req_async_handler_complete).
    const auto drain = [&] {
        while (received < job.length) {
            if (!receive(buf.first(std::min(buf.size(), job.length - received)))) {
                return;
            }
        }
    };

    std::array<std::uint8_t, kImageHeadBytes> head_bytes{};
    if (!receive(head_bytes)) {
        out.status = "400 Bad Request";
        out.body = "the upload ended before the image's header did";
        return out;
    }
    const ParsedHead parsed = parse_image_head(head_bytes);
    if (!parsed.head) {
        drain();
        out.status = "400 Bad Request";
        out.body = parsed.why;
        return out;
    }
    const ImageHead& head = *parsed.head;
    out.version = head.version;
    if (const auto why = refuse_image(head, board())) {
        drain();
        out.status = "400 Bad Request";
        out.body = *why;
        return out;
    }

    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS ||
        psa_hash_update(&hash, head_bytes.data(), head_bytes.size()) != PSA_SUCCESS) {
        (void)psa_hash_abort(&hash);
        out.body = "the board could not start a SHA-256";
        return out;
    }

    {
        const std::lock_guard lock(mutex);
        upload = FirmwareUpload{received, job.length, "erasing"};
    }
    esp_ota_handle_t ota = 0;
    const esp_err_t begun = esp_ota_begin(other, job.length, &ota);
    if (begun != ESP_OK) {
        (void)psa_hash_abort(&hash);
        out.status = begun == ESP_ERR_OTA_ROLLBACK_INVALID_STATE ? "409 Conflict" : "500 Internal Server Error";
        out.body = std::string("the slot could not be prepared: ") + esp_err_to_name(begun);
        return out;
    }
    out.erased = true;
    {
        // Erased: what the check found there no longer describes it.
        const std::lock_guard lock(mutex);
        ++other_generation;
        other_sha.reset();
        other_intact = false;
    }
    bool written = esp_ota_write(ota, head_bytes.data(), head_bytes.size()) == ESP_OK;
    {
        const std::lock_guard lock(mutex);
        upload = FirmwareUpload{received, job.length, "writing"};
    }
    while (written && received < job.length) {
        const std::span<std::uint8_t> chunk = buf.first(std::min(buf.size(), job.length - received));
        if (!receive(chunk)) {
            (void)esp_ota_abort(ota);
            (void)psa_hash_abort(&hash);
            out.status = "400 Bad Request";
            out.body = "the upload stopped after " + std::to_string(received) + " of " +
                       std::to_string(job.length) + " bytes";
            return out;
        }
        written = psa_hash_update(&hash, chunk.data(), chunk.size()) == PSA_SUCCESS &&
                  esp_ota_write(ota, chunk.data(), chunk.size()) == ESP_OK;
        const std::lock_guard lock(mutex);
        upload->received = received;
    }
    std::array<std::uint8_t, 32> body_sha{};
    std::size_t sha_length = 0;
    if (!written || psa_hash_finish(&hash, body_sha.data(), body_sha.size(), &sha_length) != PSA_SUCCESS) {
        (void)esp_ota_abort(ota);
        (void)psa_hash_abort(&hash);
        out.body = "writing the slot failed after " + std::to_string(received) + " bytes";
        return out;
    }
    if (job.digest.kind == ContentDigest::Kind::kSha256 && body_sha != job.digest.sha256) {
        (void)esp_ota_abort(ota);
        out.status = "400 Bad Request";
        out.body = "the image was damaged on the way: its SHA-256 is " + hex(body_sha) +
                   ", and the request's Content-Digest says " + hex(job.digest.sha256);
        return out;
    }

    {
        const std::lock_guard lock(mutex);
        upload->stage = "checking";
    }
    // ESP-IDF reads the image back from flash and checks its checksum, its
    // own SHA-256, the chip and the revision range (esp_image_verify).
    const esp_err_t ended = esp_ota_end(ota);
    if (ended != ESP_OK) {
        out.status = ended == ESP_ERR_OTA_VALIDATE_FAILED ? "400 Bad Request" : "500 Internal Server Error";
        out.body = ended == ESP_ERR_OTA_VALIDATE_FAILED
                       ? "the image in flash does not check out (its own SHA-256, or its layout): it was "
                         "damaged before it was sent, or on its way into flash"
                       : std::string("finishing the slot failed: ") + esp_err_to_name(ended);
        return out;
    }
    // And every byte sent, the image's and whatever follows it, as it now
    // lies in the slot.
    const auto in_flash = hash_slot(other, job.length, buf);
    if (!in_flash || *in_flash != body_sha) {
        out.status = "400 Bad Request";
        out.body = "what reached flash is not what was sent: read back, its SHA-256 is " +
                   (in_flash ? hex(*in_flash) : std::string("unreadable")) + ", not " + hex(body_sha);
        return out;
    }
    const esp_err_t selected = esp_ota_set_boot_partition(other);
    if (selected != ESP_OK) {
        out.body = std::string("the slot could not be made the next boot: ") + esp_err_to_name(selected);
        return out;
    }
    nvs_set_texts({{kKeyPending, hex(head.elf_sha256)}, {kKeyPendingVersion, head.version}, {kKeyWhy, ""}});
    {
        // Until the restart, GET /firmware says one is coming; last_update
        // is still the update before this one.
        const std::lock_guard lock(mutex);
        upload->stage = "restarting";
    }
    out.status = "200 OK";
    out.json = true;
    std::string& body = out.body;
    body = "{";
    detail::append_json_text(body, "version");
    body += ':';
    detail::append_json_text(body, head.version);
    body += ",\"slot\":";
    detail::append_json_text(body, other->label);
    body += ",\"sha256\":";
    detail::append_json_text(body, hex(body_sha));
    body += ",\"restarting\":true}\n";
    out.restart = true;
    std::printf("firmware: %s written to %s and checked (%u bytes, stack %u spare)\n", head.version.c_str(),
                other->label, static_cast<unsigned>(job.length),
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    return out;
}

void Firmware::Impl::upload_task(void* arg) {
    auto* job = static_cast<UploadJob*>(arg);
    Impl& im = *job->im;
    const Outcome out = im.run_upload(*job);
    std::printf("firmware: the upload's least free internal heap was %u bytes\n",
                static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    (void)heap_caps_monitor_local_minimum_free_size_stop();
    if (!out.restart) {
        // Settled before the answer goes out: a client that sends its next
        // request as soon as it has read this answer finds the board free.
        // A 4xx is the image or the request, a 5xx the board.
        const char* result = out.status[0] == '4' ? "refused" : "failed";
        std::printf("firmware: upload %s (%s): %s\n", result, out.status, out.body.c_str());
        record_last(out.version, result, out.body);
        // What the erased slot holds now: nothing, or the head of an image
        // that stopped part-way or did not check out.
        std::optional<SlotFacts> theirs;
        if (out.erased) {
            theirs = read_slot(im.other, false);
        }
        {
            const std::lock_guard lock(im.mutex);
            im.busy = false;
            im.upload.reset();
            im.last_update = FirmwareLastUpdate{out.version, result, out.body};
            if (theirs) {
                im.other_facts = *theirs;
            }
        }
        // Still in flash mode: a corrected image can follow, and the idle
        // timer starts again from here.
        im.arm_idle_timer();
    }
    httpd_resp_set_hdr(job->req, "Connection", "close");
    if (out.json) {
        (void)reply(job->req, out.status, "application/json", out.body);
    } else {
        (void)reply_text(job->req, out.status, out.body);
    }
    (void)httpd_req_async_handler_complete(job->req);
    delete job;
    if (out.restart) {
        im.restart_now("into the new image, on trial");
    }
    vTaskDelete(nullptr);
}

// --- Firmware ------------------------------------------------------------------------

Firmware::Firmware() = default;

Firmware::~Firmware() {
    if (impl_ == nullptr) {
        return;
    }
    if (impl_->idle_timer != nullptr) {
        (void)esp_timer_stop(impl_->idle_timer);
        (void)esp_timer_delete(impl_->idle_timer);
    }
    if (impl_->guard_timer != nullptr) {
        (void)esp_timer_stop(impl_->guard_timer);
        (void)esp_timer_delete(impl_->guard_timer);
    }
    if (impl_->trial_timer != nullptr) {
        (void)esp_timer_stop(impl_->trial_timer);
        (void)esp_timer_delete(impl_->trial_timer);
    }
    if (impl_->check_timer != nullptr) {
        impl_->stop_check();
        (void)esp_timer_delete(impl_->check_timer);
    }
    delete impl_;
}

bool Firmware::start(FirmwareHooks hooks, FirmwareConfig config) {
    if (impl_ != nullptr) {
        return true;
    }
    auto* im = new Impl{};
    im->hooks = std::move(hooks);
    im->config = config;
    im->running = esp_ota_get_running_partition();
    im->other = esp_ota_get_next_update_partition(nullptr);
    if (im->running == nullptr) {
        std::printf("firmware: no running partition found; this board takes no updates\n");
        delete im;
        return false;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        std::printf("firmware: the PSA crypto API did not start; this board takes no updates\n");
        delete im;
        return false;
    }

    std::array<std::uint8_t, 4> header{};
    if (esp_partition_read(im->running, 0, header.data(), header.size()) == ESP_OK) {
        im->flash_size_code = static_cast<std::uint8_t>(header[3] >> 4);
    }
    // The size this layout was built for, from the running image's header
    // (not the chip's own size, esp_flash_get_physical_size): what an
    // update's image has to match, and what ota.py holds it to.
    std::uint32_t flash_size = 0;
    if (esp_flash_get_size(nullptr, &flash_size) == ESP_OK) {
        im->flash_bytes = flash_size;
    }
    esp_bootloader_desc_t boot{};
    if (esp_ota_get_bootloader_description(nullptr, &boot) == ESP_OK) {
        im->bootloader_version = field_text(boot.idf_ver);
    }
    im->read_last_update();
    // Before any task of this or the board's starts, so read without the lock.
    im->coredump = read_coredump();
    im->running_facts = read_slot(im->running, true);
    if (im->other != nullptr) {
        im->other_facts = read_slot(im->other, false);
    }

    const esp_timer_create_args_t check_args = {.callback = &Impl::on_check,
                                                .arg = im,
                                                .dispatch_method = ESP_TIMER_TASK,
                                                .name = "fw_check",
                                                .skip_unhandled_events = true};
    (void)esp_timer_create(&check_args, &im->check_timer);
    const esp_timer_create_args_t idle_args = {.callback = &Impl::on_idle,
                                               .arg = im,
                                               .dispatch_method = ESP_TIMER_TASK,
                                               .name = "fw_idle",
                                               .skip_unhandled_events = true};
    (void)esp_timer_create(&idle_args, &im->idle_timer);

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(im->running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        std::printf("firmware: %s is on trial: %u s to be healthy for %u s without a break\n",
                    field_text(esp_app_get_description()->version).c_str(),
                    static_cast<unsigned>(config.trial.deadline_ms / 1000),
                    static_cast<unsigned>(config.trial.hold_ms / 1000));
        if (config.test_panic_at_trial) {
            std::printf("firmware: AC3FORGE_FIRMWARE_TEST_PANIC_ON_TRIAL - panicking so the bootloader goes back\n");
            std::abort();
        }
        im->trial.emplace(config.trial, now_ms());
        const esp_timer_create_args_t guard_args = {.callback = &Impl::on_guard,
                                                    .arg = im,
                                                    .dispatch_method = ESP_TIMER_TASK,
                                                    .name = "fw_guard",
                                                    .skip_unhandled_events = true};
        if (esp_timer_create(&guard_args, &im->guard_timer) == ESP_OK) {
            (void)esp_timer_start_once(im->guard_timer,
                                       (static_cast<std::uint64_t>(config.trial.deadline_ms) + 30'000) * 1000);
        }
        const esp_timer_create_args_t trial_args = {.callback = &Impl::on_trial,
                                                    .arg = im,
                                                    .dispatch_method = ESP_TIMER_TASK,
                                                    .name = "fw_trial",
                                                    .skip_unhandled_events = true};
        if (esp_timer_create(&trial_args, &im->trial_timer) != ESP_OK ||
            esp_timer_start_periodic(im->trial_timer, 1'000'000) != ESP_OK) {
            std::printf("firmware: could not start the trial; the guard goes back at the deadline\n");
        }
    } else {
        // On trial, accept_trial() starts it.
        im->start_check();
    }
    impl_ = im;
    std::printf("firmware: running %s from %s; %s\n", field_text(esp_app_get_description()->version).c_str(),
                im->running->label,
                im->other != nullptr ? "updates go to the other slot" : "one app slot, so no updates over the network");
    return true;
}

bool Firmware::flash_mode() const {
    if (impl_ == nullptr) {
        return false;
    }
    const std::lock_guard lock(impl_->mutex);
    return impl_->flash_mode;
}

void Firmware::restart(const char* why) {
    if (impl_ == nullptr) {
        std::printf("firmware: restarting %s\n", why);
        esp_restart();
    }
    Impl& im = *impl_;
    bool on_trial = false;
    {
        const std::lock_guard lock(im.mutex);
        on_trial = im.trial.has_value();
    }
    if (on_trial) {
        // The image that runs next reads this as why this one went back.
        nvs_set_texts({{kKeyWhy, why}});
    }
    im.restart_now(why);
}

int Firmware::on_status(httpd_req* req) {
    if (impl_ == nullptr) {
        return reply_text(req, "404 Not Found", "this board takes no firmware updates");
    }
    return reply(req, "200 OK", "application/json", render_firmware_status(impl_->status()));
}

int Firmware::on_upload(httpd_req* req) {
    if (impl_ == nullptr) {
        return reply_text(req, "404 Not Found", "this board takes no firmware updates");
    }
    Impl& im = *impl_;
    if (!im.host_allowed(req)) {
        return reply_text(req, "403 Forbidden",
                          "firmware changes are taken only on the board's own address or name (planning/esp32-ota.md)");
    }
    if (im.other == nullptr) {
        return reply_text(req, "409 Conflict",
                          "this board's partition table has one app slot: move it to the two-slot table over USB once");
    }
    if (req->content_len <= 0) {
        return reply_text(req, "411 Length Required", "PUT /firmware wants the image as the body, with its length");
    }
    const auto length = static_cast<std::size_t>(req->content_len);
    if (length < kImageHeadBytes) {
        return reply_text(req, "400 Bad Request", "that body is too short to be an application image");
    }
    if (length > im.other->size) {
        return reply_text(req, "413 Payload Too Large",
                          "that image is " + std::to_string(length) + " bytes, and the slot holds " +
                              std::to_string(im.other->size));
    }
    // None at all is fine: `curl -T` sends none. What a form or a page's
    // fetch sends by default is not.
    const std::string type = request_header(req, "Content-Type");
    if (!type.empty() && type != "application/octet-stream") {
        return reply_text(req, "415 Unsupported Media Type",
                          "PUT /firmware wants the image as application/octet-stream, or no Content-Type at all");
    }
    const ContentDigest digest = parse_content_digest(request_header(req, "Content-Digest"));
    if (digest.kind == ContentDigest::Kind::kMalformed) {
        return reply_text(req, "400 Bad Request", "the Content-Digest header's sha-256 is not 32 bytes of base64");
    }
    {
        const std::lock_guard lock(im.mutex);
        if (im.trial) {
            return reply_text(req, "409 Conflict",
                              "the running image is still on trial; wait until it is accepted (GET /firmware)");
        }
        if (im.busy) {
            return reply_text(req, "409 Conflict", "an update is already under way");
        }
        im.busy = true;
    }
    auto* job = new Impl::UploadJob{&im, nullptr, length, digest};
    if (httpd_req_async_handler_begin(req, &job->req) != ESP_OK ||
        xTaskCreate(&Impl::upload_task, "fw_upload", static_cast<std::uint32_t>(im.config.task_stack_bytes), job,
                    tskIDLE_PRIORITY + 5, nullptr) != pdPASS) {
        if (job->req != nullptr) {
            (void)httpd_req_async_handler_complete(job->req);
        }
        delete job;
        const std::lock_guard lock(im.mutex);
        im.busy = false;
        return reply_text(req, "500 Internal Server Error", "the board could not start the upload's task");
    }
    return ESP_OK;
}

int Firmware::on_mode(httpd_req* req) {
    if (impl_ == nullptr) {
        return reply_text(req, "404 Not Found", "this board takes no firmware updates");
    }
    Impl& im = *impl_;
    if (!im.host_allowed(req)) {
        return reply_text(req, "403 Forbidden",
                          "firmware changes are taken only on the board's own address or name (planning/esp32-ota.md)");
    }
    const std::string body = short_body(req);
    if (body != "flash" && body != "normal") {
        return reply_text(req, "400 Bad Request", "PUT /firmware/mode wants flash or normal");
    }
    bool in_flash_mode = false;
    {
        const std::lock_guard lock(im.mutex);
        if (im.trial) {
            return reply_text(req, "409 Conflict",
                              "the running image is still on trial; wait until it is accepted (GET /firmware)");
        }
        if (im.busy) {
            return reply_text(req, "409 Conflict", "an update is already under way");
        }
        in_flash_mode = im.flash_mode;
    }
    if (body == "flash") {
        im.enter_flash_mode();
        return reply_text(req, "200 OK", "flash mode: nothing plays until the board restarts");
    }
    if (!in_flash_mode) {
        return reply_text(req, "200 OK", "not in flash mode");
    }
    (void)reply_text(req, "200 OK", "restarting into the image that runs now");
    im.restart_now("to leave flash mode");
}

int Firmware::on_rollback(httpd_req* req) {
    if (impl_ == nullptr) {
        return reply_text(req, "404 Not Found", "this board takes no firmware updates");
    }
    Impl& im = *impl_;
    if (!im.host_allowed(req)) {
        return reply_text(req, "403 Forbidden",
                          "firmware changes are taken only on the board's own address or name (planning/esp32-ota.md)");
    }
    bool on_trial = false;
    {
        const std::lock_guard lock(im.mutex);
        if (im.busy) {
            return reply_text(req, "409 Conflict", "an update is already under way");
        }
        on_trial = im.trial.has_value();
        im.busy = on_trial;
    }
    if (on_trial) {
        // On trial, going back is giving up: the bootloader boots the image
        // before this one.
        (void)reply_text(req, "200 OK", "giving up this image's trial; going back");
        nvs_set_texts({{kKeyWhy, "rolled back by request during its trial"}});
        if (im.hooks.before_restart) {
            im.hooks.before_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        (void)esp_ota_mark_app_invalid_rollback_and_reboot();
        esp_restart();
    }
    if (im.other == nullptr) {
        return reply_text(req, "409 Conflict", "this board has one app slot, so there is nothing to go back to");
    }
    FirmwareSlot theirs;
    std::optional<bool> intact;
    {
        const std::lock_guard lock(im.mutex);
        theirs = report_slot(im.other, im.other_facts);
        intact = im.other_intact;
    }
    const bool has_image = theirs.state != "empty";
    const bool usable = has_image && theirs.state != "invalid" && theirs.state != "aborted";
    if (!usable || intact == std::optional<bool>(false)) {
        return reply_text(req, "409 Conflict",
                          std::string("there is no image to go back to in ") + im.other->label +
                              (has_image ? ": it failed its trial or does not check out" : ": it is empty"));
    }
    // esp_ota_set_boot_partition checks the image again, and makes it the
    // next boot, on trial like any image an update writes.
    im.stop_check();
    const esp_err_t selected = esp_ota_set_boot_partition(im.other);
    if (selected != ESP_OK) {
        return reply_text(req, "409 Conflict",
                          std::string("the image in ") + im.other->label + " cannot boot: " + esp_err_to_name(selected));
    }
    const std::string& version = theirs.version;
    nvs_set_texts({{kKeyPending, theirs.elf_sha256}, {kKeyPendingVersion, version}, {kKeyWhy, ""}});
    record_last(version, "rollback requested", "");
    (void)reply_text(req, "200 OK", "going back to " + version + "; the board restarts into it, on trial");
    im.restart_now("into the image before this one");
}

int Firmware::on_restart(httpd_req* req) {
    if (impl_ == nullptr) {
        return reply_text(req, "404 Not Found", "this board takes no firmware updates");
    }
    Impl& im = *impl_;
    {
        const std::lock_guard lock(im.mutex);
        if (im.trial) {
            return reply_text(req, "409 Conflict",
                              "the running image is still on trial, and a restart now goes back to the image "
                              "before it; to do that, PUT /firmware/rollback");
        }
        if (im.busy) {
            return reply_text(req, "409 Conflict", "an update is under way");
        }
        im.busy = true;
    }
    (void)reply_text(req, "200 OK", "restarting");
    im.restart_now("on request");
}

int Firmware::on_coredump(httpd_req* req) {
    if (impl_ == nullptr) {
        return reply_text(req, "404 Not Found", "this board takes no firmware updates");
    }
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    std::size_t address = 0;
    std::size_t size = 0;
    if (esp_core_dump_image_get(&address, &size) != ESP_OK) {
        return reply_text(req, "404 Not Found", "there is no core dump: nothing has crashed since the last was erased");
    }
    // As it lies in flash, a kilobyte at a time from the server's own stack,
    // for esp_coredump (`info_corefile --core-format raw`) with the ELF of
    // the image that wrote it.
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"coredump.bin\"");
    std::array<char, 1024> chunk{};
    for (std::size_t at = 0; at < size;) {
        const std::size_t n = std::min(chunk.size(), size - at);
        if (esp_flash_read(nullptr, chunk.data(), static_cast<std::uint32_t>(address + at), n) != ESP_OK ||
            httpd_resp_send_chunk(req, chunk.data(), static_cast<ssize_t>(n)) != ESP_OK) {
            return ESP_FAIL;
        }
        at += n;
    }
    return httpd_resp_send_chunk(req, nullptr, 0);
#else
    return reply_text(req, "404 Not Found", "this board keeps no core dumps");
#endif
}

int Firmware::on_coredump_erase(httpd_req* req) {
    if (impl_ == nullptr) {
        return reply_text(req, "404 Not Found", "this board takes no firmware updates");
    }
    Impl& im = *impl_;
    if (!im.host_allowed(req)) {
        return reply_text(req, "403 Forbidden",
                          "firmware changes are taken only on the board's own address or name (planning/esp32-ota.md)");
    }
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    {
        const std::lock_guard lock(im.mutex);
        if (im.busy) {
            return reply_text(req, "409 Conflict", "an update is already under way");
        }
        im.busy = true;
    }
    // An erase writes flash, which the slot check keeps clear of; it starts
    // again afterwards if it was running.
    const bool checking = im.check_running.load();
    im.stop_check();
    const esp_err_t erased = esp_core_dump_image_erase();
    {
        const std::lock_guard lock(im.mutex);
        im.busy = false;
        if (erased == ESP_OK) {
            im.coredump.reset();
        }
    }
    if (checking) {
        im.start_check();
    }
    if (erased != ESP_OK) {
        return reply_text(req, "500 Internal Server Error",
                          std::string("the core dump could not be erased: ") + esp_err_to_name(erased));
    }
    return reply_text(req, "200 OK", "erased");
#else
    return reply_text(req, "404 Not Found", "this board keeps no core dumps");
#endif
}

}  // namespace ac3forge
