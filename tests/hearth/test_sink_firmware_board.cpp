// ac3::hearth::SinkFirmware against a stand-in board on 127.0.0.1: the
// requests it makes, and how it follows a board through an update's restart
// and trial to each way one can end. The stand-in answers GET /firmware with
// the board's own renderer (ac3forge/firmware_status.hpp), so the client
// reads what a board sends. Nothing here leaves the loopback interface.

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac3/sendspin/base64.hpp"
#include "ac3forge/firmware_image.hpp"
#include "ac3forge/firmware_status.hpp"
#include "sink_firmware.hpp"
#include "sink_firmware_images.hpp"
#include "sink_firmware_view.hpp"

using ac3::hearth::SinkFirmware;
using ac3::hearth::UpdateOutcome;
using sink_firmware_test::ImageSpec;
using namespace std::chrono_literals;

namespace {

constexpr ac3::hearth::SinkFirmwareTiming kFast{
    .poll = 50ms,
    .wait_poll = 50ms,
    .wait = 10s,
    .sha_wait = 2s,
    .refusal_wait = 1s,
    .request = 2s,
    .upload_answer = 10s,
};

template <class Predicate>
bool eventually(Predicate predicate, std::chrono::milliseconds timeout = 15s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(20ms);
    }
    return predicate();
}

ac3forge::FirmwareSlot slot(std::string label, std::string state, std::string version, const ImageSpec& spec) {
    ac3forge::FirmwareSlot out;
    out.label = std::move(label);
    out.state = std::move(state);
    out.version = std::move(version);
    out.project = "ac3forge_hearth_sink";
    out.elf_sha256 = sink_firmware_test::elf_hex(spec);
    out.intact = true;
    return out;
}

// The routes ac3forge::Firmware serves, modelled as far as SinkFirmware uses
// them. After an upload it answers two reads in flash mode and one with a 503,
// as a board restarting would not answer, then plays `after_upload` out.
class FakeBoard {
   public:
    enum class AfterUpload : std::uint8_t { kAccept, kRollBack, kStayInFlashMode };

