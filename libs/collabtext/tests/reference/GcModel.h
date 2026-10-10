#pragma once

// Independent reclamation and membership reference model. This header has no
// production includes and deliberately uses only stable string/counter values.
#include <algorithm>
#include <cstdint>
#include <map>
#include <limits>
#include <set>
#include <string>
#include <vector>

namespace GcReference {

using Actor = std::string;
using Stream = std::string;
using Frontier = std::map<Actor, uint64_t>;
using Coverage = std::map<Stream, Frontier>;
using UndoFloor = std::map<Actor, uint64_t>;

inline bool covers(const Frontier& have, const Frontier& need) {
    for (const auto& [actor, counter] : need) {
        const auto found = have.find(actor);
        if (found == have.end() || found->second < counter) return false;
    }
    return true;
}

inline bool covers(const Coverage& have, const Coverage& need) {
    for (const auto& [stream, frontier] : need) {
        const auto found = have.find(stream);
        if (found == have.end() || !covers(found->second, frontier)) return false;
    }
    return true;
}

// Absence is zero coverage. This is an intersection, never componentwise max.
inline Frontier meet(const std::vector<Frontier>& vectors) {
    std::set<Actor> actors;
    for (const auto& vector : vectors)
        for (const auto& [actor, _] : vector) actors.insert(actor);
    Frontier result;
    for (const auto& actor : actors) {
        uint64_t common = UINT64_MAX;
        for (const auto& vector : vectors) {
            const auto found = vector.find(actor);
            common = std::min(common, found == vector.end() ? uint64_t{0} : found->second);
        }
        if (common) result.emplace(actor, common);
    }
    return result;
}

inline Coverage meet(const std::vector<Coverage>& coverages) {
    std::set<Stream> streams;
    for (const auto& coverage : coverages)
        for (const auto& [stream, _] : coverage) streams.insert(stream);
    Coverage result;
    for (const auto& stream : streams) {
        std::vector<Frontier> vectors;
        vectors.reserve(coverages.size());
        for (const auto& coverage : coverages) {
            const auto found = coverage.find(stream);
            vectors.push_back(found == coverage.end() ? Frontier{} : found->second);
        }
        auto common = meet(vectors);
        if (!common.empty()) result.emplace(stream, std::move(common));
    }
    return result;
}

enum class GroupKind { undo, redo };
struct RetainedGroup {
    std::string id;
    Actor owner;
    Stream stream;
    Frontier restores;
    GroupKind kind = GroupKind::undo;
    bool retained = true;
};

struct PeerAck {
    std::string epoch_hash;
    std::string proposal_id;
    Coverage durable_coverage;
    UndoFloor retired_undo_sequence;
    uint64_t pin_projection_frontier = 0;
    std::string checkpoint_hash;
    bool private_history_durable = false;
    bool durable = false;
};

struct Member {
    bool enrolled = true;
    bool available = false; // availability is diagnostic, never membership evidence
    Coverage durable_coverage;
    UndoFloor retired_undo_sequence;
    std::vector<RetainedGroup> groups;
};

struct Epoch {
    uint64_t number = 1;
    std::string hash;
    std::set<Actor> members;
    bool finalized = true;
    bool pending_transition = false;
    std::string pending_transition_id;
    bool competing_branches = false;
};

struct Checkpoint {
    std::string hash;
    std::string epoch_hash;
    Coverage coverage;
    bool durable = false;
    bool integrity_valid = false;
    bool logical_state = false;
    bool deferred_input = false;
    bool coverage_state = false;
    bool pin_projection = false;
};

struct LivePin {
    std::string id;
    Actor owner;
    Stream stream;
    Actor referent_actor;
    uint64_t referent_counter = 0;
    bool has_translation = false;
    uint32_t token_version = 1;
    bool original_reader_supported = true;
    uint64_t event_bytes = 0;
    uint64_t projection_bytes = 0;
    uint64_t translation_bytes = 0;
};

struct PinProjection {
    bool complete = false;
    uint64_t frontier = 0;
    std::vector<LivePin> live;
    bool unresolved_orphan = false;
    uint64_t unresolved_orphan_bytes = 0;
};

inline uint64_t retained_pin_metadata_bytes(const PinProjection& projection) {
    uint64_t bytes = projection.unresolved_orphan_bytes;
    const auto add = [](uint64_t a, uint64_t b) {
        return b > std::numeric_limits<uint64_t>::max() - a
            ? std::numeric_limits<uint64_t>::max() : a + b;
    };
    for (const auto& pin : projection.live) {
        bytes = add(bytes, pin.event_bytes);
        bytes = add(bytes, pin.projection_bytes);
        bytes = add(bytes, pin.translation_bytes);
    }
    return bytes;
}

struct Proposal {
    std::string id;
    std::string epoch_hash;
    std::string checkpoint_hash;
    Coverage common_coverage;
    UndoFloor common_undo_retirement;
    uint64_t pin_frontier = 0;
    std::vector<std::string> translation_hashes;
};

struct World {
    Epoch epoch;
    std::map<Actor, Member> members;
    std::map<Actor, PeerAck> acknowledgements;
    Checkpoint checkpoint;
    PinProjection pins;
    Proposal proposal;
    bool unknown_member_observed = false;
    bool competing_proposals = false;
};

inline bool floor_covers(const UndoFloor& have, const UndoFloor& need) {
    return covers(have, need);
}

inline UndoFloor minimum_undo_retirement(const std::vector<UndoFloor>& floors) {
    std::set<Actor> owners;
    for (const auto& floor : floors)
        for (const auto& [owner, _] : floor) owners.insert(owner);
    UndoFloor common;
    for (const auto& owner : owners) {
        uint64_t value = UINT64_MAX;
        for (const auto& floor : floors) {
            const auto found = floor.find(owner);
            value = std::min(value, found == floor.end() ? uint64_t{0} : found->second);
        }
        if (value) common.emplace(owner, value);
    }
    return common;
}

inline Coverage common_coverage(const World& world) {
    std::vector<Coverage> vectors;
    for (const auto& actor : world.epoch.members) {
        const auto member = world.members.find(actor);
        if (member == world.members.end() || !member->second.enrolled) return {};
        vectors.push_back(member->second.durable_coverage);
    }
    return vectors.empty() ? Coverage{} : meet(vectors);
}

inline UndoFloor common_undo_retirement(const World& world) {
    std::vector<UndoFloor> floors;
    for (const auto& actor : world.epoch.members) {
        const auto member = world.members.find(actor);
        if (member == world.members.end() || !member->second.enrolled) return {};
        floors.push_back(member->second.retired_undo_sequence);
    }
    return floors.empty() ? UndoFloor{} : minimum_undo_retirement(floors);
}

inline bool pin_referent_is_covered(const Coverage& candidate, const LivePin& pin) {
    const auto stream = candidate.find(pin.stream);
    if (stream == candidate.end()) return false;
    const auto actor = stream->second.find(pin.referent_actor);
    return actor != stream->second.end() && actor->second >= pin.referent_counter;
}

// A model certificate is eligible only at the exact current epoch and exact
// common durable frontier, with every member acknowledging the same proposal.
inline bool eligible(const World& world) {
    const auto& epoch = world.epoch;
    const auto& proposal = world.proposal;
    const auto& checkpoint = world.checkpoint;
    if (epoch.members.empty() || !epoch.finalized || epoch.pending_transition ||
        epoch.competing_branches || world.competing_proposals || world.unknown_member_observed)
        return false;
    if (proposal.id.empty() || proposal.epoch_hash != epoch.hash ||
        checkpoint.epoch_hash != epoch.hash || proposal.checkpoint_hash.empty() ||
        checkpoint.hash != proposal.checkpoint_hash || !checkpoint.durable ||
        !checkpoint.integrity_valid || !checkpoint.logical_state ||
        !checkpoint.deferred_input || !checkpoint.coverage_state ||
        !checkpoint.pin_projection)
        return false;
    if (!covers(checkpoint.coverage, proposal.common_coverage) ||
        !covers(proposal.common_coverage, checkpoint.coverage)) return false;
    if (!world.pins.complete || world.pins.unresolved_orphan ||
        proposal.pin_frontier == 0 ||
        world.pins.frontier < proposal.pin_frontier) return false;

    std::vector<Coverage> coverage;
    std::vector<UndoFloor> undo_floors;
    for (const auto& [actor, ack] : world.acknowledgements)
        if (!epoch.members.contains(actor)) return false; // unknown/stale actor evidence
    for (const auto& actor : epoch.members) {
        const auto member = world.members.find(actor);
        const auto ack = world.acknowledgements.find(actor);
        if (member == world.members.end() || !member->second.enrolled ||
            ack == world.acknowledgements.end()) return false;
        const auto& evidence = ack->second;
        if (!evidence.durable || evidence.epoch_hash != epoch.hash ||
            evidence.proposal_id != proposal.id ||
            evidence.checkpoint_hash != checkpoint.hash ||
            !evidence.private_history_durable ||
            evidence.pin_projection_frontier < proposal.pin_frontier ||
            !covers(evidence.durable_coverage, proposal.common_coverage) ||
            !floor_covers(evidence.retired_undo_sequence,
                          proposal.common_undo_retirement)) return false;
        coverage.push_back(evidence.durable_coverage);
        undo_floors.push_back(evidence.retired_undo_sequence);
        // Offline peers remain members. Their old ack only counts if it is
        // durable evidence for this exact proposal and epoch.
    }
    if (coverage.empty() || meet(coverage) != proposal.common_coverage ||
        minimum_undo_retirement(undo_floors) != proposal.common_undo_retirement)
        return false;

    for (const auto& [actor, member] : world.members) {
        if (!epoch.members.contains(actor) && member.enrolled) return false;
        for (const auto& group : member.groups) {
        if (!group.retained || !epoch.members.contains(actor)) continue;
            const auto stream = proposal.common_coverage.find(group.stream);
            if (stream != proposal.common_coverage.end() &&
                covers(stream->second, group.restores)) return false;
        }
    }
    for (const auto& pin : world.pins.live)
        if (pin_referent_is_covered(proposal.common_coverage, pin) &&
            (!pin.has_translation || !pin.original_reader_supported ||
             pin.token_version == 0 || proposal.translation_hashes.empty())) return false;
    return true;
}

enum class TransitionKind { enroll, retire };
enum class ReminderAction { show, snooze, dismiss, keep_enrolled };
struct TransitionProposal {
    TransitionKind kind = TransitionKind::enroll;
    Actor subject;
    std::string event_id;
    std::string parent_epoch_hash;
    bool explicitly_confirmed = false;
    bool durable = false;
    bool branch_preserved_for_returning_device = false;
    bool bootstrap_durable = false; // enrollment only
    std::set<Actor> acknowledgements;
    bool competing = false;
};
enum class TransitionResult { pending, committed, rejected };

inline Epoch apply_reminder(const Epoch& current, ReminderAction) {
    // Reminders are UI state. None of their actions is a membership event.
    return current;
}

inline TransitionResult commit_transition(Epoch& current,
                                          const TransitionProposal& proposal) {
    if (proposal.competing) {
        current.competing_branches = true;
        return TransitionResult::rejected;
    }
    if (proposal.event_id.empty() || proposal.parent_epoch_hash != current.hash ||
        !proposal.durable || !proposal.explicitly_confirmed)
        return TransitionResult::rejected;
    if (current.pending_transition) {
        if (current.pending_transition_id != proposal.event_id) {
            current.competing_branches = true;
            return TransitionResult::rejected;
        }
    } else if (!current.finalized || current.competing_branches) {
        return TransitionResult::rejected;
    }
    if (proposal.kind == TransitionKind::retire &&
        !current.members.contains(proposal.subject)) return TransitionResult::rejected;
    if (proposal.kind == TransitionKind::retire && current.members.size() == 1)
        return TransitionResult::rejected; // archive/delete is a separate operation
    if (proposal.kind == TransitionKind::enroll && current.members.contains(proposal.subject))
        return TransitionResult::rejected;
    if (proposal.kind == TransitionKind::retire &&
        !proposal.branch_preserved_for_returning_device) return TransitionResult::rejected;
    if (proposal.kind == TransitionKind::enroll && !proposal.bootstrap_durable)
        return TransitionResult::rejected;
    std::set<Actor> allowed_acks = current.members;
    if (proposal.kind == TransitionKind::retire) allowed_acks.erase(proposal.subject);
    else allowed_acks.insert(proposal.subject);
    for (const auto& actor : proposal.acknowledgements)
        if (!allowed_acks.contains(actor)) return TransitionResult::rejected;
    current.pending_transition = true;
    current.pending_transition_id = proposal.event_id;
    std::set<Actor> required = current.members;
    if (proposal.kind == TransitionKind::retire) required.erase(proposal.subject);
    else required.insert(proposal.subject); // the new peer attests its bootstrap
    if (!std::includes(proposal.acknowledgements.begin(), proposal.acknowledgements.end(),
                       required.begin(), required.end())) {
        current.pending_transition = true;
        return TransitionResult::pending;
    }
    if (proposal.kind == TransitionKind::enroll) current.members.insert(proposal.subject);
    else current.members.erase(proposal.subject);
    ++current.number;
    current.hash = proposal.event_id; // event ID names immutable descriptor contents
    current.finalized = true;
    current.pending_transition = false;
    current.pending_transition_id.clear();
    return TransitionResult::committed;
}

enum class ReturningPeerResult { active, export_and_reconcile, invalid };
inline ReturningPeerResult returning_peer(const Epoch& epoch, const Actor& actor,
                                           bool has_preserved_branch) {
    if (epoch.members.contains(actor)) return ReturningPeerResult::active;
    return has_preserved_branch ? ReturningPeerResult::export_and_reconcile
                                : ReturningPeerResult::invalid;
}

} // namespace GcReference
