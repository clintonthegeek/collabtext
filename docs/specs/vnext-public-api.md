# vNext public API guide

**Status:** binding design-gate artifact (P01). This is the API that P02–P08, the
L/S/G/A/X/E phases, and the installed examples in E03 are written against.
**Paired with:** [vnext-protocol.md](vnext-protocol.md) for wire and lifecycle
rules, and [vnext-transport.md](vnext-transport.md) for transactional transport
receipt and commit semantics.
**Authority:** [vnext-design-constraints.md](vnext-design-constraints.md) (D01).

The code in this document gives **target signatures**: most types do not exist
yet. Section 6.1 is now implemented by S04b; its compile checks refer to the
actual public types and facades. A02a/A06a expose and install the actually implemented
alpha subset early; A02 compiles the complete target examples and E03 compiles them
against the final installed package. Where today's API differs, §9 lists the break so
a caller knows what to expect.

---

## 1. Error handling: `Result`

C++23's `std::expected` is not required. The project ships a small C++20 type.

```cpp
namespace CollabText {

template <class T>
class Result {
public:
    Result(T value);                  // implicit: success
    Result(Error error);              // implicit: failure

    [[nodiscard]] bool ok() const noexcept;
    [[nodiscard]] const T& value() const &; // precondition ok()
    [[nodiscard]] T& value() &;
    [[nodiscard]] T&& value() &&;           // permits move-only operation values
    [[nodiscard]] const Error& error() const; // precondition !ok()

    template <class U> T value_or(U fallback) const;

    // value()/error() require the caller to check the corresponding outcome.
    // [[nodiscard]] requests a diagnostic; it does not force a compile error.
};

}  // namespace CollabText
```

`Result` is `[[nodiscard]]` and movable. Its optional-value wrapper does not
allocate separately, but a stored value or Error owns memory and may allocate.
Errors are values, so
they can be inspected, counted, and asserted on in tests without a framework.

## 2. Identity and counters

```cpp
struct ActorId {
    // 128-bit opaque value. No ordering, no arithmetic, no default identity.
    static Result<ActorId> generate();     // OS cryptographic randomness
    static Result<ActorId> parse(std::string_view text);

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::string to_string() const;
};
static bool operator==(ActorId, ActorId) noexcept;

using StreamId   = std::string;   // "text", "comments", "chat", ...
using DocumentId = std::string;   // stable, persisted with the document

struct OperationId {
    ActorId actor;
    uint64_t counter;              // per (actor, stream); checked, never reused

    [[nodiscard]] bool operator==(const OperationId&) const noexcept;
};

struct OperationContext {
    DocumentId document;
    StreamId   stream;
    uint64_t   epoch = 1;          // bumped only by explicit retirement
    OperationId id;
};
```

### 2.1 An engine is constructed from an allocator, not from an actor id

P06a adopted this and it is worth stating precisely, because the ActorId-only
signature this guide originally sketched cannot express the property that
matters.

```cpp
class IdentityAllocator {
public:
    // Reserve `count` consecutive counters, returning the first. All-or-nothing.
    [[nodiscard]] virtual Result<uint64_t> reserve_run(uint64_t count) = 0;
    // Raise past an identity observed from a peer. Derived, not durable.
    virtual void observe(uint64_t counter) = 0;
    [[nodiscard]] virtual uint64_t peek() const noexcept = 0;
    [[nodiscard]] virtual const ActorId& actor() const noexcept = 0;
};

using SharedIdentityAllocator = std::shared_ptr<IdentityAllocator>;
```

The engine never owns identity allocation. `ReplicaState` (P02) owns the durable
per-(actor, stream) counter; an engine borrows a handle naming its (document,
stream). Three consequences a consumer can rely on:

- **There is no `Buffer(ActorId)`.** Holding an actor id is not authority to
  claim durable allocation. A constructor that quietly started a counter at 1
  would hand out identities a relaunch hands out again, and nothing in the type
  would say so. An engine that cannot be given an allocator cannot mint an
  identity at all, which is a better failure than a plausible one.
- **An invalid actor is a refusal, not actor 0.** An engine may exist before its
  private state has an identity, so this is a state rather than a bug: it is not
  asserted on, and every mint site returns `ErrorCode::invalid_identity` instead
  of minting an unattributable operation. In Release as well as Debug.
- **One local operation costs one reservation.** An operation that inserts `n`
  scalars reserves `n + 1` counters: its item identities, then its own. The
  operation's identity is therefore strictly greater than every item identity it
  created and strictly below the next reservation's base
  (vnext-identity.md §3.2). Exhaustion is reported as `counter_exhausted` with
  nothing consumed; it is never wrapped.

A test that wants a throwaway counter asks for a volatile one by name. That is a
requirement on tests, not on consumers: the durability guarantees are only
meaningful where a `ReplicaState` is behind them.

There is deliberately no `ActorId()` default that means "actor 1". A
constructed-from-nothing `ActorId` is invalid and any operation carrying it is
rejected — which is what makes "silent fallback to actor 1" impossible to write.

## 3. Operation value types

