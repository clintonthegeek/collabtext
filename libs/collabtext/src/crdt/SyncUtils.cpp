#include "crdt/SyncUtils.h"

#include <algorithm>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace CollabText::Crdt::SyncUtils {

std::string bucket_hex(uint8_t bucket) {
    static const char hex[] = "0123456789abcdef";
    return {hex[bucket >> 4], hex[bucket & 0x0F]};
}

uint8_t hash_bucket_lamport(uint16_t replica_id, uint64_t seq) {
    // Polynomial hash of replica_id, matching CRDT_SYNC_SPEC §4.3
    // (simplified: replica_id is uint16_t, not a string)
    uint32_t replica_hash = replica_id * 199;
    return static_cast<uint8_t>((replica_hash + seq) % 256);
}

uint8_t hash_bucket_string(const std::string& key) {
    uint8_t h = 0;
    for (char c : key)
        h = static_cast<uint8_t>(h * 31 + static_cast<uint8_t>(c));
    return h;
}

std::unordered_map<std::string, uint64_t>
read_sequences(const fs::path& path) {
    std::unordered_map<std::string, uint64_t> result;
    if (!fs::exists(path)) return result;

    std::ifstream f(path);
    if (!f.is_open()) return result;

    // Minimal JSON object parser for {"key": number, ...}
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    size_t pos = content.find('{');
    if (pos == std::string::npos) return result;
    ++pos;

    while (pos < content.size()) {
        // Skip whitespace and commas
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == ','
               || content[pos] == '\n' || content[pos] == '\r' || content[pos] == '\t'))
            ++pos;

        if (pos >= content.size() || content[pos] == '}') break;

        // Parse key
        if (content[pos] != '"') break;
        ++pos;
        size_t key_start = pos;
        while (pos < content.size() && content[pos] != '"') ++pos;
        std::string key = content.substr(key_start, pos - key_start);
        ++pos; // closing quote

        // Skip colon
        while (pos < content.size() && content[pos] != ':') ++pos;
        ++pos;

        // Skip whitespace
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t'))
            ++pos;

        // Parse number
        size_t num_start = pos;
        while (pos < content.size() && content[pos] >= '0' && content[pos] <= '9')
            ++pos;
        if (pos > num_start) {
            result[key] = std::stoull(content.substr(num_start, pos - num_start));
        }
    }

    return result;
}

void write_sequences(const fs::path& path,
                     const std::unordered_map<std::string, uint64_t>& seqs) {
    // Write to .tmp then rename for atomic update
    auto tmp_path = path;
    tmp_path += ".tmp";

    {
        std::ofstream f(tmp_path);
        f << '{';
        bool first = true;
        // Sort keys for deterministic output
        std::vector<std::string> keys;
        keys.reserve(seqs.size());
        for (auto& [k, _] : seqs) keys.push_back(k);
        std::sort(keys.begin(), keys.end());

        for (auto& k : keys) {
            if (!first) f << ',';
            f << '"' << k << '"' << ':' << seqs.at(k);
            first = false;
        }
        f << '}';
    }

    fs::rename(tmp_path, path);
}

std::pair<std::vector<std::string>, std::streamsize>
read_lines_after(const fs::path& path, std::streamsize after_byte) {
    std::vector<std::string> lines;

    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {lines, after_byte};

    f.seekg(after_byte);

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        // Remove trailing \r if present (Windows line endings)
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty()) continue;

        lines.push_back(std::move(line));
    }

    // Return the position after all complete lines read
    std::streamsize new_offset;
    if (f.eof()) {
        // Read to end of file
        f.clear();
        f.seekg(0, std::ios::end);
        new_offset = f.tellg();
    } else {
        new_offset = f.tellg();
    }

    return {std::move(lines), new_offset};
}

void append_to_bucket(const fs::path& path, const std::string& data) {
    std::ofstream f(path, std::ios::app | std::ios::binary);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    f.flush();
}

} // namespace CollabText::Crdt::SyncUtils
