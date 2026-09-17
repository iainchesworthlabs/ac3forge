#!/usr/bin/env bash
# Regenerate apps/android/app/gradle.lockfile after dependency bumps.
#
# Run from WSL (Debian): bash tools/ci/regenerate_android_locks.sh
#
# Requires: Java 17+, curl, unzip.
#
# The Android SDK is installed under the WSL native filesystem by default
# ($HOME/.local/share/ac3forge/android-sdk), NOT under the repo on /mnt/c/...
# — sdkmanager and Gradle I/O on the Windows drive mount are painfully slow.
# Override with AC3FORGE_ANDROID_SDK or ANDROID_HOME if you already have one.
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
android_dir="$repo_root/apps/android"
sdk="${AC3FORGE_ANDROID_SDK:-${ANDROID_HOME:-$HOME/.local/share/ac3forge/android-sdk}}"
sm="$sdk/cmdline-tools/latest/bin/sdkmanager"

if [[ "$repo_root" == /mnt/* ]]; then
  echo "Note: repo is on ${repo_root%%/*} (Windows mount). SDK stays on the Linux side at:" >&2
  echo "  $sdk" >&2
fi

if [[ ! -x "$sm" ]]; then
  mkdir -p "$sdk/cmdline-tools"
  tmp="$(mktemp -d)"
  curl -fsSL -o "$tmp/cmdline-tools.zip" \
    "https://dl.google.com/android/repository/commandlinetools-linux-13114758_latest.zip"
  unzip -qo "$tmp/cmdline-tools.zip" -d "$sdk/cmdline-tools"
  mv "$sdk/cmdline-tools/cmdline-tools" "$sdk/cmdline-tools/latest"
  rm -rf "$tmp"
fi

export ANDROID_HOME="$sdk"
yes | "$sm" --licenses >/dev/null || true
"$sm" platform-tools
"$sm" 'platforms;android-36'
"$sm" 'build-tools;36.0.0'
"$sm" 'ndk;26.1.10909125'
"$sm" 'cmake;3.31.6'

printf 'sdk.dir=%s\n' "$sdk" > "$android_dir/local.properties"

cd "$android_dir"
sed 's/\r$//' gradlew > gradlew.unix
chmod +x gradlew.unix

# Resolve every canBeResolved configuration so --write-locks captures app,
# androidTest, release, and AGP Unified Test Platform
# (_internal-unified-test-platform-*) configs without needing an emulator.
# assemble* / connectedDebugAndroidTest alone miss UTP core configs that only
# resolve at connected-test time; running connected without a device fails
# before locks are persisted.
init_script="$(mktemp --suffix=.init.gradle.kts)"
cat > "$init_script" <<'EOF'
gradle.projectsLoaded {
    rootProject.allprojects {
        afterEvaluate {
            tasks.register("resolveAndLockAll") {
                notCompatibleWithConfigurationCache("resolves all configurations for locking")
                doLast {
                    configurations
                        .filter { it.isCanBeResolved }
                        .forEach { cfg ->
                            try {
                                println("Resolving ${project.path}:${cfg.name}")
                                cfg.resolve()
                            } catch (e: Exception) {
                                println("Skip ${project.path}:${cfg.name}: ${e.message}")
                            }
                        }
                }
            }
        }
    }
}
EOF

./gradlew.unix \
  -I "$init_script" \
  :app:resolveAndLockAll \
  --write-locks
rm -f gradlew.unix "$init_script"

echo "Updated $android_dir/app/gradle.lockfile"
echo "SDK at $sdk (local.properties written; both are machine-local, not committed)"
