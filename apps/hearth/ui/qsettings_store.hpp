#pragma once

#include <QSettings>
#include <QString>
#include <QVariant>

// hearth_controller.hpp's Qt headers define `slots` as a macro unless
// QT_NO_KEYWORDS is set, which this project's Qt targets do not
// (hearth-ui-qt-slots-macro-collides-with-render-layout): ac3::render::
// OutputLayout::slots() is a real method name, and left alone the macro
// rewrites its declaration into nonsense. Undefined here, before
// settings_model.hpp's own include chain reaches it, so any translation
// unit that includes this header - whether or not it has already undefined
// the macro itself - gets a safe include order.
#undef slots

#include "settings_model.hpp"

#include <optional>
#include <string>
#include <string_view>

// ac3::hearth::SettingsStore over QSettings (planning/hearth-reference-
// player.md, A5: "it stores its settings through QSettings"). The key
// strings settings_model.cpp and pairing_store.cpp already compose
// ("playback/gapless", "queue/1/path", "pairing/1/client", ...) are plain
// QSettings keys, so this is a thin pass-through that does not have to know
// what any of them mean - MemorySettingsStore (settings_model.cpp) is the
// test suites' own version of the same three methods.
//
// Used by HearthController (its own settings_), NetworkController (its own
// settings_, for the server identity and the network settings) and the one
// pairing store both share (shared_pairing_store.hpp), each over its own
// QSettings with the same "ac3forge"/"Hearth" identity - the two controllers
// are independent the same way their engine-side objects are
// (network_controller.hpp's own comment).

namespace ac3::hearth::ui {

class QSettingsStore final : public ac3::hearth::SettingsStore {
public:
    explicit QSettingsStore(QSettings& settings) : settings_(settings) {}

    [[nodiscard]] std::optional<std::string> value(std::string_view key) const override {
        const QVariant found =
            settings_.value(QString::fromUtf8(key.data(), static_cast<qsizetype>(key.size())));
        if (!found.isValid()) {
            return std::nullopt;
        }
        return found.toString().toStdString();
    }

    void set_value(std::string_view key, std::string_view value) override {
        settings_.setValue(QString::fromUtf8(key.data(), static_cast<qsizetype>(key.size())),
                           QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())));
    }

    void remove_group(std::string_view group) override {
        // QSettings::remove() already removes the key itself and everything
        // under it; MemorySettingsStore::remove_group() only says so in a
        // comment because it has to do that walk by hand.
        settings_.remove(QString::fromUtf8(group.data(), static_cast<qsizetype>(group.size())));
    }

    [[nodiscard]] bool sync() override {
        settings_.sync();
        return settings_.status() == QSettings::NoError;
    }

private:
    QSettings& settings_;
};

}  // namespace ac3::hearth::ui