`Crdt::Operation` (text) and `Crdt::IdListOperation` are distinct opaque
immutable values. Their payload structs and variant layouts live only in
`src/crdt/OperationPayload.h`; no installed header defines them. Production
helpers read payloads through private const accessors. Builders freeze by value
before publication; a retained builder cannot modify an emitted operation.

P06a step 6 revises the earlier move-only draft to **copyable immutable handles**.
Deferred queues, retry/quarantine records, callbacks and transport consumers may
retain the same operation independently. Copying shares `const` storage instead
of granting write access or requiring a new clone protocol throughout the working
engine. This is an implementation choice within the authorized fresh API, not a
relaxation of operation immutability. No encoded-byte cache is added here; the
versioned codecs return checked owned bytes/values; no byte cache is added.

```cpp
Crdt::Operation retained = received; // read-only shared ownership
Crdt::Identity id = Crdt::op_identity(retained);
auto encoded = Crdt::encode_operation(retained);
if (!encoded.ok()) return encoded.error(); // owner keeps retained operation
const std::string& bytes = encoded.value();
auto decoded = Crdt::decode_operation(bytes);
if (!decoded.ok()) return decoded.error(); // keep original bytes; no cursor commit
```

The list uses `idlist_op_identity`, `encode_idlist_operation` and
`decode_idlist_operation`. Default and moved-from handles have an invalid
identity and are rejected before mutation. Public callers cannot construct a
payload or access a variant alternative; construction comes from engine output
or decoding. Identity/counter remain the narrow existing accessor contract.
P07 adds the document/stream/epoch envelope; opacity alone does not create that
context. Version 1 codecs use CTBO/CTLO/CTGV/CTGP frames and exact-width hex
integers, with distinguishable format/version/schema errors. See
[vnext-operation-codec.md](vnext-operation-codec.md) for mandatory schemas,
bounds and allocated intervals. `CounterGap` is owned terminal interval metadata,
not an operation or proof of publication. P07/L02 integrate gaps. Keep rejected
bytes; failure never grants permission to discard an event-log record.

## 4. Decode and store errors

```cpp
enum class ErrorCode {
    // decoding
    malformed,             // syntax, truncation, trailing input, bad bounds
    unknown_format,        // identifier or version this build does not know
    unsupported_version,   // known identifier, version this build cannot read
    invalid_unicode,       // lone surrogate, invalid UTF-8, truncated sequence
    // identity and context
    invalid_identity, actor_conflict, wrong_epoch, unknown_stream,
    counter_regression, counter_exhausted,
    // apply
    unresolvable_anchor, history_unavailable, reentrant_mutation,
    // store
    corrupt_record, missing_recovery_point, coverage_gap, io_error,
    commit_uncertain,       // published; durability not confirmed; reconcile
    unsupported_operation, // platform cannot supply requested guarantee
    // limits
    deferred_queue_full, retired_member,
};

class Error {
public:
    [[nodiscard]] ErrorCode code() const noexcept;
    [[nodiscard]] std::string_view message() const noexcept;
    [[nodiscard]] std::optional<std::string> context() const; // where it happened
};
```

`Error` is cheap to copy and safe to keep. `context()` carries the operation id,
path, or stream name that failed, which is what a user needs to act on.

Decoding distinguishes **retained** from **quarantined**:

```cpp
enum class DecodeDisposition {
    accepted,
    retained,     // well-formed enough to keep for a newer reader; blocks coverage
    quarantined,  // never applicable; kept with a reason; blocks coverage
};
```

## 5. Mutation outcomes

```cpp
enum class ApplyOutcomeKind { applied, duplicate, deferred, rejected };

class ApplyOutcome {
public:
    [[nodiscard]] ApplyOutcomeKind kind() const noexcept;

    // deferred only: what is missing
    [[nodiscard]] std::optional<OperationId> awaiting() const;

    // rejected only: why, with the error
    [[nodiscard]] std::optional<Error> error() const;
};
```

Callers must branch on the kind. There is no `bool apply(...)`, because the
difference between "already applied", "waiting for a dependency", and "will never
apply" is the difference between a document that converges and one that quietly
loses work (R1).

```cpp
// Explicit, exhaustive, and testable.
switch (outcome.kind()) {
    case ApplyOutcomeKind::applied:   advance_ui(); break;
    case ApplyOutcomeKind::duplicate: break;              // idempotent, fine
    case ApplyOutcomeKind::deferred:  show_pending(outcome.awaiting()); break;
    case ApplyOutcomeKind::rejected:  quarantine(*outcome.error()); break;
}
```

The engine retains causally deferred operations under an explicit count and byte
budget. `CrdtEngine::set_deferred_operation_budget()` configures both limits;
`deferred_operation_count()` and `deferred_operation_bytes()` report current
usage. Lowering a limit below already retained work fails with `limit_exceeded`
and leaves the current limit intact. When an offered operation cannot fit,
`apply_remote()` returns `rejected` with `deferred_queue_full`. The caller still
owns that operation and must retain or retry it after dependencies or capacity
change; the engine never silently drops an accepted operation to make room.


