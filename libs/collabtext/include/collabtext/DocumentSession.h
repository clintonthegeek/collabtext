/// Optional single-document recovery composition for the native engine.
///
/// DocumentSession replays retained operation envelopes before it permits a
/// local edit. Consumers may instead own their own canonical log, engines,
/// transport and cross-document history.
#pragma once

#include <collabtext/ActorId.h>
#include <collabtext/AnchorPins.h>
#include <collabtext/Engine.h>
#include <collabtext/OpStream.h>
#include <collabtext/Operations.h>
#include <collabtext/LocalHistory.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace CollabText {

using SessionCheckpointCandidateValidator = std::function<Result<void>(
    std::string_view shareable_artifact,
    const std::optional<std::string> &local_companion)>;

#ifdef COLLABTEXT_ENABLE_DOCUMENT_SESSION_TEST_ACCESS
namespace Crdt { struct WriterConfig; }
#endif

struct DocumentSessionConfig {
  std::filesystem::path shared_store;
  std::filesystem::path private_state;
  DocumentId document;
  StreamId stream;
  uint64_t epoch = 1;
  size_t max_undo_steps = 1000;
  size_t max_undo_bytes = 16 * 1024 * 1024;
  /// Bounds the exact rejected-envelope snapshot available to checkpoint
  /// providers. Exceeding either limit makes snapshot retrieval fail closed.
  size_t max_quarantined_deliveries = 1024;
  size_t max_quarantined_bytes = 16 * 1024 * 1024;
  /// Deterministic baseline content reconstructed locally before log replay.
  std::string initial_text;
  /// Optional live membership check invoked before each local document or
  /// undo mutation. Consumers enforcing retirement should return a nonnull
  /// lease that prevents a concurrent epoch transition until the mutation's
  /// durable publication completes. An empty callback leaves the session in
  /// its uncoordinated mode for callers that manage authorization externally.
  std::function<Result<std::shared_ptr<void>>(
      const DocumentId &, const StreamId &, uint64_t, const ActorId &)>
      authorize_local_mutation;
  /// Optional durable checkpoint bundle directory. When set, startup first
  /// validates its candidate with `validate_checkpoint_candidate`, restores
  /// the shareable state, and replays retained deliveries beyond its coverage.
  std::filesystem::path checkpoint_store;
  SessionCheckpointCandidateValidator validate_checkpoint_candidate;
};

/// Local producer deliveries retained for one original document epoch.
/// Payloads and delivery tokens are preserved verbatim for explicit export or
/// reconciliation; consumers must not replay these into a newer epoch without
/// validating the required transition and dependencies.
struct LocalBranchExport {
  DocumentId document;
  StreamScope scope;
  ActorId producer;
};

struct SessionRecoveryReport {
  size_t envelopes = 0;
  size_t applied = 0;
  size_t duplicates = 0;
  size_t deferred = 0;
  size_t rejected = 0;
  bool complete = true;
  std::optional<Error> issue;
  /// Nonfatal checkpoint selection/restore failures followed by log fallback.
  std::vector<Error> diagnostics;
};

struct SessionTextEdit {
  uint32_t old_start = 0;
  uint32_t old_end = 0;
  std::string new_text;
};

/// Exact inbound bytes retained after the session rejects a delivery. The
/// token and reason are checkpoint/provider input; this record does not imply
/// that the transport receipt advanced.
struct SessionQuarantinedDelivery {
  InboundDelivery delivery;
  Error reason;
  bool operator==(const SessionQuarantinedDelivery &other) const {
    return delivery.token == other.delivery.token &&
           delivery.payload == other.delivery.payload && reason == other.reason;
  }
};

/// Provider-owned state needed to bind a shareable checkpoint to the
/// consumer's recovered membership and anchor-pin projection. The caller must
/// supply pins from its canonical event log and verify the projection hash;
/// the session validates their scope and the translation records' structure.
struct SessionCheckpointTranslation {
  std::string reference_id;
  uint64_t source_epoch = 0;
  std::string source_token_bytes;
  std::string current_token_bytes;
};

struct SessionCheckpointProviderState {
  std::string parent_descriptor_hash;
  uint64_t projected_pin_frontier = 0;
  uint64_t canonical_pin_frontier = 0;
  bool pin_projection_complete = true;
  std::string canonical_pin_projection_hash;
  std::vector<RetainedAnchorPin> live_pins;
  std::vector<uint32_t> supported_token_versions{1};
  /// Optional descriptor value to check against the canonical translations.
  /// Leave empty to receive the computed hash in SessionCheckpointCapture.
  std::string expected_translation_manifest_hash;
  std::vector<SessionCheckpointTranslation> translations;
};

/// Separately encoded shareable state and actor-private local companion for
/// constructing the corresponding membership descriptor and checkpoint bundle.
struct SessionCheckpointCapture {
  std::string shareable_artifact_bytes;
  /// SHA-256 of the complete encoded artifact, matching CheckpointStore's key.
  std::string shareable_artifact_hash;
  /// Actor-private undo/writer state bound to the shareable artifact above.
  std::string local_companion_artifact_bytes;
  std::map<ActorId, uint64_t> operation_coverage;
  std::map<ActorId, uint64_t> delivery_coverage;
  std::string canonical_pin_projection_hash;
  std::string translation_manifest_hash;
};

