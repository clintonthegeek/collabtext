/// tst_identity_allocation_model.cpp — P06a design gate, second half: prove the
/// identity *allocation* contract from docs/specs/vnext-identity.md §3.1–§3.7
/// against an independent model.
///
/// ─── Why the comparator gate was not enough ──────────────────────────────────
///
/// `tst_identity_ordering_model` proves the order's laws for identities it is
/// handed. It says nothing about who hands them out, and that is where the legacy
/// engine is genuinely at risk: `Buffer` ticks one Identity value per inserted
/// character (`Buffer.cpp:864`) and derives split origins as `origin + j`
/// (`Buffer.cpp:413`), while P02's `next_operation()` reserves exactly one counter
/// per operation. Substituting one-operation-one-counter for the character clock
/// is the obvious migration and it silently reinterprets every concurrent
/// passage, because equal locators tie-break on precisely this order.
///
/// So the questions this file answers are the ones the review said were open:
/// does a run reservation cover a whole insert, can two operations collide, does
/// a split allocate, what does a deletion or an undo entry get called, what does
/// a crash between reservation and log append leave behind, and what happens at
/// the end of the counter's range.
///
/// ─── Independence ────────────────────────────────────────────────────────────
///
/// The model (tests/reference/IdentityAllocationModel.h) implements §3.2–§3.7
/// from the specification text with its own counter, its own overflow-safe
/// bound, its own actor comparison, and its own result vocabulary. It never
/// calls `CheckedCounter`, `ReplicaState`, or an engine type. Exactly one pair of
/// tests compares the model's answers against production
/// `CheckedCounter::reserve_run` — that comparison is where two independent
/// implementations meet, which is the only way it is evidence rather than
/// tautology.
///
/// Escaped string literals throughout: Qt's moc silently drops every test slot
/// in a translation unit containing a raw string literal with braces (P04).

#include <QTest>

#include <collabtext/ActorId.h>

#include "reference/IdentityAllocationModel.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

using CollabText::CheckedCounter;
using CollabText::ErrorCode;
using namespace collabtext_test::allocation_model;

namespace {

constexpr uint64_t kExhausted = std::numeric_limits<uint64_t>::max();

/// The production error code the contract requires for a model reason.
ErrorCode expected_code(Reason reason) {
    switch (reason) {
        case Reason::empty_run:
        case Reason::counter_zero: return ErrorCode::invalid_identity;
        case Reason::exhausted:    return ErrorCode::counter_exhausted;
        case Reason::regression:   return ErrorCode::counter_regression;
        case Reason::ok:           break;
    }
    return ErrorCode::malformed;  // never compared: ok is not a failure
}

/// Every identity a scenario issued, so "no two are equal" is checked against the
/// whole set rather than pairwise as it is built.
struct IssuedSet {
    std::vector<ModelIdentity> items;
    std::vector<ModelIdentity> operations;

    void add(const ModelAllocation& alloc) {
        for (uint64_t i = 0; i < alloc.item_count; ++i)
            items.push_back(
                ModelIdentity{alloc.operation.actor, alloc.item_base + i});
        operations.push_back(alloc.operation);
    }

    [[nodiscard]] std::vector<ModelIdentity> all() const {
        std::vector<ModelIdentity> out = items;
        out.insert(out.end(), operations.begin(), operations.end());
        return out;
    }
};

/// Note: this returns from the helper on failure, not from the test slot. Every
/// caller asserts something afterwards that depends on distinctness, so a
/// failure here does not read as a passing scenario.
void assert_all_distinct(const std::vector<ModelIdentity>& identities) {
    auto sorted = identities;
    std::sort(sorted.begin(), sorted.end());
    QVERIFY2(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end(),
             "two identities compared equal");
}

}  // namespace

class TestIdentityAllocationModel : public QObject {
    Q_OBJECT
private slots:

    // =======================================================================
    // §3.5 exhaustion, at each boundary in the specification's table
    // =======================================================================