    FakeBoard() {
        ImageSpec running;
        status_.running = slot("ota_0", "valid", "v0.10.0", running);
        status_.running->image_sha256 = "aa";
        ImageSpec older = running;
        older.elf_seed = 90;
        status_.other = slot("ota_1", "valid", "v0.9.0", older);
        status_.network = "stored";
        status_.slot_bytes = 4 * 1024 * 1024;
        status_.flash_bytes = 16 * 1024 * 1024;

        server_.Get("/hardware", [](const httplib::Request&, httplib::Response& response) {
            response.set_content(R"({"target":"esp32s3","chip":"ESP32-S3","revision":"0.2",)"
                                 R"("project":"ac3forge_hearth_sink","version":"v0.10.0"})",
                                 "application/json");
        });
        server_.Get("/firmware", [this](const httplib::Request&, httplib::Response& response) {
            const std::lock_guard<std::mutex> lock(mutex_);
            ++firmware_reads_;
            if (step_ == 3) {
                ++step_;
                response.status = 503;  // restarting: nothing answers
                return;
            }
            if (step_ > 0 && step_ < 3) {
                ++step_;
            } else if (step_ == 4) {
                restarted_locked();
                ++step_;
            } else if (step_ >= 5 && after_upload_ == AfterUpload::kAccept && ++trial_reads_ == 2) {
                status_.running->state = "valid";
                status_.trial.reset();
                status_.running->intact = true;
                status_.running->image_sha256 = uploaded_image_sha256_;
                status_.last_update = ac3forge::FirmwareLastUpdate{
                    .version = status_.running->version, .result = "accepted", .reason = ""};
            }
            response.set_content(ac3forge::render_firmware_status(status_), "application/json");
        });
        server_.Put("/firmware", [this](const httplib::Request& request, httplib::Response& response,
                                        const httplib::ContentReader& content_reader) {
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                ++uploads_;
                if (drops_ > 0) {
                    // Cut off: none of the image is read, and the connection
                    // closes under a client still sending it, which finds it
                    // reset. What that did to the board is the test's to say.
                    --drops_;
                    if (after_drop_) {
                        after_drop_(status_);
                    }
                    response.status = 503;
                    response.set_header("Connection", "close");
                    return;
                }
            }
            std::string received;
            content_reader([&received](const char* data, std::size_t length) {
                received.append(data, length);
                return true;
            });
            {
                std::unique_lock<std::mutex> lock(mutex_);
                host_ = request.get_header_value("Host");
                digest_ = request.get_header_value("Content-Digest");
                body_ = received;
                if (!refusal_.empty()) {
                    // A board that refuses an image it has read waits in
                    // flash mode for another, and says why.
                    status_.mode = "flash";
                    status_.last_update =
                        ac3forge::FirmwareLastUpdate{.version = "v0.11.0", .result = "refused", .reason = refusal_};
                }
                if (answer_delay_ > 0ms) {
                    // Interrupted when the stand-in goes, so that the server
                    // stops without waiting the delay out.
                    closing_.wait_for(lock, answer_delay_, [this] { return closed_; });
                }
                if (!refusal_.empty()) {
                    response.status = 400;
                    response.set_content(refusal_ + "\n", "text/plain");
                    return;
                }
                const std::vector<std::uint8_t> bytes(received.begin(), received.end());
                ac3::hearth::ReadFirmwareFile read = ac3::hearth::read_firmware_file(bytes);
                uploaded_ = std::move(read.file);
                uploaded_image_sha256_ = uploaded_ ? uploaded_->image_sha256 : std::string();
                status_.mode = "flash";
                status_.upload = ac3forge::FirmwareUpload{
                    .received = bytes.size(), .total = bytes.size(), .stage = "restarting"};
                step_ = 1;
            }
            const std::vector<std::uint8_t> bytes(received.begin(), received.end());
            response.set_content(fmt_answer(sink_firmware_test::hex(sink_firmware_test::sha256(bytes))),
                                 "application/json");
        });
        server_.Put("/firmware/rollback", [](const httplib::Request&, httplib::Response& response) {
            response.set_content("rolling back to ota_1\n", "text/plain");
        });
        server_.Put("/firmware/mode", [this](const httplib::Request& request, httplib::Response& response) {
            const std::lock_guard<std::mutex> lock(mutex_);
            ++mode_requests_;
            mode_body_ = request.body;
            mode_type_ = request.get_header_value("Content-Type");
            if (mode_status_ != 200) {
                response.status = mode_status_;
                response.set_content(mode_text_ + "\n", "text/plain");
                return;
            }
            // Leaving flash mode is a restart into the image that runs.
            status_.mode = "normal";
            response.set_content("restarting into the image that runs now\n", "text/plain");
        });
        server_.Post("/restart", [](const httplib::Request&, httplib::Response& response) {
            response.status = 409;
            response.set_content("the running image is on trial, and a restart would roll it back\n", "text/plain");
        });
        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
    }

    ~FakeBoard() {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        closing_.notify_all();
        server_.stop();
        thread_.join();
    }
    FakeBoard(const FakeBoard&) = delete;
    FakeBoard& operator=(const FakeBoard&) = delete;

    [[nodiscard]] std::uint16_t port() const { return static_cast<std::uint16_t>(port_); }

    void after_upload(AfterUpload after) {
        const std::lock_guard<std::mutex> lock(mutex_);
        after_upload_ = after;
    }
    void refuse(std::string why) {
        const std::lock_guard<std::mutex> lock(mutex_);
        refusal_ = std::move(why);
    }
    void delay_answer(std::chrono::milliseconds delay) {
        const std::lock_guard<std::mutex> lock(mutex_);
        answer_delay_ = delay;
    }
    // The next `count` uploads are cut off, and `after` is what each one did
    // to the board: what GET /firmware answers from then on.
    void drop_uploads(int count, std::function<void(ac3forge::FirmwareStatus&)> after) {
        const std::lock_guard<std::mutex> lock(mutex_);
        drops_ = count;
        after_drop_ = std::move(after);
    }
    // What PUT /firmware/mode answers when it does not take the request.
    void refuse_mode(int status, std::string text) {
        const std::lock_guard<std::mutex> lock(mutex_);
        mode_status_ = status;
        mode_text_ = std::move(text);
    }
    [[nodiscard]] int mode_requests() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return mode_requests_;
    }
    [[nodiscard]] std::string mode_body() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return mode_body_;
    }
    [[nodiscard]] std::string mode_type() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return mode_type_;
    }
    [[nodiscard]] std::string mode() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return status_.mode;
    }
    template <class Change>
    void change(Change change_status) {
        const std::lock_guard<std::mutex> lock(mutex_);
        change_status(status_);
    }

    [[nodiscard]] int uploads() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return uploads_;
    }
    [[nodiscard]] std::string host() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return host_;
    }
    [[nodiscard]] std::string digest() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return digest_;
    }
    [[nodiscard]] std::string body() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return body_;
    }

   private:
    static std::string fmt_answer(const std::string& sha256) {
        return R"({"version":"v0.11.0","slot":"ota_1","sha256":")" + sha256 + R"(","restarting":true})" + "\n";
    }

    // The board comes back: the new image on trial, or the old one with the
    // update rolled back, or still in flash mode.
    void restarted_locked() {
        if (after_upload_ == AfterUpload::kStayInFlashMode || !uploaded_) {
            return;
        }
        status_.mode = "normal";
        status_.upload.reset();
        const ac3forge::FirmwareSlot old = *status_.running;
        ac3forge::FirmwareSlot fresh;
        fresh.label = "ota_1";
        fresh.version = uploaded_->head.version;
        fresh.project = uploaded_->head.project;
        fresh.elf_sha256 = uploaded_->elf_sha256;
        if (after_upload_ == AfterUpload::kAccept) {
            fresh.state = "trial";
            status_.running = fresh;
            status_.other = old;
            status_.trial = ac3forge::FirmwareTrial{
                .healthy_for_ms = 1000, .hold_ms = 30'000, .remaining_ms = 299'000, .waiting_for = {}};
            status_.last_update =
                ac3forge::FirmwareLastUpdate{.version = fresh.version, .result = "on trial", .reason = ""};
        } else {
            fresh.state = "aborted";
            status_.other = fresh;
            status_.last_update = ac3forge::FirmwareLastUpdate{
                .version = fresh.version, .result = "rolled back", .reason = "it panicked"};
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable closing_;
    bool closed_ = false;
    ac3forge::FirmwareStatus status_;
    AfterUpload after_upload_ = AfterUpload::kAccept;
    std::string refusal_;
    std::chrono::milliseconds answer_delay_{0};
    int drops_ = 0;
    std::function<void(ac3forge::FirmwareStatus&)> after_drop_;
    int mode_status_ = 200;
    std::string mode_text_;
    int mode_requests_ = 0;
    std::string mode_body_;
    std::string mode_type_;
    int step_ = 0;  // 0: no upload yet; 1 to 3: before the restart; 4 on: after it
    int trial_reads_ = 0;
    int firmware_reads_ = 0;
    int uploads_ = 0;
    std::string host_;
    std::string digest_;
    std::string body_;
    std::optional<ac3::hearth::FirmwareFile> uploaded_;
    std::string uploaded_image_sha256_;

    httplib::Server server_;
    int port_ = 0;
    std::thread thread_;
};

ac3::hearth::FirmwareFile update_file() {
    ImageSpec spec;
    spec.elf_seed = 40;
    spec.version = "v0.11.0";
    ac3::hearth::ReadFirmwareFile read = ac3::hearth::read_firmware_file(sink_firmware_test::make_image(spec));
    REQUIRE(read.file.has_value());
    return std::move(*read.file);
}

// Too large for the sockets' buffers to take all of before a connection
// closed under it resets: the client is still sending when it finds out, as
// a client sending to a board is. The stand-in's slot is made room for it.
ac3::hearth::FirmwareFile large_update_file(FakeBoard& board) {
    board.change([](ac3forge::FirmwareStatus& status) { status.slot_bytes = 16 * 1024 * 1024; });
    ImageSpec spec;
    spec.elf_seed = 40;
    spec.version = "v0.11.0";
    spec.segment_bytes = 8'000'000;
    ac3::hearth::ReadFirmwareFile read = ac3::hearth::read_firmware_file(sink_firmware_test::make_image(spec));
    REQUIRE(read.file.has_value());
    return std::move(*read.file);
}

// What a board records when it gives an upload up: flash mode, and why.
std::function<void(ac3forge::FirmwareStatus&)> gave_up(std::string result, std::string reason) {
    return [result = std::move(result), reason = std::move(reason)](ac3forge::FirmwareStatus& status) {
        status.mode = "flash";
        status.last_update = ac3forge::FirmwareLastUpdate{.version = "v0.11.0", .result = result, .reason = reason};
    };
}

std::optional<SinkFirmware::Update> finished(const SinkFirmware& firmware) {
    std::optional<SinkFirmware::Update> update;
    const bool done = eventually([&] {
        update = firmware.snapshot().update;
        return update && update->stage == "done";
    });
    CHECK(done);
    return update;
}

}  // namespace

