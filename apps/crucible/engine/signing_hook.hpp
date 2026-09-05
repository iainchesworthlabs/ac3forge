#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "ac3/signing/signing_key.hpp"

// The Shield app's rule, on the desktop (docs/platforms/windows-demo.md,
// "Object signing"): the key is resolved at runtime from a path the user
// gave, or the environment variables ac3cli reads, never from beside the
// executable; with no key the encoder is told to emit no object container at
// all, because an unsigned-but-present container is a hard refusal on a
// validating decoder rather than a graceful fallback.

namespace ac3::crucible {

class SigningHook {
public:
    // How the key in hand was obtained, without saying where it is: what a
    // diagnostics note may say (diagnostics.hpp), where the status string
    // below may not, since it names the file for the Settings page. The hook
    // cannot tell the two environment variables apart - the library resolves
    // them - so the controller reads that from the environment itself.
    enum class Source : std::uint8_t { kNone, kFile, kEnvironment };

    // Loads from `explicit_path` if non-empty, else $AC3FORGE_SIGNING_KEY_FILE,
    // else $AC3FORGE_SIGNING_KEY (ac3::signing::load_signing_key's own order).
    // Returns a one-line status for the UI either way.
    std::string load(std::string_view explicit_path);
    void clear();

    [[nodiscard]] bool available() const;
    [[nodiscard]] const std::string& source() const { return source_; }
    [[nodiscard]] Source source_kind() const { return kind_; }
    // Why the last load failed, when it did; nullopt after a success or a
    // clear.
    [[nodiscard]] std::optional<ac3::signing::KeyErrorKind> failure() const { return failure_; }

    // Signs one access unit in place. False when no key is loaded or the
    // unit carried no container (a bed-only frame), which is not an error.
    [[nodiscard]] bool sign(std::span<std::byte> access_unit) const;

    SigningHook();
    ~SigningHook();
    SigningHook(const SigningHook&) = delete;
    SigningHook& operator=(const SigningHook&) = delete;

private:
    struct Impl;
    Impl* impl_;  // the key, zeroised on clear() and destruction
    std::string source_;
    Source kind_ = Source::kNone;
    std::optional<ac3::signing::KeyErrorKind> failure_;
};

}  // namespace ac3::crucible