    void exhaustion_boundaries_follow_the_specification() {
        // One below the sentinel: the last value any reservation can issue.
        ModelCounter last(kExhausted - 1);
        QCOMPARE(last.reserve_run(1).value, kExhausted - 1);
        QVERIFY(last.exhausted());
        const auto after_last = last.reserve_run(1);
        QVERIFY(!after_last.ok);
        QCOMPARE(after_last.reason, Reason::exhausted);

        // A run of two does not fit, and issues nothing.
        ModelCounter nearly(kExhausted - 1);
        const auto refused = nearly.reserve_run(2);
        QVERIFY(!refused.ok);
        QCOMPARE(refused.reason, Reason::exhausted);
        QCOMPARE(nearly.peek(), kExhausted - 1);  // all-or-nothing

        // The sentinel itself is never issued, for any length.
        ModelCounter sentinel(kExhausted);
        QVERIFY(!sentinel.reserve_run(1).ok);
        QVERIFY(!sentinel.reserve_run(1024).ok);
        QCOMPARE(sentinel.peek(), kExhausted);

        // A run of zero is not an operation: every operation consumes its own
        // identity, so an empty reservation is a caller error.
        ModelCounter fresh(1);
        const auto empty = fresh.reserve_run(0);
        QVERIFY(!empty.ok);
        QCOMPARE(empty.reason, Reason::empty_run);
        QCOMPARE(fresh.peek(), 1);

        // Counter zero is the invalid-identity sentinel and is never issued.
        ModelCounter zeroed(0);
        const auto from_zero = zeroed.reserve_run(1);
        QVERIFY(!from_zero.ok);
        QCOMPARE(from_zero.reason, Reason::counter_zero);
    }

    void the_bound_is_checked_without_wrapping() {
        // A run ending exactly at the last value must be issued: `next + count`
        // overflows here, which is why the bound is tested by subtraction.
        ModelCounter near_top(kExhausted - 10);
        const auto fits = near_top.reserve_run(10);
        QVERIFY2(fits.ok, "a run ending exactly at the last value must be issued");
        QCOMPARE(fits.value, kExhausted - 10);
        QVERIFY(near_top.exhausted());

        ModelCounter just_over(kExhausted - 10);
        const auto over = just_over.reserve_run(11);
        QVERIFY(!over.ok);
        QCOMPARE(over.reason, Reason::exhausted);
        QCOMPARE(just_over.peek(), kExhausted - 10);

        // And a length whose sum would wrap past UINT64_MAX entirely.
        ModelCounter wrapping(1);
        const auto huge = wrapping.reserve_run(kExhausted);
        QVERIFY(!huge.ok);
        QCOMPARE(huge.reason, Reason::exhausted);
        QCOMPARE(wrapping.peek(), 1);
    }

    // =======================================================================
    // The one place the model and production meet
    // =======================================================================

    void production_reserve_run_agrees_with_the_model() {
        struct Case {
            uint64_t start;
            uint64_t count;
        };
        const Case cases[] = {
            {1, 1},                    {1, 6},
            {1, 1024},                 {41, 41},
            {41, 40},                  {100, 1},
            {kExhausted - 2, 1},       {kExhausted - 2, 2},
            {kExhausted - 2, 3},       {kExhausted - 1, 1},
            {kExhausted - 1, 2},       {kExhausted, 1},
            {kExhausted, 4096},        {0, 1},
            {0, 5},                    {7, 0},
        };

        for (const Case& c : cases) {
            ModelCounter model(c.start);
            CheckedCounter production(c.start);

            const auto mine = model.reserve_run(c.count);
            const auto theirs = production.reserve_run(c.count, "model comparison");

            // The outcome must agree, not merely both look plausible.
            QCOMPARE(mine.ok, theirs.ok());
            if (mine.ok) {
                QCOMPARE(mine.value, theirs.value());
                QCOMPARE(model.peek(), production.peek());
            } else {
                QCOMPARE(theirs.error().code(), expected_code(mine.reason));
                QCOMPARE(model.peek(), c.start);  // refused runs change nothing
            }
        }
    }

    void production_and_model_agree_under_randomised_runs() {
        // Seeded, not `std::random_device`: V04's contract makes an ordinary run a
        // reproducible sample, and this failure mode must be replayable.
        std::mt19937_64 rng(20261004);
        for (int round = 0; round < 400; ++round) {
            // Starts cluster near the top so exhaustion is actually reached, and
            // counts are wide enough that the subtraction check is what decides.
            const uint64_t start =
                (round % 2 == 0) ? kExhausted - (rng() % 40) : 1 + (rng() % 1000);
            const uint64_t count = 1 + (rng() % 60);

            ModelCounter model(start);
            CheckedCounter production(start);
            const auto mine = model.reserve_run(count);
            const auto theirs = production.reserve_run(count, "model comparison");

            QCOMPARE(mine.ok, theirs.ok());
            if (mine.ok) {
                QCOMPARE(mine.value, theirs.value());
                QCOMPARE(production.peek(), model.peek());
            } else {
                QCOMPARE(theirs.error().code(), expected_code(mine.reason));
                QCOMPARE(production.peek(), start);
            }
        }
    }