Implemented in P05 as `<collabtext/ApplyOutcome.h>`, with one deliberate
divergence: `awaiting()` returns `std::optional<Crdt::Lamport>` rather than
`std::optional<OperationId>`, because the engine's operation identity is still
the Lamport pair. The Lamport is a *witness* for the caller's benefit — "what are
you waiting for" — and not a key they should store. P06 makes operations opaque
and `ActorId`-keyed, at which point `awaiting()` becomes `OperationId`. This is a source break for
callers naming the provisional witness type; callers must not persist it.

The order in which an engine reaches these verdicts is itself part of the
contract; see vnext-protocol.md §4.1 for why identity is checked before
`duplicate` and validity before causality.

## 6. Engine surface

Both primitives expose the same shape. Neither is the other's alias.
S04 implements the opaque Anchor encode/decode API and the position contract in
[vnext-external-position-contract.md](vnext-external-position-contract.md).
`ResolvedTextPosition` carries an actual UTF-8 byte offset on a scalar boundary;
`ResolvedListPosition` carries an actual element index. Both report boundary
placement/collapse under S02's exact rules. Errors distinguish unavailable causal
context, foreign/unsupported tokens, and unavailable history. The final type names
and fields are frozen at S04/A02; an ApplyOutcome with no position cannot satisfy
resolution. Scoped start/end/empty anchors are values, not universal sentinels.

`AnchorPinRegistry` is the headless projection for durable external references.
Consumers supply the canonical event persistence callback, replay a contiguous
event prefix on restart, and wait for `recovery_complete(canonical_frontier)`
before any collector is enabled. `live_pins()` exposes the original token bytes
and scope for future certificate checks. This API does not persist the consumer
log itself or enable garbage collection. `validate_position_scope()` provides
typed document, stream, primitive, and epoch rejection at comment/chat ingestion
seams. `DocumentSession::edits_since()` returns `Result`; callers must handle
`history_unavailable` below its retained floor and `awaiting_state` for a future
frontier.

`DocumentSession::export_local_branch()` streams the session producer's retained
operation deliveries for the configured document, stream, and epoch to a caller
visitor. It includes a durable pending local commit when transport publication
failed, deduplicates an identical uncertain append, and rejects conflicting
bytes or sequence gaps. It preserves the original delivery tokens and opaque
operation payloads without retaining the whole branch in memory. The visitor may
receive a prefix before an error, so callers must stage output and publish it
only after the method succeeds. This is an explicit rescue/export seam: it does
not authorize the old actor to mutate a new epoch, validate a membership
transition, or automatically replay/rebase the branch into a later descriptor.

`MembershipCoordinator` is the public synchronization gate for consumers which
compose membership with `DocumentSession`. Initialize one `MembershipScope` per
document/stream with the recovered epoch and sorted or unsorted member IDs (the
factory canonicalizes them). Its local-mutation and inbound-delivery methods
return RAII permits suitable for the corresponding session authorization
callbacks. `commit_transition()` handles one scope; `commit_transitions()` atomically
advances a batch of document/stream scopes under one exclusive gate and invokes
one durable callback for the whole batch. The callback must not reenter a
membership coordinator. Supply a stable transition identity that covers the
full event and checkpoint inputs. An uncertain publication or callback
exception blocks every scope in the batch until the same identity and complete
scope set are retried or state is recovered durably; partial-batch retries are
refused. The caller remains responsible for authenticating acknowledgements
and validating the journal event and checkpoint; this helper supplies
concurrency control, not a membership database.

`DocumentSession::quarantined_deliveries()` exposes exact rejected inbound
tokens, payload bytes, and reasons for checkpoint/provider diagnostics. Its
count and byte budgets default to 1,024 records and 16 MiB. If either budget is
exceeded, retrieval returns `limit_exceeded` rather than presenting an
incomplete snapshot as complete. Repeat observations of the same token and
bytes deduplicate, preserving the first rejection reason. Quarantined input
does not advance transport receipts and keeps session recovery incomplete.

### Qt presence and document-operation composition

`SyncManager` handles presence and ephemeral state only. It does not publish or
apply CRDT operations. For a file-backed document, compose `DocumentSession`
with its durable `FileOpStream` path and keep presence as a separate helper:

```cpp
DocumentSessionConfig config{shared_store, private_state, document_id,
                             stream_id, epoch};
auto opened = DocumentSession::open(std::move(config));
if (!opened.ok()) return report(opened.error());
auto session = std::move(opened).value();

// QTextDocument positions are UTF-16 code units. Convert them to UTF-8 byte
// ranges before submitting through DocumentSession.
auto operation = session->apply_local({{start_byte, end_byte}}, {replacement_utf8});
if (!operation.ok()) return report(operation.error());

auto incoming = session->poll();
if (!incoming.ok()) return report(incoming.error());
```

Construct `SyncManager` separately for peer presence/ephemeral state. A consumer
with a different operation transport owns its session composition around the
Core engine and `OpStream`; `DocumentSession` currently composes the file-backed
`FileOpStream` internally. Neither route uses `SyncManager` as a document-
transport shortcut.

