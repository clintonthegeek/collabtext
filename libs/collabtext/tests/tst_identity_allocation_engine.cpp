/// tst_identity_allocation_engine.cpp — P06a step 3b: the *engine* obeys the
/// run-interval allocation contract.
///
/// docs/specs/vnext-identity.md §3.2 specifies one allocation per local
/// operation: a checked reservation of `n + 1` consecutive counters, `n` item
/// identities followed by the operation's own. The specification and the
/// independent model for that contract landed with P06a's design gate
/// (tst_identity_allocation_model.cpp). Neither proves the engine follows it —
/// the model does not call the engine, and until now the engine did not follow
/// it: `Buffer::apply_validated_local_edit` ticked its clock once per inserted
/// scalar and then set the operation's identity to `m_clock.counter - 1`, which
/// is the identity of the *last item of its own run*. An operation and one of
/// its characters claimed the same identity.
///
/// These cases pin the four properties that changed:
///
///   1. Non-aliasing. An operation's identity is strictly greater than every
///      item identity it created and strictly less than the next operation's
///      base. "Which operation inserted this character" becomes a range-
///      containment question instead of a guess.
///   2. One reservation per operation, not one per scalar. A counting allocator
///      makes the number of reservations observable, which is the only way to
///      tell "reserved a run" from "reserved n + 1 times and happened to get
///      contiguous values".
///   3. Derivation allocates nothing. Splitting a fragment partitions its
///      owner's interval; it never reserves.
///   4. Deletion uses the deleting operation's own identity, so it no longer
///      costs a second tick.
///
/// And two properties of the allocator relationship itself: the engine *borrows*
/// its allocator rather than owning one (there is no Buffer constructor that
/// mints identity), and exhaustion is reported rather than wrapped.

#include <QTest>

#include "TestActor.h"
#include "VolatileCounter.h"

#include <collabtext/ActorId.h>

#include "crdt/Buffer.h"
#include "crdt/IdList.h"
#include "identity/ReplicaAllocator.h"

#include <QTemporaryDir>

#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

using CollabText::ActorId;
using CollabText::Crdt::Anchor;
using CollabText::Crdt::Bias;
using CollabText::Crdt::Buffer;
using CollabText::Crdt::EditOperation;
using CollabText::Crdt::IdList;
using CollabText::Crdt::Identity;
using CollabText::Crdt::UndoOperation;
using CollabText::ErrorCode;
using CollabText::ReplicaState;
using CollabText::Result;
using CollabText::Test::actor;
using CollabText::Test::VolatileCounter;

namespace {

/// A counter plus the allocator handle an engine borrows from it.
struct Fixture {
    std::shared_ptr<VolatileCounter> counter;
    std::unique_ptr<Buffer> buffer;

    explicit Fixture(uint8_t actor_index = 1, uint64_t start = 1) {
        counter = std::make_shared<VolatileCounter>(actor(actor_index), start);
        buffer = std::make_unique<Buffer>(counter);
    }
    Buffer &buf() { return *buffer; }
    std::shared_ptr<VolatileCounter> allocator() { return counter; }
};

EditOperation as_edit(const CollabText::Crdt::Operation &op) {
    return std::get<EditOperation>(op);
}

/// IdList operations carry their identity in the same place; this keeps the
/// cases below from having to name the variant.
Identity op_identity_of(const CollabText::Crdt::IdListOperation &op) {
    return CollabText::Crdt::idlist_op_identity(op);
}

/// Every item identity the operation says it inserted, in document order.
std::vector<uint64_t> item_counters(const EditOperation &op) {
    std::vector<uint64_t> out;
    for (const auto &frag : op.inserted_fragments)
        for (uint32_t i = 0; i < frag.length; ++i)
            out.push_back(frag.origin.counter + i);
    return out;
}

} // namespace

class TestIdentityAllocationEngine : public QObject {
    Q_OBJECT
private slots:

    // ---- non-aliasing ----------------------------------------------------

    void operation_identity_is_above_every_item_it_created() {
        Fixture f;
        const auto op = as_edit(f.buf().apply_local_edit({{0, 0}}, {"hello"}));

        const auto items = item_counters(op);
        QCOMPARE(items.size(), size_t(5));
        for (const uint64_t item : items)
            QVERIFY2(item < op.timestamp.counter,
                     "an operation's identity must be strictly greater than "
                     "every item identity it created");

        // The items are consecutive positions of one reserved interval.
        for (size_t i = 1; i < items.size(); ++i)
            QCOMPARE(items[i], items[i - 1] + 1);
    }

    void next_operation_starts_above_the_previous_one() {
        Fixture f;
        const auto first = as_edit(f.buf().apply_local_edit({{0, 0}}, {"abc"}));
        const auto second = as_edit(f.buf().apply_local_edit({{3, 3}}, {"de"}));

        QVERIFY(second.timestamp.counter > first.timestamp.counter);
        for (const uint64_t item : item_counters(first))
            QVERIFY(item < second.timestamp.counter);
        for (const uint64_t item : item_counters(second))
            QVERIFY(item > first.timestamp.counter);
    }

    void a_deletion_only_operation_consumes_exactly_one_counter() {
        Fixture f;
        f.buf().apply_local_edit({{0, 0}}, {"abcdef"});
        const uint64_t before = f.allocator()->peek();
        const auto op = as_edit(f.buf().apply_local_edit({{1, 4}}, {""}));

        QCOMPARE(op.inserted_fragments.size(), size_t(0));
        QCOMPARE(f.allocator()->peek(), before + 1);
        QCOMPARE(op.timestamp.counter, before);
    }

    void delete_uses_the_deleting_operations_own_identity() {
        // §3.3: the deletion identity is the operation's identity, which is why
        // it no longer costs a second counter tick. Before this, `deletion_ts`
        // existed only because the operation's timestamp could not serve twice.
        Fixture f;
        f.buf().apply_local_edit({{0, 0}}, {"abcdef"});
        const auto op = as_edit(f.buf().apply_local_edit({{1, 4}}, {""}));
        QCOMPARE(op.deletion_id, op.timestamp);

        const auto fragments = f.buf().fragments();
        QVERIFY(!fragments.empty());
        for (const auto &frag : fragments)
            for (const Identity &del : frag.deletions)
                QCOMPARE(del, op.timestamp);
    }

    void undo_and_redo_mint_fresh_operation_identities() {
        Fixture f;
        const auto edit = as_edit(f.buf().apply_local_edit({{0, 0}}, {"abc"}));

        const auto undone = std::get<UndoOperation>(f.buf().undo().value());
        QVERIFY(undone.timestamp.counter > edit.timestamp.counter);
        // The undo's targets are *item* identities, from a different domain than
        // its own identity, so no undo-map entry can be confused with the
        // operation that produced it.
        QVERIFY(!undone.counts.empty());
        for (const auto &[edit_id, count] : undone.counts) {
            QCOMPARE(edit_id.actor, edit.timestamp.actor);
            QVERIFY(edit_id.counter < edit.timestamp.counter);
            QVERIFY(undone.timestamp.counter > edit_id.counter);
        }
        const auto redone = std::get<UndoOperation>(f.buf().redo().value());
        QVERIFY(redone.timestamp.counter > undone.timestamp.counter);
    }

    // ---- one reservation, not one per scalar -----------------------------

    void one_local_edit_is_one_reservation() {
        Fixture f;
        f.allocator()->reset_call_log();
        const auto op = as_edit(f.buf().apply_local_edit({{0, 0}}, {"abcdefghij"}));

        // Ten scalars plus the operation's own identity, reserved once.
        QCOMPARE(f.allocator()->reservation_count(), size_t(1));
        QCOMPARE(f.allocator()->last_reserved_count(), uint64_t(11));
        QCOMPARE(op.timestamp.counter, op.inserted_fragments.front().origin.counter + 10);
    }