TEST_CASE("sink firmware board: a watched sink is read at every poll", "[hearth][sink-firmware]") {
    FakeBoard board;
    SinkFirmware firmware("127.0.0.1", board.port(), kFast);
    firmware.set_watching(true);
    REQUIRE(eventually([&] {
        const SinkFirmware::Snapshot snapshot = firmware.snapshot();
        return snapshot.firmware.has_value() && snapshot.hardware.has_value();
    }));
    const SinkFirmware::Snapshot snapshot = firmware.snapshot();
    CHECK(snapshot.asked);
    CHECK(snapshot.answering);
    CHECK(snapshot.error.empty());
    CHECK(snapshot.hardware->chip == "ESP32-S3");
    CHECK(snapshot.firmware->running->version == "v0.10.0");
    CHECK(snapshot.firmware->other->label == "ota_1");

    // A change on the board shows at the next poll.
    board.change([](ac3forge::FirmwareStatus& status) { status.network = "wired"; });
    CHECK(eventually([&] { return firmware.snapshot().firmware->network == "wired"; }));
}

TEST_CASE("sink firmware board: an update is sent with its digest and followed until it is accepted",
          "[hearth][sink-firmware]") {
    FakeBoard board;
    SinkFirmware firmware("127.0.0.1", board.port(), kFast);
    const ac3::hearth::FirmwareFile file = update_file();
    REQUIRE(firmware.start_update(file));
    CHECK_FALSE(firmware.start_update(file));  // one at a time
    CHECK(firmware.busy());

    const std::optional<SinkFirmware::Update> update = finished(firmware);
    REQUIRE(update.has_value());
    CHECK(update->outcome == UpdateOutcome::kUpdated);
    CHECK(update->text ==
          "updated: runs v0.11.0 from ota_1, accepted; the image's SHA-256 on the board matches the file's");
    CHECK(update->sent == file.data.size());
    CHECK(update->total == file.data.size());
    CHECK(update->target == "esp32s3");

    CHECK(board.uploads() == 1);
    CHECK(board.body() == std::string(file.data.begin(), file.data.end()));
    CHECK(board.digest() == "sha-256=:" + ac3::sendspin::base64::encode(file.file_sha256) + ":");
    CHECK(board.host() == "127.0.0.1:" + std::to_string(board.port()));
    CHECK(eventually([&] { return !firmware.busy(); }));
    CHECK(board.mode_requests() == 0);
}

