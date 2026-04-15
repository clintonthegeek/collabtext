#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace CollabText::Crdt::SyncUtils {

/// Format a bucket number as 2-char hex (e.g. 0x0A -> "0a").
std::string bucket_hex(uint8_t bucket);

/// Compute hash bucket (0-255) for a Lamport timestamp given its components.
uint8_t hash_bucket_lamport(uint16_t replica_id, uint64_t seq);

/// Compute hash bucket (0-255) for an arbitrary string key.
/// Uses polynomial hash: h = h * 31 + (uint8_t)c, mod 256.
uint8_t hash_bucket_string(const std::string& key);

/// Read sequences.json for a replica. Returns {bucket_hex -> sequence}.
std::unordered_map<std::string, uint64_t>
read_sequences(const std::filesystem::path& path);

/// Write sequences.json atomically (write to .tmp, then rename).
void write_sequences(const std::filesystem::path& path,
                     const std::unordered_map<std::string, uint64_t>& seqs);

/// Read raw lines from a file starting after byte offset after_byte.
/// Returns {lines, new_byte_offset}. Skips empty lines and strips trailing \r.
std::pair<std::vector<std::string>, std::streamsize>
read_lines_after(const std::filesystem::path& path, std::streamsize after_byte);

/// Append data to a bucket file, creating it if necessary. Flushes after write.
void append_to_bucket(const std::filesystem::path& path, const std::string& data);

} // namespace CollabText::Crdt::SyncUtils
