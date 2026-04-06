#pragma once

#include "collabtext/Identity.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace CollabText::Identity {

class IdentityProjector {
public:
    explicit IdentityProjector(std::filesystem::path shared_folder);

    // Write identity to <shared_folder>/identities/<identity_id>/profile.json.
    // Only updates if the identity's `updated` timestamp is strictly newer
    // than the existing projection (lexicographic ISO 8601 comparison).
    void project(const Identity& identity);

    // Write avatar bytes to <shared_folder>/identities/<identity_id>/avatar.png.
    void project_avatar(const std::string& identity_id, const std::vector<uint8_t>& data);

    // Enumerate all <shared_folder>/identities/*/profile.json files and return
    // the parsed identities; silently skips any that fail to parse.
    std::vector<Identity> read_all() const;

    // Read a single identity by id; returns nullopt if not found or unparseable.
    std::optional<Identity> read(const std::string& identity_id) const;

private:
    std::filesystem::path m_shared_folder;

    std::filesystem::path identity_dir(const std::string& identity_id) const;
    std::filesystem::path profile_path(const std::string& identity_id) const;
};

} // namespace CollabText::Identity
