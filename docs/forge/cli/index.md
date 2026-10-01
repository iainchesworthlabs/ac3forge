# forge

`forge` is the command-line front end over the ICL Forge library — 44 commands covering
synthesis, file encoding/decoding, container wrapping, inspection, live capture/playback, and the
tool's own self-description (`help`, `man`, `completions`). It reads and writes AC-3, E-AC-3 (with
the Atmos object layer) and AC-4; the [Commands](commands.md) page says which command takes which.
Two of the 44 (`atmos-adm` and `atmos-iab`) only *run* in a build configured with
`-DICLFORGE_BUILD_ADM=ON`, but are always *listed* — the same "shown, not hidden" treatment
this page's own live-audio commands get when the platform can't run them either (see
[Commands](commands.md)'s own ADM section). Every command it can run is backed by the same public
library documented under [Library](../../library/index.md); every codec and format decision lives in
the library, and the CLI keeps only small local helpers of its own (the DASH MPD document wrapper
`fmp4` writes; the scene files behind `atmos-path`/`atmos-encode` are parsed by the library's
own `iclforge::oba::read_scene`).

Run it with no arguments for the full usage text — the command list in [Commands](commands.md)
is transcribed from it by hand. Nothing in the build compares the two, so the binary is the
authority where they disagree; `forge help` on your own build settles any question either page
raises.

```bash
forge
```

## Installing

`forge` installs as part of Forge. See [Installing Forge](../index.md#installing) for the
prebuilt archives, Homebrew, winget, and source-build paths.

## Version

```bash
forge --version
```

Prints the semantic version plus git provenance — commit, branch, the build target, the vector
kernel set the codec was compiled with, and a dirty flag. The version itself is derived from the
nearest reachable `v*` git tag at configure time, so it tracks the latest release tag (see
[Releasing](../../releasing.md) and `cmake/GitVersionDerivation.cmake`); the rest is stamped in at
build time by `cmake/GenerateVersion.cmake`. A build from past the tag looks like this:

```
ac3forge 0.10.0-beta.1+2637
  release: v0.10.0-beta.1-2637-g8d797cb05
  commit:  8d797cb05765bacfc61ef23c72429d8fd3aff045
  branch:  main
  target:  Windows x86_64 (MSVC 19.51.36260.0)
  kernels: x86_64-sse2
```

The headline carries the commits past the tag as semver build metadata: `+2637` is 2 637 commits
past `v0.10.0-beta.1` (the `release:` line carries git's own describe of it), so a build from
between releases is not mistaken for the tagged one. `kernels:` is `generic`, `x86_64-sse2` or
`aarch64-neon`, the `arch-*` directory of `src/arithmetic/variants/` the binary was built from. A tree with
uncommitted changes adds a `state:   dirty (uncommitted changes)` line.

`--version` (or its `-v` alias) is a flag, not one of the 44 commands — it's handled
before argument parsing and exits immediately. So are `--help` and `-h`, which print the named
command's own help (or the full listing when no command was named).

## Conventions shared across commands

- **Layouts** (`mono | stereo | 1+1 | 51 | 71 | 512 | 514 | 714`) name a channel bed by
  ear-friendly shorthand. AC-3 only reaches `mono | stereo | 1+1 | 51`; anything wider needs
  the dependent substreams that only E-AC-3 has. A layout can also be a comma-separated
  [Table E2.5](../../library/channel-plans-and-routing.md) location list
  (e.g. `L,C,R,LFE,Vhl,Vhr`) for a channel set none of the named layouts cover — AC-3 accepts
  one too, as long as it needs no dependent substream — see
  [Options & grammars](metadata-options.md) for the full grammar.
- **`out.ac3`, `out.ec3` and `out.ac4`** are the convention the examples follow. Most commands
  ignore the suffix: the command decides the codec (`encode` writes AC-3, `eac3-encode` E-AC-3,
  `ac4-encode` AC-4). Four read it: `transcode` takes its output codec from it (`codec=ac3|eac3|ac4`
  where the name cannot say, such as `-`), `ac4-encode` and `atmos-encode` with `codec=ac4` write an
  MP4 file when the name ends in `.mp4`, `.m4a` or `.mov` and a raw stream otherwise, and `remux`
  picks the container from the extension of its output.
- **`ac4-encode` takes no `[layout]`.** It takes its layout from the WAV's channel count (1 mono,
  2 stereo, 5 and 6 5.0 and 5.1, 9 and 10 5.0.4 and 5.1.4; see
  [`ac4-encode`](commands.md#ac4-encode)), and `record`/`live` with `codec=ac4` take `layout=`
  from mono, stereo, 5.0 (`layout=L,C,R,Ls,Rs`) and 5.1.
- **`-` means stdin or stdout** in place of a path, the conventional Unix pipe convention, so a
  WAV or stream never needs to touch a disk at all:

  ```bash
  forge encode - - 448 couple < in.wav > out.ac3
  forge decode - - < out.ac3 > out.wav
  ```

  Everything else about the command is unchanged; only the argument's meaning changes from "open
  this path" to "use the standard stream instead". As an input, `-` reads an encoded stream or
  container for `decode`, `probe`, `qc`, `levels`, `loudness`, `transcode`, `metadata`,
  `normalize`, `cut`, `cat` (one of its inputs), `strip-objects`, `demux`, `remux`, `mp4`, `mkv`,
  `ts`, `fmp4` and `spdif`, IEC 61937 bursts for `unspdif`, and a WAV file for `encode`,
  `eac3-encode`, `ac4-encode`, `atmos-encode` and `atmos-cbi`; `levels` and `loudness` cannot take
  a WAV file on `-`. As an
  output, `-` writes stdout for `encode`, `eac3-encode`, `ac4-encode`, `atmos-encode`,
  `atmos-cbi`, `decode`, `transcode`, `metadata`, `normalize`, `cut`, `cat`, `strip-objects`,
  `unspdif`, `demux` and the generators (`silence`, `sine`, `orbit`, `atmos`, `atmos-path`,
  `eac3-silence`, `eac3-sine`). `mp4`, `mkv`, `ts` and `spdif` take `-` as the name of a file to
  write, and `remux` refuses it. Windows needs no special handling on the caller's part — forge
  puts stdin/stdout into binary mode itself before the first byte crosses either one.
- **Metadata options** (`drc=`, `heavy`, `dialnorm=`, `cmixlev=`, …) can follow the positional
  arguments of any encoding command, in any order — see
  [Options & grammars](metadata-options.md), including which commands ignore which options.
- **PCM-carrying commands report per-channel peak/RMS levels when they finish**; `record` meters
  live. With `-` as the output path, every encode and decode path sends that report (and the rest
  of its status text — `dialnorm=auto`'s measurement line and the `src=`/`map=` multi-source
  paths' summary/routing/levels report included) to stderr, so it never lands in the middle of
  the piped stream.
- **Exit codes are documented and distinct**: `0` success, `1` usage, `2` input, `3` output,
  `4` unavailable here, `5` runtime, `6` a failed QC gate, `7` internal. `forge help exit-codes`
  prints the table; [Options & grammars](metadata-options.md#exit-codes) explains each.
- **`quiet` and `verbose`** follow the positional arguments of any command: `quiet` silences the
  status output (never the errors, never a reporting command's report, never a `-` payload), and
  `verbose` turns on the stderr progress line whatever the run's length.
- **`help <command>`, `--help` and `-h`** print one command's own row and the grammars it uses;
  `man` and `completions <shell>` print a generated man page and shell completion script, all
  four rendered from the same command table so none of them can drift from what dispatch accepts.
- **Commands needing audio hardware** (`devices`, `record`, `monitor`, `live`, `outputs`,
  `identify`, `play`, `spatial`) report themselves unavailable on a build with no capture,
  passthrough, monitor or spatial backend, rather than failing to link — see the per-OS Platform
  notes pages
  ([Windows](../../platforms/windows.md), [Linux](../../platforms/linux.md),
  [Raspberry Pi](../../platforms/raspberry-pi.md), [macOS](../../platforms/macos.md),
  [Android](../../platforms/android.md)) for what's actually hardware-confirmed on each OS.
- **`play` follows the sink**: given a `device_index`, it reads that endpoint's own
  advertised capabilities before committing to a format. That read is itself backend-specific —
  real today only on ALSA, a live probe everywhere else, same per-OS pages above — see
  [Commands → Following the sink](commands.md#following-the-sink).

## Next

- [Commands](commands.md) — all 44 commands, grouped and with the usage text they print
  (`atmos-adm` and `atmos-iab` only *run* with `-DICLFORGE_BUILD_ADM=ON`, but are listed either
  way), plus the exit-code table.
- [Options & grammars](metadata-options.md) — the `drc=`/`heavy`/`dialnorm=`/… options grammar,
  the `tools` argument grammar, and the full layout/location-list grammar.
- [Concepts](../../concepts/index.md) — if `bsid`, `syncframe`, or `JOC` aren't already familiar.
