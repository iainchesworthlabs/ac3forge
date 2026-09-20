# Library capabilities in the applications

The library contains more functionality than any one application exposes. This table answers
which application provides a route to each broad capability. It records the public product
surface, not every internal call site.

--8<-- "docs-snippets/generated/application-capabilities.md"

“Internal audio layer” means `ac3::audio`, which applications in this repository link directly
but the library development packages do not install. “Output policy” means Crucible selects that
path according to the active device rather than presenting it as a separate command.

For codec-level detail—layouts, rates, metadata fields and individual coding tools—see
[Capabilities and limitations](capabilities.md). For operating-system and architecture
differences, see [Platforms](../platforms/index.md).
