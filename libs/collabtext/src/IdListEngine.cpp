/// src/IdListEngine.cpp — the IdListEngine facade over Crdt::IdList.
///
/// Roadmap task A02a. Deliberately a thin forwarder: every method is one call
/// into the engine, and nothing here adds policy. A facade that started
/// deciding things would be a second implementation of the list, and the list
/// already has one.

#include "collabtext/IdListEngine.h"

#include "crdt/IdList.h"

#include <utility>

namespace CollabText {

class IdListEngine::Impl {
public:
    explicit Impl(Crdt::SharedIdentityAllocator allocator)
        : list(std::move(allocator)) {}

    Crdt::IdList list;
};

IdListEngine::IdListEngine(Crdt::SharedIdentityAllocator allocator)
    : m_impl(std::make_unique<Impl>(std::move(allocator))) {}

IdListEngine::~IdListEngine() = default;
IdListEngine::IdListEngine(IdListEngine &&) noexcept = default;
IdListEngine &IdListEngine::operator=(IdListEngine &&) noexcept = default;

Result<Crdt::IdListOperation> IdListEngine::try_insert_after(
    const Crdt::Anchor &after, uint64_t id) {
    return m_impl->list.try_insert_after(after, id);
}

Result<Crdt::IdListOperation> IdListEngine::try_remove_at(const Crdt::Anchor &target) {
    return m_impl->list.try_remove_at(target);
}

std::vector<uint64_t> IdListEngine::ids() const { return m_impl->list.ids(); }
uint32_t IdListEngine::size() const { return m_impl->list.size(); }
size_t IdListEngine::tombstone_count() const { return m_impl->list.tombstone_count(); }
size_t IdListEngine::entry_count() const { return m_impl->list.entry_count(); }

Result<Crdt::Anchor> IdListEngine::try_anchor_of(uint64_t id, Crdt::Bias bias) const {
    return m_impl->list.try_anchor_of(id, bias);
}

const Crdt::Global &IdListEngine::version() const { return m_impl->list.version(); }
const ActorId &IdListEngine::actor_id() const { return m_impl->list.actor_id(); }

std::vector<ApplyOutcome> IdListEngine::apply_ops(
    const std::vector<Crdt::IdListOperation> &ops) {
    return m_impl->list.apply_ops(ops);
}

void IdListEngine::set_on_local_op(LocalOpCallback cb) {
    m_impl->list.set_on_local_op(std::move(cb));
}
void IdListEngine::set_on_change(ChangeCallback cb) {
    m_impl->list.set_on_change(std::move(cb));
}

std::optional<Crdt::IdListOperation> IdListEngine::undo() { return m_impl->list.undo(); }
std::optional<Crdt::IdListOperation> IdListEngine::redo() { return m_impl->list.redo(); }
size_t IdListEngine::undo_depth() const { return m_impl->list.undo_depth(); }
bool IdListEngine::coalesce_last_undo() { return m_impl->list.coalesce_last_undo(); }
void IdListEngine::set_max_undo_depth(size_t depth) {
    m_impl->list.set_max_undo_depth(depth);
}
size_t IdListEngine::max_undo_depth() const { return m_impl->list.max_undo_depth(); }

void IdListEngine::local_clear() noexcept { m_impl->list.local_clear(); }

}  // namespace CollabText