    // =======================================================================
    // §3.2 non-aliasing: the defect the interval layout exists to remove
    // =======================================================================

    void operation_identity_never_aliases_its_own_items() {
        // The legacy engine sets `op.timestamp = m_clock.value - 1`
        // (Buffer.cpp:900) — the *last* item identity of its own run — so a
        // one-character insert and its single character share an identity today.
        // Under §3.2 the operation sits strictly above every item it created.
        ModelAllocator allocator("doc", "text", ModelActor::from_seed(1));
        const auto single = allocator.allocate_operation(1);
        QVERIFY(single.ok);

        QCOMPARE(single.value.item_base, 1u);
        QCOMPARE(single.value.operation_counter(), 2u);

        const ModelIdentity item{single.value.operation.actor, 1};
        QVERIFY2(item != single.value.operation, "operation aliased its own item");
        QVERIFY(item < single.value.operation);

        // Same for a multi-scalar run: the operation is above all of them, and
        // its own counter is outside the item range it owns.
        const auto run = allocator.allocate_operation(5);
        QVERIFY(run.ok);
        for (uint64_t i = 0; i < 5; ++i) {
            const ModelIdentity id{run.value.operation.actor, run.value.item_base + i};
            QVERIFY(id < run.value.operation);
            QVERIFY(run.value.item_in_range(id.counter));
        }
        QVERIFY(!run.value.item_in_range(run.value.operation_counter()));
    }

    void intervals_are_disjoint_across_many_operations() {
        // Enough varied shapes that a run-length mistake would collide:
        // deletions (0 items), single characters, and long runs, one actor.
        ModelAllocator allocator("doc", "text", ModelActor::from_seed(7));
        IssuedSet issued;
        uint64_t previous_base = 0;

        for (int i = 0; i < 120; ++i) {
            const uint64_t items =
                (i % 7 == 0) ? 0 : 1 + static_cast<uint64_t>(i % 23);
            const auto alloc = allocator.allocate_operation(items);
            QVERIFY2(alloc.ok, "allocation refused well below exhaustion");
            if (previous_base == 0)
                QCOMPARE(alloc.value.item_base, 1u);
            else
                QVERIFY(alloc.value.item_base > previous_base);
            previous_base = alloc.value.item_base;
            issued.add(alloc.value);
        }

        // Every identity ever issued, items and operations together, is distinct:
        // intervals are half-open and monotonic, so nothing can overlap.
        assert_all_distinct(issued.all());

        // And every item identity lies in exactly one interval.
        for (const ModelAllocation& alloc : allocator.allocations()) {
            for (uint64_t i = 0; i < alloc.item_count; ++i) {
                const uint64_t counter = alloc.item_base + i;
                int covering = 0;
                for (const ModelAllocation& other : allocator.allocations())
                    if (other.item_in_range(counter)) ++covering;
                QCOMPARE(covering, 1);
            }
        }
    }

    void two_actors_never_collide_even_with_identical_counters() {
        ModelAllocator a("doc", "text", ModelActor::from_seed(3));
        ModelAllocator b("doc", "text", ModelActor::from_seed(4));
        IssuedSet issued;
        for (int i = 0; i < 40; ++i) {
            issued.add(a.allocate_operation(3).value);
            issued.add(b.allocate_operation(3).value);
        }
        // Identical counters, different actors: equal counters must still be
        // distinguishable, which is what the actor half of the order is for.
        QCOMPARE(a.high_water(), b.high_water());
        assert_all_distinct(issued.all());
    }

    // =======================================================================
    // §3.3 derived identities: split, delete, undo
    // =======================================================================