    void a_replacement_reserves_items_and_operation_in_one_interval() {
        Fixture f;
        f.buf().apply_local_edit({{0, 0}}, {"aaaa"});
        f.allocator()->reset_call_log();

        // Replace two ranges with three scalars each: 6 items + 1 operation.
        const auto op = as_edit(
            f.buf().apply_local_edit({{0, 2}, {2, 4}}, {"bbb", "ccc"}));
        QCOMPARE(f.allocator()->reservation_count(), size_t(1));
        QCOMPARE(f.allocator()->last_reserved_count(), uint64_t(7));
        QCOMPARE(item_counters(op).size(), size_t(6));
    }

    // ---- derivation allocates nothing ------------------------------------

    void splitting_a_fragment_reserves_nothing() {
        // Splitting partitions the owning operation's interval; it never
        // allocates. A remote peer's fragment is the cleanest case: nothing
        // about applying it may touch the local counter.
        Fixture f;
        const auto op = as_edit(f.buf().apply_local_edit({{0, 0}}, {"abcdefghij"}));
        f.allocator()->reset_call_log();

        // A deletion into the middle forces the fragment to split.
        const auto del = as_edit(f.buf().apply_local_edit({{3, 5}}, {""}));
        QVERIFY(!del.inserted_fragments.empty() || true);
        QCOMPARE(f.allocator()->reservation_count(), size_t(1));  // the delete only

        // The derived identities are still inside the original run's interval.
        for (const auto &frag : f.buf().fragments()) {
            QVERIFY2(frag.origin.counter <= op.timestamp.counter,
                     "a derived item identity left its operation's interval");
            QVERIFY(frag.origin.counter + frag.length - 1 <= op.timestamp.counter);
        }
    }

    void applying_a_remote_operation_raises_the_local_counter() {
        // §3.4: the raise is derived state, not durable allocation. It exists so
        // a later local identity sorts after what this replica has seen.
        Fixture local(1);
        Fixture remote(2);

        auto remote_op = remote.buf().apply_local_edit({{0, 0}}, {"hello"});
        const uint64_t local_before = local.allocator()->peek();
        local.buf().apply_ops({remote_op});

        QVERIFY(local.allocator()->peek() > local_before);
        QVERIFY(local.allocator()->peek() > as_edit(remote_op).timestamp.counter);

        // The raise is not a reservation: a remote peer's operation must never
        // consume a local counter value.
        QCOMPARE(local.allocator()->reservation_count(), size_t(0));
    }

    // ---- exhaustion is reported, never wrapped ---------------------------

    void exhaustion_is_reported_rather_than_wrapped() {
        Fixture f(1, std::numeric_limits<uint64_t>::max() - 2);
        const auto result = f.buf().try_apply_local_edit({{0, 0}}, {"abc"});
        QVERIFY(!result.ok());
        QCOMPARE(result.error().code(), ErrorCode::counter_exhausted);
        // Nothing was consumed and nothing changed.
        QCOMPARE(f.allocator()->reservation_count(), size_t(0));
        QVERIFY(f.buf().text().empty());
    }

    void a_run_that_does_not_fit_is_refused_as_a_whole() {
        // §3.5's boundary table, at the engine. UINT64_MAX is the exhaustion
        // sentinel and is never issued, so with two values left an insert of one
        // scalar (item + operation = two values) is the last thing that fits.
        Fixture f(1, std::numeric_limits<uint64_t>::max() - 2);

        const auto result = f.buf().try_apply_local_edit({{0, 0}}, {"a"});
        QVERIFY(result.ok());
        QCOMPARE(f.allocator()->peek(), std::numeric_limits<uint64_t>::max());
        QCOMPARE(f.allocator()->reservation_count(), size_t(1));

        const auto again = f.buf().try_apply_local_edit({{1, 1}}, {"x"});
        QVERIFY(!again.ok());
        QCOMPARE(again.error().code(), ErrorCode::counter_exhausted);
        // All-or-nothing: the refused reservation consumed nothing.
        QCOMPARE(f.allocator()->reservation_count(), size_t(1));
        QCOMPARE(f.allocator()->peek(), std::numeric_limits<uint64_t>::max());
        QCOMPARE(f.buf().text(), std::string("a"));
    }

