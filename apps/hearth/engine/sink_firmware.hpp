#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ac3forge/firmware_image.hpp"
#include "ac3forge/firmware_status.hpp"

// A Hearth sink's firmware, from this computer (planning/esp32-ota.md, O5).
//
// The board's own HTTP routes on port 80 - GET /hardware and GET /firmware,
// PUT /firmware, PUT /firmware/rollback and POST /restart - which
// tools/hearth/ota.py and the board's own page use as well. Nothing here goes
// through Sendspin: an update stops the board's Sendspin player and restarts
// the board, and the Network page has to go on following it through that.
//
// GET /firmware's answer is read into ac3forge::FirmwareStatus, the struct the
// board renders it from (esp-idf/ac3forge/include/ac3forge/
// firmware_status.hpp), and an image is held to the board's own rules
// (firmware_image.hpp) before a byte of it is sent. The rest is ota.py's push
// in C++: the checks it makes before an upload, the upload with its
// Content-Digest, and how it reads the board's answers through the restart and
// the trial. Its words too, where the page shows them, so that the tool, the
// board's page and this app describe one update the same way.

namespace ac3::hearth {

// GET /hardware, as much of it as an update needs.
struct SinkHardware {
    std::string target{};    // "esp32s3": what the running image was built for
    std::string chip{};      // "ESP32-S3": what runs it
    std::string revision{};  // "0.2"
    std::string project{};   // "ac3forge_hearth_sink"
    std::string version{};
};

[[nodiscard]] std::optional<SinkHardware> parse_sink_hardware(std::string_view json);

// 1665792 as "1,665,792", without the process locale, which Qt sets from the
// user's.
[[nodiscard]] std::string grouped_number(std::uint64_t value);

// A trial in ota.py's words: "healthy for 12s of 30s, waiting for a network
// address (274s left)".
[[nodiscard]] std::string trial_words(const ac3forge::FirmwareTrial& trial);

// Nothing when `json` is not a GET /firmware answer at all. Keys a board does
// not send are left at their defaults: firmware from before O4 has no
// `coredump`, and firmware_status.hpp says every other key is always there.
[[nodiscard]] std::optional<ac3forge::FirmwareStatus> parse_firmware_status(std::string_view json);

// An application image, read and checked on this computer.
struct FirmwareFile {
    std::vector<std::uint8_t> data{};
    ac3forge::ImageHead head{};
    // Of the whole file: the upload's Content-Digest, which the board checks
    // against what it received.
    std::array<std::uint8_t, 32> file_sha256{};
    // Hex: the SHA-256 the build appended to the image, which the board
    // reports as the running slot's image_sha256 once it has checked it.
    std::string image_sha256{};
    std::string elf_sha256{};  // hex, from the image's description
};

struct ReadFirmwareFile {
    std::optional<FirmwareFile> file = std::nullopt;
    std::string why{};  // why `bytes` is not an image to send, when it is not
};

// Walks the image's segments as the bootloader does, and checks its checksum
// and the SHA-256 the build appended (ota.py's read_image): a file that is
// not an application image, or is damaged, is found here rather than by a
// board that has already stopped playing to take it.
[[nodiscard]] ReadFirmwareFile read_firmware_file(std::vector<std::uint8_t> bytes);

// Why this sink must not take `file`, in ota.py's words, or nothing when it
// may: another chip or a revision the image excludes, another project, the
// board's slot too small or its flash size another, the board on trial or
// busy with another update, one app slot, or a network the board keeps only
// in its running image (a bare .bin cannot say whether it has one).
[[nodiscard]] std::optional<std::string> refuse_update(const FirmwareFile& file, const SinkHardware& hardware,
                                                       const ac3forge::FirmwareStatus& firmware);

enum class UpdateOutcome : std::uint8_t {
    kNone,        // not decided yet
    kUpdated,     // runs the new image, accepted
    kRolledBack,  // tried it, and went back to the image before
    kRefused,     // would not take it: the file, or the board's state
    kFailed,      // the upload broke off, or the board failed
    kSilent,      // did not say how it ended before the wait ran out
};

// Where the wait after an upload has got to, from one GET /firmware answer or
// none (ota.py's wait_for, a poll at a time).
struct WaitVerdict {
    UpdateOutcome outcome = UpdateOutcome::kNone;
    std::string text{};  // what the board is doing, or how it ended
    // The new image runs, accepted, and the board has not yet reported the
    // SHA-256 its own check of the slot found: the wait goes on a little
    // (SinkFirmwareTiming::sha_wait) so that the outcome can compare it with
    // the file's.
    bool accepted = false;
};

struct WaitContext {
    // The slot the upload's answer said it wrote; empty when the answer was
    // lost, when any slot the image runs from counts.
    std::string slot{};
    // The upload's answer never came: a board in flash mode with no upload
    // running then refused the image, and one that still reports
    // `last_before` never had it.
    bool reply_lost = false;
    std::optional<ac3forge::FirmwareLastUpdate> last_before = std::nullopt;
    // The wait for the board's check of the slot is over: an accepted image
    // is an update whether or not the check has reported.
    bool sha_wait_over = false;
};

[[nodiscard]] WaitVerdict judge_wait(const ac3forge::FirmwareStatus* firmware, const FirmwareFile& file,
                                     const WaitContext& context);

// What to say when the wait ran out, from the last answer there was.
[[nodiscard]] std::string silent_text(const ac3forge::FirmwareStatus* last, const FirmwareFile& file,
                                      const WaitContext& context, std::chrono::seconds waited);

// How often and how long SinkFirmware asks; the defaults are ota.py's.
struct SinkFirmwareTiming {
    std::chrono::milliseconds poll{1000};      // GET /firmware while watched
    std::chrono::milliseconds wait_poll{1500};  // through the restart and the trial
    std::chrono::seconds wait{360};            // for the board to decide
    // Once it has accepted the image, for its check of the slot to report.
    std::chrono::seconds sha_wait{20};
    // After an upload broke off, for the board to say why it gave up; it
    // gives up on a stalled upload after 30 s.
    std::chrono::seconds refusal_wait{40};
    std::chrono::seconds request{4};           // a GET's connect and read
    // The upload's answer comes once the board has read the slot back.
    std::chrono::seconds upload_answer{120};
};

// One sink's firmware routes, from a thread of its own: an upload takes tens
// of seconds and the wait after it minutes, and none of it may hold up the
// window. Everything the Network page shows comes from snapshot().
class SinkFirmware {
   public:
    // `host` is the address mDNS gave for the sink; its web server is on
    // `port`, 80 on every board.
    explicit SinkFirmware(std::string host, std::uint16_t port = 80, SinkFirmwareTiming timing = {});
    // Stops the thread. An upload under way is cut off (the board refuses
    // what it got and stays in flash mode until it restarts); a wait just
    // stops, and the board decides about its image by itself. Returns within
    // about a second: a request the thread is inside when this runs - an
    // upload's answer can take two minutes to come - ends on its own
    // timeout, with the thread let go to finish it, since on Windows nothing
    // wakes a socket's wait from another thread (shutdown() does not).
    ~SinkFirmware();
    SinkFirmware(const SinkFirmware&) = delete;
    SinkFirmware& operator=(const SinkFirmware&) = delete;