    void splitting_derives_identities_without_allocating() {
        ModelAllocator allocator("doc", "text", ModelActor::from_seed(5));
        const auto alloc = allocator.allocate_operation(5);
        QVERIFY(alloc.ok);
        const uint64_t high_water = allocator.high_water();

        const ModelRun run{alloc.value, 0, 5};
        QCOMPARE(run.item(0).counter, alloc.value.item_base);

        const ModelRun tail = split_run(run, 2);
        QCOMPARE(tail.offset, 2u);
        QCOMPARE(tail.length, 3u);

        // Derived, not allocated: the second half's identities are the owner's
        // positions, and the counter did not move.
        QCOMPARE(allocator.high_water(), high_water);
        for (uint64_t i = 0; i < tail.length; ++i) {
            QCOMPARE(tail.item(i).counter, alloc.value.item_base + 2 + i);
            QVERIFY(tail.holds(tail.item(i)));
        }

        // Repeated splitting stays inside the owning interval.
        ModelRun piece = tail;
        for (uint64_t at = 0; at <= piece.length; ++at) {
            piece = split_run(piece, at);
            for (uint64_t i = 0; i < piece.length; ++i)
                QVERIFY(alloc.value.item_in_range(piece.item(i).counter));
        }

        // A split at either extreme is the identity, not a special case.
        QCOMPARE(split_run(run, 0).offset, 0u);
        QCOMPARE(split_run(run, 5).length, 0u);
    }

    void deletion_is_named_by_the_deleting_operation() {
        ModelAllocator allocator("doc", "text", ModelActor::from_seed(6));
        const auto insert = allocator.allocate_operation(4);
        const auto remove = allocator.allocate_operation(0);  // deletion only
        QVERIFY(insert.ok);
        QVERIFY(remove.ok);

        // No extra tick: legacy reserved a deletion_ts of its own
        // (Buffer.cpp:586) because the operation's own identity could not serve
        // twice. Here it can, so a deletion-only operation still consumes exactly
        // one value, and its base is its own identity.
        QCOMPARE(remove.value.item_count, 0u);
        QCOMPARE(remove.value.item_base, remove.value.operation_counter());
        QCOMPARE(allocator.high_water(), insert.value.operation_counter() + 2);

        const ModelIdentity deletion = deletion_identity(remove.value);
        QCOMPARE(deletion, remove.value.operation);
        for (uint64_t i = 0; i < insert.value.item_count; ++i) {
            const ModelIdentity inserted{insert.value.operation.actor,
                                         insert.value.item_base + i};
            QVERIFY(deletion != inserted);
        }
    }

    void undo_entries_key_items_by_item_identity_and_undos_by_operation() {
        ModelAllocator allocator("doc", "text", ModelActor::from_seed(8));
        const auto insert = allocator.allocate_operation(3);
        QVERIFY(insert.ok);

        const ModelIdentity item{insert.value.operation.actor,
                                 insert.value.item_base};
        std::vector<ModelUndoEntry> undo_map;

        // The first undo hides the item, the redo brings it back, and the second
        // undo hides it again: the parity rule the engine's own undo map uses
        // (`m_undo_map.insert({edit_id, undo_id}, current + 1)`).
        const auto first_undo = allocator.allocate_operation(0);
        QVERIFY(first_undo.ok);
        undo_map.push_back(undo_entry(item, first_undo.value, 0));
        QVERIFY(is_undone(undo_map, item));

        const auto redo = allocator.allocate_operation(0);
        QVERIFY(redo.ok);
        undo_map.push_back(undo_entry(item, redo.value, 1));
        QVERIFY(!is_undone(undo_map, item));

        const auto second_undo = allocator.allocate_operation(0);
        QVERIFY(second_undo.ok);
        undo_map.push_back(undo_entry(item, second_undo.value, 2));
        QVERIFY(is_undone(undo_map, item));

        // The two key fields come from different domains, so no entry can be
        // mistaken for the operation that created the characters it hides.
        for (const ModelUndoEntry& e : undo_map) {
            QVERIFY(insert.value.item_in_range(e.edit_id.counter));
            QVERIFY(e.edit_id != e.undo_id);
        }
    }

    // =======================================================================
    // The whole lifecycle in one fixture: §3.2 + §3.3 + §3.4 + §3.6
    // =======================================================================

