#include "signing_hook.hpp"

#include <utility>

#include "ac3/signing/emdf_atmos_signer.hpp"
#include "ac3/signing/signing_key.hpp"

namespace ac3::crucible {

struct SigningHook::Impl {
    ac3::signing::SigningKey key;
};

SigningHook::SigningHook() : impl_(new Impl) {}

SigningHook::~SigningHook() {
    delete impl_;
}

std::string SigningHook::load(std::string_view explicit_path) {
    auto loaded = ac3::signing::load_signing_key(explicit_path);
    if (!loaded) {
        clear();
        switch (loaded.error().kind) {
            case ac3::signing::KeyErrorKind::kAbsent:
                source_.clear();
                return "no signing key: objects off, streaming the 5.1 bed only";
            default:
                source_.clear();
                return "signing key not loaded (" + loaded.error().message +
                       "): objects off, streaming the 5.1 bed only";
        }
    }
    impl_->key = std::move(*loaded);
    source_ = explicit_path.empty() ? "environment" : std::string(explicit_path);
    return "signing key loaded from " + source_ + ": object container will be signed";
}

void SigningHook::clear() {
    impl_->key = ac3::signing::SigningKey{};
    source_.clear();
}

bool SigningHook::available() const {
    return !impl_->key.empty();
}

bool SigningHook::sign(std::span<std::byte> access_unit) const {
    if (impl_->key.empty()) {
        return false;
    }
    return ac3::signing::sign_atmos_frame(access_unit, impl_->key);
}

}  // namespace ac3::crucible
