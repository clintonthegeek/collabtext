/// tst_operation_validation.cpp — P05: semantic operation validation happens
/// before any engine mutation.
///
/// The evaluation's finding F10 is that the engine accepted operations it
/// should have refused. Every test here is written so that the *old* behaviour
/// is observably wrong, not merely undocumented:
///
///   * a malformed operation used to be applied anyway, so the document ended
///     up holding fragments whose declared length bore no relation to their
///     bytes;
///   * `assert` was the only thing standing between a bad local edit or a bad
///     anchor and undefined behaviour, so every one of these cases was a crash
///     in Debug and silent corruption in Release;
///   * `apply_ops` returned `void`, so a caller could not tell "applied" from
///     "waiting for a dependency" from "will never apply" — the difference
///     between converging and quietly losing work.
///
/// Note: this file avoids raw string literals. Qt's moc silently drops every
/// test slot in a translation unit containing an R"(...)" literal with braces
/// (recorded as a P04 limitation), so byte-level payloads use escaped literals.

#include <QTest>

#include "crdt/Buffer.h"
#include "crdt/IdList.h"
#include "crdt/OperationValidation.h"

#include <collabtext/ApplyOutcome.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>
#include "TestActor.h"

using CollabText::Test::actor;

using namespace CollabText;
using namespace CollabText::Crdt;

namespace {

/// Snapshot of everything acceptance requires an invalid operation to leave
/// untouched: content, version, and undo depth.
struct BufferState {
    std::string text;
    Global version;
    size_t undo_depth;
    size_t fragment_count;
    size_t tombstone_count;

    bool operator==(const BufferState& other) const {
        return text == other.text && version == other.version &&
               undo_depth == other.undo_depth &&
               fragment_count == other.fragment_count &&
               tombstone_count == other.tombstone_count;
    }
};

BufferState snapshot(const Buffer& buf) {
    return BufferState{buf.text(), buf.version(), buf.undo_depth(),
                       buf.fragment_count(), buf.tombstone_count()};
}

/// A real, valid single-insert operation produced by `src`.
Operation valid_edit_op(Buffer& src, const std::string& text = "hello") {
    return src.apply_local_edit({{0, 0}}, {text});
}

/// The EditOperation inside a Buffer operation, failing the test if it is not
/// an edit.
EditOperation as_edit(const Operation& op) {
    auto* edit = std::get_if<EditOperation>(&op);
    Q_ASSERT(edit);
    return *edit;
}

EditOperation valid_edit_payload(uint16_t replica = 7, uint32_t counter = 3) {
    EditOperation op;
    op.timestamp = Identity(actor(replica), counter);
    op.inserted_fragments.push_back(
        EditOperation::InsertedFragment{Identity(actor(replica), counter),
                                        Locator(static_cast<uint64_t>(counter)),
                                        "hi", 2});
    op.deletion_id = Identity(actor(replica), counter);
    return op;
}

}  // namespace

class TestOperationValidation : public QObject {
    Q_OBJECT
private slots:
    void overflow_retention_is_bounded_and_drainable_for_both_primitives() {
        Buffer source(actor(1)), target(actor(2));
        auto blocked = as_edit(source.apply_local_edit({{0, 0}}, {"payload"}));
        blocked.version.observe(Identity(actor(9), 9));
        target.set_deferred_budget({0, 0});
        QVERIFY(target.set_quarantine_budget({2, 1u << 20}).ok());
        for (int i = 0; i < 20; ++i)
            QCOMPARE(target.apply_ops({Operation{blocked}})[0].kind(), ApplyOutcomeKind::rejected);
        QCOMPARE(target.quarantined().size(), size_t(2));
        QVERIFY(!target.set_quarantine_budget({0, 0}).ok());
        QCOMPARE(target.take_quarantined().size(), size_t(2));
        QVERIFY(target.quarantined().empty());
        QVERIFY(target.set_quarantine_budget({2, 0}).ok());
        QCOMPARE(target.apply_ops({Operation{blocked}})[0].kind(), ApplyOutcomeKind::rejected);
        QVERIFY(target.quarantined().empty());

        IdList list_source(actor(1)), list_target(actor(2));
        auto operation = list_source.insert_after(Anchor::min(), 42);
        std::get<IdListInsertOp>(operation).version.observe(Identity(actor(9), 9));
        list_target.set_deferred_budget({0, 0});
        QVERIFY(list_target.set_quarantine_budget({2, 1u << 20}).ok());
        for (int i = 0; i < 20; ++i)
            QCOMPARE(list_target.apply_ops({operation})[0].kind(), ApplyOutcomeKind::rejected);
        QCOMPARE(list_target.quarantined().size(), size_t(2));
        QCOMPARE(list_target.take_quarantined().size(), size_t(2));
        QVERIFY(list_target.quarantined().empty());
        QVERIFY(list_target.set_quarantine_budget({2, 0}).ok());
        QCOMPARE(list_target.apply_ops({operation})[0].kind(), ApplyOutcomeKind::rejected);
        QVERIFY(list_target.quarantined().empty());
    }