TEST_CASE("sink firmware board: an image that does not last is reported rolled back, with the board's reason",
          "[hearth][sink-firmware]") {
    FakeBoard board;
    board.after_upload(FakeBoard::AfterUpload::kRollBack);
    SinkFirmware firmware("127.0.0.1", board.port(), kFast);
    REQUIRE(firmware.start_update(update_file()));
    const std::optional<SinkFirmware::Update> update = finished(firmware);
    REQUIRE(update.has_value());
    CHECK(update->outcome == UpdateOutcome::kRolledBack);
    CHECK(update->text ==
          "rolled back: v0.11.0 did not last, and the board runs v0.10.0 from ota_0 again. The board says: "
          "it panicked");
    CHECK(board.mode_requests() == 0);
}

TEST_CASE("sink firmware board: a refusal is reported in the board's own words", "[hearth][sink-firmware]") {
    FakeBoard board;
    SinkFirmware firmware("127.0.0.1", board.port(), kFast);

    SECTION("the board refuses what it received, and is told to leave the flash mode it waits in") {
        board.refuse("the image was damaged on the way");
        REQUIRE(firmware.start_update(update_file()));
        const std::optional<SinkFirmware::Update> update = finished(firmware);
        REQUIRE(update.has_value());
        CHECK(update->outcome == UpdateOutcome::kRefused);
        CHECK(update->text == "refused (400): the image was damaged on the way. Told the board to leave flash mode: "
                              "it restarts into the image it runs");
        CHECK(board.uploads() == 1);
        CHECK(board.mode_requests() == 1);
        CHECK(board.mode_body() == "normal");
        CHECK(board.mode_type() == "text/plain");
        CHECK(board.mode() == "normal");
    }
    SECTION("a board that will not leave flash mode says why") {
        board.refuse("the image was damaged on the way");
        board.refuse_mode(409, "an update is already under way");
        REQUIRE(firmware.start_update(update_file()));
        const std::optional<SinkFirmware::Update> update = finished(firmware);
        REQUIRE(update.has_value());
        CHECK(update->outcome == UpdateOutcome::kRefused);
        CHECK(update->text == "refused (400): the image was damaged on the way. The board stays in flash mode: "
                              "leaving it answered 409 an update is already under way");
        CHECK(board.mode_requests() == 1);
        CHECK(board.mode() == "flash");
    }
    SECTION("the pre-flight refuses a board on trial, and nothing is sent") {
        board.change([](ac3forge::FirmwareStatus& status) { status.running->state = "trial"; });
        REQUIRE(firmware.start_update(update_file()));
        const std::optional<SinkFirmware::Update> update = finished(firmware);
        REQUIRE(update.has_value());
        CHECK(update->outcome == UpdateOutcome::kRefused);
        CHECK(update->text.starts_with("refused: the running image is still on trial"));
        CHECK(board.uploads() == 0);
        CHECK(board.mode_requests() == 0);
    }
}

