#pragma once

#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <vector>

// What a person who already used forge-gui, Hearth or Crucible under their old names keeps
// (stage N1A of the re-layout: the programs stopped being `ac3gui`, `ac3hearth` and `ac3crucible`,
// and QSettings stores what they keep under the organisation and the application name the program
// sets, so a program that changes either starts with nothing). This copies the old store to the new
// one, once, at start-up, before anything reads a setting:
//
//   - every key of the old store (the workbench's preferences and session, the language, Hearth's
//     paired sinks, pairing keys and server identity, Crucible's first-run state) goes to the new
//     store when the new store holds no key of its own and the old one holds some;
//   - the files under the old QStandardPaths directories of the program (its data, its local data
//     and its configuration directory) go to the new directories the same way;
//   - a key of the new store records that it happened, which is also what makes the second start
//     find the new store already in use;
//   - the old store and the old files are never written, renamed or removed: a person who goes
//     back to a release that still has the old names finds everything where they left it. (Qt on
//     Windows creates the empty registry key of every store it reads, so a machine that never had
//     a former store gets that key, empty, under the old names; no value is written in it, and
//     the first start after the new store holds keys does not read a former store at all.)
//
// This is data preservation and not a compatibility shim: the old names are not accepted anywhere,
// the program reads only the new store, and nothing is copied after the first start. The old
// organisation and application names are written here and nowhere else (n1b_programs.py leaves this
// file as it is on purpose).
//
// Qt's QSettings has a fallback from an application's store to its organisation's: the old store
// is read without it (only the application's own keys are copied), and so is the new store when it
// is asked whether it holds anything. Both are opened with QSettings::defaultFormat() and the user
// scope, as every store of these programs is, so the format a test or a smoke run sets (an INI file
// in a temporary directory through QSettings::setPath) is the one read and written.

namespace iclforge::settings_migration {

struct Identity {
    QString organization;
    QString application;
};

// A store that can be copied from. `marker_key`, when it is not empty, is one more key the copy
// sets to true in the new store, for a reader that cares which store the settings came from
// (Crucible's first-run dialog says so once when they came from the desktop demo's).
struct Former {
    Identity identity;
    QString marker_key;
};

enum class Status {
    kNothingToMigrate,  // no former store holds a key and no former directory a file
    kAlreadyMigrated,   // the new store holds keys already: a start after the first, or a new start
    kMigrated,
    kFailed,  // the new store or a directory could not be written; the old data is as it was
};

struct Outcome {
    Status status = Status::kNothingToMigrate;
    Identity source{};  // the former identity the keys came from (kMigrated)
    int keys = 0;       // keys copied to the new store, without the records of the copy
    int files = 0;      // files copied to the new directories
};

// The keys of the first store in `former` that holds any, into the `current` store, once: the
// rules above. Never touches a former store.
Outcome migrate_settings(const Identity& current, const std::vector<Former>& former);

// Copies the tree at `from` into `to` when `from` holds files and `to` holds nothing, and returns
// the files copied; -1 when a file could not be copied. An existing file of `to` is never replaced.
// A directory named `cache` at the top of either is Qt's own cache (QML and shader caches, made for
// the program that wrote them): it is not copied, and a `to` that holds only one still holds
// nothing.
int copy_tree_once(const QString& from, const QString& to);

// The directory QStandardPaths gives `where` for a process that set `id`'s names, the way Qt builds
// it on this platform (`<base>/<organization>/<application>`), whatever the process's own names
// are.
QString standard_directory(const Identity& id, QStandardPaths::StandardLocation where);

// The settings and the directories (data, local data, configuration) of `former` into those of
// `current`: what a program does at start-up.
Outcome migrate(const Identity& current, const std::vector<Former>& former);

// The three programs, by the names they have now and the names they had.
enum class Program { kForgeGui, kHearth, kCrucible };

Identity identity_of(Program program);
std::vector<Former> former_identities(Program program);

// migrate() for a program, with the identities above. Call it once, after the application and
// organisation names are set and before anything constructs a QSettings or reads one.
Outcome migrate_program(Program program);

}  // namespace iclforge::settings_migration
