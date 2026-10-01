#include "settings_migration.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QVariant>

namespace iclforge::settings_migration {

namespace {

// The keys that record the copy in the new store. They are not counted in Outcome::keys.
constexpr auto kSourceKey = "migration/fromIdentity";
constexpr auto kKeysKey = "migration/copiedKeys";

// The directory of QStandardPaths::CacheLocation inside a program's local data directory.
constexpr auto kCacheDirectory = "cache";

// The organisation every program of the family stored its settings under before N1A, and the
// application names of the three programs and of the desktop demo Crucible grew out of.
constexpr auto kFormerOrganization = "ac3forge";

// The four-argument constructor, as every store of these programs is opened: the two-argument one
// always takes the native store whatever QSettings::setDefaultFormat() says, which would make this
// read and write the developer's real registry from a test process.
QSettings open(const Identity& id) {
    return QSettings(QSettings::defaultFormat(), QSettings::UserScope, id.organization,
                     id.application);
}

[[nodiscard]] bool holds_keys(const Identity& id) {
    QSettings store = open(id);
    store.setFallbacksEnabled(false);
    return !store.allKeys().isEmpty();
}

[[nodiscard]] QString label(const Identity& id) {
    return id.organization + QLatin1Char('/') + id.application;
}

constexpr QStandardPaths::StandardLocation kDirectories[] = {
    QStandardPaths::AppDataLocation,
    QStandardPaths::AppLocalDataLocation,
    QStandardPaths::AppConfigLocation,
};

}  // namespace

QString standard_directory(const Identity& id, QStandardPaths::StandardLocation where) {
    const QString organization = QCoreApplication::organizationName();
    const QString application = QCoreApplication::applicationName();
    QCoreApplication::setOrganizationName(id.organization);
    QCoreApplication::setApplicationName(id.application);
    const QString path = QStandardPaths::writableLocation(where);
    QCoreApplication::setOrganizationName(organization);
    QCoreApplication::setApplicationName(application);
    return path;
}

int copy_tree_once(const QString& from, const QString& to) {
    const QDir source(from);
    if (!source.exists() ||
        QFileInfo(from).absoluteFilePath() == QFileInfo(to).absoluteFilePath()) {
        return 0;
    }
    const QDir target(to);
    const QStringList taken =
        target.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QString& entry : taken) {
        if (entry.compare(QLatin1String(kCacheDirectory), Qt::CaseInsensitive) != 0) {
            return 0;
        }
    }
    int copied = 0;
    QDirIterator files(from, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                       QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString path = files.next();
        const QString relative = source.relativeFilePath(path);
        // Qt keeps its QML and shader caches under <local data>/cache: regenerable, and made for
        // the program that wrote them, so not what a person saved.
        if (relative.startsWith(QLatin1String(kCacheDirectory) + QLatin1Char('/'),
                                Qt::CaseInsensitive)) {
            continue;
        }
        const QString destination = target.filePath(relative);
        if (!QDir().mkpath(QFileInfo(destination).absolutePath()) ||
            (!QFileInfo::exists(destination) && !QFile::copy(path, destination))) {
            qWarning("settings migration: could not copy %s to %s", qPrintable(path),
                     qPrintable(destination));
            return -1;
        }
        ++copied;
    }
    return copied;
}

Outcome migrate_settings(const Identity& current, const std::vector<Former>& former) {
    Outcome outcome;
    if (holds_keys(current)) {
        outcome.status = Status::kAlreadyMigrated;
        return outcome;
    }
    for (const Former& candidate : former) {
        QSettings previous = open(candidate.identity);
        previous.setFallbacksEnabled(false);
        const QStringList keys = previous.allKeys();
        if (keys.isEmpty()) {
            continue;
        }
        QSettings store = open(current);
        for (const QString& key : keys) {
            store.setValue(key, previous.value(key));
        }
        store.setValue(QLatin1String(kSourceKey), label(candidate.identity));
        store.setValue(QLatin1String(kKeysKey), static_cast<int>(keys.size()));
        if (!candidate.marker_key.isEmpty()) {
            store.setValue(candidate.marker_key, true);
        }
        store.sync();
        if (store.status() != QSettings::NoError) {
            qWarning("settings migration: could not write the settings of %s",
                     qPrintable(label(current)));
            outcome.status = Status::kFailed;
            return outcome;
        }
        outcome.status = Status::kMigrated;
        outcome.source = candidate.identity;
        outcome.keys = static_cast<int>(keys.size());
        return outcome;
    }
    return outcome;
}