    // An update from this computer, as far as it has got.
    struct Update {
        std::string version{};  // the image's
        std::string target{};   // the image's chip, as the board names targets
        // "checking", "sending", "answering" (the board reading back what it
        // wrote), "waiting" (the restart and the trial), then "done".
        std::string stage{};
        std::size_t sent = 0;
        std::size_t total = 0;
        UpdateOutcome outcome = UpdateOutcome::kNone;
        std::string text{};  // the latest thing to say about it
    };

    struct Snapshot {
        std::string host{};
        std::uint16_t port = 80;
        bool asked = false;     // GET /firmware has been sent at least once
        bool answering = false;  // the last one was answered
        // The last one's failure: no answer, or not firmware this app can
        // update (a 404 from a board without O1).
        std::string error{};
        std::optional<SinkHardware> hardware = std::nullopt;
        std::optional<ac3forge::FirmwareStatus> firmware = std::nullopt;
        std::optional<Update> update = std::nullopt;
        // The board's answer to the last restart or rollback asked for here.
        std::string action{};
        // Changes whenever anything above does, so a reader can skip the rest.
        std::uint64_t generation = 0;
    };
    [[nodiscard]] Snapshot snapshot() const;

    // Reads GET /hardware once and GET /firmware at `timing.poll` while true.
    void set_watching(bool watching);

    // Sends `file` and follows it to the end, on the thread: GET /hardware
    // and /firmware first, refuse_update() against both, PUT /firmware, then
    // judge_wait() on each answer. False when an update or an action is
    // already under way here, when nothing starts.
    bool start_update(FirmwareFile file);
    // PUT /firmware/rollback and POST /restart. False while an update or
    // another action is under way.
    bool rollback();
    bool restart();

    // Whether an update or an action is under way.
    [[nodiscard]] bool busy() const;

   private:
    // Everything the thread uses, shared with it so that it can outlive this
    // object (sink_firmware.cpp).
    class Worker;
    std::shared_ptr<Worker> worker_;
    std::thread thread_;
};

}  // namespace ac3::hearth
