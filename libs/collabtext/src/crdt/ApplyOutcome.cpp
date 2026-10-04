/// ApplyOutcome.cpp — out-of-line definitions for the apply outcome value.

#include <collabtext/ApplyOutcome.h>

namespace CollabText {

ApplyOutcome ApplyOutcome::applied() { return ApplyOutcome{}; }

ApplyOutcome ApplyOutcome::duplicate() {
    ApplyOutcome outcome;
    outcome.m_kind = ApplyOutcomeKind::duplicate;
    return outcome;
}

ApplyOutcome ApplyOutcome::deferred(Crdt::Lamport awaiting) {
    ApplyOutcome outcome;
    outcome.m_kind = ApplyOutcomeKind::deferred;
    outcome.m_awaiting = awaiting;
    return outcome;
}

ApplyOutcome ApplyOutcome::rejected(Error error) {
    ApplyOutcome outcome;
    outcome.m_kind = ApplyOutcomeKind::rejected;
    outcome.m_error = std::move(error);
    return outcome;
}

std::string_view to_string(ApplyOutcomeKind kind) {
    switch (kind) {
    case ApplyOutcomeKind::applied: return "applied";
    case ApplyOutcomeKind::duplicate: return "duplicate";
    case ApplyOutcomeKind::deferred: return "deferred";
    case ApplyOutcomeKind::rejected: return "rejected";
    }
    return "unknown";
}

}  // namespace CollabText