Outcome migrate(const Identity& current, const std::vector<Former>& former) {
    if (holds_keys(current)) {
        Outcome done;
        done.status = Status::kAlreadyMigrated;
        return done;
    }
    Outcome outcome = migrate_settings(current, former);
    if (outcome.status == Status::kFailed) {
        return outcome;
    }
    // The files of a program, after its settings: the same rule, per directory. A former identity
    // that has files in a directory is the one copied from, in the order they are listed.
    int files = 0;
    bool failed = false;
    for (const QStandardPaths::StandardLocation where : kDirectories) {
        const QString target = standard_directory(current, where);
        for (const Former& candidate : former) {
            const int copied =
                copy_tree_once(standard_directory(candidate.identity, where), target);
            if (copied < 0) {
                failed = true;
                break;
            }
            if (copied > 0) {
                files += copied;
                break;
            }
        }
    }
    outcome.files = files;
    if (failed) {
        outcome.status = Status::kFailed;
        return outcome;
    }
    if (files > 0 && outcome.status == Status::kNothingToMigrate) {
        // Only files: the record is what tells the next start the new store is in use.
        QSettings store = open(current);
        store.setValue(QLatin1String(kSourceKey), label(former.front().identity));
        store.setValue(QLatin1String(kKeysKey), 0);
        store.sync();
        outcome.status = store.status() == QSettings::NoError ? Status::kMigrated : Status::kFailed;
    }
    return outcome;
}

Identity identity_of(Program program) {
    switch (program) {
        case Program::kForgeGui:
            return {QStringLiteral("iclforge"), QStringLiteral("forge-gui")};
        case Program::kHearth:
            return {QStringLiteral("iclforge"), QStringLiteral("Hearth")};
        case Program::kCrucible:
            return {QStringLiteral("iclforge"), QStringLiteral("Crucible")};
    }
    return {};
}

std::vector<Former> former_identities(Program program) {
    const QString organization = QLatin1String(kFormerOrganization);
    switch (program) {
        case Program::kForgeGui:
            // The GUI's application name was the organisation's.
            return {{{organization, organization}, {}}};
        case Program::kHearth:
            return {{{organization, QStringLiteral("Hearth")}, {}}};
        case Program::kCrucible:
            // The product's own store first, then the desktop demo's, which the product copied
            // the first time it found its own empty: a machine that never ran the product
            // keeps the demo's settings, and its first-run dialog says where they came from.
            return {{{organization, QStringLiteral("Crucible")}, {}},
                    {{organization, QStringLiteral("DesktopAtmos")},
                     QStringLiteral("migration/fromDesktopAtmos")}};
    }
    return {};
}

Outcome migrate_program(Program program) {
    const Identity current = identity_of(program);
    const Outcome outcome = migrate(current, former_identities(program));
    switch (outcome.status) {
        case Status::kMigrated:
            qInfo("settings migration: %d keys and %d files of %s are now %s", outcome.keys,
                  outcome.files, qPrintable(label(outcome.source)), qPrintable(label(current)));
            break;
        case Status::kFailed:
            qWarning("settings migration of %s failed; the old settings are untouched",
                     qPrintable(label(current)));
            break;
        case Status::kNothingToMigrate:
        case Status::kAlreadyMigrated:
            break;
    }
    return outcome;
}

}  // namespace iclforge::settings_migration