    void quarantine_does_not_bypass_the_default_pending_operation_bound() {
        Buffer source(actor(1)), target(actor(2));
        auto blocked = as_edit(source.apply_local_edit({{0, 0}}, {"a"}));
        blocked.version.observe(Identity(actor(9), 9));
        target.set_deferred_budget({0, 0});
        for (size_t i = 0; i <= DeferredBudget{}.max_operations; ++i) {
            const auto verdict = target.apply_ops({Operation{blocked}})[0];
            QCOMPARE(verdict.kind(), ApplyOutcomeKind::rejected);
        }
        QVERIFY(target.quarantined().size() <= DeferredBudget{}.max_operations);
    }

    void missing_idlist_lookup_has_a_checked_not_found_result() {
        IdList list(actor(1));
        QVERIFY(!list.try_anchor_of(100).ok());
        list.insert_after(Anchor::min(), 100);
        QVERIFY(list.try_anchor_of(100).ok());
        const auto missing = list.try_anchor_of(200);
        QVERIFY(!missing.ok());
        QCOMPARE(missing.error().code(), ErrorCode::unresolvable_anchor);
        QCOMPARE(list.ids(), std::vector<uint64_t>{100});
    }

    void local_utf8_interior_boundaries_are_refused_without_mutation() {
        for (const auto range : {std::pair<uint32_t, uint32_t>{1, 1},
                                 {0, 1}, {1, 2}}) {
            Buffer buffer(actor(1));
            buffer.apply_local_edit({{0, 0}}, {"\xc3\xa9"});
            const auto version = buffer.version();
            const auto depth = buffer.undo_depth();
            const auto result = buffer.try_apply_local_edit({range}, {"X"});
            QVERIFY(!result.ok());
            QCOMPARE(result.error().code(), ErrorCode::invalid_unicode);
            QCOMPARE(buffer.text(), std::string("\xc3\xa9"));
            QVERIFY(buffer.version() == version);
            QCOMPARE(buffer.undo_depth(), depth);
        }
    }

    void valid_scalar_boundaries_in_fragmented_text_remain_editable() {
        const std::string original = "\xc3\xa9\xf0\x9f\x99\x82Z";
        for (const uint32_t offset : {0u, 2u, 6u, 7u}) {
            Buffer buffer(actor(1));
            QVERIFY(buffer.try_apply_local_edit({{0, 0}}, {original}).ok());
            QVERIFY(buffer.try_apply_local_edit({{2, 2}}, {"q"}).ok());
            QVERIFY(buffer.try_apply_local_edit({{2, 3}}, {""}).ok());
            QCOMPARE(buffer.text(), original);
            auto expected = original;
            expected.insert(offset, "X");
            const auto edit = buffer.try_apply_local_edit({{offset, offset}}, {"X"});
            QVERIFY2(edit.ok(), "a scalar/run boundary is a valid insertion point");
            QCOMPARE(buffer.text(), expected);
        }
    }

    void blocked_replica_does_not_bypass_semantic_validation() {
        Buffer source(actor(1)), target(actor(2));
        auto first = source.apply_local_edit({{0, 0}}, {"a"});
        auto blocked = source.apply_local_edit({{1, 1}}, {"b"});
        QCOMPARE(target.apply_ops({blocked})[0].kind(), ApplyOutcomeKind::deferred);
        auto malformed = as_edit(source.apply_local_edit({{2, 2}}, {"c"}));
        malformed.inserted_fragments[0].length = 999;
        QCOMPARE(target.apply_ops({Operation{malformed}})[0].kind(), ApplyOutcomeKind::rejected);
        QCOMPARE(target.deferred_count(), size_t(1));
        target.apply_ops({first});
        QCOMPARE(target.text(), std::string("ab"));
    }

    void pending_byte_estimate_does_not_depend_on_counter_value() {
        Buffer source(actor(1));
        auto early = as_edit(source.apply_local_edit({{0, 0}}, {"payload"}));
        auto later = early;
        later.timestamp.counter += 1000000;
        QCOMPARE(estimate_operation_bytes(Operation{early}),
                 estimate_operation_bytes(Operation{later}));
    }




    // =======================================================================
    // A. Operation identity and counter intervals
    // =======================================================================

    void zero_counter_operation_is_rejected() {
        // Global::observe() drops value 0 and observed() is satisfied by it, so
        // a counter-0 operation can never be deduplicated and never advances
        // coverage. It is not a legal identity.
        auto op = valid_edit_payload(3, 0);
        const auto result = validate_edit_operation(op, Global{});
        QCOMPARE(int(result.verdict), int(ValidationVerdict::rejected));
        QCOMPARE(result.error.code(), ErrorCode::invalid_identity);
    }