/// Stable actor-local grouping metadata for correlating undo with consumer events.
struct LocalUndoGroupMetadata {
  Crdt::Identity id;
  std::vector<Crdt::Identity> operations;
};

class DocumentSession {
public:
  [[nodiscard]] static Result<std::unique_ptr<DocumentSession>>
      open(DocumentSessionConfig);
  ~DocumentSession();
  DocumentSession(const DocumentSession &) = delete;
  DocumentSession &operator=(const DocumentSession &) = delete;

  [[nodiscard]] Result<SessionRecoveryReport> recover();
  [[nodiscard]] Result<Crdt::Operation>
  apply_local(const std::vector<std::pair<uint32_t, uint32_t>> &ranges,
              const std::vector<std::string> &replacements);
  /// Publish a compensating local operation and persist the updated undo cursor.
  [[nodiscard]] Result<Crdt::Operation> undo();
  /// Publish a compensating local operation and persist the updated redo cursor.
  [[nodiscard]] Result<Crdt::Operation> redo();
  /// Merge the newest local undo group into its predecessor and checkpoint it.
  [[nodiscard]] Result<bool> coalesce_last_undo();
  [[nodiscard]] std::optional<LocalUndoGroupMetadata> next_undo_group() const;
  [[nodiscard]] std::optional<LocalUndoGroupMetadata> next_redo_group() const;
  [[nodiscard]] std::vector<Crdt::LocalUndoEntrySnapshot>
  pending_undo_retirements() const;
  /// Call only after the consumer's retirement event is durable. A failed
  /// checkpoint keeps the retirement pending so publication can be retried.
  [[nodiscard]] Result<void> acknowledge_undo_retirements(
      const std::vector<Crdt::Identity> &group_ids);
  [[nodiscard]] Result<PollReport> poll(PollBudget budget = {});
  [[nodiscard]] Result<void> save();
  /// Capture the current text engine, a complete provider projection, and the
  /// actor-private undo/writer companion. The caller must validate provider
  /// state against its canonical durable records and membership descriptor
  /// before publishing this bundle.
  [[nodiscard]] Result<SessionCheckpointCapture>
  capture_checkpoint(const SessionCheckpointProviderState &) const;
  /// Validate and atomically publish a freshly captured bundle through the
  /// configured CheckpointStore. Requires a provider validator that checks
  /// membership and other canonical provider-owned state.
  [[nodiscard]] Result<SessionCheckpointCapture>
  publish_checkpoint(const SessionCheckpointProviderState &);
  /// Stream this actor's durable operations in their original scope, including
  /// a durable pending local commit if its log append failed. The visitor may
  /// receive a prefix before an error; stage output and publish it only on
  /// success. Visitor exceptions are returned as io_error. The metadata output
  /// is valid only when this returns success.
  [[nodiscard]] Result<void> export_local_branch(
      LocalBranchExport &metadata,
      const std::function<Result<void>(const OutboundDelivery &)> &visitor);
  [[nodiscard]] Result<void> close();

  [[nodiscard]] std::string text() const;
  [[nodiscard]] uint32_t visible_length() const;
  [[nodiscard]] size_t undo_depth() const;
  [[nodiscard]] Crdt::Anchor anchor_at(uint32_t byte_offset,
                                       Crdt::Bias bias = Crdt::Bias::Left) const;
  [[nodiscard]] uint32_t resolve_anchor(const Crdt::Anchor &) const;
  [[nodiscard]] Crdt::Global recovery_frontier() const;
  /// Returns a delta only when `since` is within this session's retained
  /// history. Requests below the floor fail with history_unavailable.
  [[nodiscard]] Result<std::vector<SessionTextEdit>>
  edits_since(const Crdt::Global &) const;
  [[nodiscard]] const SessionRecoveryReport &recovery_report() const noexcept;
  /// Deferred envelopes retained verbatim until their causal dependencies
  /// arrive. The stable token and original payload bytes are suitable for a
  /// checkpoint or provider reconciliation snapshot.
  [[nodiscard]] std::vector<InboundDelivery> deferred_deliveries() const;
  /// Rejected inbound envelopes retained verbatim for checkpoint or provider
  /// diagnostics. Repeated scans with the same token and bytes are deduplicated;
  /// the first rejection reason is retained.
  [[nodiscard]] Result<std::vector<SessionQuarantinedDelivery>>
  quarantined_deliveries() const;
  [[nodiscard]] const ActorId &actor() const noexcept;
  [[nodiscard]] bool ready() const noexcept;

private:
#ifdef COLLABTEXT_ENABLE_DOCUMENT_SESSION_TEST_ACCESS
  friend class DocumentSessionTestAccess;
#endif
  struct Impl;
  explicit DocumentSession(std::unique_ptr<Impl>);
#ifdef COLLABTEXT_ENABLE_DOCUMENT_SESSION_TEST_ACCESS
  [[nodiscard]] static Result<std::unique_ptr<DocumentSession>>
  open_with_writer_config(DocumentSessionConfig, const Crdt::WriterConfig &);
#endif
  std::unique_ptr<Impl> m_impl;
};

} // namespace CollabText
