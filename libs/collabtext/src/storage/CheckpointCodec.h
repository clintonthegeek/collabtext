#pragma once

#include <collabtext/ActorId.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace CollabText::Storage {

enum class CheckpointKind { Shareable, Local };

struct CheckpointSection {
  uint16_t id = 0;
  uint16_t version = 0;
  std::string bytes;
  auto operator<=>(const CheckpointSection &) const = default;
};

struct CheckpointArtifact {
  CheckpointKind kind = CheckpointKind::Shareable;
  std::vector<CheckpointSection> sections;
};

inline constexpr size_t kCheckpointMaxBytes = 256u * 1024u * 1024u;
inline constexpr uint16_t kCheckpointShareableSectionCount = 7;
inline constexpr uint16_t kCheckpointLocalSectionCount = 3;

/// Encode the canonical CTCK/1 shareable or CTCL/1 local companion frame.
/// This layer checks framing only; section-specific semantic validation belongs
/// to the typed checkpoint adapters.
[[nodiscard]] Result<std::string> encode_checkpoint(
    CheckpointKind kind, std::vector<CheckpointSection> sections);

/// Decode one complete artifact of the expected kind. Unknown and missing
/// sections fail closed; bytes are never silently skipped.
[[nodiscard]] Result<CheckpointArtifact> decode_checkpoint(
    std::string_view bytes, CheckpointKind expected_kind);

}  // namespace CollabText::Storage