    void valid_counter_operation_passes_validation() {
        const auto op = valid_edit_payload(3, 4);
        QCOMPARE(int(validate_edit_operation(op, Global{}).verdict),
                 int(ValidationVerdict::valid));
    }

    void insertion_origin_interval_overflow_is_rejected() {
        auto op = valid_edit_payload();
        // length 2 matches the content's two scalars, so the only thing wrong
        // here is the interval: origin + length leaves the counter width.
        op.inserted_fragments[0].origin =
            Identity(actor(7), std::numeric_limits<uint64_t>::max());
        op.inserted_fragments[0].length = 2;
        const auto result = validate_edit_operation(op, Global{});
        QCOMPARE(int(result.verdict), int(ValidationVerdict::rejected));
        QCOMPARE(result.error.code(), ErrorCode::counter_exhausted);
    }

    void deletion_run_interval_overflow_is_rejected() {
        auto op = valid_edit_payload();
        op.inserted_fragments.clear();
        op.deletion_runs.push_back(EditOperation::DeletionRun{
            Identity(actor(9), std::numeric_limits<uint64_t>::max()), 3});
        const auto result = validate_edit_operation(op, Global{});
        QCOMPARE(int(result.verdict), int(ValidationVerdict::rejected));
        QCOMPARE(result.error.code(), ErrorCode::counter_exhausted);
    }

    void split_relocation_interval_overflow_is_rejected() {
        auto op = valid_edit_payload();
        op.inserted_fragments.clear();
        op.split_relocations.push_back(EditOperation::SplitRelocation{
            Identity(actor(4), std::numeric_limits<uint64_t>::max() - 1), 0, 8,
            Locator(static_cast<uint64_t>(1))});
        const auto result = validate_edit_operation(op, Global{});
        QCOMPARE(int(result.verdict), int(ValidationVerdict::rejected));
        QCOMPARE(result.error.code(), ErrorCode::counter_exhausted);
    }

    // =======================================================================
    // B. Insertion text self-consistency (length versus Unicode scalars)
    // =======================================================================

    void inserted_length_must_equal_utf8_scalar_count() {
        // "hello" is 5 bytes and 5 scalars. Claiming 6 makes the fragment's
        // char count disagree with its bytes, which is what corrupts every
        // downstream index built from Fragment::length.
        Buffer src(actor(2));
        Operation raw = valid_edit_op(src, "hello");
        EditOperation op = as_edit(raw);
        QVERIFY(!op.inserted_fragments.empty());
        op.inserted_fragments[0].length += 1;

        const auto result = validate_edit_operation(op, Global{});
        QCOMPARE(int(result.verdict), int(ValidationVerdict::rejected));
        QCOMPARE(result.error.code(), ErrorCode::malformed);
    }

    void multibyte_inserted_length_must_equal_scalar_count_not_byte_count() {
        // "e" + U+00E9 (2 bytes) + "e" = 4 bytes, 3 scalars. A decoder that
        // used the byte count would accept length=4 here.
        Buffer src(actor(2));
        Operation raw = src.apply_local_edit({{0, 0}}, {std::string("e\xC3\xA9") + "e"});
        EditOperation op = as_edit(raw);
        QVERIFY(!op.inserted_fragments.empty());

        EditOperation wrong = op;
        wrong.inserted_fragments[0].length = 4;  // the byte count
        QCOMPARE(int(validate_edit_operation(wrong, Global{}).verdict),
                 int(ValidationVerdict::rejected));

        EditOperation right = op;
        right.inserted_fragments[0].length = 3;  // the scalar count
        QCOMPARE(int(validate_edit_operation(right, Global{}).verdict),
                 int(ValidationVerdict::valid));
    }

    void non_bmp_inserted_length_is_counted_in_scalars() {
        // U+1F600 is 4 bytes and 1 scalar.
        Buffer src(actor(2));
        Operation raw = src.apply_local_edit({{0, 0}}, {std::string("\xF0\x9F\x98\x80")});
        EditOperation op = as_edit(raw);
        QVERIFY(!op.inserted_fragments.empty());

        EditOperation wrong = op;
        wrong.inserted_fragments[0].length = 4;
        QCOMPARE(int(validate_edit_operation(wrong, Global{}).verdict),
                 int(ValidationVerdict::rejected));

        QCOMPARE(int(validate_edit_operation(op, Global{}).verdict),
                 int(ValidationVerdict::valid));
    }

    void invalid_utf8_in_inserted_content_is_rejected() {
        auto op = valid_edit_payload();
        op.inserted_fragments[0].content = std::string("a\x80z");  // bare continuation
        op.inserted_fragments[0].length = 3;
        const auto result = validate_edit_operation(op, Global{});
        QCOMPARE(int(result.verdict), int(ValidationVerdict::rejected));
        QCOMPARE(result.error.code(), ErrorCode::invalid_unicode);
    }

