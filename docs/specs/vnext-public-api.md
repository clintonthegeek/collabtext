# vNext public API guide

**Status:** binding design-gate artifact (P01). This is the API that P02–P08, the
L/S/G/A/X/E phases, and the installed examples in E03 are written against.
**Paired with:** [vnext-protocol.md](vnext-protocol.md) for the wire and lifecycle
rules these signatures implement.
**Authority:** [vnext-design-constraints.md](vnext-design-constraints.md) (D01).

The code in this document is **target signature**, not compiled code: most of
these types do not exist yet. A02 compiles the examples; E03 compiles them
against the installed package. Where today's API differs, §9 lists the break so
a caller knows what to expect.

---

## 1. Error handling: `Result`

C++23's `std::expected` is not required. The project ships a small C++20 type.

```cpp
namespace collabtext {

template <class T>
class Result {
public:
    Result(T value);                  // implicit: success
    Result(Error error);              // implicit: failure

    [[nodiscard]] bool ok() const noexcept;
    [[nodiscard]] const T& value() const;   // precondition ok()
    [[nodiscard]] const Error& error() const; // precondition !ok()

    template <class U> T value_or(U fallback) const;

    // No throwing accessor on purpose: a caller that ignores an error gets a
    // compile error, not a runtime surprise.
};

}  // namespace collabtext
```

`Result` is `[[nodiscard]]`, movable, and never allocates. Errors are values, so
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

There is deliberately no `ActorId()` default that means "actor 1". A
constructed-from-nothing `ActorId` is invalid and any operation carrying it is
rejected — which is what makes "silent fallback to actor 1" impossible to write.

## 3. Operation value types

`BufferOperation` and `IdListOperation` are distinct, opaque, and never
interchangeable. Both are non-copyable handles over an immutable encoded
operation plus its decoded header, so passing one around cannot mutate it.

```cpp
class BufferOperation {
public:
    BufferOperation(const BufferOperation&) = delete;
    BufferOperation& operator=(const BufferOperation&) = delete;
    BufferOperation(BufferOperation&&) noexcept;
    BufferOperation& operator=(BufferOperation&&) noexcept;
    ~BufferOperation();

    [[nodiscard]] OperationContext context() const;
    [[nodiscard]] uint64_t lamport() const;      // the ordering key, narrow contract

    // Round-trip is the stable surface; the payload layout is not.
    [[nodiscard]] std::string encode() const;
    static Result<BufferOperation> decode(std::string_view bytes);
};

class IdListOperation { /* same shape, own payload and own codec */ };
```

**The narrow contract.** Callers may depend on `context()`, `lamport()`, and
`encode()`/`decode()` round-tripping. They may not depend on any field layout,
serialization bytes, or the existence of a field they once read. This replaces
today's habit of reading `Operation`'s fields directly (D01 §3, B8).

```cpp
// What a caller may write:
auto op = engine.local_edit(...);
send(op.encode());

// What a caller must not write: depends on layout, and will be removed.
send(JSON{op.timestamp, op.ranges, op.new_text});
```

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
    unresolvable_anchor,
    // store
    corrupt_record, missing_recovery_point, coverage_gap, io_error,
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

## 6. Engine surface

Both primitives expose the same shape. Neither is the other's alias.

```cpp
class Buffer {
public:
    explicit Buffer(ActorId actor);

    // Local mutation. Fails without mutating anything.
    [[nodiscard]] Result<BufferOperation> apply_local_edit(
        std::span<const TextRange> ranges,
        std::span<const std::string_view> replacement);

    [[nodiscard]] Result<BufferOperation> undo();
    [[nodiscard]] Result<BufferOperation> redo();

    // Remote application. Never throws; always says what happened.
    [[nodiscard]] ApplyOutcome apply_remote(const BufferOperation& op);

    [[nodiscard]] std::string_view text() const;

    // Anchors
    [[nodiscard]] Result<Anchor> anchor_at(uint32_t byte_offset, Affinity) const;
    [[nodiscard]] ApplyOutcome resolve_anchor(const Anchor&);   // explicit failure
};

class IdList {
public:
    explicit IdList(ActorId actor);

    [[nodiscard]] Result<IdListOperation> insert_after(const Anchor&, uint64_t id);
    [[nodiscard]] Result<IdListOperation> remove_at(const Anchor&);
    [[nodiscard]] Result<IdListOperation> undo();
    [[nodiscard]] Result<IdListOperation> redo();

    [[nodiscard]] ApplyOutcome apply_remote(const IdListOperation& op);
    [[nodiscard]] std::vector<uint64_t> ids() const;
    [[nodiscard]] Anchor anchor_of(uint64_t id, Affinity) const;
};
```