    void multi_scalar_insert_split_delete_undo_redo_restart() {
        const ModelActor local_actor = ModelActor::from_seed(11);
        const ModelActor remote_actor = ModelActor::from_seed(22);

        ModelAllocator local("doc", "text", local_actor);
        ModelAllocator remote("doc", "text", remote_actor);

        IssuedSet issued;

        // Insert five scalars.
        const auto insert = local.allocate_operation(5);
        QVERIFY(insert.ok);
        issued.add(insert.value);
        local.mark_published(0);

        // A concurrent remote insert of three, applied here.
        const auto remote_insert = remote.allocate_operation(3);
        QVERIFY(remote_insert.ok);
        issued.add(remote_insert.value);
        remote.mark_published(0);
        QVERIFY(local.observe_remote(remote_insert.value.operation).ok);

        // The next local identities sort after everything that operation used.
        // That is the property the legacy clock's `observe` provided
        // (Buffer.cpp:1297-1305), and the reason the causal raise exists at all:
        // observing the operation's identity alone must cover its whole run.
        const auto after_remote = local.allocate_operation(2);
        QVERIFY(after_remote.ok);
        issued.add(after_remote.value);
        QVERIFY(after_remote.value.item_base > remote_insert.value.operation_counter());
        for (uint64_t i = 0; i < remote_insert.value.item_count; ++i) {
            const ModelIdentity remote_item{remote_actor,
                                            remote_insert.value.item_base + i};
            QVERIFY(remote_item < after_remote.value.operation);
        }

        // Split the local five-scalar run, then split the tail again.
        const ModelRun head{insert.value, 0, 2};
        const ModelRun tail = split_run(ModelRun{insert.value, 2, 3}, 1);
        QCOMPARE(head.length, 2u);
        QCOMPARE(tail.offset, 3u);
        QCOMPARE(tail.item(0).counter, insert.value.item_base + 3);

        // Delete: one operation, no extra identity of its own.
        const auto remove = local.allocate_operation(0);
        QVERIFY(remove.ok);
        issued.add(remove.value);
        QCOMPARE(deletion_identity(remove.value), remove.value.operation);

        // Undo then redo, each an operation from the same counter space.
        std::vector<ModelUndoEntry> undo_map;
        const auto undo = local.allocate_operation(0);
        QVERIFY(undo.ok);
        issued.add(undo.value);
        undo_map.push_back(undo_entry(tail.item(0), undo.value, 0));
        QVERIFY(is_undone(undo_map, tail.item(0)));

        const auto redo = local.allocate_operation(0);
        QVERIFY(redo.ok);
        issued.add(redo.value);
        undo_map.push_back(undo_entry(tail.item(0), redo.value, 1));
        QVERIFY(!is_undone(undo_map, tail.item(0)));

        // Through all of that, no two identities ever compared equal.
        assert_all_distinct(issued.all());

        // Restart: the same actor resumes at its persisted high-water mark and
        // never reissues. Reopening is a reopen, not a fresh start at 1.
        const uint64_t before = local.high_water();
        ModelAllocator reopened = local.reopen();
        QCOMPARE(reopened.high_water(), before);
        QCOMPARE(reopened.actor().bytes, local_actor.bytes);

        const auto after_restart = reopened.allocate_operation(4);
        QVERIFY(after_restart.ok);
        QVERIFY(after_restart.value.item_base >= before);
        issued.add(after_restart.value);
        assert_all_distinct(issued.all());

        // A restore that tries to move backwards is refused, so a truncated or
        // rewritten state file cannot reissue identifiers.
        ModelCounter counter(before);
        QVERIFY(!counter.restore(1).ok);
        QCOMPARE(counter.peek(), before);
    }

    // =======================================================================
    // §3.7 one actor, many documents and streams
    // =======================================================================

    void two_documents_and_two_streams_have_independent_counters() {
        const ModelActor actor = ModelActor::from_seed(13);
        ModelAllocator text_a("doc-a", "text", actor);
        ModelAllocator text_b("doc-b", "text", actor);
        ModelAllocator comments("doc-a", "comments", actor);

        const auto a = text_a.allocate_operation(2);
        const auto b = text_b.allocate_operation(2);
        const auto c = comments.allocate_operation(2);
        QVERIFY(a.ok);
        QVERIFY(b.ok);
        QVERIFY(c.ok);

        // The same counter value in two streams is two independent facts, not a
        // collision and not coverage of one another.
        QCOMPARE(a.value.item_base, b.value.item_base);
        QCOMPARE(b.value.item_base, c.value.item_base);
        QCOMPARE(text_a.high_water(), 4u);  // two items plus the operation
        QCOMPARE(comments.high_water(), 4u);

        // Frontiers are per (stream, actor): one stream's progress never moves
        // another's, and neither covers the other.
        ModelFrontier frontier;
        frontier.receive(ModelRecord{actor, "text", 1, 3, false});
        QCOMPARE(frontier.frontier("text", actor), 2u);
        QCOMPARE(frontier.frontier("comments", actor), 0u);

        // Same actor, same stream, different document: still separate contexts.
        QVERIFY(text_a.document() != text_b.document());
    }

