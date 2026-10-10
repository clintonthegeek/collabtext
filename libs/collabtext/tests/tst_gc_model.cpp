// G01 model gate. It uses no production engine, coverage, membership, pin or
// checkpoint implementation; all state is supplied as immutable test values.
#include "reference/GcModel.h"

#include <QtTest>

using namespace GcReference;

namespace {
World ready_world() {
    World world;
    world.epoch.number = 4;
    world.epoch.hash = "epoch-4";
    world.epoch.members = {"a", "b", "c"};
    world.proposal = {"proposal-9", "epoch-4", "checkpoint-9",
                      {{"text", {{"a", 10}, {"b", 7}}}}, {{"a", 3}}, 5,
                      {"translation-9"}};
    world.checkpoint = {"checkpoint-9", "epoch-4",
                        {{"text", {{"a", 10}, {"b", 7}}}}, true, true,
                        true, true, true, true};
    world.pins = {true, 5,
                  {{"reply-1", "c", "text", "a", 3, true, 1, true, 48, 32, 64}},
                  false, 0};
    for (const Actor& actor : world.epoch.members) {
        Member member;
        member.available = actor == "a";
        member.durable_coverage = {{"text", {{"a", 10}, {"b", 7}}}};
        member.retired_undo_sequence = {{"a", 3}};
        world.members.emplace(actor, member);
        world.acknowledgements.emplace(actor, PeerAck{
            "epoch-4", "proposal-9", member.durable_coverage,
            member.retired_undo_sequence, 5, "checkpoint-9", true, true});
    }
    return world;
}
}

class TestGcModel : public QObject {
    Q_OBJECT
private slots:
    void componentwise_common_coverage_uses_minimum_and_absence_as_zero() {
        const auto common = meet(std::vector<Frontier>{{{"a", 9}, {"b", 8}},
                                                        {{"a", 4}, {"c", 3}}});
        QCOMPARE(common, (Frontier{{"a", 4}}));
        QVERIFY(covers(Frontier{{"a", 4}}, Frontier{{"a", 4}}));
        QVERIFY(!covers(Frontier{{"a", 4}}, Frontier{{"a", 5}}));
    }

    void complete_current_epoch_certificate_is_eligible() {
        const auto world = ready_world();
        QCOMPARE(common_coverage(world), world.proposal.common_coverage);
        QCOMPARE(common_undo_retirement(world), world.proposal.common_undo_retirement);
        QVERIFY(eligible(world));
        // One acknowledgement is durable but tied to a different epoch.
        auto stale = world;
        stale.acknowledgements["c"].epoch_hash = "epoch-3";
        QVERIFY(!eligible(stale));
        // An old receipt from a silent member is not an acknowledgement of this
        // proposal. Time offline does not remove that member.
        stale = world;
        stale.acknowledgements.erase("c");
        QVERIFY(!eligible(stale));
        stale = world;
        for (auto& [_, member] : stale.members) member.available = false;
        stale.acknowledgements.clear();
        QVERIFY(!eligible(stale)); // no peer can supply this proposal's evidence
    }

    void f1_insertion_redo_and_f2_remote_undo_block_reclamation() {
        auto world = ready_world();
        world.members["a"].groups.push_back(
            {"redo-insert-a-6", "a", "text", {{"a", 6}}, GroupKind::redo, true});
        QVERIFY(!eligible(world)); // F1: redo still needs the insertion's items

        world.members["a"].groups.clear();
        world.members["b"].groups.push_back(
            {"remote-undo-a-7", "b", "text", {{"a", 7}}, GroupKind::undo, true});
        QVERIFY(!eligible(world)); // F2: remote actor restoration is an obligation

        world.members["b"].groups.clear();
        world.members["c"].groups.push_back(
            {"future-undo-a-11", "c", "text", {{"a", 11}}, GroupKind::undo, true});
        QVERIFY(eligible(world)); // data beyond the proposed floor is not removed
    }

    void f3_common_coverage_is_intersection_not_maximum() {
        auto world = ready_world();
        world.acknowledgements["c"].durable_coverage["text"]["a"] = 4;
        world.members["c"].durable_coverage["text"]["a"] = 4;
        const auto actual = meet(std::vector<Coverage>{
            world.acknowledgements["a"].durable_coverage,
            world.acknowledgements["b"].durable_coverage,
            world.acknowledgements["c"].durable_coverage});
        QCOMPARE(actual.at("text").at("a"), uint64_t(4));
        world.proposal.common_coverage["text"]["a"] = 10;
        QVERIFY(!eligible(world)); // F3: max would claim work b/c c has not seen
    }