    void overlong_utf8_in_inserted_content_is_rejected() {
        auto op = valid_edit_payload();
        op.inserted_fragments[0].content = std::string("\xC0\xAF");  // overlong '/'
        op.inserted_fragments[0].length = 1;
        const auto result = validate_edit_operation(op, Global{});
        QCOMPARE(int(result.verdict), int(ValidationVerdict::rejected));
        QCOMPARE(result.error.code(), ErrorCode::invalid_unicode);
    }

    void surrogate_encoded_as_utf8_is_rejected() {
        auto op = valid_edit_payload();
        op.inserted_fragments[0].content = std::string("\xED\xA0\x80");  // U+D800
        op.inserted_fragments[0].length = 1;
        const auto result = validate_edit_operation(op, Global{});
        QCOMPARE(int(result.verdict), int(ValidationVerdict::rejected));
        QCOMPARE(result.error.code(), ErrorCode::invalid_unicode);
    }

    // =======================================================================
    // C. Locator and boundary constraints
    // =======================================================================

    void sentinel_locator_is_not_a_valid_insertion_position() {
        auto op = valid_edit_payload();
        op.inserted_fragments[0].locator = Locator::min();
        QCOMPARE(int(validate_edit_operation(op, Global{}).verdict),
                 int(ValidationVerdict::rejected));

        EditOperation at_max = valid_edit_payload();
        at_max.inserted_fragments[0].locator = Locator::max();
        QCOMPARE(int(validate_edit_operation(at_max, Global{}).verdict),
                 int(ValidationVerdict::rejected));
    }

    void ordinary_locator_is_accepted() {
        const auto op = valid_edit_payload();
        QVERIFY(op.inserted_fragments[0].locator > Locator::min());
        QVERIFY(op.inserted_fragments[0].locator < Locator::max());
        QCOMPARE(int(validate_edit_operation(op, Global{}).verdict),
                 int(ValidationVerdict::valid));
    }

    // =======================================================================
    // D. Edit range / text cardinality and ordering
    // =======================================================================

    void range_and_text_cardinality_must_match() {
        auto op = valid_edit_payload();
        op.ranges.push_back({0, 0});
        op.ranges.push_back({0, 0});
        op.new_text.push_back("x");  // two ranges, one replacement
        const auto result = validate_edit_operation(op, Global{});
        QCOMPARE(int(result.verdict), int(ValidationVerdict::rejected));
        QCOMPARE(result.error.code(), ErrorCode::malformed);
    }

    void inverted_range_is_rejected() {
        auto op = valid_edit_payload();
        op.ranges.push_back({9, 4});  // start after end
        op.new_text.push_back("x");
        const auto result = validate_edit_operation(op, Global{});
        QCOMPARE(int(result.verdict), int(ValidationVerdict::rejected));
        QCOMPARE(result.error.code(), ErrorCode::malformed);
    }

    void overlapping_ranges_are_rejected() {
        auto op = valid_edit_payload();
        op.ranges.push_back({0, 5});
        op.ranges.push_back({3, 8});  // overlaps the first
        op.new_text.push_back("a");
        op.new_text.push_back("b");
        const auto result = validate_edit_operation(op, Global{});
        QCOMPARE(int(result.verdict), int(ValidationVerdict::rejected));
        QCOMPARE(result.error.code(), ErrorCode::malformed);
    }

    void unsorted_ranges_are_rejected() {
        auto op = valid_edit_payload();
        op.ranges.push_back({8, 9});
        op.ranges.push_back({0, 1});
        op.new_text.push_back("a");
        op.new_text.push_back("b");
        const auto result = validate_edit_operation(op, Global{});
        QCOMPARE(int(result.verdict), int(ValidationVerdict::rejected));
        QCOMPARE(result.error.code(), ErrorCode::malformed);
    }

    void adjacent_and_empty_ranges_are_accepted() {
        auto op = valid_edit_payload();
        op.ranges.push_back({0, 3});
        op.ranges.push_back({3, 7});   // adjacent, not overlapping
        op.ranges.push_back({9, 9});   // empty insertion point
        op.new_text = {"a", "b", "c"};
        QCOMPARE(int(validate_edit_operation(op, Global{}).verdict),
                 int(ValidationVerdict::valid));
    }

    // =======================================================================
    // E. Undo operations
    // =======================================================================

    void undo_with_zero_counter_target_is_rejected() {
        UndoOperation op;
        op.timestamp = Identity(actor(2), 4);
        op.counts.push_back({Identity(actor(1), 0), 1});  // target can never be observed
        const auto result = validate_undo_operation(op, Global{});
        QCOMPARE(int(result.verdict), int(ValidationVerdict::rejected));
        QCOMPARE(result.error.code(), ErrorCode::invalid_identity);
    }

    void undo_with_self_referential_target_is_rejected() {
        UndoOperation op;
        op.timestamp = Identity(actor(2), 4);
        op.counts.push_back({Identity(actor(2), 9), 1});  // undoes something newer than itself
        const auto result = validate_undo_operation(op, Global{});
        QCOMPARE(int(result.verdict), int(ValidationVerdict::rejected));
    }

