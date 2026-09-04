#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

// The Shield app's rule, on the desktop (docs/platforms/windows-demo.md,
// "Object signing"): the key is resolved at runtime from a path the user
// gave, or the environment variables ac3cli reads, never from beside the
// executable; with no key the encoder is told to emit no object container at
// all, because an unsigned-but-present container is a hard refusal on a
// validating decoder rather than a graceful fallback.

namespace ac3::crucible {

class SigningHook {
public:
    // Loads from `explicit_path` if non-empty, else $AC3FORGE_SIGNING_KEY_FILE,
    // else $AC3FORGE_SIGNING_KEY (ac3::signing::load_signing_key's own order).
    // Returns a one-line status for the UI either way.
    std::string load(std::string_view explicit_path);
    void clear();

    [[nodiscard]] bool available() const;
    [[nodiscard]] const std::string& source() const { return source_; }

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
};

}  // namespace ac3::crucible