    void checkpoint_and_external_pin_projection_are_hard_requirements() {
        auto world = ready_world();
        world.checkpoint.integrity_valid = false;
        QVERIFY(!eligible(world));
        world = ready_world();
        world.checkpoint.deferred_input = false;
        QVERIFY(!eligible(world));
        world = ready_world();
        world.acknowledgements["b"].private_history_durable = false;
        QVERIFY(!eligible(world)); // actor-private undo/redo is a separate durable input
        world = ready_world();
        world.pins.complete = false;
        QVERIFY(!eligible(world));
        world = ready_world();
        world.pins.frontier = 4;
        QVERIFY(!eligible(world)); // a partially committed pin log cannot advance
        world = ready_world();
        world.pins.unresolved_orphan = true;
        QVERIFY(!eligible(world));
        world = ready_world();
        world.pins.live[0].has_translation = false;
        QVERIFY(!eligible(world)); // pin remains live after author retirement too
        world = ready_world();
        world.pins.live[0].original_reader_supported = false;
        QVERIFY(!eligible(world));
    }

    void unknown_members_competing_proposals_and_transition_windows_block() {
        auto world = ready_world();
        world.unknown_member_observed = true;
        QVERIFY(!eligible(world));
        world = ready_world();
        world.competing_proposals = true;
        QVERIFY(!eligible(world));
        world = ready_world();
        world.epoch.pending_transition = true;
        QVERIFY(!eligible(world));
        world = ready_world();
        world.epoch.competing_branches = true;
        QVERIFY(!eligible(world));
        world = ready_world();
        world.epoch.members.clear();
        QVERIFY(!eligible(world));
    }

    void reminders_never_change_membership_and_enrollment_invalidates_old_epoch() {
        auto epoch = ready_world().epoch;
        const auto before = epoch;
        for (auto action : {ReminderAction::show, ReminderAction::snooze,
                            ReminderAction::dismiss, ReminderAction::keep_enrolled})
            epoch = apply_reminder(epoch, action);
        QCOMPARE(epoch.members, before.members);
        QCOMPARE(epoch.hash, before.hash);

        auto world = ready_world();
        TransitionProposal enrollment{TransitionKind::enroll, "d", "epoch-5",
                                      "epoch-4", true, true, false, true,
                                      {"a", "b", "c", "d"}, false};
        QCOMPARE(commit_transition(world.epoch, enrollment), TransitionResult::committed);
        QCOMPARE(world.epoch.number, uint64_t(5));
        QCOMPARE(world.epoch.members.size(), size_t(4));
        // Neither an ack nor a checkpoint bound to epoch-4 can authorize epoch-5.
        QVERIFY(!eligible(world));
    }

    void retirement_is_explicit_preserves_branch_and_requires_transition_acks() {
        auto epoch = ready_world().epoch;
        TransitionProposal retirement{TransitionKind::retire, "c", "epoch-5-retire-c",
                                     "epoch-4", true, true, true, false,
                                     {"a"}, false};
        QCOMPARE(commit_transition(epoch, retirement), TransitionResult::pending);
        QVERIFY(epoch.pending_transition);
        // A pending transition does not remove the peer or authorize a cert.
        QVERIFY(epoch.members.contains("c"));
        retirement.acknowledgements.insert("b");
        QCOMPARE(commit_transition(epoch, retirement), TransitionResult::committed);
        QVERIFY(!epoch.members.contains("c"));
        QCOMPARE(returning_peer(epoch, "c", true),
                 ReturningPeerResult::export_and_reconcile);
        QCOMPARE(returning_peer(epoch, "c", false), ReturningPeerResult::invalid);

        auto invalid = ready_world().epoch;
        retirement.branch_preserved_for_returning_device = false;
        QCOMPARE(commit_transition(invalid, retirement), TransitionResult::rejected);
        QVERIFY(invalid.members.contains("c"));
        retirement.branch_preserved_for_returning_device = true;
        retirement.explicitly_confirmed = false;
        QCOMPARE(commit_transition(invalid, retirement), TransitionResult::rejected);
        retirement.explicitly_confirmed = true;
        invalid.members = {"c"};
        QCOMPARE(commit_transition(invalid, retirement), TransitionResult::rejected);
        QVERIFY(invalid.members.contains("c")); // last actor needs archive/delete
    }