    void well_formed_undo_is_accepted() {
        UndoOperation op;
        op.timestamp = Identity(actor(2), 4);
        op.counts.push_back({Identity(actor(1), 2), 1});
        QCOMPARE(int(validate_undo_operation(op, Global{}).verdict),
                 int(ValidationVerdict::valid));
    }

    // =======================================================================
    // F. Missing dependency is DEFERRED, not REJECTED
    // =======================================================================

    void unmet_causal_dependency_defers_with_the_missing_identity() {
        Global current;
        current.observe(Identity(actor(1), 3));
        auto op = valid_edit_payload(2, 5);
        op.version.observe(Identity(actor(1), 7));  // one past what we have

        const auto result = validate_edit_operation(op, current);
        QCOMPARE(int(result.verdict), int(ValidationVerdict::deferred));
        QVERIFY(result.awaiting.has_value());
        QCOMPARE(*result.awaiting, Identity(actor(1), 7));
        QVERIFY(!result.error);
    }

    void met_causal_dependency_is_not_a_deferral() {
        Global current;
        current.observe(Identity(actor(1), 7));
        auto op = valid_edit_payload(2, 5);
        op.version.observe(Identity(actor(1), 7));
        QCOMPARE(int(validate_edit_operation(op, current).verdict),
                 int(ValidationVerdict::valid));
    }

    void malformed_operation_is_rejected_even_when_dependencies_are_missing() {
        // An operation that can never be valid must not sit in the deferral
        // queue forever waiting for a dependency that will not help.
        Global current;  // nothing observed
        auto op = valid_edit_payload(2, 5);
        op.version.observe(Identity(actor(1), 7));
        op.inserted_fragments[0].length = 99;  // and is malformed

        const auto result = validate_edit_operation(op, current);
        QCOMPARE(int(result.verdict), int(ValidationVerdict::rejected));
    }

    // =======================================================================
    // G. Failure atomicity: a rejected operation mutates nothing
    // =======================================================================

    void rejected_operation_leaves_engine_state_untouched() {
        Buffer src(actor(2));
        Buffer dst(actor(3));
        dst.apply_local_edit({{0, 0}}, {"base"});
        const BufferState before = snapshot(dst);

        Operation raw = valid_edit_op(src, "corrupt");
        EditOperation op = as_edit(raw);
        op.inserted_fragments[0].length += 7;

        const auto outcomes = dst.apply_ops({Operation{op}});
        QCOMPARE(outcomes.size(), std::size_t(1));
        QCOMPARE(outcomes[0].kind(), ApplyOutcomeKind::rejected);
        QCOMPARE(outcomes[0].error()->code(), ErrorCode::malformed);
        QCOMPARE(snapshot(dst), before);
    }

    void each_rejection_reason_leaves_engine_state_untouched_data_driven() {
        // One loop over every rejecting rule, asserting the atomicity invariant
        // each time, so a new rule cannot be added without this covering it.
        // Payloads come from a separate source buffer: the buffer under test
        // must not be mutated by the setup.
        Buffer src(actor(2));
        Buffer dst(actor(3));
        dst.apply_local_edit({{0, 0}}, {"base"});
        const BufferState before = snapshot(dst);

        struct Case {
            const char* name;
            std::function<void(EditOperation&)> corrupt;
        };
        const std::vector<Case> cases = {
            {"zero_counter", [](EditOperation& op) { op.timestamp = Identity(actor(2), 0); }},
            {"bad_utf8", [](EditOperation& op) {
                 op.inserted_fragments[0].content = std::string("\xC0\xAF");
                 op.inserted_fragments[0].length = 1;
             }},
            {"length_mismatch", [](EditOperation& op) {
                 op.inserted_fragments[0].length += 3;
             }},
            {"sentinel_locator", [](EditOperation& op) {
                 op.inserted_fragments[0].locator = Locator::max();
             }},
            {"cardinality", [](EditOperation& op) {
                 op.ranges.push_back({0, 0});
             }},
            {"inverted_range", [](EditOperation& op) {
                 op.ranges.push_back({7, 2});
                 op.new_text.push_back("x");
             }},
            {"overlap", [](EditOperation& op) {
                 op.ranges.push_back({0, 4});
                 op.ranges.push_back({2, 6});
                 op.new_text.push_back("x");
                 op.new_text.push_back("y");
             }},
            {"origin_overflow", [](EditOperation& op) {
                 op.inserted_fragments[0].origin =
                     Identity(actor(2), std::numeric_limits<uint64_t>::max());
                 op.inserted_fragments[0].length = 2;
             }},
            {"run_overflow", [](EditOperation& op) {
                 op.inserted_fragments.clear();
                 op.deletion_runs.push_back(EditOperation::DeletionRun{
                     Identity(actor(9), std::numeric_limits<uint64_t>::max()), 2});
             }},
        };

        for (const auto& c : cases) {
            Operation raw = valid_edit_op(src, "payload");
            EditOperation op = as_edit(raw);
            c.corrupt(op);
            const auto outcomes = dst.apply_ops({Operation{op}});
            QCOMPARE(outcomes.size(), std::size_t(1));
            QVERIFY2(outcomes[0].kind() == ApplyOutcomeKind::rejected, c.name);
            QVERIFY2(outcomes[0].error().has_value(), c.name);
            QVERIFY2(snapshot(dst) == before, c.name);
        }
    }

