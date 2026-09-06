#pragma once

#include <string>
#include <string_view>

// What a macOS process's executable path says about the application a person
// means by it: the bundle it lives in, and the name to put beside it. Pure
// string work over paths - no CoreAudio, no libproc, no AppKit - kept beside
// session_monitor.cpp for the reason platform/linux/process_tree.hpp is kept
// beside its own: the file that includes <CoreAudio/CoreAudio.h> cannot be
// compiled anywhere but a Mac, and none of what is below needs to be.
//
// **NOTHING IN THIS FILE HAS BEEN RUN.** Written 2026-09-06 against Apple's
// documented bundle layout; the only compiler that will read it is the macOS
// CI leg (docs/crucible/promotion.md, "What cannot be verified, and why").
// The paths quoted in the comments are the documented shapes, not paths
// observed on a machine.
//
// The rule this file exists for is the macOS answer to a question Windows and
// Linux each answer differently.
//
// Windows groups audio sessions by process tree and stops where the image
// name changes: a browser's utility process and the browser share an
// executable name, so the walk finds the browser. Linux stops at the stream's
// own process, because on PipeWire the process that owns a stream is the one
// a person means.
//
// Neither rule works here. A Chromium browser's audio comes from a helper
// whose executable is a DIFFERENT binary with a different name - the
// documented layout is
//
//   /Applications/Google Chrome.app/Contents/MacOS/Google Chrome
//   /Applications/Google Chrome.app/Contents/Frameworks/Google Chrome
//       Framework.framework/Versions/<v>/Helpers/Google Chrome
//       Helper (Renderer).app/Contents/MacOS/Google Chrome Helper (Renderer)
//
// so a same-image walk stops immediately and a same-name test finds nothing.
// What the two paths DO share is the outermost `.app` bundle, and that is
// what macOS itself treats as one application: it is what the Dock shows, what
// the icon belongs to, and what a person would name. So the grouping key here
// is the outermost bundle, and app_bundle_path() below is the whole of it.
//
// Outermost rather than innermost deliberately. `Google Chrome Helper
// (Renderer).app` is a bundle too, nested inside the browser's; taking the
// innermost would put a row called "Google Chrome Helper (Renderer)" in the
// room, which is the machine's word for it and not a person's.

namespace ac3::crucible {

inline constexpr std::string_view kAppBundleSuffix = ".app";

// The path's last segment, with no trailing slash handling: these are
// executable paths from libproc, which never end in one.
[[nodiscard]] inline std::string_view path_basename(std::string_view path) {
    const auto slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// The OUTERMOST `.app` bundle `executable_path` lies inside, or empty when it
// lies in none - a command-line program, an interpreter, a daemon. See the
// file header for why outermost.
//
// The search is for ".app/" with the separator, not for ".app" alone, so a
// directory called "receipts.appointments" is not mistaken for a bundle; a
// path that IS a bundle (no executable under it) is taken as it stands, which
// is the shape NSRunningApplication's bundleURL would give if this ever grows
// one.
[[nodiscard]] inline std::string app_bundle_path(std::string_view executable_path) {
    if (executable_path.ends_with(kAppBundleSuffix)) {
        return std::string{executable_path};
    }
    const auto found = executable_path.find(".app/");
    if (found == std::string_view::npos) {
        return {};
    }
    return std::string{executable_path.substr(0, found + kAppBundleSuffix.size())};
}

// "Google Chrome" from "/Applications/Google Chrome.app". Empty in, empty out.
[[nodiscard]] inline std::string bundle_display_name(std::string_view bundle_path) {
    if (bundle_path.empty()) {
        return {};
    }
    std::string_view name = path_basename(bundle_path);
    if (name.ends_with(kAppBundleSuffix)) {
        name.remove_suffix(kAppBundleSuffix.size());
    }
    return std::string{name};
}

// "Chrome" from "com.google.Chrome": the last component of a reverse-DNS
// bundle identifier. A poorer name than the bundle's own - it is the
// developer's token rather than the localised display name - so it is only
// ever the fallback for a process whose executable path could not be read,
// which is what happens across a sandbox boundary or for another user's
// process.
[[nodiscard]] inline std::string name_from_bundle_id(std::string_view bundle_id) {
    if (bundle_id.empty()) {
        return {};
    }
    const auto dot = bundle_id.rfind('.');
    return std::string{dot == std::string_view::npos ? bundle_id : bundle_id.substr(dot + 1)};
}

// What to put in the room beside the icon, in the order of how much it tells
// a person: the bundle's own name, then the bundle identifier's last
// component, then the executable's file name. Never empty for a process that
// answered anything at all; empty for one that answered nothing, which the
// caller then leaves out of the list.
[[nodiscard]] inline std::string display_name(std::string_view executable_path,
                                              std::string_view bundle_id) {
    if (std::string bundle = bundle_display_name(app_bundle_path(executable_path));
        !bundle.empty()) {
        return bundle;
    }
    if (std::string from_id = name_from_bundle_id(bundle_id); !from_id.empty()) {
        return from_id;
    }
    return std::string{path_basename(executable_path)};
}

}  // namespace ac3::crucible