Two consequences worth stating:

- `undo`/`redo` return `Result`, because after C3 they persist: failing to record
  the new history entry is a real failure, not something to swallow.
- `resolve_anchor` returns an outcome, not an integer, because an unresolvable or
  wrong-epoch anchor is an explicit outcome (R6). Today's
  `uint32_t resolve_anchor(...)` that silently clamps to the document end is the
  behaviour this replaces.

`IdList::local_clear()` keeps its current semantics (documented in
`api-inventory.md` §4) and returns a `Result` so a caller can tell a failed clear
from a successful one. It is not a collaboration operation and peers never see
it.

## 7. Callbacks: single-owner, non-reentrant

```cpp
class Buffer {
public:
    using ChangeCallback = std::function<void(ChangeInfo)>;
    using LocalOpCallback = std::function<void(const BufferOperation&)>;

    // Registering replaces any previous registration; the previous one is
    // released before this call returns.
    void set_on_change(ChangeCallback);
    void set_on_local_op(LocalOpCallback);

    // Explicit removal. After this, no callback fires. Safe during a callback.
    void clear_on_change();
    void clear_on_local_op();
};
```

The rules, stated once for every callback in the library:

1. **Single owner.** One callback per event. Registering a second replaces the
   first; there is no list and no fan-out inside the engine.
2. **Non-reentrant.** A callback must not call back into the engine or its
   transport. Doing so is undefined behaviour, not a deadlock to be diagnosed.
   To act on a change, return from the callback and then act.
3. **Lifetimes.** The engine owns the `std::function`. A callback capturing a raw
   pointer to an object that dies first is a caller bug; the library does not
   keep the object alive and does not check.
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
    virtual Result<void> push(std::string_view stream, std::string_view payload) = 0;
    virtual void set_on_inbound(InboundCallback) = 0;   // replacement semantics
    virtual void clear_on_inbound() = 0;

    // Per-stream receipt summary. Receipt is not application and never
    // authorizes reclamation.
    virtual std::vector<StreamAck> receipts() const = 0;
    virtual void set_on_receipts_update(ReceiptsCallback) = 0;
};

enum class PollOutcomeKind { progress, idle, quarantined, coverage_gap, error };

class PollOutcome {
public:
    [[nodiscard]] PollOutcomeKind kind() const noexcept;
    [[nodiscard]] std::size_t applied() const noexcept;      // operations applied
    [[nodiscard]] std::size_t quarantined_records() const noexcept;
    [[nodiscard]] std::optional<Error> error() const noexcept;
    [[nodiscard]] std::optional<RecoveryPoint> frontier() const noexcept;
};
```

`lowest_peer_acked_lamport()` is **replaced** by per-stream receipts, because a
scalar across streams is the coverage error described in
[vnext-protocol.md](vnext-protocol.md) §1.2. `PollOutcome` reports quarantine and
coverage gaps as outcomes rather than as silence, which is what F5/F6/F10 all
needed and did not have.

## 9. Breaks from today's API

| Today | Becomes | Why |
|---|---|---|
| `Buffer(uint16_t replica_id)` | `Buffer(ActorId)` | 16-bit ids cannot survive offline enrollment (R1) |
| `apply_ops(...)` returning `void` | `apply_remote(...) -> ApplyOutcome` | duplicate/deferred/rejected must be distinguishable (R1) |
| `undo()/redo()` returning `optional<Operation>` | `Result<BufferOperation>` | persistence can fail (C3) |
| `uint32_t resolve_anchor(...)` | `ApplyOutcome resolve_anchor(...)` | unresolvable is an explicit outcome (R6) |
| `Operation` with public fields | opaque `BufferOperation` | field layout is not a contract |
| `OpStream::lowest_peer_acked_lamport()` | `receipts()` per stream | a scalar cannot mean cross-stream coverage |
| `collect_garbage()` / `compact(Global)` | replaced by G06's certificate | receipt is not authority (C14) |
| `src/` on the public include path | internal, `PRIVATE` | internals are not a contract (A01) |
| `set_on_change` with no removal | `clear_on_change()` | lifecycle must be explicit (R9) |

## 10. Examples

These are the compile-oriented examples A02 turns into code and E03 compiles
against the installed package. They are written to be transcribed exactly.

### 10.1 A text edit, sent and applied

```cpp
#include <collabtext/Buffer.h>
#include <collabtext/Result.h>

