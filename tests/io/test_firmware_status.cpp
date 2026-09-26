// GET /firmware's body, tested on the host - see ac3forge/firmware_status.hpp.

#include <string>

#include <catch2/catch_test_macros.hpp>

#include "ac3forge/firmware_status.hpp"

using ac3forge::FirmwareLastUpdate;
using ac3forge::FirmwarePartition;
using ac3forge::FirmwareSlot;
using ac3forge::FirmwareStatus;
using ac3forge::FirmwareTrial;
using ac3forge::FirmwareUpload;
using ac3forge::render_firmware_status;

TEST_CASE("a board with nothing to report has every key, the parts that do not apply null",
          "[io][firmware_status]") {
    const std::string body = render_firmware_status(FirmwareStatus{});
    CHECK(body ==
          "{\"mode\":\"normal\",\"running\":null,\"other\":null,\"trial\":null,\"upload\":null,"
          "\"last_update\":null,\"coredump\":null,\"network\":\"none\",\"slot_bytes\":0,\"flash_bytes\":0,"
          "\"partitions\":[],\"bootloader_version\":\"\",\"reset_reason\":\"\",\"uptime_ms\":0}\n");
}

TEST_CASE("why the board last started, and how long ago, end the document", "[io][firmware_status]") {
    FirmwareStatus status;
    status.bootloader_version = "v6.1";
    status.reset_reason = "sw";
    status.uptime_ms = 12'345;
    const std::string body = render_firmware_status(status);
    CHECK(body.find("\"bootloader_version\":\"v6.1\",\"reset_reason\":\"sw\",\"uptime_ms\":12345}\n") !=
          std::string::npos);
}

TEST_CASE("a core dump the last crash left is reported with where it came from", "[io][firmware_status]") {
    FirmwareStatus status;
    status.coredump = ac3forge::FirmwareCoredump{23'456, true, "fw_trial", "0x4037a1b2",
                                                 "abort() was called at PC 0x4200abcd on core 0", "2366bde99"};
    const std::string body = render_firmware_status(status);
    CHECK(body.find("\"last_update\":null,\"coredump\":{\"bytes\":23456,\"intact\":true,\"task\":\"fw_trial\","
                    "\"pc\":\"0x4037a1b2\",\"reason\":\"abort() was called at PC 0x4200abcd on core 0\","
                    "\"elf_sha256\":\"2366bde99\"},\"network\"") != std::string::npos);
}

TEST_CASE("a board on trial reports both slots, the trial and the table", "[io][firmware_status]") {
    FirmwareStatus status;
    status.mode = "normal";
    FirmwareSlot running;
    running.label = "ota_1";
    running.state = "trial";
    running.version = "v0.10.0-beta.1-1900-gabc";
    running.project = "ac3forge_hearth_sink";
    running.idf_version = "v6.1";
    running.elf_sha256 = "aa";
    running.image_sha256 = "bb";
    running.intact = true;
    status.running = running;
    FirmwareSlot other = running;
    other.label = "ota_0";
    other.state = "valid";
    other.intact.reset();
    status.other = other;
    status.trial = FirmwareTrial{12'000, 30'000, 241'000, {"a network address"}};
    status.network = "stored";
    status.slot_bytes = 4'194'304;
    status.flash_bytes = 16'777'216;
    status.partitions.push_back(FirmwarePartition{"nvs", 1, 2, 0x9000, 0x6000});
    status.partitions.push_back(FirmwarePartition{"ota_0", 0, 0x10, 0x20000, 0x400000});
    status.bootloader_version = "v6.1";

    const std::string body = render_firmware_status(status);
    CHECK(body.find("\"running\":{\"label\":\"ota_1\",\"state\":\"trial\",\"version\":"
                    "\"v0.10.0-beta.1-1900-gabc\",\"project\":\"ac3forge_hearth_sink\",\"idf_version\":"
                    "\"v6.1\",\"elf_sha256\":\"aa\",\"image_sha256\":\"bb\",\"intact\":true}") !=
          std::string::npos);
    CHECK(body.find("\"other\":{\"label\":\"ota_0\",\"state\":\"valid\"") != std::string::npos);
    CHECK(body.find("\"intact\":null}") != std::string::npos);
    CHECK(body.find("\"trial\":{\"healthy_for_ms\":12000,\"hold_ms\":30000,\"remaining_ms\":241000,"
                    "\"waiting_for\":[\"a network address\"]}") != std::string::npos);
    CHECK(body.find("\"partitions\":[{\"label\":\"nvs\",\"type\":1,\"subtype\":2,\"offset\":36864,"
                    "\"size\":24576},{\"label\":\"ota_0\",\"type\":0,\"subtype\":16,\"offset\":131072,"
                    "\"size\":4194304}]") != std::string::npos);
    CHECK(body.find("\"network\":\"stored\",\"slot_bytes\":4194304,\"flash_bytes\":16777216") !=
          std::string::npos);
}

TEST_CASE("an upload and the last update are reported as they stand", "[io][firmware_status]") {
    FirmwareStatus status;
    status.mode = "flash";
    status.upload = FirmwareUpload{524'288, 1'419'104, "writing"};
    status.last_update = FirmwareLastUpdate{"v0.10.0-beta.1-1850-gdef", "rolled back", "panic"};
    const std::string body = render_firmware_status(status);
    CHECK(body.find("\"mode\":\"flash\"") != std::string::npos);
    CHECK(body.find("\"upload\":{\"received\":524288,\"total\":1419104,\"stage\":\"writing\"}") !=
          std::string::npos);
    CHECK(body.find("\"last_update\":{\"version\":\"v0.10.0-beta.1-1850-gdef\",\"result\":\"rolled back\","
                    "\"reason\":\"panic\"}") != std::string::npos);
}

TEST_CASE("text a build or a failure chose cannot break the document", "[io][firmware_status]") {
    FirmwareStatus status;
    status.last_update = FirmwareLastUpdate{"v\"1\\", "failed", "line one\nline two\t"};
    const std::string body = render_firmware_status(status);
    CHECK(body.find("\"version\":\"v\\\"1\\\\\"") != std::string::npos);
    CHECK(body.find("\"reason\":\"line one\\u000aline two\\u0009\"") != std::string::npos);
}
