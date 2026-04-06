#include "collabtext/IdentityProjector.h"

#include <fstream>
#include <sstream>

namespace CollabText::Identity {

IdentityProjector::IdentityProjector(std::filesystem::path shared_folder)
    : m_shared_folder(std::move(shared_folder))
{
}

std::filesystem::path IdentityProjector::identity_dir(const std::string& identity_id) const
{
    return m_shared_folder / "identities" / identity_id;
}

std::filesystem::path IdentityProjector::profile_path(const std::string& identity_id) const
{
    return identity_dir(identity_id) / "profile.json";
}

void IdentityProjector::project(const Identity& identity)
{
    // Check existing timestamp before writing
    auto existing = read(identity.identity_id);
    if (existing.has_value() && identity.updated <= existing->updated) {
        return; // not newer — skip
    }

    auto dir = identity_dir(identity.identity_id);
    std::filesystem::create_directories(dir);

    auto tmp_path   = dir / "profile.json.tmp";
    auto final_path = dir / "profile.json";

    {
        std::ofstream out(tmp_path, std::ios::out | std::ios::trunc);
        if (!out) return;
        out << to_json(identity);
    }

    std::filesystem::rename(tmp_path, final_path);
}

void IdentityProjector::project_avatar(const std::string& identity_id,
                                        const std::vector<uint8_t>& data)
{
    auto dir = identity_dir(identity_id);
    std::filesystem::create_directories(dir);

    auto tmp_path   = dir / "avatar.png.tmp";
    auto final_path = dir / "avatar.png";

    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }

    std::filesystem::rename(tmp_path, final_path);
}

std::vector<Identity> IdentityProjector::read_all() const
{
    std::vector<Identity> result;
    auto identities_dir = m_shared_folder / "identities";

    std::error_code ec;
    if (!std::filesystem::exists(identities_dir, ec)) return result;

    for (const auto& entry : std::filesystem::directory_iterator(identities_dir, ec)) {
        if (!entry.is_directory()) continue;
        auto ppath = entry.path() / "profile.json";
        std::ifstream in(ppath);
        if (!in) continue;

        std::ostringstream ss;
        ss << in.rdbuf();
        auto parsed = identity_from_json(ss.str());
        if (parsed.has_value()) {
            result.push_back(std::move(*parsed));
        }
    }

    return result;
}

std::optional<Identity> IdentityProjector::read(const std::string& identity_id) const
{
    auto ppath = profile_path(identity_id);
    std::ifstream in(ppath);
    if (!in) return std::nullopt;

    std::ostringstream ss;
    ss << in.rdbuf();
    return identity_from_json(ss.str());
}

} // namespace CollabText::Identity
