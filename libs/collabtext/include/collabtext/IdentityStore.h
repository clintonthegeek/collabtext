#pragma once

#include "collabtext/Identity.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace CollabText::Identity {

class IdentityStore {
public:
    explicit IdentityStore(std::filesystem::path config_dir);

    std::optional<Identity> load() const;
    void save(const Identity& identity);
    Identity generate(const std::string& display_name);

    std::filesystem::path avatar_path() const;
    std::vector<uint8_t> load_avatar() const;
    bool save_avatar(const std::vector<uint8_t>& data);

    std::filesystem::path signing_key_path() const;

private:
    std::filesystem::path m_config_dir;

    static std::string slugify(const std::string& name);
};

} // namespace CollabText::Identity