TEST_CASE("sink firmware board: an upload whose answer was lost leaves flash mode only if the board waits in it",
          "[hearth][sink-firmware]") {
    FakeBoard board;
    // The board's answer never comes: the upload's wait for it runs out
    // first.
    board.delay_answer(5s);
    ac3::hearth::SinkFirmwareTiming timing = kFast;
    timing.upload_answer = 1s;
    SinkFirmware firmware("127.0.0.1", board.port(), timing);
    SECTION("the board refused the image and waits in flash mode: it is told to leave") {
        board.refuse("the image in flash does not check out");
        REQUIRE(firmware.start_update(update_file()));
        const std::optional<SinkFirmware::Update> update = finished(firmware);
        REQUIRE(update.has_value());
        CHECK(update->outcome == UpdateOutcome::kRefused);
        CHECK(update->text == "refused: the image in flash does not check out. Told the board to leave flash mode: "
                              "it restarts into the image it runs");
        CHECK(board.mode_requests() == 1);
        CHECK(board.mode() == "normal");
    }
    SECTION("the board runs what it ran and says nothing of the upload: nothing to leave") {
        REQUIRE(firmware.start_update(update_file()));
        const std::optional<SinkFirmware::Update> update = finished(firmware);
        REQUIRE(update.has_value());
        CHECK(update->outcome == UpdateOutcome::kFailed);
        CHECK(update->text == "failed: the board runs the image it ran before and says nothing of this upload, so it "
                              "did not take it");
        CHECK(board.mode_requests() == 0);
    }
}

TEST_CASE("sink firmware board: an upload that broke off is sent again, and the second one goes through",
          "[hearth][sink-firmware]") {
    FakeBoard board;
    const ac3::hearth::FirmwareFile file = large_update_file(board);
    SECTION("the board never started it: its connection was reset before the board read any of it") {
        board.drop_uploads(1, nullptr);
    }
    SECTION("the board gave it up when the connection went, and says so from flash mode") {
        board.drop_uploads(1, gave_up("refused", "the upload stopped after 65536 of 8000080 bytes"));
    }
    SinkFirmware firmware("127.0.0.1", board.port(), kFast);
    REQUIRE(firmware.start_update(file));
    const std::optional<SinkFirmware::Update> update = finished(firmware);
    REQUIRE(update.has_value());
    INFO(update->text);
    CHECK(update->outcome == UpdateOutcome::kUpdated);
    CHECK(update->text.starts_with("updated: runs v0.11.0 from ota_1, accepted"));
    CHECK(update->attempt == 2);
    CHECK(update->sent == file.data.size());
    CHECK(board.uploads() == 2);
    const bool whole = board.body() == std::string(file.data.begin(), file.data.end());
    CHECK(whole);
    CHECK(board.mode_requests() == 0);
}

