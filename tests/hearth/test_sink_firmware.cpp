// A Hearth sink's firmware from this computer (apps/hearth/engine/
// sink_firmware.hpp, planning/esp32-ota.md O5): reading what the board says,
// checking an image file, deciding whether a board may take it and how an
// update ended - everything but the HTTP, which test_sink_firmware_board.cpp
// runs against a stand-in board.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac3forge/firmware_status.hpp"
#include "sink_firmware.hpp"
#include "sink_firmware_images.hpp"
#include "sink_firmware_view.hpp"

using ac3::hearth::FirmwareFile;
using ac3::hearth::SinkHardware;
using ac3::hearth::UpdateOutcome;
using ac3::hearth::WaitContext;
using sink_firmware_test::ImageSpec;
using sink_firmware_test::make_image;

namespace {

FirmwareFile file_of(const ImageSpec& spec) {
    ac3::hearth::ReadFirmwareFile read = ac3::hearth::read_firmware_file(make_image(spec));
    REQUIRE(read.file.has_value());
    return std::move(*read.file);
}

std::string why_not(std::vector<std::uint8_t> bytes) {
    const ac3::hearth::ReadFirmwareFile read = ac3::hearth::read_firmware_file(std::move(bytes));
    CHECK_FALSE(read.file.has_value());
    return read.why;
}

SinkHardware s3_hardware() {
    return SinkHardware{.target = "esp32s3",
                        .chip = "ESP32-S3",
                        .revision = "0.2",
                        .project = "ac3forge_hearth_sink",
                        .version = "v0.10.0"};
}

ac3forge::FirmwareSlot slot(std::string label, std::string state, std::string version, const ImageSpec& spec) {
    ac3forge::FirmwareSlot out;
    out.label = std::move(label);
    out.state = std::move(state);
    out.version = std::move(version);
    out.project = "ac3forge_hearth_sink";
    out.idf_version = "v6.1";
    out.elf_sha256 = sink_firmware_test::elf_hex(spec);
    return out;
}

// A board running `running_spec` from ota_0, accepted, with an older image in
// ota_1 and room for 4 MiB in either.
ac3forge::FirmwareStatus s3_firmware(const ImageSpec& running_spec) {
    ac3forge::FirmwareStatus status;
    status.running = slot("ota_0", "valid", "v0.10.0", running_spec);
    status.running->intact = true;
    ImageSpec older = running_spec;
    older.elf_seed = 90;
    status.other = slot("ota_1", "valid", "v0.9.0", older);
    status.network = "stored";
    status.slot_bytes = 4 * 1024 * 1024;
    status.flash_bytes = 16 * 1024 * 1024;
    return status;
}

}  // namespace

