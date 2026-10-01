#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QUuid>
#include <QVariant>

#include <vector>

#include "settings_migration.hpp"

// What forge-gui, Hearth and Crucible copy at start-up from the store their old names kept
// (apps/gui/settings_migration.hpp): once, before anything reads a setting, only when the new store
// holds nothing of its own, never writing the old one. The stores are INI files in a temporary
// directory (QSettings::setPath), the way the Qt Quick suites isolate theirs, so no test reads or
// writes the registry or a real user's settings, and the directories of QStandardPaths are the
// ones Qt keeps for tests (setTestModeEnabled) under identities no real program has.

using iclforge::settings_migration::copy_tree_once;
using iclforge::settings_migration::Former;
using iclforge::settings_migration::former_identities;
using iclforge::settings_migration::Identity;
using iclforge::settings_migration::identity_of;
using iclforge::settings_migration::migrate;
using iclforge::settings_migration::migrate_program;
using iclforge::settings_migration::migrate_settings;
using iclforge::settings_migration::Outcome;
using iclforge::settings_migration::Program;
using iclforge::settings_migration::standard_directory;
using iclforge::settings_migration::Status;

namespace {

const Identity kNow{QStringLiteral("iclforge"), QStringLiteral("forge-gui")};
const Identity kBefore{QStringLiteral("ac3forge"), QStringLiteral("ac3forge")};

// The settings format and directory the test runs under. The temporary directory goes when the
// test does, and with it every store written to it.
class Scratch {
   public:
    Scratch() {
        REQUIRE(dir_.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir_.path());
        QStandardPaths::setTestModeEnabled(true);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    [[nodiscard]] QString path() const { return dir_.path(); }

   private:
    QTemporaryDir dir_;
};

QSettings open(const Identity& id) {
    return QSettings(QSettings::IniFormat, QSettings::UserScope, id.organization, id.application);
}

void write(const Identity& id, const QMap<QString, QVariant>& values) {
    QSettings store = open(id);
    for (auto it = values.begin(); it != values.end(); ++it) {
        store.setValue(it.key(), it.value());
    }
    store.sync();
    REQUIRE(store.status() == QSettings::NoError);
}

// Every key of the application's own store with its text, without the organisation's fallback.
QMap<QString, QString> read(const Identity& id) {
    QSettings store = open(id);
    store.setFallbacksEnabled(false);
    QMap<QString, QString> out;
    for (const QString& key : store.allKeys()) {
        out.insert(key, store.value(key).toString());
    }
    return out;
}

QString file_of(const Identity& id) {
    return open(id).fileName();
}

QByteArray bytes_of(const QString& path) {
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}

// What a person's store holds: a preference of each kind, a session, and the keys Hearth keeps its
// pairings and its server identity under (qsettings_store.hpp, pairing_store.hpp).
const QString kHex =
    QStringLiteral("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");

QMap<QString, QVariant> a_persons_settings() {
    return {
        {QStringLiteral("workbench/theme"), QStringLiteral("dark")},
        {QStringLiteral("workbench/textScale"), 1.25},
        {QStringLiteral("workbench/restoreSession"), false},
        {QStringLiteral("workbench/defaultBitrateKbps"), 448},
        {QStringLiteral("workbench/recent"),
         QStringList{QStringLiteral("a.wav"), QStringLiteral("b.wav")}},
        {QStringLiteral("workbench/geometry"), QByteArray("\x01\x02\xd9\xff", 4)},
        {QStringLiteral("workbench/outputFolder"),
         QStringLiteral("C:/Users/\u00c9lo\u00efse/Music")},
        {QStringLiteral("language/code"), QStringLiteral("fr")},
        {QStringLiteral("pairing/size"), 1},
        {QStringLiteral("pairing/1/client"), kHex},
        {QStringLiteral("pairing/1/psk"), kHex},
        {QStringLiteral("pairing/1/name"), QStringLiteral("Living room")},
        {QStringLiteral("pairing/1/paired"), QStringLiteral("2026-09-01")},
        {QStringLiteral("identity/server"), kHex},
    };
}

}  // namespace

TEST_CASE("the old store is copied to a new one that holds nothing", "[settings][migration]") {
    const Scratch scratch;
    write(kBefore, a_persons_settings());
    const auto before = read(kBefore);

    const Outcome outcome = migrate_settings(kNow, {{kBefore, {}}});

    CHECK(outcome.status == Status::kMigrated);
    CHECK(outcome.keys == before.size());
    CHECK(outcome.source.organization == QStringLiteral("ac3forge"));
    const auto after = read(kNow);
    for (auto it = before.begin(); it != before.end(); ++it) {
        INFO(it.key().toStdString());
        REQUIRE(after.contains(it.key()));
        CHECK(after.value(it.key()) == it.value());
    }
}

TEST_CASE("the pairing records and the server identity are copied to the letter",
          "[settings][migration]") {
    const Scratch scratch;
    write(kBefore, a_persons_settings());

    REQUIRE(migrate_settings(kNow, {{kBefore, {}}}).status == Status::kMigrated);

    QSettings store = open(kNow);
    CHECK(store.value(QStringLiteral("pairing/size")).toInt() == 1);
    CHECK(store.value(QStringLiteral("pairing/1/client")).toString() == kHex);
    CHECK(store.value(QStringLiteral("pairing/1/psk")).toString() == kHex);
    CHECK(store.value(QStringLiteral("pairing/1/name")).toString() ==
          QStringLiteral("Living room"));
    CHECK(store.value(QStringLiteral("identity/server")).toString() == kHex);
    CHECK(store.value(QStringLiteral("workbench/outputFolder")).toString() ==
          QStringLiteral("C:/Users/\u00c9lo\u00efse/Music"));
    CHECK(store.value(QStringLiteral("workbench/recent")).toStringList().size() == 2);
    CHECK(store.value(QStringLiteral("workbench/geometry")).toByteArray() ==
          QByteArray("\x01\x02\xd9\xff", 4));
}

TEST_CASE("the copy is recorded in the new store and not counted as a setting",
          "[settings][migration]") {
    const Scratch scratch;
    write(kBefore, {{QStringLiteral("a"), 1}, {QStringLiteral("b/c"), 2}});

    const Outcome outcome = migrate_settings(kNow, {{kBefore, {}}});

    CHECK(outcome.keys == 2);
    QSettings store = open(kNow);
    CHECK(store.value(QStringLiteral("migration/fromIdentity")).toString() ==
          QStringLiteral("ac3forge/ac3forge"));
    CHECK(store.value(QStringLiteral("migration/copiedKeys")).toInt() == 2);
}

TEST_CASE("the old store is never written", "[settings][migration]") {
    const Scratch scratch;
    write(kBefore, a_persons_settings());
    const QString file = file_of(kBefore);
    const QByteArray bytes = bytes_of(file);
    const QDateTime modified = QFileInfo(file).lastModified();

    REQUIRE(migrate_settings(kNow, {{kBefore, {}}}).status == Status::kMigrated);

    CHECK(bytes_of(file) == bytes);
    CHECK(QFileInfo(file).lastModified() == modified);
    CHECK(read(kBefore).size() == a_persons_settings().size());
}

TEST_CASE("a new store with settings of its own is left as it is", "[settings][migration]") {
    const Scratch scratch;
    write(kNow, {{QStringLiteral("workbench/theme"), QStringLiteral("light")}});
    const QByteArray bytes = bytes_of(file_of(kNow));

    const Outcome outcome = migrate_settings(kNow, {{kBefore, {}}});

    CHECK(outcome.status == Status::kAlreadyMigrated);
    CHECK(bytes_of(file_of(kNow)) == bytes);
    CHECK_FALSE(QFileInfo::exists(file_of(kBefore)));
}

TEST_CASE("when both stores hold settings the new one wins and keeps only its own",
          "[settings][migration]") {
    const Scratch scratch;
    write(kBefore, a_persons_settings());
    write(kNow, {{QStringLiteral("workbench/theme"), QStringLiteral("light")}});
    const QByteArray old_bytes = bytes_of(file_of(kBefore));

    const Outcome outcome = migrate_settings(kNow, {{kBefore, {}}});

    CHECK(outcome.status == Status::kAlreadyMigrated);
    const auto after = read(kNow);
    CHECK(after.size() == 1);
    CHECK(after.value(QStringLiteral("workbench/theme")) == QStringLiteral("light"));
    CHECK(bytes_of(file_of(kBefore)) == old_bytes);
}

TEST_CASE("with no store at all nothing is written", "[settings][migration]") {
    const Scratch scratch;

    const Outcome outcome = migrate_settings(kNow, {{kBefore, {}}});

    CHECK(outcome.status == Status::kNothingToMigrate);
    CHECK(outcome.keys == 0);
    CHECK_FALSE(QFileInfo::exists(file_of(kNow)));
    CHECK_FALSE(QFileInfo::exists(file_of(kBefore)));
}

TEST_CASE("a second start finds the new store in use and changes nothing",
          "[settings][migration]") {
    const Scratch scratch;
    write(kBefore, {{QStringLiteral("workbench/theme"), QStringLiteral("dark")}});
    REQUIRE(migrate_settings(kNow, {{kBefore, {}}}).status == Status::kMigrated);
    const QByteArray bytes = bytes_of(file_of(kNow));

    const Outcome again = migrate_settings(kNow, {{kBefore, {}}});

    CHECK(again.status == Status::kAlreadyMigrated);
    CHECK(bytes_of(file_of(kNow)) == bytes);

    // What the old release writes afterwards does not flow across: it is copied once.
    write(kBefore, {{QStringLiteral("workbench/later"), QStringLiteral("yes")}});
    CHECK(migrate_settings(kNow, {{kBefore, {}}}).status == Status::kAlreadyMigrated);
    CHECK_FALSE(read(kNow).contains(QStringLiteral("workbench/later")));
}

TEST_CASE("the organisation's own keys are not the application's and are not copied",
          "[settings][migration]") {
    const Scratch scratch;
    write(kBefore, {{QStringLiteral("workbench/theme"), QStringLiteral("dark")}});
    {
        QSettings organisation(QSettings::IniFormat, QSettings::UserScope,
                               QStringLiteral("ac3forge"));
        organisation.setValue(QStringLiteral("organisation/wide"), 9);
        organisation.sync();
    }

    const Outcome outcome = migrate_settings(kNow, {{kBefore, {}}});

    CHECK(outcome.keys == 1);
    CHECK_FALSE(read(kNow).contains(QStringLiteral("organisation/wide")));
}

TEST_CASE("the first former store that holds settings is the one copied", "[settings][migration]") {
    const Scratch scratch;
    const Identity product{QStringLiteral("ac3forge"), QStringLiteral("Crucible")};
    const Identity demo{QStringLiteral("ac3forge"), QStringLiteral("DesktopAtmos")};
    const Identity current{QStringLiteral("iclforge"), QStringLiteral("Crucible")};
    const std::vector<Former> former{{product, {}},
                                     {demo, QStringLiteral("migration/fromDesktopAtmos")}};

    SECTION("the product's store wins over the demo's") {
        write(product, {{QStringLiteral("endpoint"), QStringLiteral("product")}});
        write(demo, {{QStringLiteral("endpoint"), QStringLiteral("demo")}});
        const Outcome outcome = migrate_settings(current, former);
        CHECK(outcome.status == Status::kMigrated);
        CHECK(outcome.source.application == QStringLiteral("Crucible"));
        CHECK(read(current).value(QStringLiteral("endpoint")) == QStringLiteral("product"));
        CHECK_FALSE(read(current).contains(QStringLiteral("migration/fromDesktopAtmos")));
    }
    SECTION(
        "a machine that only ever ran the demo gets the demo's, and the record the dialog reads") {
        write(demo, {{QStringLiteral("endpoint"), QStringLiteral("demo")}});
        const Outcome outcome = migrate_settings(current, former);
        CHECK(outcome.status == Status::kMigrated);
        CHECK(outcome.source.application == QStringLiteral("DesktopAtmos"));
        CHECK(read(current).value(QStringLiteral("endpoint")) == QStringLiteral("demo"));
        CHECK(open(current).value(QStringLiteral("migration/fromDesktopAtmos")).toBool());
    }
}

TEST_CASE("a store that cannot be written is reported and the old one is left alone",
          "[settings][migration]") {
    const Scratch scratch;
    write(kBefore, {{QStringLiteral("workbench/theme"), QStringLiteral("dark")}});
    const QByteArray bytes = bytes_of(file_of(kBefore));
    // The new organisation's directory is a file, so its store cannot be created.
    QFile blocker(QDir(scratch.path()).filePath(kNow.organization));
    REQUIRE(blocker.open(QIODevice::WriteOnly));
    blocker.close();

    const Outcome outcome = migrate_settings(kNow, {{kBefore, {}}});

    CHECK(outcome.status == Status::kFailed);
    CHECK(bytes_of(file_of(kBefore)) == bytes);
}

TEST_CASE("the three programs have the new names and the old organisation",
          "[settings][migration]") {
    CHECK(identity_of(Program::kForgeGui).organization == QStringLiteral("iclforge"));
    CHECK(identity_of(Program::kForgeGui).application == QStringLiteral("forge-gui"));
    CHECK(identity_of(Program::kHearth).application == QStringLiteral("Hearth"));
    CHECK(identity_of(Program::kCrucible).application == QStringLiteral("Crucible"));

    for (const Program program : {Program::kForgeGui, Program::kHearth, Program::kCrucible}) {
        const auto former = former_identities(program);
        REQUIRE_FALSE(former.empty());
        for (const Former& f : former) {
            CHECK(f.identity.organization == QStringLiteral("ac3forge"));
        }
    }
    CHECK(former_identities(Program::kForgeGui).front().identity.application ==
          QStringLiteral("ac3forge"));
    CHECK(former_identities(Program::kHearth).front().identity.application ==
          QStringLiteral("Hearth"));
    const auto crucible = former_identities(Program::kCrucible);
    REQUIRE(crucible.size() == 2);
    CHECK(crucible[0].identity.application == QStringLiteral("Crucible"));
    CHECK(crucible[1].identity.application == QStringLiteral("DesktopAtmos"));
    CHECK(crucible[1].marker_key == QStringLiteral("migration/fromDesktopAtmos"));
}

TEST_CASE("a program's start-up copy takes its old store and leaves the old one",
          "[settings][migration]") {
    const Scratch scratch;
    // Hearth's real identities, in the scratch directory: nothing here reads the registry.
    const Identity old_hearth = former_identities(Program::kHearth).front().identity;
    write(old_hearth, a_persons_settings());
    const QByteArray bytes = bytes_of(file_of(old_hearth));

    const Outcome first = migrate_program(Program::kHearth);
    const Outcome second = migrate_program(Program::kHearth);

    CHECK(first.status == Status::kMigrated);
    CHECK(first.keys == a_persons_settings().size());
    CHECK(second.status == Status::kAlreadyMigrated);
    CHECK(read(identity_of(Program::kHearth)).value(QStringLiteral("pairing/1/psk")) == kHex);
    CHECK(bytes_of(file_of(old_hearth)) == bytes);
    // The other two programs did not see Hearth's store as theirs.
    CHECK(migrate_program(Program::kForgeGui).status == Status::kNothingToMigrate);
    CHECK(migrate_program(Program::kCrucible).status == Status::kNothingToMigrate);
}

namespace {

void put(const QString& path, const QByteArray& bytes) {
    REQUIRE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    REQUIRE(file.write(bytes) == bytes.size());
}

QByteArray get(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<missing>");
}

}  // namespace

TEST_CASE("a directory tree is copied once to a place that is empty", "[settings][migration]") {
    const QTemporaryDir root;
    REQUIRE(root.isValid());
    const QString from = QDir(root.path()).filePath(QStringLiteral("old"));
    const QString to = QDir(root.path()).filePath(QStringLiteral("new"));
    put(from + QStringLiteral("/a.bin"), QByteArray("\x00\x01\x02", 3));
    put(from + QStringLiteral("/sub/deeper/b.txt"), QByteArray("hello"));
    put(from + QStringLiteral("/.hidden"), QByteArray("h"));

    CHECK(copy_tree_once(from, to) == 3);
    CHECK(get(to + QStringLiteral("/a.bin")) == QByteArray("\x00\x01\x02", 3));
    CHECK(get(to + QStringLiteral("/sub/deeper/b.txt")) == QByteArray("hello"));
    CHECK(get(to + QStringLiteral("/.hidden")) == QByteArray("h"));
    CHECK(get(from + QStringLiteral("/a.bin")) == QByteArray("\x00\x01\x02", 3));

    // The place is not empty now: nothing is copied over it, and an edit stays.
    put(to + QStringLiteral("/a.bin"), QByteArray("edited"));
    CHECK(copy_tree_once(from, to) == 0);
    CHECK(get(to + QStringLiteral("/a.bin")) == QByteArray("edited"));
}

TEST_CASE("Qt's own cache directory is neither copied nor counted as something already there",
          "[settings][migration]") {
    const QTemporaryDir root;
    REQUIRE(root.isValid());
    const QString from = QDir(root.path()).filePath(QStringLiteral("old"));
    const QString to = QDir(root.path()).filePath(QStringLiteral("new"));
    put(from + QStringLiteral("/sinks.json"), QByteArray("[]"));
    put(from + QStringLiteral("/cache/qmlcache/Main_qml.qmlc"),
        QByteArray("compiled for the old binary"));
    put(from + QStringLiteral("/cache/qtpipelinecache-x86_64"), QByteArray("shaders"));
    // the new program has already started its own cache before the copy looks
    put(to + QStringLiteral("/cache/qmlcache/other.qmlc"), QByteArray("new"));

    CHECK(copy_tree_once(from, to) == 1);
    CHECK(get(to + QStringLiteral("/sinks.json")) == QByteArray("[]"));
    CHECK_FALSE(QFileInfo::exists(to + QStringLiteral("/cache/qmlcache/Main_qml.qmlc")));
    CHECK_FALSE(QFileInfo::exists(to + QStringLiteral("/cache/qtpipelinecache-x86_64")));
    CHECK(get(to + QStringLiteral("/cache/qmlcache/other.qmlc")) == QByteArray("new"));

    // a tree that holds nothing but a cache is nothing to copy
    const QString only_cache = QDir(root.path()).filePath(QStringLiteral("only-cache"));
    put(only_cache + QStringLiteral("/cache/x"), QByteArray("x"));
    CHECK(copy_tree_once(only_cache, QDir(root.path()).filePath(QStringLiteral("elsewhere"))) == 0);
}

TEST_CASE("a directory that exists and is empty is filled and one with a file in it is not",
          "[settings][migration]") {
    const QTemporaryDir root;
    REQUIRE(root.isValid());
    const QString from = QDir(root.path()).filePath(QStringLiteral("old"));
    put(from + QStringLiteral("/a.bin"), QByteArray("a"));

    const QString empty = QDir(root.path()).filePath(QStringLiteral("empty"));
    REQUIRE(QDir().mkpath(empty));
    CHECK(copy_tree_once(from, empty) == 1);

    const QString taken = QDir(root.path()).filePath(QStringLiteral("taken"));
    put(taken + QStringLiteral("/mine.bin"), QByteArray("mine"));
    CHECK(copy_tree_once(from, taken) == 0);
    CHECK_FALSE(QFileInfo::exists(taken + QStringLiteral("/a.bin")));
}

TEST_CASE("a directory that is not there, or is the same one, is not a copy",
          "[settings][migration]") {
    const QTemporaryDir root;
    REQUIRE(root.isValid());
    const QString here = QDir(root.path()).filePath(QStringLiteral("here"));
    put(here + QStringLiteral("/a.bin"), QByteArray("a"));

    CHECK(copy_tree_once(QDir(root.path()).filePath(QStringLiteral("absent")), here) == 0);
    CHECK(copy_tree_once(here, here) == 0);
    CHECK(get(here + QStringLiteral("/a.bin")) == QByteArray("a"));
}

TEST_CASE("the directories under the old names follow the settings to the new ones",
          "[settings][migration]") {
    const Scratch scratch;
    // Identities no program has, so the directories Qt keeps for tests belong to this test alone.
    const QString tag = QUuid::createUuid().toString(QUuid::Id128).left(12);
    const Identity current{QStringLiteral("iclforge-test-") + tag, QStringLiteral("now")};
    const Identity old{QStringLiteral("ac3forge-test-") + tag, QStringLiteral("before")};
    const QString old_data = standard_directory(old, QStandardPaths::AppDataLocation);
    const QString new_data = standard_directory(current, QStandardPaths::AppDataLocation);
    REQUIRE(old_data != new_data);
    REQUIRE(old_data.contains(old.application));
    REQUIRE(new_data.contains(current.application));
    put(old_data + QStringLiteral("/sinks/list.json"), QByteArray("[1,2]"));

    SECTION("with settings") {
        write(old, {{QStringLiteral("k"), QStringLiteral("v")}});
        const Outcome outcome = migrate(current, {{old, {}}});
        CHECK(outcome.status == Status::kMigrated);
        CHECK(outcome.keys == 1);
        CHECK(outcome.files >= 1);
        CHECK(get(new_data + QStringLiteral("/sinks/list.json")) == QByteArray("[1,2]"));
        CHECK(get(old_data + QStringLiteral("/sinks/list.json")) == QByteArray("[1,2]"));
        CHECK(migrate(current, {{old, {}}}).status == Status::kAlreadyMigrated);
    }
    SECTION("with no settings, the files alone are a migration and the second start knows it") {
        const Outcome outcome = migrate(current, {{old, {}}});
        CHECK(outcome.status == Status::kMigrated);
        CHECK(outcome.keys == 0);
        CHECK(outcome.files >= 1);
        CHECK(get(new_data + QStringLiteral("/sinks/list.json")) == QByteArray("[1,2]"));
        CHECK(migrate(current, {{old, {}}}).status == Status::kAlreadyMigrated);
    }

    // what Qt's test area holds for these two identities goes, organisation directories too
    for (const QString& dir : {old_data, new_data}) {
        QDir(dir).removeRecursively();
        QDir().rmdir(QFileInfo(dir).absolutePath());
    }
}

TEST_CASE("the directory of an identity is what Qt builds for that identity",
          "[settings][migration]") {
    const Scratch scratch;
    const Identity id{QStringLiteral("iclforge-shape"), QStringLiteral("probe")};

    const QString data = standard_directory(id, QStandardPaths::AppDataLocation);
    const QString config = standard_directory(id, QStandardPaths::AppConfigLocation);

    CHECK(data.endsWith(QStringLiteral("/iclforge-shape/probe")));
    CHECK(config.endsWith(QStringLiteral("/iclforge-shape/probe")));
    // and the process's own names are back the way they were
    CHECK(QCoreApplication::applicationName() != QStringLiteral("probe"));
}