```cpp
class Buffer {
public:
    // Takes the allocator identities are minted from, and learns its actor from
    // it. See §2.1: there is deliberately no ActorId-only constructor.
    explicit Buffer(SharedIdentityAllocator allocator);

    // Local mutation. Fails without mutating anything.
    [[nodiscard]] Result<BufferOperation> apply_local_edit(
        std::span<const TextRange> ranges,
        std::span<const std::string_view> replacement);

    [[nodiscard]] Result<BufferOperation> undo();
    [[nodiscard]] Result<BufferOperation> redo();

    // Remote application reports applied/duplicate/deferred/rejected explicitly.
    [[nodiscard]] ApplyOutcome apply_remote(const BufferOperation& op);

    [[nodiscard]] std::string_view text() const;

    // Anchors
    [[nodiscard]] Result<Anchor> anchor_at(uint32_t byte_offset, Affinity) const;
    [[nodiscard]] Result<ResolvedTextPosition> resolve_anchor(const Anchor&) const;
};

class IdList {
public:
    explicit IdList(SharedIdentityAllocator allocator);

    [[nodiscard]] Result<IdListOperation> insert_after(const Anchor&, uint64_t id);
    [[nodiscard]] Result<IdListOperation> remove_at(const Anchor&);
    [[nodiscard]] Result<IdListOperation> undo();
    [[nodiscard]] Result<IdListOperation> redo();

    [[nodiscard]] ApplyOutcome apply_remote(const IdListOperation& op);
    [[nodiscard]] std::vector<uint64_t> ids() const;
    [[nodiscard]] Result<Anchor> anchor_of(uint64_t id, Affinity) const;
    [[nodiscard]] Result<ResolvedListPosition> resolve_anchor(const Anchor&) const;
};
```

The installed `IdListEngine` facade also exposes `local_history_snapshot()` and
`restore_local_history(snapshot)`, plus next-undo/redo group inspection and
step/retained-byte limits. The snapshot contains only actor-local identity
references, the undo cursor, and pending group retirements. A consumer persists
it beside its canonical operation events, replays those events, then restores
the snapshot. Restore rejects references not covered by the replayed owner
history. After a consumer durably publishes a pending retirement, it may call
`acknowledge_undo_retirements()` and persist the resulting snapshot; until then,
the retirement remains available for idempotent retry. This does not replace the
consumer's canonical event log or make a multi-block transaction.

`IdListEngine::try_undo()` and `try_redo()` return
`Result<std::optional<IdListOperation>>`: an empty optional means the relevant
history stack is empty, and an error reports a refused identity reservation.
The legacy `undo()` and `redo()` wrappers remain available and return an empty
optional for either condition.

The public step and byte limit setters return `Result<void>`. If lowering a
limit would create retirement records beyond the engine's independent pending
queue budget, the setter leaves both the limit and history unchanged. The same
backpressure can refuse a local edit before identity reservation. Pending
active and pending history together is limited to 100,000 groups, and pending
retirement snapshots are limited to 8 MiB of estimated snapshot and
identity-array storage. Acknowledge records only after their consumer-owned
durable publication.

Two consequences worth stating:

- `undo`/`redo` return `Result`, because after C3 they persist: failing to record
  the new history entry is a real failure, not something to swallow.
- `resolve_anchor` returns `Result<ResolvedTextPosition>` or
  `Result<ResolvedListPosition>`: success contains a position and failure contains
  an error (R6). Today's integer fallback to document end is the behavior replaced.
  Encoding/decoding a token does not register a retention pin.

Reset behavior is explicit in §10.4. `discard_local_view()` is a local-only
discard that preserves observed coverage; it is not a synchronized clear or a
recovery primitive. Collaborative clear emits ordinary removal operations.

### 6.1 Implemented scoped positions

The binding [boundary contract](vnext-boundary-semantics.md) is implemented in
`<collabtext/Anchor.h>`, `<collabtext/CrdtEngine.h>` and
`<collabtext/IdListEngine.h>`. `tests/reference/BoundaryApiContract.h` now checks
actual public signatures rather than declaring draft classes. The distinct
TextBoundary, TextAttachment, ListBoundary and ListElement opaque values replace
legacy generic Anchor semantics for scoped queries.

Both facades expose `create(SharedIdentityAllocator, PositionScope)` returning
`Result<std::unique_ptr<...>>`. Scope permanently binds document, stream,
primitive and epoch; invalid context is refused before construction. Allocator-only
engines can edit but cannot mint/resolve scoped positions. No default scope exists.

| Facade | Mint | Resolve | Compare |
|---|---|---|---|
| CrdtEngine | `boundary_at(uint32_t utf8_byte_offset, Affinity)`; `attachment_at(uint32_t utf8_byte_offset, Affinity, ReferentSide)` | overloads for TextBoundary and TextAttachment returning `Result<ResolvedTextPosition>` | `compare(TextBoundary, TextBoundary)` returning `Result<int>` |
| IdListEngine | `boundary_at(uint32_t element_index, Affinity)`; `element_of(uint64_t application_value)` | overloads for ListBoundary and ListElement returning `Result<ResolvedListPosition>` | `compare(ListBoundary, ListBoundary)` returning `Result<int>` |

