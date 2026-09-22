# Code scanning triage after dependency bumps

Use `tools/ci/summarize_code_scanning_alerts.sh` (requires `gh auth login`) to
count open alerts by tool and rule before and after the bump PR lands.

## Dismiss (not fix) when

| Alert source | Package / rule class | Justification |
|--------------|---------------------|---------------|
| OSV-Scanner | `catch2`, `fmt`, `tracy` | vcpkg test/tooling only; codec ships no third-party runtime |
| OSV-Scanner | `requirements-*` Python packages | CI/docs/lint/coverage tooling, not embedded in releases |
| OSV-Scanner | `js/` and `apps/wasm/tests/` npm | Dev-only WASM demo and browser tests |
| OSV-Scanner | Android `androidTest*` / UTP transitives | Test harness only; release APK uses forced Netty/protobuf/commons-io pins |
| OSV-Scanner | Duplicate CVE across lockfiles | Same advisory on the same pinned version in two manifests |
| SonarQube Cloud | `CODE_SMELL` (if any reach Code scanning) | Maintainability; triage on sonarcloud.io, not Security |

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
