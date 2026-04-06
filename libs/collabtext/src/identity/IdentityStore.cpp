#include "collabtext/IdentityStore.h"

#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>

namespace CollabText::Identity {

// Curated palette of distinct, readable colors
static constexpr std::array<const char*, 16> kColorPalette = {
    "#E57373", // red
    "#F06292", // pink
    "#BA68C8", // purple
    "#9575CD", // deep purple
    "#7986CB", // indigo
    "#64B5F6", // blue
    "#4FC3F7", // light blue
    "#4DD0E1", // cyan
    "#4DB6AC", // teal
    "#81C784", // green
    "#AED581", // light green
    "#FFD54F", // amber
    "#FFB74D", // orange
    "#FF8A65", // deep orange
    "#A1887F", // brown
    "#90A4AE", // blue grey
};

IdentityStore::IdentityStore(std::filesystem::path config_dir)
    : m_config_dir(std::move(config_dir))
{
}

// ---------------------------------------------------------------------------
// slugify: lowercase ASCII only, strip non-alphanumeric, fallback "user"
// ---------------------------------------------------------------------------

std::string IdentityStore::slugify(const std::string& name)
{
    std::string result;
    result.reserve(name.size());

    for (unsigned char c : name) {
        // Only pass through 7-bit ASCII
        if (c > 127) continue;
        if (c >= 'A' && c <= 'Z') {
            result += static_cast<char>(c + 32); // to lower
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            result += static_cast<char>(c);
        }
        // anything else (spaces, punctuation) is stripped
    }

    if (result.empty()) result = "user";
    return result;
}

// ---------------------------------------------------------------------------
// generate
// ---------------------------------------------------------------------------

Identity IdentityStore::generate(const std::string& display_name)
{
    std::random_device rd;
    std::mt19937_64 rng(rd());

    // 6 random hex characters
    std::uniform_int_distribution<uint32_t> hex_dist(0, 0xFFFFFF);
    uint32_t hex_val = hex_dist(rng);
    char hex_buf[7];
    snprintf(hex_buf, sizeof(hex_buf), "%06x", hex_val);

    // Pick a random color from the palette
    std::uniform_int_distribution<size_t> color_dist(0, kColorPalette.size() - 1);
    const char* color = kColorPalette[color_dist(rng)];

    // Current UTC timestamp in ISO 8601
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    struct tm tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &tt);
#else
    gmtime_r(&tt, &tm_utc);
#endif
    char ts_buf[32];
    strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);

    Identity id;
    id.identity_id = slugify(display_name) + "-" + hex_buf;
    id.display_name = display_name;
    id.color = color;
    id.updated = ts_buf;
    return id;
}

// ---------------------------------------------------------------------------
// save / load
// ---------------------------------------------------------------------------

void IdentityStore::save(const Identity& identity)
{
    std::filesystem::create_directories(m_config_dir);

    auto tmp_path = m_config_dir / "identity.json.tmp";
    auto final_path = m_config_dir / "identity.json";

    {
        std::ofstream out(tmp_path, std::ios::out | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("IdentityStore: cannot open " + tmp_path.string() + " for writing");
        }
        out << to_json(identity);
    }

    std::filesystem::rename(tmp_path, final_path);
}

std::optional<Identity> IdentityStore::load() const
{
    auto path = m_config_dir / "identity.json";
    std::ifstream in(path);
    if (!in) return std::nullopt;

    std::ostringstream ss;
    ss << in.rdbuf();
    if (!in && !in.eof()) return std::nullopt;

    return identity_from_json(ss.str());
}

// ---------------------------------------------------------------------------
// Avatar
// ---------------------------------------------------------------------------

std::filesystem::path IdentityStore::avatar_path() const
{
    return m_config_dir / "avatar.png";
}

std::vector<uint8_t> IdentityStore::load_avatar() const
{
    auto path = avatar_path();
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};

    in.seekg(0, std::ios::end);
    auto size = in.tellg();
    in.seekg(0, std::ios::beg);
    if (size <= 0) return {};

    std::vector<uint8_t> data(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(data.data()), size);
    if (!in && !in.eof()) return {};
    return data;
}

bool IdentityStore::save_avatar(const std::vector<uint8_t>& data)
{
    constexpr size_t kMaxBytes = 256 * 1024;
    if (data.size() > kMaxBytes) return false;

    std::filesystem::create_directories(m_config_dir);

    auto path = avatar_path();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;

    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    return out.good();
}

// ---------------------------------------------------------------------------
// Signing key
// ---------------------------------------------------------------------------

std::filesystem::path IdentityStore::signing_key_path() const
{
    return m_config_dir / "identity.key";
}

} // namespace CollabText::Identity