collabtext::Result<collabtext::BufferOperation> type(const std::string& where) {
    const collabtext::ActorId actor = collabtext::ActorId::generate().value();
    collabtext::Buffer buffer(actor);

    // Local mutation reports failure instead of pretending to succeed.
    const auto edited = buffer.apply_local_edit({{{0, 0}}}, {"hello"});
    if (!edited.ok()) return edited.error();
    return edited.value();
}
```

### 10.2 Applying remote operations, with every outcome handled

```cpp
void on_inbound(collabtext::Buffer& buffer, const collabtext::BufferOperation& op) {
    const collabtext::ApplyOutcome outcome = buffer.apply_remote(op);
    switch (outcome.kind()) {
        case collabtext::ApplyOutcomeKind::applied:
            redraw(buffer.text());
            break;
        case collabtext::ApplyOutcomeKind::duplicate:
            break;                       // idempotent; not an error
        case collabtext::ApplyOutcomeKind::deferred:
            note_pending(*outcome.awaiting());
            break;
        case collabtext::ApplyOutcomeKind::rejected:
            quarantine(*outcome.error());
            break;
    }
}
```

### 10.3 Two streams, independent counters

```cpp
// A scalar maximum would be wrong here: these two counters are unrelated.
const auto text_op    = buffer.apply_local_edit({{{0, 0}}}, {"a"}).value();
const auto comment_op = comments.insert_after(anchor, 42).value();

assert(text_op.context().id.counter == 1);
assert(comment_op.context().id.counter == 1);   // separate space, same value
assert(text_op.context().stream != comment_op.context().stream);
```

### 10.4 A structural list, with reset semantics stated

```cpp
collabtext::Result<collabtext::IdListOperation> append(collabtext::IdList& list,
                                                        uint64_t id) {
    const auto anchor = list.anchor_of(list.ids().back(), collabtext::Affinity::Left);
    return list.insert_after(anchor, id);
}

// A clear is local, is not a collaboration operation, and can fail visibly.
if (const auto cleared = list.local_clear(); !cleared.ok()) {
    report(cleared.error().message());
}
```

### 10.5 Callback lifecycle, done explicitly

```cpp
// A named callback, so the example is unambiguous about who owns it.
void show_visible_text(const collabtext::ChangeInfo& info) {
    // Only visible change arrives here, and only outside internal locks.
    if (info.visible_change()) status_line().showText(info.text());
}

const collabtext::Buffer::ChangeCallback on_change = show_visible_text;

buffer.set_on_change(on_change);

// Registering another replaces this one; the previous is released before the
// call returns, so no dangling capture can outlive it.
buffer.set_on_change(replacement_callback);

// Removal is explicit, and safe to call from inside a callback.
buffer.clear_on_change();
```

### 10.6 Transport receipts are not authority

```cpp
for (const auto& ack : stream.receipts()) {
    log(ack.stream, ack.actor, ack.counter);   // per stream, per actor
}
// Deliberately absent: anything that would treat a receipt as permission to
// reclaim state. Reclamation is authorized by a validated certificate (G06).
```

## 11. What this guide does not specify

Merge ordering (S02), reclamation eligibility (G01), snapshot and recovery
layouts (L03/G04), and the Qt adapter (A03) are specified by their own tasks.
Where this guide names a type they will define — `RecoveryPoint`, `StreamAck`,
`ChangeInfo`, `Affinity`, `TextRange`, `Anchor` — it fixes the *shape of the
contract* (what it means, when it may fail, who owns it), not the internals.
Amendments to this guide are amendments to the plan and update both documents.