    // =======================================================================
    // §3.6 abandoned reservations and contiguous coverage
    // =======================================================================

    void an_unpublished_reservation_becomes_an_explicit_gap() {
        const ModelActor actor = ModelActor::from_seed(17);
        ModelAllocator allocator("doc", "text", actor);

        const auto first = allocator.allocate_operation(2);
        QVERIFY(first.ok);
        allocator.mark_published(0);

        // Reserved, then the process dies before the log append.
        const auto abandoned = allocator.allocate_operation(3);
        QVERIFY(abandoned.ok);

        const auto third = allocator.allocate_operation(1);
        QVERIFY(third.ok);
        allocator.mark_published(2);

        QCOMPARE(allocator.unpublished().size(), 1u);

        // Reopen sees the same high-water mark, so nothing is reissued.
        ModelAllocator reopened = allocator.reopen();
        QCOMPARE(reopened.high_water(), allocator.high_water());
        const auto after = reopened.allocate_operation(1);
        QVERIFY(after.ok);
        QVERIFY(after.value.item_base > abandoned.value.operation_counter());

        // Recovery turns the abandoned interval into a gap record rather than
        // leaving a hole no frontier can cross.
        const std::vector<ModelRecord> gaps = gap_records_for(allocator);
        QCOMPARE(gaps.size(), 1u);
        QVERIFY(gaps.front().is_gap);
        QCOMPARE(gaps.front().lo, abandoned.value.item_base);
        QCOMPARE(gaps.front().hi, abandoned.value.operation_counter() + 1);

        ModelFrontier frontier;
        frontier.receive(ModelRecord{actor, "text", first.value.item_base,
                                     first.value.operation_counter() + 1, false});
        frontier.receive(ModelRecord{actor, "text", third.value.item_base,
                                     third.value.operation_counter() + 1, false});
        // The operations on both sides of the hole are known and the frontier is
        // still below them: an unexplained gap blocks advancement.
        QCOMPARE(frontier.frontier("text", actor), first.value.operation_counter());

        frontier.receive(gaps.front());
        QVERIFY(frontier.frontier("text", actor) >= third.value.operation_counter());
    }

    void maximum_observed_counter_is_not_contiguity() {
        const ModelActor actor = ModelActor::from_seed(19);
        ModelFrontier frontier;

        frontier.receive(ModelRecord{actor, "text", 1, 41, false});
        QCOMPARE(frontier.frontier("text", actor), 40u);

        frontier.receive(ModelRecord{actor, "text", 41, 42, false});
        QCOMPARE(frontier.frontier("text", actor), 41u);

        // Counter 43 arrives while 42 is still missing. The frontier must not move
        // to 43, however high the maximum observed counter goes.
        frontier.receive(ModelRecord{actor, "text", 43, 44, false});
        QCOMPARE(frontier.frontier("text", actor), 41u);

        // Filling the hole advances it across all three.
        frontier.receive(ModelRecord{actor, "text", 42, 43, false});
        QCOMPARE(frontier.frontier("text", actor), 43u);

        // A replica that has never seen counter 1 has no frontier at all, however
        // far the records it holds reach: coverage is a prefix, not a maximum. This
        // is the case a mutation that drops the "nothing from the beginning"
        // requirement would silently pass without.
        ModelFrontier late_joiner;
        QCOMPARE(late_joiner.frontier("text", actor), 0u);
        late_joiner.receive(ModelRecord{actor, "text", 41, 44, false});
        QCOMPARE(late_joiner.frontier("text", actor), 0u);

        // An explicit gap record serves the same purpose, and is equally not
        // skipped: the frontier stays put until the hole is accounted for.
        ModelFrontier with_gap;
        with_gap.receive(ModelRecord{actor, "text", 1, 2, false});
        QCOMPARE(with_gap.frontier("text", actor), 1u);
        with_gap.receive(ModelRecord{actor, "text", 5, 6, true});
        QCOMPARE(with_gap.frontier("text", actor), 1u);
        with_gap.receive(ModelRecord{actor, "text", 2, 5, true});
        QCOMPARE(with_gap.frontier("text", actor), 5u);
    }
};

QTEST_MAIN(TestIdentityAllocationModel)
#include "tst_identity_allocation_model.moc"