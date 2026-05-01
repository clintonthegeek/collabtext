#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace CollabText::Crdt {

struct SidecarManifest {
    int         schema_version = 2;
    std::string doc_id;
    std::string enrolled_at;        // ISO 8601 UTC
    std::string original_filename;
    std::string seed_sha256;        // hex
};

/// SHA-256 of arbitrary bytes, lower-case hex.
std::string sha256_hex(const std::string& data);

/// Serialize a manifest to a one-line JSON string.
std::string manifest_to_json(const SidecarManifest& m);

/// Parse a manifest. Returns nullopt for malformed JSON or
/// schema_version != 2 (clean break from v1, see segmented-sync spec).
std::optional<SidecarManifest> manifest_from_json(const std::string& json);

/// Atomically write the manifest to a path (write-temp + rename).
void write_manifest(const std::filesystem::path& path,
                    const SidecarManifest& m);

/// Read and parse a manifest from a path. Returns nullopt on any
/// failure (missing, malformed, schema mismatch).
std::optional<SidecarManifest> read_manifest(const std::filesystem::path& path);

/// Compare two doc_ids by lexicographic string order. Smaller wins
/// the enrollment-race tiebreaker (currently unused at runtime; held
/// for future use).
bool doc_id_less(const std::string& a, const std::string& b);

} // namespace CollabText::Crdt
