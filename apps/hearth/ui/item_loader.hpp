#pragma once

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

}  // namespace ac3::hearth::ui
