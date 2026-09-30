#pragma once

// Zone macros for Tracy instrumentation, no-op variant - see the
// tracy_enabled sibling directory's identically-pathed profiling.hpp for the
// real-Tracy variant and why CMake (not #ifdef) selects between the two.
// Every ICLFORGE_ZONE_SCOPED()/ICLFORGE_ZONE_SCOPED_N() call site expands to nothing at
// all here, so instrumented source compiles identically (down to the object
// code) to how it would with no instrumentation at all - this is the default
// (ICLFORGE_ENABLE_TRACY is OFF unless explicitly turned on).

#define ICLFORGE_ZONE_SCOPED()
#define ICLFORGE_ZONE_SCOPED_N(name)
#define ICLFORGE_ZONE_BEGIN(var, name)
#define ICLFORGE_ZONE_END(var)
#define ICLFORGE_FRAME_MARK()
#define ICLFORGE_FRAME_MARK_NAMED(name)
