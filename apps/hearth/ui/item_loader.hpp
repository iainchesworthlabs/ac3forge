#pragma once

#include <string>
#include <vector>

#include "session.hpp"

// The application's ItemLoader (apps/hearth/engine/session.hpp: "the path is
// turned into bytes by an ItemLoader, which the application supplies").
//
// This first slice reads a raw `.ac3`/`.ec3` elementary stream from disk and
// nothing else: apps/common/container_input.hpp's Matroska/MP4/MPEG-TS
// readers join this loader in a later slice, the same way the plan's Media
// section describes. A path this loader does not recognise is not a crash -
// it comes back as an error, which Session::open() turns into the item's
// unplayable reason, so the queue lists the file and says why rather than
// leaving it out.

namespace ac3::hearth::ui {

[[nodiscard]] ac3::hearth::ItemLoader make_file_item_loader();

// Every media file under `folder`, found by walking it (and every
// subfolder) and sorted for a stable queue order. "Media file" is wider than
// what make_file_item_loader() above can actually open: a `.mp4`/`.mkv`/
// `.ts` container is listed too, on the same footing as `.ac3`/`.ec3`,
// because a real folder of media mixes them and leaving them out would
// silently drop files "Add folder..." looks like it ought to find. Nothing
// here judges playability - a container path comes back and, handed to
// make_file_item_loader() like any other queue item, gets the same
// unplayable reason a container passed to addFiles() directly already gets.
// A folder that does not exist, or cannot be read, comes back empty rather
// than throwing.
[[nodiscard]] std::vector<std::string> list_folder_items(const std::string& folder);

}  // namespace ac3::hearth::ui
