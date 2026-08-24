#pragma once

#include <cstdint>
#include <span>
#include <string_view>

// Everything ac3cli prints ABOUT itself: the usage listing, per-command help,
// the exit-code table, the generated man page and the generated shell
// completions.
//
// Split out of main.cpp for the same reason support.hpp/multi_source.hpp were
// (the repo-structure review's H4 monolith split): main.cpp owns the command
// table and dispatch, and nothing else. The table is still the single source
// of truth - every function here is fed a span of CommandInfo built from it,
// so a command that exists cannot be missing from the help, the man page or
// the completions, and none of the four can disagree with dispatch about what
// a command takes.
//
// The one thing that changed with this split is how MUCH gets printed. The old
// print_usage() was ~130 lines of prose emitted in full on any argument error,
// which buried the one line the operator needed. Each paragraph now belongs to
// a topic (the bit constants below), every command row declares the topics its
// own arguments and options actually reach, and `ac3cli help <command>` /
// `ac3cli <command> --help` print that row plus exactly those paragraphs. The
// no-argument listing still prints everything, because there it IS the manual.
namespace ac3cli {

// Which grammar sections a command's help needs. A bitmask rather than an
// enum class: the natural expression of a command's topics is `kLayout |
// kMeta`, and the values are only ever OR'd, tested and passed around.
namespace topic {

inline constexpr std::uint32_t kNone = 0;
// The "-" stdin/stdout file-argument convention.
inline constexpr std::uint32_t kStdio = 1U << 0;
// Named layouts plus the Table E2.5 location-list alternative.
inline constexpr std::uint32_t kLayout = 1U << 1;
// The Annex E coding-tool token grammar (eac3-encode's [tools]).
inline constexpr std::uint32_t kTools = 1U << 2;
// The VBR grammar (eac3-encode's [vbr]).
inline constexpr std::uint32_t kVbr = 1U << 3;
// What the Atmos commands emit, and the objects/bed51 container choice.
inline constexpr std::uint32_t kAtmos = 1U << 4;
// The authored keyframe (paths.txt) file.
inline constexpr std::uint32_t kPaths = 1U << 5;
// The metadata option block (drc=, dialnorm=, mixmeta, ...).
inline constexpr std::uint32_t kMeta = 1U << 6;
// Multi-source input: src=, map=, offset=.
inline constexpr std::uint32_t kMulti = 1U << 7;
// Decode-side options: drc=<scale>, heavy, objects_dir.
inline constexpr std::uint32_t kDecode = 1U << 8;
// qc's preset= gate.
inline constexpr std::uint32_t kQc = 1U << 9;
// live's device arguments, mode=, capture2=.
inline constexpr std::uint32_t kLive = 1U << 10;
// record/live's take options: container=, watchdog=, layout=, objects=.
inline constexpr std::uint32_t kTake = 1U << 11;
// What 'mkv' derives from the bitstream rather than being told.
inline constexpr std::uint32_t kMkv = 1U << 12;
// fmp4's segment/manifest output.
inline constexpr std::uint32_t kFmp4 = 1U << 13;
// ts's DVB-profile signalling.
inline constexpr std::uint32_t kTs = 1U << 14;
// EMDF object signing/verification.
inline constexpr std::uint32_t kObjects = 1U << 15;
// probe's own json=/detail= option block.
inline constexpr std::uint32_t kProbe = 1U << 16;

inline constexpr std::uint32_t kAll = 0x1FFFFU;

}  // namespace topic

// One row of main.cpp's command table, reduced to what printing needs: no
// Needs enum and no handler pointer, just whether THIS build/platform can run
// it and why not. Built fresh by main.cpp on each call - 26 rows, so there is
// nothing to cache and nothing that can go stale.
struct CommandInfo {
    std::string_view name;
    std::string_view spec;
    std::string_view note;
    std::uint32_t topics = topic::kNone;
    bool available = true;
    // Empty when available; the platform's/build's own words otherwise, the
    // same string dispatch refuses with.
    std::string_view unavailable_reason;
};

// The whole manual: every command row, then every topic paragraph. What
// `ac3cli` with no arguments and `ac3cli help` print.
void print_usage(std::span<const CommandInfo> commands);

// Just this command: its row, the paragraphs its own topics name, and the
// common options every command takes. What `ac3cli help <command>` and
// `ac3cli <command> --help` print.
void print_command_help(const CommandInfo& command);

// The one-line-per-command index, with no topic paragraphs at all - what an
// argument error prints instead of the old full block, alongside a pointer to
// `ac3cli help <command>`.
void print_command_index(std::span<const CommandInfo> commands);

// The documented exit-code scheme (exit_codes.hpp), as a table. `ac3cli help
// exit-codes` prints this; so does the man page's own EXIT STATUS section.
void print_exit_codes();

// The options every command accepts regardless of what it does: quiet,
// verbose, and where to read about exit codes.
void print_common_options();

// The metadata/source/take/live/qc option blocks in full - what an unknown
// option token prints, and the kMeta+ half of the usage listing.
void print_meta_usage();

// A groff man page (section 1) on stdout, generated from `commands`. Written
// to a file at build time by apps/cli/CMakeLists.txt and installed as
// ac3cli.1; `ac3cli man` is also perfectly usable on its own through a pipe
// into `man -l -`.
void print_man_page(std::span<const CommandInfo> commands);

// A completion script for `shell` on stdout: bash, zsh, fish or powershell.
// Returns an ac3cli exit code - kExitUsage for a shell name it does not know.
[[nodiscard]] int print_completions(std::string_view shell,
                                    std::span<const CommandInfo> commands);

// The shells print_completions knows, in the order the packaging installs
// them. Also what `ac3cli completions` prints when asked for something else.
inline constexpr std::string_view kCompletionShells = "bash | zsh | fish | powershell";

}  // namespace ac3cli
