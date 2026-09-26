#include "sink_firmware_view.hpp"

#include <cctype>

#include <fmt/format.h>

namespace ac3::hearth {

namespace {

// A slot's state as the page says it (ota.py's slot_text, in words).
[[nodiscard]] std::string state_words(std::string_view state) {
    if (state == "valid") {
        return "accepted";
    }
    if (state == "trial") {
        return "on trial";
    }
    if (state == "new") {
        return "written, not booted yet";
    }
    if (state == "aborted") {
        return "did not last";
    }
    if (state == "undefined") {
        return "flashed over USB, not booted yet";
    }
    return state.empty() ? std::string("in a state it does not name") : std::string(state);
}

[[nodiscard]] std::string intact_words(const std::optional<bool>& intact) {
    if (!intact) {
        return "not checked yet";
    }
    return *intact ? "checked intact" : "does not check out";
}

[[nodiscard]] std::string slot_text(const ac3forge::FirmwareSlot& slot) {
    if (slot.state == "empty") {
        return fmt::format("{} · empty", slot.label);
    }
    return fmt::format("{} · {} · {} · {}", slot.version.empty() ? std::string("(no version)") : slot.version,
                       slot.label, state_words(slot.state), intact_words(slot.intact));
}

// An address in a URL: an IPv6 one in brackets.
[[nodiscard]] std::string url_host(std::string_view host, std::uint16_t port) {
    std::string out = host.find(':') != std::string_view::npos ? fmt::format("[{}]", host) : std::string(host);
    if (port != 80) {
        out += fmt::format(":{}", port);
    }
    return out;
}

// Which slot's image wrote a core dump, by the start of the ELF SHA-256 the
// dump keeps (ota.py's coredump_source).
[[nodiscard]] std::string coredump_source(const ac3forge::FirmwareCoredump& dump,
                                          const ac3forge::FirmwareStatus& firmware) {
    if (dump.elf_sha256.empty()) {
        return "an image the dump does not name";
    }
    for (const std::optional<ac3forge::FirmwareSlot>* held : {&firmware.running, &firmware.other}) {
        if (*held && (*held)->elf_sha256.starts_with(dump.elf_sha256)) {
            return fmt::format("{} in {}", (*held)->version, (*held)->label);
        }
    }
    return fmt::format("an image neither slot holds now (ELF SHA-256 {}...)", dump.elf_sha256);
}

[[nodiscard]] std::string crash_text(const ac3forge::FirmwareCoredump& dump,
                                     const ac3forge::FirmwareStatus& firmware) {
    std::string text = fmt::format("{} bytes", grouped_number(dump.bytes));
    if (!dump.intact) {
        text += ", which do not check out";
    }
    if (!dump.task.empty()) {
        text += fmt::format(" · {} at {}", dump.task, dump.pc);
    }
    if (!dump.reason.empty()) {
        text += " · " + dump.reason;
    }
    return text + " · written by " + coredump_source(dump, firmware);
}

// ota.py's outcome lines start in lower case, being printed after the board's
// name; here each is a sentence of its own.
[[nodiscard]] std::string sentence(std::string text) {
    if (!text.empty()) {
        text.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(text.front())));
    }
    return text;
}

[[nodiscard]] std::string outcome_name(UpdateOutcome outcome) {
    switch (outcome) {
        case UpdateOutcome::kUpdated: return "updated";
        case UpdateOutcome::kRolledBack: return "rolledBack";
        case UpdateOutcome::kRefused: return "refused";
        case UpdateOutcome::kFailed: return "failed";
        case UpdateOutcome::kSilent: return "silent";
        case UpdateOutcome::kNone: default: return {};
    }
}

}  // namespace

