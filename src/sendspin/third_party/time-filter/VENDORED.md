# Vendored: Sendspin time filter

- Source: `github.com/Sendspin/time-filter`, commit `39dd3f4a7c37bcd63d8e2ee3885bf4d76fcaa61a`
  (2026-04-27).
- Licence: Apache-2.0, in `LICENSE` beside this file.
- Files: `cpp/sendspin_time_filter.h` and `cpp/sendspin_time_filter.cpp` as
  `sendspin_time_filter.h` and `sendspin_time_filter.cpp`, with `README.md`, `docs/theory.md`
  and `LICENSE`, all unmodified.

The Sendspin specification requires a player to use this filter to map server timestamps to its
own clock (`messaging.md`, Clock Synchronization). `src/sendspin` builds it as its own target,
`ac3sendspin_time_filter`, outside the project's warning set, so the files stay as upstream
wrote them. Update by replacing the five files from a newer commit and changing the commit
above.
