#pragma once

#include <collabtext/ActorId.h>
#include <collabtext/OpStream.h>
#include <collabtext/AnchorPins.h>
#include <collabtext/LocalHistory.h>
#include "crdt/Fragment.h"
#include "crdt/IdList.h"
#include "crdt/UndoMap.h"

#include "storage/MembershipStore.h"

#include <cstddef>
#include <compare>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace CollabText::Storage {

enum class CheckpointKind { Shareable, Local };
enum class CheckpointEngineKind : uint16_t { Text = 1, IdList = 2 };

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

struct CheckpointContext {
  DocumentId document;
  StreamId stream;
  uint64_t epoch = 0;
  /// Empty only at genesis; otherwise 64 lowercase hex SHA-256 digits.
  std::string parent_descriptor_hash;
  CheckpointEngineKind engine = CheckpointEngineKind::Text;
  auto operator<=>(const CheckpointContext &) const = default;
};

struct CheckpointCausalState {
  Crdt::Global version;
  std::vector<Crdt::UndoMapEntry> undo_entries;
};

struct CheckpointEngineState {
  CheckpointEngineKind kind = CheckpointEngineKind::Text;
  std::vector<Crdt::Fragment> fragments;
  std::vector<Crdt::IdListEntry> entries;
};

struct CheckpointShareableCore {
  CheckpointContext context;
  CheckpointEngineState engine;
  CheckpointCausalState causal;
  struct DeferredRecord {
    DeliveryToken token;
    std::string operation_bytes;
    bool quarantined = false;
    std::string reason;
    bool operator==(const DeferredRecord &) const = default;
  };
  std::vector<DeferredRecord> deferred;
  struct Coverage {
    std::map<ActorId, uint64_t> operation;
    std::map<ActorId, uint64_t> delivery;
    bool operator==(const Coverage &) const = default;
  } coverage;
  struct Pins {
    uint64_t projected_frontier = 0;
    uint64_t canonical_frontier = 0;
    bool complete = true;
    std::string canonical_projection_hash;
    std::vector<RetainedAnchorPin> live_pins;
  } pins;
  struct Translation {
    std::string reference_id;
    uint64_t source_epoch = 0;
    std::string source_token_bytes;
    std::string current_token_bytes;
    bool operator==(const Translation &) const = default;
  };
  struct TokenTranslations {
    std::vector<uint32_t> supported_token_versions;
    std::string manifest_hash;
    std::vector<Translation> entries;
    bool operator==(const TokenTranslations &) const = default;
  } translations;
};

struct CheckpointLocalOwner {
  ActorId actor;
  std::string shareable_checkpoint_hash;
  bool operator==(const CheckpointLocalOwner &) const = default;
};

struct CheckpointLocalWriter {
  ActorId actor;
  DocumentId document;
  StreamId stream;
  uint64_t epoch = 0;
  uint64_t next_identity_counter = 0;
  uint64_t durable_delivery_frontier = 0;
  bool operator==(const CheckpointLocalWriter &) const = default;
};

struct CheckpointLocalCore {
  CheckpointLocalOwner owner;
  Crdt::LocalHistorySnapshot history;
  CheckpointLocalWriter writer;
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

/// Canonical section 1 payload codec for both artifact kinds.
[[nodiscard]] Result<CheckpointSection>
encode_checkpoint_context(const CheckpointContext &context);
[[nodiscard]] Result<CheckpointContext>
decode_checkpoint_context(const CheckpointSection &section);
[[nodiscard]] Result<CheckpointSection>
encode_checkpoint_causal(const CheckpointCausalState &state);
[[nodiscard]] Result<CheckpointCausalState>
decode_checkpoint_causal(const CheckpointSection &section);
[[nodiscard]] Result<CheckpointSection>
encode_checkpoint_engine(const CheckpointEngineState &state);
[[nodiscard]] Result<CheckpointEngineState>
decode_checkpoint_engine(const CheckpointSection &section);
[[nodiscard]] Result<CheckpointShareableCore>
decode_checkpoint_core(const CheckpointArtifact &artifact);
[[nodiscard]] Result<CheckpointSection>
encode_checkpoint_deferred(
    const std::vector<CheckpointShareableCore::DeferredRecord> &records,
    const CheckpointContext &context,
    const Crdt::Global &applied_version);
[[nodiscard]] Result<std::vector<CheckpointShareableCore::DeferredRecord>>
decode_checkpoint_deferred(const CheckpointSection &section,
                           const CheckpointContext &context,
                           const Crdt::Global &applied_version);
[[nodiscard]] Result<CheckpointSection>
encode_checkpoint_coverage(const CheckpointShareableCore::Coverage &coverage);
[[nodiscard]] Result<CheckpointShareableCore::Coverage>
decode_checkpoint_coverage(const CheckpointSection &section);
[[nodiscard]] Result<void>
validate_checkpoint_coverage(const CheckpointContext &context,
                             const CheckpointShareableCore::Coverage &coverage,
                             const MembershipDescriptor &descriptor);
[[nodiscard]] Result<CheckpointSection>
encode_checkpoint_pins(const CheckpointShareableCore::Pins &pins,
                       const CheckpointContext &context);
[[nodiscard]] Result<CheckpointShareableCore::Pins>
decode_checkpoint_pins(const CheckpointSection &section,
                       const CheckpointContext &context);
[[nodiscard]] Result<CheckpointSection>
encode_checkpoint_translations(
    const CheckpointShareableCore::TokenTranslations &translations,
    const CheckpointContext &context,
    const CheckpointShareableCore::Pins &pins);
[[nodiscard]] Result<CheckpointShareableCore::TokenTranslations>
decode_checkpoint_translations(
    const CheckpointSection &section, const CheckpointContext &context,
    const CheckpointShareableCore::Pins &pins);
[[nodiscard]] Result<void>
validate_checkpoint_translations(
    const CheckpointShareableCore::TokenTranslations &translations,
    const MembershipDescriptor &descriptor);
[[nodiscard]] Result<CheckpointSection>
encode_checkpoint_local_owner(const CheckpointLocalOwner &owner);
[[nodiscard]] Result<CheckpointLocalOwner>
decode_checkpoint_local_owner(const CheckpointSection &section);
[[nodiscard]] Result<CheckpointSection>
encode_checkpoint_local_history(const Crdt::LocalHistorySnapshot &history,
                               const ActorId &owner);
[[nodiscard]] Result<Crdt::LocalHistorySnapshot>
decode_checkpoint_local_history(const CheckpointSection &section,
                                const ActorId &owner);
[[nodiscard]] Result<CheckpointSection>
encode_checkpoint_local_writer(const CheckpointLocalWriter &writer,
                               const CheckpointContext &context);
[[nodiscard]] Result<CheckpointLocalWriter>
decode_checkpoint_local_writer(const CheckpointSection &section,
                              const CheckpointContext &context);
[[nodiscard]] Result<CheckpointLocalCore>
decode_checkpoint_local_core(const CheckpointArtifact &artifact,
                             const ActorId &expected_owner,
                             std::string_view expected_shareable_hash,
                             const CheckpointContext &context);

}  // namespace CollabText::Storage