Positions share immutable owned payloads, are copyable, cannot be default
constructed or converted across kinds, and mint no actor identity. Moved-from
positions are refused. Resolutions contain scalar-aligned UTF-8 byte offsets or
list indices with `lost_left`, `lost_right` and `collapsed()`. Strict selected-item
loss returns `detached` without an offset. Undo can restore the original token.
Text editing retains its existing UTF-16 units; position reads use UTF-8 bytes.
`discard_local_view()` makes retained position history unavailable, including
edge tokens.

Typed failures include `off_end`, `not_scalar_boundary`, `not_found`,
`wrong_document`, `wrong_stream`, `wrong_primitive`, `wrong_epoch`,
`awaiting_state`, `detached`, `history_unavailable`, `limit_exceeded`,
`malformed`, `unknown_format` and `unsupported_version`. Scope is checked before
item lookup. Queries and refusals create no operation, pin, reservation or callback.

`encode_position(PositionToken)` returns `Result<std::string>` containing binary
CTPT v1 bytes. `decode_position(std::string_view)` returns `Result<PositionToken>`;
`decode_position_as<T>` refuses a different kind. The decoder owns its value,
requires exact EOF and rejects malformed fields. JSON wrappers must ASCII-encode
bytes before Unicode conversion. See [USING_VNEXT](../USING_VNEXT.md) for an example.

The version is experimental and unpublished; E07b prepares the supported-format
reader obligation for approval. Golden bytes, all-prefix/all-suffix refusal,
native differential histories and fresh-process replay are tested by S04b.
Installed adapter certification remains W03; repaired ordering S02/S03 and real
store recovery, persistent undo and durable retention remain their named gates.

## 7. Callbacks: single-owner, non-reentrant

```cpp
class CrdtEngine {
public:
    using ChangeCallback = std::function<void()>;
    using LocalOpCallback = std::function<void(const Crdt::Operation&)>;

    // Registering replaces any prior callback for the same event.
    Result<CallbackSubscription> subscribe_on_change(ChangeCallback);
    Result<CallbackSubscription> subscribe_on_local_op(LocalOpCallback);

    // Legacy single-slot registration remains available with explicit clear.
    void setOnChange(ChangeCallback);
    void setOnLocalOp(LocalOpCallback);
};
```

The returned move-only subscription disconnects on `reset()` or destruction,
releases its captured callable, and is safe to reset after the engine is
destroyed. Store the handle with the object whose callback it protects. Reset
during dispatch prevents later events; the current event completes from its
stable callback snapshot. The legacy setters remain single-slot and require
explicit replacement or clearing.

The rules, stated once for every callback in the library:

1. **Single owner.** One callback per event. Registering a second replaces the
   first; there is no list and no fan-out inside the engine.
2. **Non-reentrant mutation.** `CrdtEngine` rejects same-engine mutation from a
   notification callback with `ErrorCode::reentrant_mutation` before changing
   state. Checked local APIs (`try_insert`, `try_remove`, `try_replace`,
   `try_undo`, and `try_redo`) expose that error; `apply_ops()` returns one
   rejected outcome per offered operation. Legacy void `insert()` and
   `remove()` and bool `undo()`/`redo()` leave state unchanged and cannot report
   the typed error. Read-only inspection and callback replacement/removal remain
   available; dispatch uses a stable callback snapshot, so self-removal affects
   later notifications only. Defer follow-up mutation until notification
   returns. Transport callback reentrancy follows its own `OpStream` contract.
3. **Lifetimes.** A scoped subscription disconnects when its handle is reset or
   destroyed; the handle may outlive the engine safely. Keep the handle with the
   listener it protects. Legacy setter callbacks stay registered until replaced,
   cleared, or the engine is destroyed. Capturing a raw pointer and allowing its
   object to die while the callback remains connected is still a caller bug.
4. **Only for visible change.** `on_change` fires when visible text changed. An
   operation that is duplicate, deferred, rejected, or purely structural (a
   tombstone, an undo-map update) does not fire it. A batch of remote operations
   fires it at most once, after the batch, if the batch changed anything visible.
5. **Never while holding an internal lock.** A callback is invoked from a state
   where the engine is internally consistent and no lock is held.
6. **Thread affinity.** An engine is owned by one thread at a time. Callbacks
   fire on the calling thread. Transferring an engine between threads is the
   caller's synchronization responsibility.

## 8. Transport and store surface

```cpp
class OpStream {
public:
    virtual Result<void> register_stream(const StreamScope&) = 0;
    virtual Result<void> push(const OutboundDelivery&, PushDurability) = 0;
    virtual Result<SubscriptionId> subscribe(const StreamScope&, InboundHandler) = 0;
    virtual Result<SubscriptionId> subscribe_receipts(ReceiptHandler) = 0;
    virtual Result<void> unsubscribe(SubscriptionId) = 0;
    virtual Result<PollReport> poll(const StreamScope&, PollBudget = {}) = 0;
    virtual Result<std::vector<StreamReceipt>> receipts(const StreamScope&) const = 0;
};
```

