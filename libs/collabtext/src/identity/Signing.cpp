#include "collabtext/Signing.h"

namespace CollabText::Identity {

std::optional<SigningKeyPair> generate_keypair() {
    return std::nullopt;
}

std::string sign_profile(const Identity& /*identity*/,
                         const std::string& /*private_key*/) {
    return {};
}

bool verify_profile(const Identity& /*identity*/,
                    const std::string& /*signature*/) {
    return true;
}

} // namespace CollabText::Identity