TEST_CASE("sink firmware board: an upload the board gave up for another reason is not sent again",
          "[hearth][sink-firmware]") {
    FakeBoard board;
    const ac3::hearth::FirmwareFile file = large_update_file(board);
    board.drop_uploads(1, gave_up("failed", "writing the slot failed after 4096 bytes"));
    SinkFirmware firmware("127.0.0.1", board.port(), kFast);
    REQUIRE(firmware.start_update(file));
    const std::optional<SinkFirmware::Update> update = finished(firmware);
    REQUIRE(update.has_value());
    INFO(update->text);
    CHECK(update->outcome == UpdateOutcome::kFailed);
    CHECK(update->text == "failed: the image was not taken; the board gave it up: writing the slot failed after 4096 "
                          "bytes. Told the board to leave flash mode: it restarts into the image it runs");
    CHECK(update->attempt == 1);
    CHECK(board.uploads() == 1);
    // Out of flash mode, rather than silent until its idle timeout.
    CHECK(board.mode_requests() == 1);
    CHECK(board.mode_body() == "normal");
    CHECK(board.mode_type() == "text/plain");
    CHECK(board.mode() == "normal");
}

TEST_CASE("sink firmware board: a second break ends the update, with no third try", "[hearth][sink-firmware]") {
    FakeBoard board;
    const ac3::hearth::FirmwareFile file = large_update_file(board);
    std::string said;
    int left = 0;
    SECTION("the board gave it up both times, and is told to leave flash mode") {
        board.drop_uploads(2, gave_up("refused", "the upload stopped after 65536 of 8000080 bytes"));
        said = "failed: the image was not taken; the board gave it up: the upload stopped after 65536 of 8000080 "
               "bytes. Told the board to leave flash mode: it restarts into the image it runs";
        left = 1;
    }
    SECTION("the board never started it either time, and is in normal mode") {
        board.drop_uploads(2, nullptr);
        said = "failed: the image was not taken; the board is in normal mode and says nothing of it: it never "
               "started it";
    }
    SinkFirmware firmware("127.0.0.1", board.port(), kFast);
    REQUIRE(firmware.start_update(file));
    const std::optional<SinkFirmware::Update> update = finished(firmware);
    REQUIRE(update.has_value());
    CHECK(update->outcome == UpdateOutcome::kFailed);
    CHECK(update->text == said);
    CHECK(update->attempt == 2);
    CHECK(board.uploads() == 2);
    CHECK(board.mode_requests() == left);
}

TEST_CASE("sink firmware board: a board that never decides is reported when the wait runs out",
          "[hearth][sink-firmware]") {
    FakeBoard board;
    board.after_upload(FakeBoard::AfterUpload::kStayInFlashMode);
    ac3::hearth::SinkFirmwareTiming timing = kFast;
    timing.wait = 1s;
    SinkFirmware firmware("127.0.0.1", board.port(), timing);
    REQUIRE(firmware.start_update(update_file()));
    const std::optional<SinkFirmware::Update> update = finished(firmware);
    REQUIRE(update.has_value());
    CHECK(update->outcome == UpdateOutcome::kSilent);
    CHECK(update->text.starts_with("still in flash mode after 1 s: it has not restarted into the new image."));
    // Not told to leave: a board still busy with the image may yet restart
    // into it, as ota.py leaves it.
    CHECK(board.mode_requests() == 0);
}

TEST_CASE("sink firmware board: a rollback and a restart report the board's answer", "[hearth][sink-firmware]") {
    FakeBoard board;
    SinkFirmware firmware("127.0.0.1", board.port(), kFast);
    REQUIRE(firmware.rollback());
    CHECK(eventually([&] { return firmware.snapshot().action == "roll back: 200 rolling back to ota_1"; }));
    CHECK(eventually([&] { return !firmware.busy(); }));
    REQUIRE(firmware.restart());
    CHECK(eventually([&] {
        return firmware.snapshot().action ==
               "restart: 409 the running image is on trial, and a restart would roll it back";
    }));
}