FirmwarePanel to_firmware_panel(const SinkFirmware::Snapshot& snapshot, std::string_view app_version) {
    FirmwarePanel panel;
    const std::string base = "http://" + url_host(snapshot.host, snapshot.port);
    panel.page_url = base + "/";
    panel.log_url = base + "/log";
    panel.answering = snapshot.answering;
    if (!snapshot.asked) {
        panel.status_text = "Asking the sink about its firmware…";
    } else if (!snapshot.answering) {
        panel.status_text = sentence(fmt::format("not answering at {}: {}", panel.page_url, snapshot.error));
    }

    const bool updating = snapshot.update && snapshot.update->stage != "done";
    panel.updating = updating;
    if (snapshot.update) {
        const SinkFirmware::Update& update = *snapshot.update;
        if (updating) {
            panel.progress = update.stage == "sending" && update.total > 0
                                 ? static_cast<double>(update.sent) / static_cast<double>(update.total)
                                 : -1.0;
            panel.progress_text = update.stage == "sending" && update.total > 0
                                      ? fmt::format("Sending {}: {} of {} bytes", update.version,
                                                    grouped_number(update.sent), grouped_number(update.total))
                                      : sentence(update.text);
        } else {
            panel.outcome = outcome_name(update.outcome);
            panel.outcome_text = sentence(update.text);
        }
    }
    panel.action_text = sentence(snapshot.action);

    if (!snapshot.firmware) {
        return panel;
    }
    const ac3forge::FirmwareStatus& firmware = *snapshot.firmware;
    panel.reported = true;
    if (firmware.other && firmware.other->state != "empty") {
        panel.other_version = firmware.other->version;
    }
    if (firmware.running) {
        panel.running_text = slot_text(*firmware.running);
        panel.running_version = firmware.running->version;
        if (!app_version.empty()) {
            // The board keeps 31 characters of a version (esp_app_desc_t's
            // field), so a longer describe is compared by as much.
            const std::string_view ours = app_version.substr(0, 31);
            panel.same_build = firmware.running->version == ours;
            panel.build_text = panel.same_build
                                   ? std::string("The same build as this app.")
                                   : fmt::format("Not this app's build: this app is {}.", app_version);
        }
    }
    panel.other_text = firmware.other ? slot_text(*firmware.other) : std::string("none: one app slot");
    if (firmware.mode == "flash") {
        panel.mode_text = "Flash mode: nothing plays until the board restarts.";
    }
    if (firmware.trial) {
        panel.trial_text = sentence(trial_words(*firmware.trial));
    }
    // What another client is sending; this app's own upload shows as its
    // progress instead.
    if (firmware.upload && !updating) {
        panel.upload_text = fmt::format("Another update is under way: {}, {} of {} bytes", firmware.upload->stage,
                                        grouped_number(firmware.upload->received),
                                        grouped_number(firmware.upload->total));
    }
    if (firmware.last_update) {
        const ac3forge::FirmwareLastUpdate& last = *firmware.last_update;
        panel.last_update_text =
            fmt::format("{}: {}", last.version.empty() ? std::string("(no version)") : last.version, last.result);
        if (!last.reason.empty()) {
            panel.last_update_text += fmt::format(" ({})", last.reason);
        }
    }
    if (firmware.coredump) {
        panel.crash_text = crash_text(*firmware.coredump, firmware);
        panel.coredump_url = base + "/firmware/coredump";
    }

    const bool on_trial = firmware.trial || (firmware.running && firmware.running->state == "trial");
    const bool idle = snapshot.answering && !updating && !firmware.upload;
    panel.can_update = idle && firmware.other.has_value() && !on_trial;
    // On trial, a rollback gives the trial up (planning/esp32-ota.md, Routes).
    panel.can_rollback = idle && (on_trial || (firmware.other && firmware.other->state == "valid"));
    // A restart on trial would roll back, which the board refuses.
    panel.can_restart = idle && !on_trial;
    return panel;
}

FirmwareCandidate to_candidate(const FirmwareFile& file, const SinkFirmware::Snapshot& snapshot) {
    FirmwareCandidate candidate;
    candidate.version = file.head.version;
    const std::string chip = ac3forge::detail::chip_name(file.head.chip_id);
    candidate.text = fmt::format("{} {}, for {} {}, {} bytes",
                                 file.head.project.empty() ? std::string("an unnamed project") : file.head.project,
                                 file.head.version, chip.starts_with("ESP") ? "an" : "a", chip,
                                 grouped_number(file.data.size()));
    if (!snapshot.hardware || !snapshot.firmware) {
        candidate.refusal = "the sink has not said what it is yet";
        return candidate;
    }
    if (snapshot.firmware->running && snapshot.firmware->running->elf_sha256 == file.elf_sha256) {
        candidate.refusal = "the sink already runs this image";
        return candidate;
    }
    if (std::optional<std::string> why = refuse_update(file, *snapshot.hardware, *snapshot.firmware)) {
        candidate.refusal = std::move(*why);
    }
    return candidate;
}

}  // namespace ac3::hearth