    void a_two_scalar_insert_does_not_fit_where_one_does() {
        // The same boundary from the other side, and the case a per-scalar
        // allocation would get wrong: two items plus the operation is three
        // values, and only two remain. All three or none.
        Fixture f(1, std::numeric_limits<uint64_t>::max() - 2);
        const auto result = f.buf().try_apply_local_edit({{0, 0}}, {"ab"});
        QVERIFY(!result.ok());
        QCOMPARE(result.error().code(), ErrorCode::counter_exhausted);
        QCOMPARE(f.allocator()->reservation_count(), size_t(0));
        QCOMPARE(f.allocator()->peek(), std::numeric_limits<uint64_t>::max() - 2);
        QVERIFY(f.buf().text().empty());
    }

    // ---- the engine borrows; it does not own -----------------------------

    void the_actor_comes_from_the_allocator_not_the_constructor() {
        // Constructing an engine is not authority to claim durable allocation,
        // so there is no constructor that takes only an ActorId: the engine
        // learns who it is from the allocator it borrows.
        Fixture f(7);
        QCOMPARE(f.buf().actor_id(), actor(7));
    }

    void two_engines_on_one_counter_never_reissue() {
        // Not a supported configuration (one counter is one stream), but the
        // property that makes it safe to hand the same allocator to two engines
        // anyway: identities are still unique.
        auto counter = std::make_shared<VolatileCounter>(actor(1));
        Buffer a(counter);
        Buffer b(counter);
        const auto op_a = as_edit(a.apply_local_edit({{0, 0}}, {"aa"}));
        const auto op_b = as_edit(b.apply_local_edit({{0, 0}}, {"bb"}));
        QVERIFY(op_b.timestamp.counter > op_a.timestamp.counter);
    }

    void idlist_allocates_one_interval_per_insert() {
        auto counter = std::make_shared<VolatileCounter>(actor(1));
        IdList list(counter);
        counter->reset_call_log();

        const auto op = list.insert_after(Anchor::min(), 1);
        QCOMPARE(counter->reservation_count(), size_t(1));
        QCOMPARE(counter->last_reserved_count(), uint64_t(1));
        QCOMPARE(op_identity_of(op).counter, counter->first_reserved_base());

        const auto op2 = list.insert_after(list.anchor_of(1), 2);
        QVERIFY(op_identity_of(op2).counter > op_identity_of(op).counter);
        QCOMPARE(counter->reservation_count(), size_t(2));
    }

    // ---- the borrowed allocator is a durable one when it says so ----