    // =======================================================================
    // H. Duplicate is distinct from applied, and is idempotent
    // =======================================================================

    void duplicate_operation_reports_duplicate_and_changes_nothing() {
        Buffer src(actor(2));
        Operation op = valid_edit_op(src, "hi");

        Buffer dst(actor(3));
        const auto first = dst.apply_ops({op});
        QCOMPARE(first.size(), std::size_t(1));
        QCOMPARE(first[0].kind(), ApplyOutcomeKind::applied);
        const BufferState after_first = snapshot(dst);

        const auto second = dst.apply_ops({op});
        QCOMPARE(second.size(), std::size_t(1));
        QCOMPARE(second[0].kind(), ApplyOutcomeKind::duplicate);
        QCOMPARE(snapshot(dst), after_first);
    }

    // =======================================================================
    // I. Local edit validation happens before any mutation
    // =======================================================================

    void local_edit_with_mismatched_cardinality_is_refused_without_mutation() {
        // Previously guarded only by assert(): undefined behaviour in Release.
        Buffer buf(actor(1));
        buf.apply_local_edit({{0, 0}}, {"hello"});
        const BufferState before = snapshot(buf);

        const auto result = buf.try_apply_local_edit({{0, 0}, {1, 2}}, {"a"});
        QVERIFY(!result.ok());
        QCOMPARE(result.error().code(), ErrorCode::malformed);
        QCOMPARE(snapshot(buf), before);
    }

    void local_edit_past_end_is_refused_without_mutation() {
        Buffer buf(actor(1));
        buf.apply_local_edit({{0, 0}}, {"hello"});
        const BufferState before = snapshot(buf);

        const auto result = buf.try_apply_local_edit({{99, 99}}, {"x"});
        QVERIFY(!result.ok());
        QCOMPARE(snapshot(buf), before);
    }

    void local_edit_with_end_past_start_is_refused_without_mutation() {
        Buffer buf(actor(1));
        buf.apply_local_edit({{0, 0}}, {"hello"});
        const BufferState before = snapshot(buf);

        const auto result = buf.try_apply_local_edit({{4, 2}}, {"x"});
        QVERIFY(!result.ok());
        QCOMPARE(result.error().code(), ErrorCode::malformed);
        QCOMPARE(snapshot(buf), before);
    }

    void local_edit_with_overlapping_ranges_is_refused_without_mutation() {
        Buffer buf(actor(1));
        buf.apply_local_edit({{0, 0}}, {"hello"});
        const BufferState before = snapshot(buf);

        const auto result = buf.try_apply_local_edit({{0, 3}, {1, 4}}, {"a", "b"});
        QVERIFY(!result.ok());
        QCOMPARE(result.error().code(), ErrorCode::malformed);
        QCOMPARE(snapshot(buf), before);
    }

    void local_edit_at_end_boundary_is_accepted() {
        // The insertion point one past the last byte is legal, so refusing it
        // would be a regression rather than a repair.
        Buffer buf(actor(1));
        buf.apply_local_edit({{0, 0}}, {"hello"});
        const auto result = buf.try_apply_local_edit({{5, 5}}, {"!"});
        QVERIFY(result.ok());
        QCOMPARE(buf.text(), std::string("hello!"));
    }

    void local_edit_with_unsorted_ranges_is_refused() {
        Buffer buf(actor(1));
        buf.apply_local_edit({{0, 0}}, {"abcdef"});
        const BufferState before = snapshot(buf);

        const auto result = buf.try_apply_local_edit({{4, 5}, {1, 2}}, {"x", "y"});
        QVERIFY(!result.ok());
        QCOMPARE(snapshot(buf), before);
    }

    void legacy_local_edit_entry_point_stays_non_UB_on_bad_input() {
        // The old signature returns a plain Operation and cannot report a
        // failure. It must still refuse to mutate and must not read out of
        // bounds, which is what the assert() used to leave to chance.
        Buffer buf(actor(1));
        buf.apply_local_edit({{0, 0}}, {"hello"});
        const BufferState before = snapshot(buf);

        const Operation ignored = buf.apply_local_edit({{0, 0}, {1, 2}}, {"a"});
        Q_UNUSED(ignored);
        QCOMPARE(snapshot(buf), before);
    }

    // =======================================================================
    // J. Deferral, convergence, and the deferred budget
    // =======================================================================