Every envelope has a caller-stable
`DeliveryToken {stream, epoch, producer, sequence}`. The sequence is independent
of the CRDT operation counter. Inbound handlers return `commit(recovery_point)`,
`retry()`, or `quarantine(error)`; only `commit` after durable application state
can advance a per-stream/per-epoch/per-producer receipt. `queued` and `durable`
push modes distinguish volatile acceptance from recoverable sender-side bytes.
See [vnext-transport.md](vnext-transport.md) for token ordering, budgets,
subscription lifetime, retry/rejection behavior, and the exact durability
boundary. Receipts are transport progress evidence, never GC certificates.

The internal file-backed `StreamSync` does not implement this new contract yet.
Its observed-operation scalar is not a durable receipt; L03/L08 must supply
recoverable receiver state before that adapter can claim the transactional
interface.

## 9. Breaks from today's API

| Today | Becomes | Why |
|---|---|---|
| `Buffer(uint16_t replica_id)` | `Buffer(SharedIdentityAllocator)` | 16-bit ids cannot survive offline enrollment (R1), and an engine must not own its counter |
| `apply_ops(...)` returning `void` | `apply_remote(...) -> ApplyOutcome` | duplicate/deferred/rejected must be distinguishable (R1) |
| `undo()/redo()` returning `optional<Operation>` | `Result<BufferOperation>` | persistence can fail (C3) |
| `uint32_t resolve_anchor(...)` | `Result<ResolvedTextPosition>` / `Result<ResolvedListPosition>` | success carries a position; failure never fabricates end (R6) |
| `Operation` with public fields | opaque `BufferOperation` | field layout is not a contract |
| `Anchor::max()` as a largest-identity sentinel | explicit past-the-end | a 128-bit actor space has no largest identity |
| `OpStream::lowest_peer_acked_lamport()` | `receipts()` per stream | a scalar cannot mean cross-stream coverage |
| `collect_garbage()` / `compact(Global)` | replaced by G06's certificate | receipt is not authority (C14) |
| `src/` on the public include path | internal, `PRIVATE` | internals are not a contract (A01) |
| `set_on_change` with no removal | `clear_on_change()` | lifecycle must be explicit (R9) |

### 9.1 P05 implementation history and current facade

P05 first made outcomes distinguishable on the internal Buffer API. A02 has since
completed the public facade: `CrdtEngine::apply_remote` returns one `ApplyOutcome`,
and `CrdtEngine::apply_ops` returns a result for each offered operation. The
P05 implementation notes below describe that intermediate stage, not the current
installed API.

Landed:

- The internal `Buffer::apply_ops(...)` returns `std::vector<ApplyOutcome>`, one
  per offered operation. P05 added that return while leaving internal call sites
  alone; A02 completed the corresponding typed API at the public facade.
- `ApplyOutcome` exists and is public, with `kind()`, `awaiting()`, and
  `error()`.
- Local mutation refuses invalid requests instead of asserting:
  `try_apply_local_edit`, `try_insert_after`, and `try_remove_at` return
  `Result<...>`. The old `apply_local_edit` / `insert_after` / `remove_at`
  remain, now delegating to them and returning an empty value on refusal. This is
  the one place where keeping the old signature was not merely convenient: it
  was the difference between a documented refusal and undefined behaviour, since
  the old bodies asserted and then used the unvalidated value anyway.
- A change callback on `Buffer`, which had none, firing only for applied
  operations. Final visible-change batch filtering/removal/reentrancy remains A04.
- Review amendments: `IdList::try_anchor_of` returns a typed miss; local text edits
  reject UTF-8 interior boundaries; blocked operations still undergo semantic
  validation. Deferred and diagnostic quarantine queues have separate count/byte
  budgets; `take_quarantined()` drains diagnostic retention. The caller retains
  ownership of rejected input and the transport/store must preserve its bytes.

### 9.2 What P06a landed from this table

- Engines are constructed from a borrowed allocator rather than an ActorId, and
  expose `actor_id()`. There is no legacy integer constructor to keep, because
  an integer could not be made safe: any value a caller supplied would be
  indistinguishable from an identity the engine had authority to claim.
- Operation identity is a persistent 128-bit actor plus a checked 64-bit counter,
  and the engine allocates it as a run interval per operation (§2.1). A deletion
  marks its tombstones with the deleting operation's own identity.
- An all-zero `ActorId` is refused at every mint site with
  `ErrorCode::invalid_identity`, in Release as well as Debug.
- `idlist_op_lamport()`/`op_lamport()` became `idlist_op_identity()`/
  `op_identity()`, and `Anchor`'s largest-identity sentinel became an explicit
  past-the-end flag, because a 128-bit actor space has no largest identity.

Still owed, and still not invented here:

- Epoch, document and stream context on the operation value. The operation still
  carries no epoch to compare, so P05's validator records that check as a gap
  rather than fabricating one. It belongs with P07 (envelope and transport
  outcomes) and L04 (persisted envelope identity).
- `Buffer::undo()` / `redo()` still return `std::optional<Operation>` and return
  `nullopt` on a refused reservation (exhaustion, or an allocator with no valid
  actor). The public `CrdtEngine` facade exposes checked `try_undo()` /
  `try_redo()` as `Result<std::optional<Operation>>`; its bool convenience
  methods remain for editors that only need a handled/not-handled answer.