    void retirement_does_not_release_a_live_pin_owned_by_that_device() {
        auto world = ready_world();
        QCOMPARE(retained_pin_metadata_bytes(world.pins), uint64_t(144));
        world.pins.live[0].has_translation = false;
        world.epoch.members.erase("c");
        world.epoch.number = 5;
        world.epoch.hash = "epoch-5";
        world.members["c"].enrolled = false;
        world.acknowledgements.erase("c");
        world.proposal.epoch_hash = "epoch-5";
        world.proposal.id = "proposal-10";
        world.checkpoint.epoch_hash = "epoch-5";
        world.checkpoint.hash = "checkpoint-10";
        world.proposal.checkpoint_hash = "checkpoint-10";
        for (const auto& actor : world.epoch.members) {
            world.acknowledgements[actor].epoch_hash = "epoch-5";
            world.acknowledgements[actor].proposal_id = "proposal-10";
            world.acknowledgements[actor].checkpoint_hash = "checkpoint-10";
        }
        QVERIFY(!eligible(world));
        world.pins.live[0].has_translation = true;
        QVERIFY(eligible(world));
        QCOMPARE(retained_pin_metadata_bytes(world.pins), uint64_t(144));
    }

    void orphan_pin_cost_is_retained_and_accounting_saturates() {
        auto world = ready_world();
        world.pins.unresolved_orphan = true;
        world.pins.unresolved_orphan_bytes = 9;
        QCOMPARE(retained_pin_metadata_bytes(world.pins), uint64_t(153));
        world.pins.live[0].translation_bytes = UINT64_MAX;
        QCOMPARE(retained_pin_metadata_bytes(world.pins), UINT64_MAX);
        QVERIFY(!eligible(world));
    }

    void concurrent_membership_proposals_do_not_pick_a_winner_by_arrival_order() {
        auto epoch = ready_world().epoch;
        TransitionProposal first{TransitionKind::enroll, "d", "epoch-5-a",
                                 "epoch-4", true, true, false, true,
                                 {"a"}, false};
        QCOMPARE(commit_transition(epoch, first), TransitionResult::pending);
        auto competing = first;
        competing.event_id = "epoch-5-b";
        competing.subject = "e";
        competing.competing = true;
        QCOMPARE(commit_transition(epoch, competing), TransitionResult::rejected);
        QVERIFY(epoch.pending_transition);
        QVERIFY(epoch.members.contains("a") && epoch.members.contains("b") &&
                epoch.members.contains("c"));
    }

    void no_member_ack_can_claim_unseen_deferred_or_cross_stream_work() {
        auto world = ready_world();
        world.acknowledgements["b"].durable_coverage["text"]["a"] = 9;
        QVERIFY(!eligible(world));
        world = ready_world();
        world.proposal.common_coverage["list"] = {{"a", 3}};
        QVERIFY(!eligible(world));
        world = ready_world();
        world.acknowledgements["b"].proposal_id = "competing-proposal";
        QVERIFY(!eligible(world));
    }

    void exhaustive_bounded_undo_and_redo_retention_matrix() {
        // Three enrolled actors, each with an undo and redo slot. Every slot is
        // absent, references retained state at/below the candidate floor, or
        // references strictly later state. This enumerates 3^6 causal-history
        // shapes for F1/F2 without consulting a production history structure.
        constexpr int slot_count = 6;
        constexpr int cases = 729;
        for (int encoded = 0; encoded < cases; ++encoded) {
            auto world = ready_world();
            int value = encoded;
            bool expected_eligible = true;
            for (int slot = 0; slot < slot_count; ++slot) {
                const int state = value % 3;
                value /= 3;
                if (state == 0) continue;
                const Actor owner = slot / 2 == 0 ? "a" : slot / 2 == 1 ? "b" : "c";
                const GroupKind kind = slot % 2 == 0 ? GroupKind::undo : GroupKind::redo;
                const uint64_t restore_counter = state == 1 ? 3 : 11;
                const std::string id = owner + (kind == GroupKind::undo ? "/undo/" : "/redo/") +
                                       std::to_string(restore_counter);
                world.members[owner].groups.push_back(
                    {id, owner, "text", {{"a", restore_counter}}, kind, true});
                if (restore_counter <= 10) expected_eligible = false;
            }
            QCOMPARE(eligible(world), expected_eligible);
        }
    }
};

QTEST_GUILESS_MAIN(TestGcModel)
#include "tst_gc_model.moc"