    void out_of_order_valid_operations_still_converge() {
        Buffer alice(actor(1));
        Buffer bob(actor(2));
        Buffer carol(actor(3));

        Operation from_alice = alice.apply_local_edit({{0, 0}}, {"A"});
        bob.apply_ops({from_alice});
        // Bob's edit now causally depends on Alice's, so Carol can only apply
        // it after the operation it waits for.
        Operation from_bob = bob.apply_local_edit({{1, 1}}, {"B"});

        const auto deferred = carol.apply_ops({from_bob});
        QCOMPARE(deferred.size(), std::size_t(1));
        QCOMPARE(deferred[0].kind(), ApplyOutcomeKind::deferred);
        QVERIFY(deferred[0].awaiting().has_value());
        QCOMPARE(carol.text(), std::string(""));

        const auto applied = carol.apply_ops({from_alice});
        QCOMPARE(applied.size(), std::size_t(1));
        QCOMPARE(applied[0].kind(), ApplyOutcomeKind::applied);

        QCOMPARE(carol.text(), bob.text());
        QCOMPARE(carol.text(), std::string("AB"));
    }

    void deferred_operation_does_not_fire_a_change_callback() {
        // A deferred operation is not a visible change. Firing the callback
        // would make observers reconcile against a document that has not moved.
        Buffer src(actor(1));
        Buffer dst(actor(2));
        EditOperation op = as_edit(src.apply_local_edit({{0, 0}}, {"X"}));
        op.version.observe(Identity(actor(9), 9));  // never satisfiable here

        int changes = 0;
        dst.set_on_change([&] { ++changes; });

        const auto outcomes = dst.apply_ops({Operation{op}});
        QCOMPARE(outcomes.size(), std::size_t(1));
        QCOMPARE(outcomes[0].kind(), ApplyOutcomeKind::deferred);
        QCOMPARE(changes, 0);
    }

    void rejected_operation_does_not_fire_a_change_callback() {
        Buffer src(actor(1));
        Buffer dst(actor(2));
        EditOperation op = as_edit(src.apply_local_edit({{0, 0}}, {"X"}));
        op.inserted_fragments[0].length += 5;

        int changes = 0;
        dst.set_on_change([&] { ++changes; });

        const auto outcomes = dst.apply_ops({Operation{op}});
        QCOMPARE(outcomes[0].kind(), ApplyOutcomeKind::rejected);
        QCOMPARE(changes, 0);
    }

    void duplicate_operation_does_not_fire_a_change_callback() {
        Buffer src(actor(1));
        Operation op = src.apply_local_edit({{0, 0}}, {"X"});

        Buffer dst(actor(2));
        int changes = 0;
        dst.set_on_change([&] { ++changes; });

        QCOMPARE(dst.apply_ops({op})[0].kind(), ApplyOutcomeKind::applied);
        QCOMPARE(changes, 1);
        QCOMPARE(dst.apply_ops({op})[0].kind(), ApplyOutcomeKind::duplicate);
        QCOMPARE(changes, 1);
    }

    void deferral_beyond_budget_is_reported_and_quarantined_not_dropped() {
        Buffer src(actor(1));
        Buffer dst(actor(2));

        // Every operation waits for a dependency that will never arrive.
        std::vector<Operation> blocked;
        for (int i = 0; i < 8; ++i) {
            EditOperation op = as_edit(src.apply_local_edit({{0, 0}}, {"z"}));
            op.timestamp = Identity(actor(1), static_cast<uint32_t>(100 + i));
            op.inserted_fragments[0].origin = op.timestamp;
            op.inserted_fragments[0].locator = Locator(static_cast<uint64_t>(200 + i));
            op.version.observe(Identity(actor(9), 9));  // unresolvable
            blocked.push_back(Operation{op});
        }

        dst.set_deferred_budget(DeferredBudget{3, 1u << 20});
        const auto outcomes = dst.apply_ops(blocked);

        // Backpressure is explicit: the caller is told the queue is full.
        int reported_full = 0;
        for (const auto& outcome : outcomes) {
            if (outcome.kind() == ApplyOutcomeKind::rejected &&
                outcome.error()->code() == ErrorCode::deferred_queue_full) {
                ++reported_full;
            }
        }
        QCOMPARE(reported_full, 5);  // 8 offered, 3 fit

        // And nothing vanished: every operation is either still queued or is
        // retrievable from the quarantine with a reason.
        QCOMPARE(dst.deferred_count(), std::size_t(3));
        QCOMPARE(dst.quarantined().size(), std::size_t(5));
        for (const auto& quarantined : dst.quarantined()) {
            QCOMPARE(quarantined.reason.code(), ErrorCode::deferred_queue_full);
        }
        QCOMPARE(dst.text(), std::string(""));
    }