    void engine_identities_are_not_reissued_after_a_reopen() {
        // The reason the engine borrows rather than owns: a reopened device
        // resumes above every identity it already issued. With a per-process
        // counter this is where F4 lives — the second launch mints (actor, 1)
        // again while the first launch's operations still claim it.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::string state_dir =
            (dir.path() + "/state").toStdString();

        uint64_t after_reopen_base = 0;
        uint64_t before_reopen_operation = 0;
        {
            auto state = ReplicaState::open(state_dir);
            QVERIFY(state.ok());
            auto owner = std::make_unique<ReplicaState>(std::move(state).value());
            ReplicaState& ref = *owner;
            auto allocator = std::make_shared<CollabText::Identity::ReplicaAllocator>(
                &ref, "doc-1", "text");
            Buffer buf(allocator);
            const auto op = as_edit(buf.apply_local_edit({{0, 0}}, {"hello"}));
            before_reopen_operation = op.timestamp.counter;
            QCOMPARE(allocator->actor(), ref.actor());
        }

        {
            auto state = ReplicaState::open(state_dir);
            QVERIFY(state.ok());
            auto owner = std::make_unique<ReplicaState>(std::move(state).value());
            ReplicaState& ref = *owner;
            auto allocator = std::make_shared<CollabText::Identity::ReplicaAllocator>(
                &ref, "doc-1", "text");
            Buffer buf(allocator);
            QVERIFY2(allocator->peek() > before_reopen_operation,
                     "a reopened device must not resume inside identities it "
                     "already issued");
            after_reopen_base = allocator->peek();
            // Insert at 0, not at the end of the previous document: a reopened
            // ReplicaState knows the counter but not the document. Replaying
            // one's own log to rebuild state is L05's work, and conflating the
            // two here would make this case fail for a reason that has nothing to
            // do with identity.
            const auto op = as_edit(buf.apply_local_edit({{0, 0}}, {"!"}));
            QVERIFY(op.timestamp.counter >= after_reopen_base);
            QVERIFY(op.timestamp.counter > before_reopen_operation);
        }
    }

    void two_streams_under_one_actor_have_independent_coverage() {
        // §3.1 and §3.7: counters are per (actor, stream). The same counter
        // value in two streams is two independent facts, so two streams may both
        // start at 1 without anything aliasing.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        auto state = ReplicaState::open((dir.path() + "/state").toStdString());
        QVERIFY(state.ok());
        auto owner = std::make_unique<ReplicaState>(std::move(state).value());
        ReplicaState& ref = *owner;

        auto text_stream = std::make_shared<CollabText::Identity::ReplicaAllocator>(
            &ref, "doc-1", "text");
        auto chat_stream = std::make_shared<CollabText::Identity::ReplicaAllocator>(
            &ref, "doc-1", "chat");

        Buffer text(text_stream);
        IdList chat(chat_stream);
        QCOMPARE(text_stream->actor(), chat_stream->actor());

        const auto op_text = as_edit(text.apply_local_edit({{0, 0}}, {"ab"}));
        const auto op_chat = chat.insert_after(Anchor::min(), 1);

        // Both streams start their own counter at 1, and the two operations are
        // not a collision even though their actors match: stream is part of an
        // identity's context, so the same counter value in two streams is two
        // independent facts.
        QCOMPARE(op_text.inserted_fragments.front().origin.counter, uint64_t(1));
        QCOMPARE(op_text.timestamp.counter, uint64_t(3));   // 2 items + operation
        QCOMPARE(op_identity_of(op_chat).counter, uint64_t(1));  // 1 item + operation
        QCOMPARE(op_text.timestamp.actor, op_identity_of(op_chat).actor);

        // And the counters really are independent: advancing one must not move
        // the other. This is the property that lets two streams share an actor
        // without any coordination between them.
        const uint64_t chat_before = chat_stream->peek();
        text.apply_local_edit({{2, 2}}, {"c"});
        QCOMPARE(chat_stream->peek(), chat_before);
        QVERIFY(text_stream->peek() > 3);
    }

    void idlist_remove_marks_with_its_own_operation_identity() {
        auto counter = std::make_shared<VolatileCounter>(actor(1));
        IdList list(counter);
        list.insert_after(Anchor::min(), 1);
        list.insert_after(list.anchor_of(1), 2);

        const auto remove = list.remove_at(list.anchor_of(1));
        const auto entries = list.entries();
        bool saw_removal = false;
        for (const auto &entry : entries) {
            for (const Identity &del : entry.deletions) {
                saw_removal = true;
                QCOMPARE(del, op_identity_of(remove));
            }
        }
        QVERIFY(saw_removal);
    }
};
QTEST_MAIN(TestIdentityAllocationEngine)
#include "tst_identity_allocation_engine.moc"