- Typed position resolution and opaque operations are implemented by S04/A02
  and available from the installed facade; their exact contracts are in §§7–8.

## 10. Examples

These are the compile-oriented examples A02 turns into code and E03 compiles
against the installed package. They are written to be transcribed exactly.

### 10.1 A text edit, sent and applied

```cpp
#include <collabtext/Buffer.h>
#include <collabtext/Result.h>
#include <array>
#include <string_view>
#include <utility>

// buffer belongs to a session with a durable ReplicaState owner.
CollabText::Result<CollabText::BufferOperation> type(CollabText::Buffer& buffer) {
    const std::array<CollabText::TextRange, 1> ranges{{{0, 0}}};
    const std::array<std::string_view, 1> replacements{{"hello"}};
    auto edited = buffer.apply_local_edit(ranges, replacements);
    if (!edited.ok()) return edited.error();
    return std::move(edited).value(); // the operation is move-only
}
```

### 10.2 Applying remote operations, with every outcome handled

```cpp
void on_inbound(CollabText::Buffer& buffer, const CollabText::BufferOperation& op) {
    const CollabText::ApplyOutcome outcome = buffer.apply_remote(op);
    switch (outcome.kind()) {
        case CollabText::ApplyOutcomeKind::applied:
            redraw(buffer.text());
            break;
        case CollabText::ApplyOutcomeKind::duplicate:
            break;                       // idempotent; not an error
        case CollabText::ApplyOutcomeKind::deferred:
            note_pending(*outcome.awaiting());
            break;
        case CollabText::ApplyOutcomeKind::rejected:
            quarantine(*outcome.error());
            break;
    }
}
```

### 10.3 Two streams, independent counters

```cpp
// A scalar maximum would be wrong here: these two counters are unrelated.
const std::array<CollabText::TextRange, 1> ranges{{{0, 0}}};
const std::array<std::string_view, 1> replacements{{"a"}};
auto text = buffer.apply_local_edit(ranges, replacements);
auto comment = comments.insert_after(anchor, 42);
if (!text.ok()) { report(text.error()); return; }
if (!comment.ok()) { report(comment.error()); return; }
assert(text.value().context().id.counter == 1);
assert(comment.value().context().id.counter == 1); // fresh, independent streams
assert(text.value().context().stream != comment.value().context().stream);
```

### 10.4 Reset, collaborative clear, and recovery

Use operation APIs to clear shared content. This list example produces one
removal operation per currently visible item and publishes each operation as it
succeeds, so peers receive the same clear:

```cpp
const auto current_ids = list.ids();
for (uint64_t value : current_ids) {
    auto anchor = list.try_anchor_of(value);
    if (!anchor) { report(anchor.error()); return; }
    auto removal = list.try_remove_at(anchor.value());
    if (!removal) { report(removal.error()); return; }
    transport.publish(removal.value());
}
```

Text uses one ordinary replacement operation:

```cpp
auto removal = text_engine.try_replace(0, text_engine.length(), "");
if (!removal) { report(removal.error()); return; }
transport.publish(removal.value());
```

`IdListEngine::discard_local_view()` and `CrdtEngine::discard_local_view()` are
for a detached local view being replaced outside collaboration. They emit no
operation or callback; preserve the actor allocator and observed version; clear
current content, undo/redo, and deferred/quarantined input; and invalidate
scoped position tokens. Old legacy anchors are not promised to resolve after
their retained items are discarded. Replaying previously observed operations
into a discarded engine yields duplicates, leaving its view empty. Do not use
these methods before replay or recovery.

Recovery uses fresh engine state, or imports a valid checkpoint and replays only
uncovered retained operations. `DocumentSession::open()` performs that recovery
from its retained log and optional validated checkpoint. Replacing a shared
document is an epoch-aware lifecycle decision: publish edits in the current
epoch or bootstrap a new document/epoch through membership protocol. It is not
a local discard.

| Action | Preserves | Clears or invalidates | Emits |
|---|---|---|---|
| `discard_local_view()` | Actor/allocator counter, observed version, configured limits, callbacks, pending undo-retirement records | Current CRDT content, active undo/redo, deferred/quarantined input, scoped positions | No operation or callback |
| Collaborative text/list clear | Actor state; retained CRDT identities and tombstones remain subject to normal retention | Visible items through ordinary delete operations; local undo may restore the caller's own edits | Local operation and change callbacks; peers apply the same operations |
| Fresh replay/checkpoint recovery | Durable actor reservation and supported actor-local history from the selected recovery source | Rebuilds state from baseline plus retained operations/checkpoint; listeners must be reattached | No synthetic local edits during replay |

### 10.5 Callback lifecycle, done explicitly

```cpp
// A named callback, so the example is unambiguous about who owns it.
void show_visible_text(const CollabText::ChangeInfo& info) {
    // Only visible change arrives here, and only outside internal locks.
    if (info.visible_change()) status_line().showText(info.text());
}

const CollabText::Buffer::ChangeCallback on_change = show_visible_text;

buffer.set_on_change(on_change);

// Registering another replaces this one; the previous is released before the
// call returns, so no dangling capture can outlive it.
buffer.set_on_change(replacement_callback);

// Removal is explicit, and safe to call from inside a callback.
buffer.clear_on_change();
```