    void byte_budget_also_bounds_the_deferred_queue() {
        Buffer src(actor(1));
        Buffer dst(actor(2));

        EditOperation op = as_edit(src.apply_local_edit({{0, 0}}, {"payload"}));
        op.version.observe(Identity(actor(9), 9));

        dst.set_deferred_budget(DeferredBudget{1000, 4});  // far too few bytes
        const auto outcomes = dst.apply_ops({Operation{op}});
        QCOMPARE(outcomes[0].kind(), ApplyOutcomeKind::rejected);
        QCOMPARE(outcomes[0].error()->code(), ErrorCode::deferred_queue_full);
        QCOMPARE(dst.deferred_count(), std::size_t(0));
        QCOMPARE(dst.quarantined().size(), std::size_t(1));
    }

    // =======================================================================
    // K. IdList: the assert-only paths
    // =======================================================================

    void idlist_insert_after_unresolvable_anchor_is_refused() {
        // Previously assert(found): in Release the predecessor/successor stayed
        // default-constructed and the insert landed at a fabricated position.
        IdList list(actor(1));
        list.insert_after(Anchor::min(), 100);
        const auto before = list.ids();

        Anchor missing(Identity(actor(1), 999), Bias::Left);  // never inserted
        const auto result = list.try_insert_after(missing, 200);
        QVERIFY(!result.ok());
        QCOMPARE(list.ids(), before);
    }

    void idlist_remove_at_missing_anchor_is_refused_without_out_of_bounds_read() {
        // Previously assert(idx < entries.size()) followed by entries[idx]:
        // an out-of-bounds read on every Release build.
        IdList list(actor(1));
        list.insert_after(Anchor::min(), 100);
        const auto before = list.ids();

        const auto result = list.try_remove_at(Anchor(Identity(actor(1), 999), Bias::Left));
        QVERIFY(!result.ok());
        QCOMPARE(list.ids(), before);
    }

    void idlist_legacy_entry_points_do_not_mutate_on_bad_anchor() {
        IdList list(actor(1));
        list.insert_after(Anchor::min(), 100);
        const auto before = list.ids();

        list.insert_after(Anchor(Identity(actor(1), 999), Bias::Left), 200);
        list.remove_at(Anchor(Identity(actor(1), 999), Bias::Left));
        QCOMPARE(list.ids(), before);
    }

    void idlist_rejected_remote_operation_changes_nothing() {
        IdList alice(actor(1));
        IdList bob(actor(2));
        alice.insert_after(Anchor::min(), 42);

        int changes = 0;
        bob.set_on_change([&] { ++changes; });

        IdListOperation op = alice.insert_after(Anchor::min(), 77);
        std::get<IdListInsertOp>(op).timestamp = Identity(actor(1), 0);  // never a legal identity

        const auto outcomes = bob.apply_ops({op});
        QCOMPARE(outcomes.size(), std::size_t(1));
        QCOMPARE(outcomes[0].kind(), ApplyOutcomeKind::rejected);
        QCOMPARE(bob.ids(), std::vector<uint64_t>{});
        QCOMPARE(changes, 0);
    }

    void idlist_sentinel_locator_is_rejected() {
        IdListInsertOp op;
        op.timestamp = Identity(actor(2), 3);
        op.id = 5;
        op.locator = Locator::min();
        const auto result = validate_idlist_operation(IdListOperation{op}, Global{});
        QCOMPARE(int(result.verdict), int(ValidationVerdict::rejected));
    }

    void idlist_valid_operation_is_accepted() {
        IdListInsertOp op;
        op.timestamp = Identity(actor(2), 3);
        op.id = 5;
        op.locator = Locator(static_cast<uint64_t>(3));
        QCOMPARE(int(validate_idlist_operation(IdListOperation{op}, Global{}).verdict),
                 int(ValidationVerdict::valid));
    }

    void idlist_out_of_order_operations_converge() {
        IdList alice(actor(1));
        IdList bob(actor(2));
        IdList carol(actor(3));

        IdListOperation from_alice = alice.insert_after(Anchor::min(), 100);
        bob.apply_ops({from_alice});
        IdListOperation from_bob = bob.insert_after(Anchor::min(), 200);

        const auto deferred = carol.apply_ops({from_bob});
        QCOMPARE(deferred[0].kind(), ApplyOutcomeKind::deferred);
        QVERIFY(carol.ids().empty());

        carol.apply_ops({from_alice});
        QCOMPARE(carol.ids(), bob.ids());
    }

    void idlist_duplicate_reports_duplicate() {
        IdList alice(actor(1));
        IdList bob(actor(2));
        IdListOperation op = alice.insert_after(Anchor::min(), 100);

        QCOMPARE(bob.apply_ops({op})[0].kind(), ApplyOutcomeKind::applied);
        const auto again = bob.apply_ops({op});
        QCOMPARE(again[0].kind(), ApplyOutcomeKind::duplicate);
        QCOMPARE(bob.ids(), std::vector<uint64_t>{100});
    }
};

QTEST_MAIN(TestOperationValidation)
#include "tst_operation_validation.moc"
