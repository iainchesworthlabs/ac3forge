# Code scanning triage after dependency bumps

Use `tools/ci/summarize_code_scanning_alerts.sh` (requires `gh auth login`) to
count open alerts by tool and rule before and after the bump PR lands.

## Dismiss (not fix) when

| Alert source | Package / rule class | Justification |
|--------------|---------------------|---------------|
| OSV-Scanner | `catch2`, `tracy` | vcpkg test/tooling only; neither is linked into a release binary |
| OSV-Scanner | `fmt` | Compiled into the codec libraries and the applications at 12.2.0; dismiss an advisory only when 12.2.0 is outside its range or the tree never calls the code it names (see fmt below) |
| OSV-Scanner | `requirements-*` Python packages | CI/docs/lint/coverage tooling, not embedded in releases |
| OSV-Scanner | `js/` and `apps/wasm/tests/` npm | Dev-only WASM demo and browser tests |
| OSV-Scanner | Android `androidTest*` / UTP transitives | Test harness only; release APK uses forced Netty/protobuf/commons-io pins |
| OSV-Scanner | Duplicate CVE across lockfiles | Same advisory on the same pinned version in two manifests |
| SonarQube Cloud | `CODE_SMELL` (if any reach Code scanning) | Maintainability; triage on sonarcloud.io, not Security |

### fmt

{fmt} is compiled into libac3forge and libmp4 (a private header-only copy in the namespace
`fmt::ac3_private`, set up in `cmake/Fmt.cmake`) and into ac3cli, ac3gui, Hearth, Crucible and the
WebAssembly modules. The minimum-footprint profile, which the ESP-IDF and ESPHome components and the
arm-none-eabi build use, links no {fmt}.

It is pinned in three places, all at 12.2.0:

- the `builtin-baseline` in `vcpkg.json`, which resolves `fmt` to 12.2.0 for the desktop builds;
- `AC3FORGE_FMT_VERSION` in `cmake/Fmt.cmake`, the version of the FetchContent copy that builds
  without vcpkg take (WebAssembly, Android, the Python wheels, the Rust crate);
- `fmt/12.2.0` in `packaging/conan/conanfile.py`.

`cmake/Fmt.cmake` also accepts a {fmt} already installed on the machine from 11.1.0
(`AC3FORGE_FMT_MINIMUM_VERSION`), and a build that uses one carries that version.

OSV-Scanner v2.6.0 reads no file that names {fmt}. Its scan of `main` on 2026-09-25 parsed the
lockfiles and requirements files for npm, PyPI, Gradle, NuGet, uv and Cargo, and none of
`vcpkg.json`, `cmake/Fmt.cmake` or `packaging/conan/conanfile.py`. As of that date no OSV-Scanner
alert in Code scanning, open, dismissed or fixed, is against {fmt}. Searches of {fmt}'s own security
advisories, the GitHub Advisory Database, NVD and OSV on 2026-09-25 found the advisories below, and
none applies to 12.2.0:

| Advisory | Affected | Outcome |
|----------|----------|---------|
| CVE-2018-1000052 | before 4.1.0 | fixed in 4.1.0 |
| CVE-2021-45959 (OSV-2021-991) | 7.1.0 to 8.0.1 | fixed in 8.1.0 |
| OSV-2022-165, OSV-2022-168 | development code between 8.1.1 and 9.0.0 | no release carries them |
| GHSA-65g5-63wg-xjh4 | 8.0.0 to before 12.0.0 | fixed in 12.0.0; the flaw is in `fmt::say()` from `<fmt/os.h>` (macOS only), a header the tree never includes |

A query of OSV by commit matches no {fmt} tag from 11.1.0 through 12.2.0, and NVD lists no CVE for
`fmt:fmt` at 11.1.0 or 12.2.0.

Put a new finding through two questions: does its range include 12.2.0, and does the tree call the
code it names? If both answers are yes, it is a fix. Raise the three pins together, in a change of
its own.

## Fix (version bump) when

| Ecosystem | Where to bump |
|-----------|---------------|
| vcpkg | `vcpkg.json` `builtin-baseline`; overlay ports under `cmake/vcpkg/ports/` |
| PyPI | `requirements/*.in` then `pip-compile` |
| Maven | `apps/android/app/build.gradle.kts` then `./gradlew --write-locks` |
| npm | `npm update` in `js/` and `apps/wasm/tests/` |
| crates.io | `cargo update` in `rust/` |
| GitHub Actions | Dependabot PRs (`.github/dependabot.yml`) |

Close related `nightly-analysis` issues once CodeQL/PREfast/clang-tidy/Sonar
findings are fixed or dismissed with a one-line reason.