TEST_CASE("sink firmware: GET /firmware is read back into what the board renders it from",
          "[hearth][sink-firmware]") {
    ImageSpec spec;
    ac3forge::FirmwareStatus status = s3_firmware(spec);
    status.mode = "flash";
    status.running->image_sha256 = "ab12";
    status.other->intact = false;
    status.trial = ac3forge::FirmwareTrial{.healthy_for_ms = 12'000,
                                           .hold_ms = 30'000,
                                           .remaining_ms = 274'000,
                                           .waiting_for = {"a network address", "the Sendspin player"}};
    status.upload = ac3forge::FirmwareUpload{.received = 4096, .total = 1'480'768, .stage = "writing"};
    status.last_update = ac3forge::FirmwareLastUpdate{
        .version = "v0.9.9", .result = "rolled back", .reason = "it panicked \"early\""};
    status.coredump = ac3forge::FirmwareCoredump{.bytes = 5600,
                                                 .intact = true,
                                                 .task = "main",
                                                 .pc = "0x408078e8",
                                                 .reason = "abort() was called at PC 0x42029b27 on core 0",
                                                 .elf_sha256 = "a1b2c3d4"};
    status.partitions = {{.label = "nvs", .type = 1, .subtype = 2, .offset = 0x9000, .size = 0x6000},
                         {.label = "ota_0", .type = 0, .subtype = 0x10, .offset = 0x20000, .size = 0x400000}};
    status.bootloader_version = "v6.1";
    status.reset_reason = "sw";
    status.uptime_ms = 61'234;

    const std::optional<ac3forge::FirmwareStatus> parsed =
        ac3::hearth::parse_firmware_status(ac3forge::render_firmware_status(status));
    REQUIRE(parsed.has_value());
    CHECK(parsed->mode == "flash");
    REQUIRE(parsed->running.has_value());
    CHECK(parsed->running->label == "ota_0");
    CHECK(parsed->running->state == "valid");
    CHECK(parsed->running->version == "v0.10.0");
    CHECK(parsed->running->project == "ac3forge_hearth_sink");
    CHECK(parsed->running->idf_version == "v6.1");
    CHECK(parsed->running->elf_sha256 == status.running->elf_sha256);
    CHECK(parsed->running->image_sha256 == "ab12");
    CHECK(parsed->running->intact == std::optional<bool>(true));
    REQUIRE(parsed->other.has_value());
    CHECK(parsed->other->intact == std::optional<bool>(false));
    REQUIRE(parsed->trial.has_value());
    CHECK(parsed->trial->healthy_for_ms == 12'000);
    CHECK(parsed->trial->hold_ms == 30'000);
    CHECK(parsed->trial->remaining_ms == 274'000);
    CHECK(parsed->trial->waiting_for == std::vector<std::string>{"a network address", "the Sendspin player"});
    REQUIRE(parsed->upload.has_value());
    CHECK(parsed->upload->received == 4096);
    CHECK(parsed->upload->total == 1'480'768);
    CHECK(parsed->upload->stage == "writing");
    REQUIRE(parsed->last_update.has_value());
    CHECK(parsed->last_update->result == "rolled back");
    CHECK(parsed->last_update->reason == "it panicked \"early\"");
    REQUIRE(parsed->coredump.has_value());
    CHECK(parsed->coredump->bytes == 5600);
    CHECK(parsed->coredump->intact);
    CHECK(parsed->coredump->task == "main");
    CHECK(parsed->coredump->pc == "0x408078e8");
    CHECK(parsed->coredump->reason == "abort() was called at PC 0x42029b27 on core 0");
    CHECK(parsed->coredump->elf_sha256 == "a1b2c3d4");
    CHECK(parsed->network == "stored");
    CHECK(parsed->slot_bytes == 4 * 1024 * 1024);
    CHECK(parsed->flash_bytes == 16 * 1024 * 1024);
    REQUIRE(parsed->partitions.size() == 2);
    CHECK(parsed->partitions[1].label == "ota_0");
    CHECK(parsed->partitions[1].subtype == 0x10);
    CHECK(parsed->partitions[1].offset == 0x20000);
    CHECK(parsed->partitions[1].size == 0x400000);
    CHECK(parsed->bootloader_version == "v6.1");
    CHECK(parsed->reset_reason == "sw");
    CHECK(parsed->uptime_ms == 61'234);

    SECTION("nulls stay absent") {
        ac3forge::FirmwareStatus bare;
        const std::optional<ac3forge::FirmwareStatus> back =
            ac3::hearth::parse_firmware_status(ac3forge::render_firmware_status(bare));
        REQUIRE(back.has_value());
        CHECK_FALSE(back->running.has_value());
        CHECK_FALSE(back->trial.has_value());
        CHECK_FALSE(back->upload.has_value());
        CHECK_FALSE(back->last_update.has_value());
        CHECK_FALSE(back->coredump.has_value());
        CHECK(back->network == "none");
    }
    SECTION("firmware from before O4 has no coredump key") {
        const std::optional<ac3forge::FirmwareStatus> back =
            ac3::hearth::parse_firmware_status(R"({"mode":"normal","running":null,"other":null,"trial":null,)"
                                               R"("upload":null,"last_update":null,"network":"stored",)"
                                               R"("slot_bytes":0,"flash_bytes":0,"partitions":[]})");
        REQUIRE(back.has_value());
        CHECK_FALSE(back->coredump.has_value());
        CHECK(back->network == "stored");
    }
    SECTION("anything else is not one") {
        CHECK_FALSE(ac3::hearth::parse_firmware_status("").has_value());
        CHECK_FALSE(ac3::hearth::parse_firmware_status("this image takes no updates").has_value());
        CHECK_FALSE(ac3::hearth::parse_firmware_status("[1,2]").has_value());
        CHECK_FALSE(ac3::hearth::parse_firmware_status(R"({"running":null})").has_value());
    }
}

TEST_CASE("sink firmware: GET /hardware gives the chip, its revision and the project", "[hearth][sink-firmware]") {
    const std::optional<SinkHardware> hardware = ac3::hearth::parse_sink_hardware(
        R"({"target":"esp32c6","chip":"ESP32-C6","revision":"0.2","cores":1,"fpu":false,"cpu_freq_mhz":160,)"
        R"("psram_bytes":0,"project":"ac3forge_hearth_sink","version":"v0.10.0","idf_version":"v6.1",)"
        R"("capabilities":[],"notices":[]})");
    REQUIRE(hardware.has_value());
    CHECK(hardware->target == "esp32c6");
    CHECK(hardware->chip == "ESP32-C6");
    CHECK(hardware->revision == "0.2");
    CHECK(hardware->project == "ac3forge_hearth_sink");
    CHECK(hardware->version == "v0.10.0");
    CHECK_FALSE(ac3::hearth::parse_sink_hardware(R"({"chip":"ESP32-C6"})").has_value());
    CHECK_FALSE(ac3::hearth::parse_sink_hardware("<html>").has_value());
}

TEST_CASE("sink firmware: an image file is walked as the bootloader walks it", "[hearth][sink-firmware]") {
    ImageSpec spec;
    const std::vector<std::uint8_t> image = make_image(spec);

    SECTION("a whole image is read, with its hashes") {
        const ac3::hearth::ReadFirmwareFile read = ac3::hearth::read_firmware_file(image);
        REQUIRE(read.file.has_value());
        CHECK(read.why.empty());
        CHECK(read.file->head.version == "v0.11.0");
        CHECK(read.file->head.project == "ac3forge_hearth_sink");
        CHECK(read.file->head.chip_id == 0x0009);
        CHECK(read.file->data == image);
        CHECK(read.file->elf_sha256 == sink_firmware_test::elf_hex(spec));
        const std::size_t hashed = sink_firmware_test::checksum_at(spec) + 1;
        CHECK(read.file->image_sha256 ==
              sink_firmware_test::hex(std::span<const std::uint8_t>(image).subspan(hashed, 32)));
        CHECK(read.file->file_sha256 == sink_firmware_test::sha256(image));
    }
    SECTION("too short to be one") {
        CHECK(why_not(std::vector<std::uint8_t>(100, 0xE9)) == "it is 100 bytes, too short to be an application image");
    }
    SECTION("not an application image") {
        std::vector<std::uint8_t> bytes = image;
        bytes[0] = 0x7F;  // an ELF's first byte
        CHECK(why_not(bytes).starts_with("this is not an ESP-IDF application image"));
    }
    SECTION("a segment that is not whole words") {
        std::vector<std::uint8_t> bytes = image;
        sink_firmware_test::put32(bytes, 28, spec.segment_bytes - 2);
        CHECK(why_not(bytes) == "segment 0 is 1022 bytes, not a whole number of words");
    }
    SECTION("cut short inside its segment") {
        std::vector<std::uint8_t> bytes(image.begin(), image.begin() + 600);
        CHECK(why_not(bytes) == "it ends inside segment 0: it is cut short or damaged");
    }
    SECTION("cut short before its SHA-256") {
        std::vector<std::uint8_t> bytes(image.begin(), image.end() - 10);
        CHECK(why_not(bytes) == "it ends before the SHA-256 the build appended: it is cut short");
    }
    SECTION("no SHA-256 of its own") {
        ImageSpec unhashed = spec;
        unhashed.hash_appended = false;
        CHECK(why_not(make_image(unhashed)).starts_with("it carries no SHA-256 of itself"));
    }
    SECTION("a byte changed after the build") {
        std::vector<std::uint8_t> bytes = image;
        bytes[700] = static_cast<std::uint8_t>(bytes[700] ^ 0x01U);
        CHECK(why_not(bytes) == "its SHA-256 does not match the one the build appended to it: the file is damaged");
    }
    SECTION("a checksum that does not add up, under a SHA-256 that does") {
        std::vector<std::uint8_t> bytes = image;
        bytes[sink_firmware_test::checksum_at(spec)] =
            static_cast<std::uint8_t>(bytes[sink_firmware_test::checksum_at(spec)] ^ 0xFFU);
        sink_firmware_test::rehash(bytes, spec);
        CHECK(why_not(bytes).ends_with(": the image is damaged"));
        CHECK(why_not(bytes).starts_with("its checksum byte is 0x"));
    }
}

TEST_CASE("sink firmware: a board is refused an image it must not take, in ota.py's words",
          "[hearth][sink-firmware]") {
    ImageSpec running;
    ImageSpec update;
    update.elf_seed = 40;
    const SinkHardware hardware = s3_hardware();
    ac3forge::FirmwareStatus firmware = s3_firmware(running);

    CHECK_FALSE(ac3::hearth::refuse_update(file_of(update), hardware, firmware).has_value());

    SECTION("another chip") {
        ImageSpec c6 = update;
        c6.chip_id = 0x000D;
        CHECK(ac3::hearth::refuse_update(file_of(c6), hardware, firmware) ==
              "this image is for an ESP32-C6, and this board is an ESP32-S3");
    }
    SECTION("a revision the image excludes") {
        ImageSpec newer = update;
        newer.min_rev = 3;
        CHECK(ac3::hearth::refuse_update(file_of(newer), hardware, firmware) ==
              "this image needs chip revision v0.3 or newer, and this chip is v0.2");
    }
    SECTION("another project") {
        ImageSpec other = update;
        other.project = "hello_world";
        CHECK(ac3::hearth::refuse_update(file_of(other), hardware, firmware) ==
              "this image is hello_world, not ac3forge_hearth_sink");
    }
    SECTION("another flash size") {
        firmware.flash_bytes = 4 * 1024 * 1024;
        CHECK(ac3::hearth::refuse_update(file_of(update), hardware, firmware) ==
              "this image was built for 16 MB of flash, and this board's is set for 4 MB of flash");
    }
    SECTION("a slot too small") {
        firmware.slot_bytes = 1000;
        // 24 + 8 + 1,024, padded to 1,072 with its checksum, and 32 of SHA-256.
        CHECK(ac3::hearth::refuse_update(file_of(update), hardware, firmware) ==
              "the image is 1,104 bytes, and the board's slot holds 1,000");
    }
    SECTION("on trial") {
        firmware.running->state = "trial";
        CHECK(ac3::hearth::refuse_update(file_of(update), hardware, firmware)->starts_with(
            "the running image is still on trial"));
    }
    SECTION("an update already under way") {
        firmware.upload = ac3forge::FirmwareUpload{.received = 1, .total = 2, .stage = "writing"};
        CHECK(ac3::hearth::refuse_update(file_of(update), hardware, firmware) ==
              "an update is already under way on this board");
    }
    SECTION("one app slot") {
        firmware.other.reset();
        CHECK(ac3::hearth::refuse_update(file_of(update), hardware, firmware)->starts_with(
            "this board's partition table has one app slot"));
    }
    SECTION("a network the running image alone has") {
        firmware.network = "built-in";
        CHECK(ac3::hearth::refuse_update(file_of(update), hardware, firmware)->starts_with(
            "the board's only network is built into the image it runs"));
    }
    SECTION("a board this app cannot place") {
        SinkHardware odd = hardware;
        odd.target = "esp8266";
        odd.chip = "ESP8266";
        CHECK(ac3::hearth::refuse_update(file_of(update), odd, firmware) ==
              "the board reports its chip as ESP8266, which this app does not know");
        SinkHardware unread = hardware;
        unread.revision = "two";
        CHECK(ac3::hearth::refuse_update(file_of(update), unread, firmware) ==
              "the board reports its chip revision as 'two', not as M.m");
    }
}

TEST_CASE("sink firmware: the wait after an upload reads each answer as ota.py does", "[hearth][sink-firmware]") {
    ImageSpec running;
    ImageSpec update_spec;
    update_spec.elf_seed = 40;
    update_spec.version = "v0.11.0";
    const FirmwareFile update = file_of(update_spec);
    WaitContext context;
    context.slot = "ota_1";

    // The board's view of the new image once it runs from ota_1.
    ac3forge::FirmwareStatus after = s3_firmware(running);
    after.other = after.running;
    after.other->label = "ota_0";
    after.running = slot("ota_1", "trial", "v0.11.0", update_spec);

    SECTION("no answer: restarting") {
        const ac3::hearth::WaitVerdict verdict = ac3::hearth::judge_wait(nullptr, update, context);
        CHECK(verdict.outcome == UpdateOutcome::kNone);
        CHECK(verdict.text == "no answer yet: restarting");
    }
    SECTION("flash mode, before the restart") {
        ac3forge::FirmwareStatus flash = s3_firmware(running);
        flash.mode = "flash";
        flash.upload = ac3forge::FirmwareUpload{.received = 1, .total = 1, .stage = "restarting"};
        CHECK(ac3::hearth::judge_wait(&flash, update, context).outcome == UpdateOutcome::kNone);
        SECTION("with the upload's answer lost and no upload running, it refused the image") {
            flash.upload.reset();
            flash.last_update = ac3forge::FirmwareLastUpdate{
                .version = "v0.11.0", .result = "refused", .reason = "the image does not check out"};
            context.reply_lost = true;
            const ac3::hearth::WaitVerdict verdict = ac3::hearth::judge_wait(&flash, update, context);
            CHECK(verdict.outcome == UpdateOutcome::kRefused);
            CHECK(verdict.text == "refused: the image does not check out");
        }
    }
    SECTION("the new image on trial") {
        after.trial = ac3forge::FirmwareTrial{
            .healthy_for_ms = 11'000, .hold_ms = 30'000, .remaining_ms = 286'000, .waiting_for = {}};
        const ac3::hearth::WaitVerdict verdict = ac3::hearth::judge_wait(&after, update, context);
        CHECK(verdict.outcome == UpdateOutcome::kNone);
        CHECK(verdict.text == "on trial: healthy for 11s of 30s (286s left)");
    }
    SECTION("accepted, with the board's own SHA-256 of the slot") {
        after.running->state = "valid";
        after.running->intact = true;
        after.running->image_sha256 = update.image_sha256;
        const ac3::hearth::WaitVerdict verdict = ac3::hearth::judge_wait(&after, update, context);
        CHECK(verdict.outcome == UpdateOutcome::kUpdated);
        CHECK(verdict.text ==
              "updated: runs v0.11.0 from ota_1, accepted; the image's SHA-256 on the board matches the file's");
        SECTION("or another SHA-256") {
            after.running->image_sha256 = "00ff";
            CHECK(ac3::hearth::judge_wait(&after, update, context).text.ends_with(
                "; but the board reports the image's SHA-256 as 00ff, and the file's is " + update.image_sha256));
        }
    }
    SECTION("accepted before the board has checked the slot: a little longer, then updated anyway") {
        after.running->state = "valid";
        const ac3::hearth::WaitVerdict waiting = ac3::hearth::judge_wait(&after, update, context);
        CHECK(waiting.outcome == UpdateOutcome::kNone);
        CHECK(waiting.accepted);
        context.sha_wait_over = true;
        const ac3::hearth::WaitVerdict verdict = ac3::hearth::judge_wait(&after, update, context);
        CHECK(verdict.outcome == UpdateOutcome::kUpdated);
        CHECK(verdict.text.ends_with("; the board has not reported the image's SHA-256 yet"));
    }
    SECTION("the image before it, and the last update rolled back") {
        ac3forge::FirmwareStatus back = s3_firmware(running);
        back.last_update =
            ac3forge::FirmwareLastUpdate{.version = "v0.11.0", .result = "rolled back", .reason = "it panicked"};
        const ac3::hearth::WaitVerdict verdict = ac3::hearth::judge_wait(&back, update, context);
        CHECK(verdict.outcome == UpdateOutcome::kRolledBack);
        CHECK(verdict.text ==
              "rolled back: v0.11.0 did not last, and the board runs v0.10.0 from ota_0 again. The board says: "
              "it panicked");
    }
    SECTION("the upload's answer lost, and the board says nothing of it") {
        ac3forge::FirmwareStatus same = s3_firmware(running);
        context.reply_lost = true;
        context.last_before = same.last_update;
        const ac3::hearth::WaitVerdict verdict = ac3::hearth::judge_wait(&same, update, context);
        CHECK(verdict.outcome == UpdateOutcome::kFailed);
        CHECK(verdict.text.starts_with("failed: the board runs the image it ran before"));
    }
    SECTION("the wait running out") {
        using std::chrono::seconds;
        CHECK(ac3::hearth::silent_text(nullptr, update, context, seconds(360)).starts_with(
            "did not come back within 360 s. Cycling the board's power"));
        ac3forge::FirmwareStatus flash = s3_firmware(running);
        flash.mode = "flash";
        CHECK(ac3::hearth::silent_text(&flash, update, context, seconds(360)).starts_with(
            "still in flash mode after 360 s: it has not restarted into the new image."));
        CHECK(ac3::hearth::silent_text(&after, update, context, seconds(360)).starts_with(
            "still runs the new image on trial after 360 s."));
        ac3forge::FirmwareStatus before = s3_firmware(running);
        CHECK(ac3::hearth::silent_text(&before, update, context, seconds(360)).starts_with(
            "runs v0.10.0 from ota_0 after 360 s, and has not said how the update to ota_1 ended."));
    }
}

TEST_CASE("sink firmware: an upload that broke off is judged as ota.py's after_break judges it",
          "[hearth][sink-firmware]") {
    using ac3::hearth::BreakVerdict;
    using ac3::hearth::judge_break;
    using namespace std::chrono_literals;
    ImageSpec running;
    ac3forge::FirmwareStatus board = s3_firmware(running);
    board.uptime_ms = 600'000;  // up since long before the upload began
    const std::chrono::milliseconds since = 20s;

    SECTION("the board is asked again while it does not answer, or still reports the upload") {
        CHECK_FALSE(judge_break(nullptr, since, false).decided);
        ac3forge::FirmwareStatus still = board;
        still.mode = "flash";
        still.upload = ac3forge::FirmwareUpload{.received = 4096, .total = 1'480'768, .stage = "writing"};
        CHECK_FALSE(judge_break(&still, since, false).decided);

        // Once the wait runs out, neither is sent again.
        const BreakVerdict silent = judge_break(nullptr, since, true);
        CHECK(silent.decided);
        CHECK(silent.text == "the board has not answered since");
        CHECK_FALSE(silent.retry);
        CHECK_FALSE(silent.flash_mode);
        const BreakVerdict writing = judge_break(&still, since, true);
        CHECK(writing.decided);
        CHECK(writing.text == "the board still reports the upload as running");
        CHECK_FALSE(writing.retry);
        CHECK(writing.flash_mode);
    }
    SECTION("in flash mode the board gave it up and says why, and a connection that went is tried again") {
        board.mode = "flash";
        board.last_update = ac3forge::FirmwareLastUpdate{
            .version = "v0.11.0", .result = "refused", .reason = "the upload stopped after 65536 of 1480768 bytes"};
        BreakVerdict verdict = judge_break(&board, since, false);
        CHECK(verdict.decided);
        CHECK(verdict.text == "the board gave it up: the upload stopped after 65536 of 1480768 bytes");
        CHECK(verdict.retry);
        CHECK(verdict.flash_mode);
        board.last_update->reason = "the upload ended before the image's header did";
        CHECK(judge_break(&board, since, false).retry);

        SECTION("any other reason is not tried again") {
            board.last_update = ac3forge::FirmwareLastUpdate{
                .version = "v0.11.0", .result = "failed", .reason = "writing the slot failed after 4096 bytes"};
            verdict = judge_break(&board, since, false);
            CHECK(verdict.text == "the board gave it up: writing the slot failed after 4096 bytes");
            CHECK_FALSE(verdict.retry);
            CHECK(verdict.flash_mode);
        }
        SECTION("with no reason it is") {
            board.last_update->reason.clear();
            verdict = judge_break(&board, since, false);
            CHECK(verdict.text == "the board is in flash mode and says nothing of it");
            CHECK(verdict.retry);
            CHECK(verdict.flash_mode);
            board.last_update.reset();
            CHECK(judge_break(&board, since, false).text == "the board is in flash mode and says nothing of it");
        }
    }
    SECTION("in normal mode, a board that restarted during it says so, and is sent it again") {
        board.last_update = ac3forge::FirmwareLastUpdate{
            .version = "v0.11.0",
            .result = "interrupted",
            .reason = "the board restarted while the image was being written, on a panic"};
        BreakVerdict verdict = judge_break(&board, since, false);
        CHECK(verdict.text ==
              "the board restarted during it: the board restarted while the image was being written, on a panic");
        CHECK(verdict.retry);
        CHECK_FALSE(verdict.flash_mode);

        // Firmware that records nothing of it: up for less time than the
        // upload has been going.
        board.last_update.reset();
        board.uptime_ms = 1'500;
        board.reset_reason = "sw";
        verdict = judge_break(&board, since, false);
        CHECK(verdict.text == "the board restarted during it (reset reason: sw)");
        CHECK(verdict.retry);
        CHECK_FALSE(verdict.flash_mode);
        board.reset_reason.clear();
        CHECK(judge_break(&board, since, false).text == "the board restarted during it (reset reason: not reported)");
    }
    SECTION("in normal mode and up throughout, it never started the upload, which is sent again") {
        const BreakVerdict verdict = judge_break(&board, since, false);
        CHECK(verdict.text == "the board is in normal mode and says nothing of it: it never started it");
        CHECK(verdict.retry);
        CHECK_FALSE(verdict.flash_mode);
        // A board that reports no uptime is not taken to have restarted.
        board.uptime_ms = 0;
        CHECK(judge_break(&board, since, false).text ==
              "the board is in normal mode and says nothing of it: it never started it");
    }
}

TEST_CASE("sink firmware: the Firmware tab's rows and what it may offer", "[hearth][sink-firmware]") {
    ImageSpec running;
    ac3::hearth::SinkFirmware::Snapshot snapshot;
    snapshot.host = "192.168.1.117";
    snapshot.port = 80;

    SECTION("before the first answer") {
        const ac3::hearth::FirmwarePanel panel = ac3::hearth::to_firmware_panel(snapshot, "v0.10.0");
        CHECK(panel.status_text == "Asking the sink about its firmware…");
        CHECK_FALSE(panel.reported);
        CHECK_FALSE(panel.can_update);
        CHECK(panel.page_url == "http://192.168.1.117/");
        CHECK(panel.log_url == "http://192.168.1.117/log");
    }

    snapshot.asked = true;
    snapshot.answering = true;
    snapshot.hardware = s3_hardware();
    snapshot.firmware = s3_firmware(running);

    SECTION("a board that runs this app's build") {
        const ac3::hearth::FirmwarePanel panel = ac3::hearth::to_firmware_panel(snapshot, "v0.10.0");
        CHECK(panel.answering);
        CHECK(panel.status_text.empty());
        CHECK(panel.reported);
        CHECK(panel.running_text == "v0.10.0 · ota_0 · accepted · checked intact");
        CHECK(panel.other_text == "v0.9.0 · ota_1 · accepted · not checked yet");
        CHECK(panel.running_version == "v0.10.0");
        CHECK(panel.other_version == "v0.9.0");
        CHECK(panel.same_build);
        CHECK(panel.build_text == "The same build as this app.");
        CHECK(panel.can_update);
        CHECK(panel.can_rollback);
        CHECK(panel.can_restart);
        CHECK(panel.crash_text.empty());
        CHECK(panel.coredump_url.empty());
    }
    SECTION("another build, a crash and a rollback") {
        snapshot.firmware->last_update =
            ac3forge::FirmwareLastUpdate{.version = "v0.11.0", .result = "rolled back", .reason = "it panicked"};
        ImageSpec failed;
        failed.elf_seed = 40;
        snapshot.firmware->other = slot("ota_1", "aborted", "v0.11.0", failed);
        snapshot.firmware->coredump = ac3forge::FirmwareCoredump{
            .bytes = 5600,
            .intact = true,
            .task = "main",
            .pc = "0x408078e8",
            .reason = "abort() was called at PC 0x42029b27 on core 0",
            .elf_sha256 = sink_firmware_test::elf_hex(failed).substr(0, 8)};
        const ac3::hearth::FirmwarePanel panel = ac3::hearth::to_firmware_panel(snapshot, "v0.11.0-3-gabcdef012");
        CHECK_FALSE(panel.same_build);
        CHECK(panel.build_text == "Not this app's build: this app is v0.11.0-3-gabcdef012.");
        CHECK(panel.other_text == "v0.11.0 · ota_1 · did not last · not checked yet");
        CHECK(panel.last_update_text == "v0.11.0: rolled back (it panicked)");
        CHECK(panel.crash_text == "5,600 bytes · main at 0x408078e8 · abort() was called at PC 0x42029b27 on core 0 "
                                  "· written by v0.11.0 in ota_1");
        CHECK(panel.coredump_url == "http://192.168.1.117/firmware/coredump");
        CHECK(panel.can_update);
        CHECK_FALSE(panel.can_rollback);  // nothing valid to go back to
    }
    SECTION("on trial: a rollback gives the trial up, and a restart is not offered") {
        snapshot.firmware->running->state = "trial";
        snapshot.firmware->trial = ac3forge::FirmwareTrial{
            .healthy_for_ms = 3000, .hold_ms = 30'000, .remaining_ms = 290'000, .waiting_for = {"a network address"}};
        const ac3::hearth::FirmwarePanel panel = ac3::hearth::to_firmware_panel(snapshot, "");
        CHECK(panel.trial_text == "Healthy for 3s of 30s, waiting for a network address (290s left)");
        CHECK(panel.build_text.empty());
        CHECK_FALSE(panel.can_update);
        CHECK(panel.can_rollback);
        CHECK_FALSE(panel.can_restart);
    }
    SECTION("another client's upload, and flash mode") {
        snapshot.firmware->mode = "flash";
        snapshot.firmware->upload = ac3forge::FirmwareUpload{.received = 4096, .total = 1'480'768, .stage = "writing"};
        const ac3::hearth::FirmwarePanel panel = ac3::hearth::to_firmware_panel(snapshot, "");
        CHECK(panel.mode_text == "Flash mode: nothing plays until the board restarts.");
        CHECK(panel.upload_text == "Another update is under way: writing, 4,096 of 1,480,768 bytes");
        CHECK_FALSE(panel.can_update);
        CHECK_FALSE(panel.can_restart);
    }
    SECTION("this app's own update, sending and then ended") {
        snapshot.update = ac3::hearth::SinkFirmware::Update{
            .version = "v0.11.0", .stage = "sending", .sent = 370'192, .total = 1'480'768, .text = "sending"};
        ac3::hearth::FirmwarePanel panel = ac3::hearth::to_firmware_panel(snapshot, "");
        CHECK(panel.updating);
        CHECK(panel.progress == 0.25);
        CHECK(panel.progress_text == "Sending v0.11.0: 370,192 of 1,480,768 bytes");
        CHECK_FALSE(panel.can_update);
        CHECK_FALSE(panel.can_restart);

        // The upload broke off: what became of it is asked, and the image is
        // sent again.
        snapshot.update->stage = "broken";
        snapshot.update->text = "the upload broke off after 370,192 bytes: Failed to write connection";
        panel = ac3::hearth::to_firmware_panel(snapshot, "");
        CHECK(panel.updating);
        CHECK(panel.progress == -1);
        CHECK(panel.progress_text == "The upload broke off after 370,192 bytes: Failed to write connection");
        snapshot.update->stage = "sending";
        snapshot.update->attempt = 2;
        snapshot.update->sent = 740'384;
        panel = ac3::hearth::to_firmware_panel(snapshot, "");
        CHECK(panel.progress == 0.5);
        CHECK(panel.progress_text == "Sending v0.11.0 again: 740,384 of 1,480,768 bytes");

        snapshot.update->stage = "waiting";
        snapshot.update->text = "on trial: healthy for 1s of 30s (296s left)";
        panel = ac3::hearth::to_firmware_panel(snapshot, "");
        CHECK(panel.progress == -1);
        CHECK(panel.progress_text == "On trial: healthy for 1s of 30s (296s left)");

        snapshot.update->stage = "done";
        snapshot.update->outcome = UpdateOutcome::kRolledBack;
        snapshot.update->text = "rolled back: v0.11.0 did not last";
        panel = ac3::hearth::to_firmware_panel(snapshot, "");
        CHECK_FALSE(panel.updating);
        CHECK(panel.outcome == "rolledBack");
        CHECK(panel.outcome_text == "Rolled back: v0.11.0 did not last");
        CHECK(panel.can_update);
    }
    SECTION("a board that stopped answering keeps what it said last") {
        snapshot.answering = false;
        snapshot.error = "no answer: Connection";
        const ac3::hearth::FirmwarePanel panel = ac3::hearth::to_firmware_panel(snapshot, "");
        CHECK(panel.status_text == "Not answering at http://192.168.1.117/: no answer: Connection");
        CHECK(panel.reported);
        CHECK(panel.running_text == "v0.10.0 · ota_0 · accepted · checked intact");
        CHECK_FALSE(panel.can_update);
        CHECK_FALSE(panel.can_rollback);
        CHECK_FALSE(panel.can_restart);
    }
    SECTION("an IPv6 address goes into its URLs in brackets") {
        snapshot.host = "fe80::1";
        CHECK(ac3::hearth::to_firmware_panel(snapshot, "").page_url == "http://[fe80::1]/");
    }
    SECTION("an empty other slot has no version to name") {
        snapshot.firmware->other->state = "empty";
        const ac3::hearth::FirmwarePanel panel = ac3::hearth::to_firmware_panel(snapshot, "");
        CHECK(panel.other_text == "ota_1 · empty");
        CHECK(panel.other_version.empty());
        CHECK_FALSE(panel.can_rollback);
    }
}

TEST_CASE("sink firmware: the page keeps a sink's client, and its row, while there is something to show",
          "[hearth][sink-firmware]") {
    using ac3::hearth::FirmwareClientPlan;
    using ac3::hearth::plan_firmware_client;
    ac3::hearth::SinkFirmware::Snapshot snapshot;
    snapshot.host = "192.168.1.117";

    SECTION("with nothing under way and nothing ended, the client goes with the tab and keeps no row") {
        FirmwareClientPlan plan = plan_firmware_client(false, snapshot, true, "192.168.1.117");
        CHECK_FALSE(plan.let_go);
        CHECK_FALSE(plan.keep_sink);
        plan = plan_firmware_client(false, snapshot, false, "");
        CHECK(plan.let_go);
        CHECK_FALSE(plan.keep_sink);
    }
    SECTION("an update under way keeps the client and the row, shown or not, and at the address it began at") {
        snapshot.update =
            ac3::hearth::SinkFirmware::Update{.version = "v0.11.0", .stage = "sending", .text = "sending"};
        for (const bool shown : {true, false}) {
            const FirmwareClientPlan plan = plan_firmware_client(true, snapshot, shown, shown ? "192.168.1.117" : "");
            CHECK_FALSE(plan.let_go);
            CHECK(plan.keep_sink);
        }
        const FirmwareClientPlan moved = plan_firmware_client(true, snapshot, true, "192.168.1.200");
        CHECK_FALSE(moved.let_go);
        CHECK(moved.keep_sink);
    }
    SECTION("an update that ended keeps the row while the tab shows how, and not once the tab moves on") {
        // Rolled back: the board can say so before mDNS lists it again.
        snapshot.update = ac3::hearth::SinkFirmware::Update{.version = "v0.11.0",
                                                            .stage = "done",
                                                            .outcome = UpdateOutcome::kRolledBack,
                                                            .text = "rolled back: v0.11.0 did not last"};
        FirmwareClientPlan plan = plan_firmware_client(false, snapshot, true, "192.168.1.117");
        CHECK_FALSE(plan.let_go);
        CHECK(plan.keep_sink);
        // The tab closed, or another sink selected.
        plan = plan_firmware_client(false, snapshot, false, "");
        CHECK(plan.let_go);
        CHECK_FALSE(plan.keep_sink);
    }
    SECTION("a sink at a new address is asked there, and the client let go keeps nothing") {
        snapshot.update = ac3::hearth::SinkFirmware::Update{
            .version = "v0.11.0", .stage = "done", .outcome = UpdateOutcome::kUpdated, .text = "updated"};
        const FirmwareClientPlan plan = plan_firmware_client(false, snapshot, true, "192.168.1.200");
        CHECK(plan.let_go);
        CHECK_FALSE(plan.keep_sink);
    }
}

TEST_CASE("sink firmware: a chosen file, as the dialog asks about it", "[hearth][sink-firmware]") {
    ImageSpec running;
    ImageSpec update_spec;
    update_spec.elf_seed = 40;
    ac3::hearth::SinkFirmware::Snapshot snapshot;
    snapshot.host = "192.168.1.156";

    ac3::hearth::FirmwareCandidate candidate = ac3::hearth::to_candidate(file_of(update_spec), snapshot);
    CHECK(candidate.version == "v0.11.0");
    CHECK(candidate.text.starts_with("ac3forge_hearth_sink v0.11.0, for an ESP32-S3, 1,"));
    CHECK(candidate.refusal == "the sink has not said what it is yet");

    snapshot.hardware = s3_hardware();
    snapshot.firmware = s3_firmware(running);
    candidate = ac3::hearth::to_candidate(file_of(update_spec), snapshot);
    CHECK(candidate.refusal.empty());
    CHECK(ac3::hearth::to_candidate(file_of(running), snapshot).refusal == "the sink already runs this image");
    ImageSpec c6 = update_spec;
    c6.chip_id = 0x000D;
    CHECK(ac3::hearth::to_candidate(file_of(c6), snapshot).refusal ==
          "this image is for an ESP32-C6, and this board is an ESP32-S3");
}
