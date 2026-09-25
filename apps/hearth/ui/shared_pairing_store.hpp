#pragma once

#include <QDate>
#include <QSettings>
#include <QString>

// qsettings_store.hpp undefines Qt's `slots` macro before settings_model.hpp's
// include chain reaches ac3::render (that header's own comment), so it comes
// before pairing_store.hpp here.
#include "qsettings_store.hpp"

#include "pairing_store.hpp"

#include <memory>
#include <string>

// The one PairingStore a Hearth process has. NetworkController's ServerHost pairs
// through it, and HearthController's Settings page lists and forgets its records
// - two stores over the same settings would each keep their own copy of the
// records (PairingStore reads the settings once, when it is made), so a pairing
// made on the Network page would be missing from the Settings page until a
// restart, and a record forgotten there would still be used by the network side
// and written back by its next save.
//
// The store has a QSettings of its own, which nothing else uses: PairingStore's
// own comment says the settings store under it must be one nothing else touches,
// since the network's thread writes it too. Changes made through one QSettings
// object are visible at once through any other in the same process over the
// same settings, so the controllers' own QSettings still read what it writes.
//
// Only the GUI thread calls this (each controller's constructor). The store
// lives while a controller holds it, so it is made and destroyed while Qt is
// up rather than by static destruction after it has gone.

namespace ac3::hearth::ui {

[[nodiscard]] inline std::shared_ptr<ac3::hearth::PairingStore> shared_pairing_store() {
    struct Shared {
        // The four-argument constructor, as both controllers' own: the
        // two-argument one always uses the native store, whatever
        // QSettings::setDefaultFormat() says (hearth_controller.cpp's own
        // comment on why that matters for the test suites).
        QSettings settings{QSettings::defaultFormat(), QSettings::UserScope, QStringLiteral("ac3forge"),
                           QStringLiteral("Hearth")};
        QSettingsStore store{settings};
        ac3::hearth::PairingStore pairing{
            store, [] { return QDate::currentDate().toString(Qt::ISODate).toStdString(); }};
    };
    static std::weak_ptr<Shared> held;
    std::shared_ptr<Shared> shared = held.lock();
    if (!shared) {
        shared = std::make_shared<Shared>();
        held = shared;
    }
    return {shared, &shared->pairing};
}

}  // namespace ac3::hearth::ui
