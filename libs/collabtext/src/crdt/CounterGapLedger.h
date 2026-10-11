#pragma once
#include <collabtext/Serialization.h>
#include <collabtext/ApplyOutcome.h>
#include "crdt/OperationPayload.h"
#include <algorithm>
#include <vector>

namespace CollabText::Crdt {
// Stream-local terminal allocation evidence. Never infer a gap from a clock.
class CounterGapLedger {
public:
    static constexpr size_t max_records = 4096, max_bytes = 8u << 20;
    const std::vector<CounterGap>& records() const { return m_records; }
    bool overlaps(const ActorId& actor, uint64_t first, uint64_t last) const {
        for (const auto& gap : m_records)
            if (actor == gap.actor && first < gap.first + gap.count && gap.first <= last) return true;
        return false;
    }
    bool contains(const Identity& id) const { return overlaps(id.actor, id.counter, id.counter); }
    bool conflicts(const Operation& op) const {
        auto id = op_identity(op); auto first = id.counter;
        if (const auto* edit = operation_get_if<EditOperation>(&op))
            for (const auto& item : edit->inserted_fragments)
                if (item.origin.actor == id.actor) first = std::min(first, item.origin.counter);
        return overlaps(id.actor, first, id.counter);
    }
    bool conflicts(const IdListOperation& op) const { return contains(idlist_op_identity(op)); }
    static Error conflict() { return Error(ErrorCode::actor_conflict, "terminal counter interval conflicts with retained state"); }
    ApplyOutcome check(const CounterGap& gap, const Global& current) const {
        auto bytes = encode_counter_gap(gap);
        if (!bytes) return ApplyOutcome::rejected(bytes.error());
        for (const auto& old : m_records) {
            if (gap.actor != old.actor || gap.first >= old.first + old.count || old.first >= gap.first + gap.count) continue;
            if (gap.first == old.first && gap.count == old.count && gap.prior == old.prior) return ApplyOutcome::duplicate();
            return ApplyOutcome::rejected(conflict());
        }
        if (current.get(gap.actor) >= gap.first) return ApplyOutcome::rejected(conflict());
        for (size_t i = 0; i < gap.prior.pair_count(); ++i) {
            const auto [actor, counter] = gap.prior.pair(i);
            if (current.get(actor) < counter) return ApplyOutcome::deferred({actor,counter});
        }
        if (m_records.size() >= max_records || bytes.value().size() > max_bytes - m_bytes)
            return ApplyOutcome::rejected(Error(ErrorCode::limit_exceeded, "terminal counter registry is full"));
        return ApplyOutcome::applied();
    }
    void commit(const CounterGap& gap, Global& current) {
        m_bytes += encode_counter_gap(gap).value().size();
        m_records.push_back(gap);
        current.observe({gap.actor, gap.first + gap.count - 1});
    }
    Result<void> restore(const std::vector<CounterGap>& records, const Global& current) {
        CounterGapLedger restored;
        for (const auto& gap : records) {
            auto bytes = encode_counter_gap(gap); if (!bytes) return bytes.error();
            if (!current.observed_all(gap.prior) || current.get(gap.actor) < gap.first + gap.count - 1)
                return Error(ErrorCode::counter_regression, "terminal interval exceeds checkpoint coverage");
            if (restored.overlaps(gap.actor, gap.first, gap.first + gap.count - 1)) return conflict();
            if (restored.m_records.size() >= max_records || bytes.value().size() > max_bytes - restored.m_bytes)
                return Error(ErrorCode::limit_exceeded, "terminal counter registry is full");
            restored.m_bytes += bytes.value().size(); restored.m_records.push_back(gap);
        }
        *this = std::move(restored); return {};
    }
private:
    std::vector<CounterGap> m_records;
    size_t m_bytes = 0;
};
}