TEST_CASE("sink firmware board: going away does not wait out the board's answer to an upload",
          "[hearth][sink-firmware]") {
    FakeBoard board;
    board.delay_answer(30s);
    auto firmware = std::make_unique<SinkFirmware>("127.0.0.1", board.port(), kFast);
    REQUIRE(firmware->start_update(update_file()));
    REQUIRE(eventually([&] {
        const std::optional<SinkFirmware::Update> update = firmware->snapshot().update;
        return update && update->stage == "answering";
    }));
    const auto start = std::chrono::steady_clock::now();
    firmware.reset();
    CHECK(std::chrono::steady_clock::now() - start < 5s);
}

// An image onto a real board with this app's own client, hidden (it needs the
// board, and the board keeps the image):
//
//   AC3HEARTH_LIVE_FIRMWARE_HOST   the board's address or .local name; the case
//                                  is skipped without it
//   AC3HEARTH_LIVE_FIRMWARE_IMAGE  the image, as a build leaves it
//                                  (ac3forge_hearth_sink.bin)
//
// Each step prints as the Firmware tab would show it.
TEST_CASE("sink firmware live: an image goes onto a real board and is accepted",
          "[.][hearth][sink-firmware][live]") {
    const char* const host = std::getenv("AC3HEARTH_LIVE_FIRMWARE_HOST");
    const char* const path = std::getenv("AC3HEARTH_LIVE_FIRMWARE_IMAGE");
    if (host == nullptr || *host == '\0' || path == nullptr || *path == '\0') {
        SKIP("AC3HEARTH_LIVE_FIRMWARE_HOST and AC3HEARTH_LIVE_FIRMWARE_IMAGE name the board and the image");
    }
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    ac3::hearth::ReadFirmwareFile read = ac3::hearth::read_firmware_file(
        std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
    INFO(read.why);
    REQUIRE(read.file.has_value());

    SinkFirmware firmware(host);
    firmware.set_watching(true);
    REQUIRE(eventually([&] { return firmware.snapshot().firmware.has_value(); }, 30s));
    const auto print_panel = [&](const char* when) {
        const ac3::hearth::FirmwarePanel panel = ac3::hearth::to_firmware_panel(firmware.snapshot(), "");
        std::printf("--- %s\n  running      %s\n  other        %s\n  last update  %s\n  last crash   %s\n", when,
                    panel.running_text.c_str(), panel.other_text.c_str(), panel.last_update_text.c_str(),
                    panel.crash_text.empty() ? "none" : panel.crash_text.c_str());
        std::fflush(stdout);
    };
    print_panel("before");
    const ac3::hearth::FirmwareCandidate candidate = ac3::hearth::to_candidate(*read.file, firmware.snapshot());
    std::printf("image: %s%s%s\n", candidate.text.c_str(), candidate.refusal.empty() ? "" : "; refused: ",
                candidate.refusal.c_str());
    REQUIRE(candidate.refusal.empty());

    REQUIRE(firmware.start_update(std::move(*read.file)));
    std::string said;
    std::optional<SinkFirmware::Update> update;
    const bool done = eventually(
        [&] {
            update = firmware.snapshot().update;
            if (!update) {
                return false;
            }
            const ac3::hearth::FirmwarePanel panel = ac3::hearth::to_firmware_panel(firmware.snapshot(), "");
            const std::string now =
                update->stage == "sending" ? update->stage : (panel.updating ? panel.progress_text : update->text);
            if (now != said) {
                said = now;
                std::printf("%s\n", said.c_str());
                std::fflush(stdout);
            }
            return update->stage == "done";
        },
        std::chrono::minutes(8));
    REQUIRE(done);
    print_panel("after");
    CHECK(update->outcome == UpdateOutcome::kUpdated);
}

TEST_CASE("sink firmware board: a sink that does not answer says so", "[hearth][sink-firmware]") {
    // A port nothing listens on: bound, then let go.
    int port = 0;
    {
        httplib::Server probe;
        port = probe.bind_to_any_port("127.0.0.1");
    }
    SinkFirmware firmware("127.0.0.1", static_cast<std::uint16_t>(port), kFast);
    firmware.set_watching(true);
    REQUIRE(eventually([&] { return firmware.snapshot().asked; }));
    const SinkFirmware::Snapshot snapshot = firmware.snapshot();
    CHECK_FALSE(snapshot.answering);
    CHECK(snapshot.error.starts_with("no answer: "));
    CHECK_FALSE(snapshot.firmware.has_value());
}