### 10.6 Transport receipts are not authority

```cpp
auto snapshot = stream.receipts(scope);
if (snapshot) {
    for (const auto& receipt : snapshot.value()) {
        log(receipt.peer, receipt.through.stream, receipt.through.producer,
            receipt.through.sequence);
    }
}
// Deliberately absent: anything that would treat a receipt as permission to
// reclaim state. Reclamation is authorized by a validated certificate (G06).
```

## 11. What this guide does not specify

Merge ordering (S02), reclamation eligibility (G01), snapshot and recovery
layouts (L03/G04), and the Qt adapter (A03) are specified by their own tasks.
Where this guide names a type they will define — `ChangeInfo`, `Affinity`,
`TextRange`, `Anchor` — it fixes the *shape of the contract* (what it means,
when it may fail, who owns it), not the internals.
Amendments to this guide are amendments to the plan and update both documents.


## 11. Review amendments: durable ownership and external consumers

P02 private ReplicaState is a move-only, single-owner object; one owner serves
all its documents. Close it before reopening the same private directory. Kernel
leases persist as files but ownership ends with descriptor close/process death.
Operation counters start at 1 (zero is invalid); UINT64_MAX is exhaustion.
Current state-file stream names are ASCII `[A-Za-z0-9_.:-]`, at most 128 bytes.
Current per-document lease keys allow nonempty document IDs without NUL up to 80
bytes and stream names up to 32 bytes, encoded as collision-free hex path parts.
These are provisional P02 file-layout limits, not limits consumers may infer for
future arbitrary Wave block IDs. L05/A02 must publish the final key policy.

L01 errors after a rename/directory barrier may be `commit_uncertain`: complete
new contents are already visible, so retry/recovery must reconcile rather than
assume rollback. Append errors can leave a partial tail; L02 framing/recovery owns
that case. L01 primitives do not yet give the editor or DocumentSession a working
save/reopen guarantee. Linux tests do not establish Windows/macOS support.

[External consumer requirements W1–W6](vnext-external-position-contract.md) are
normative amendments for S04a/S04b/S04/S05, L05/L06, G01/G06/G08,
A02a/A06a/W01–W03/A02/A06, E07a/E07b, and E05.
Published supported vNext position-token bytes must remain readable independently
of C++ layout. External storage does not create a pin; pin recovery precedes GC.
Historical playback uses its own matching projection/archive. Formatting is
explicitly consumer-owned outside the active plain-text/IdList release scope.

## 12. Early installed API and live-position sequencing

The [consumer lane](../roadmap/2026-10-reliability/00-consumer-integration.md)
defines the earlier implementation schedule. A01 splits component targets;
A02a exposes only compiled current capabilities behind self-contained public
headers; A06a installs/exports them and verifies a relocated external consumer.
Target-only signatures in this guide do not become implemented because an alpha
package exists. Each capability has explicit executable evidence and limits.

S04a specifies scoped boundary identity, affinity, typed resolution/collapse,
comparison and a bounded canonical token codec using stable item identities.
Its independent boundary model consumes supplied ordered-item/visibility states;
the complete S02 ordering interpreter remains a separate design gate. S04b
implements usable live positions after P06b permits real operation replay. W03
certifies those positions through the installed surface before S03/L08 complete.
S04 then certifies repaired ordering and real durable recovery; S05 supplies
durable pin policy and G-series tasks authorize reclamation. Physical GC remains
disabled in the early alphas.

Result shapes must distinguish resolved UTF-8 byte/list-index positions, explicit
collapse, missing causal context, unavailable history, and foreign/malformed or
unsupported tokens. S04a fixes the precise public names and ownership; no current
uncompiled code example supplies the missing algorithm. A strict attachment
consumer can surface detachment while Markoff-style carets retain defined collapse
behavior. A token from the wrong object receives a typed failure before lookup.

DocumentSession/file transport/Qt adapters are optional compositions. Consumers
can own their per-block models, Markdown rendering/formatting, operation routing,
canonical event log, persistence, transport and cross-block undo using Core
primitives. The early installed smoke and later full examples must demonstrate
that route without forcing a file-backed session or a Wave-specific event log.

E07a prepares the package alpha; E07b prepares a position alpha and records
supported-token versions/golden bytes before persistence is advertised. An
approved published supported token version remains readable through later
supported releases even if C++ API/ABI changes. Preparing a release candidate
does not publish it or merge master.


### 12.1 A02 installed facade status

A02 exposes typed `CrdtEngine::apply_remote` and ordered `apply_ops`, checked
`try_undo` / `try_redo`, and undo depth/byte limits. `Buffer` remains an internal
implementation detail. The installed consumer demonstrates a consumer-owned
`OpStream` implementation with stable envelope types; no transport implementation
layout is part of the API. Text edit offsets are UTF-16 code units, while public
position mint/resolve offsets are UTF-8 bytes at scalar boundaries.
