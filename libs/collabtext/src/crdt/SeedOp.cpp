#include "crdt/SeedOp.h"
#include "crdt/Buffer.h"
#include "crdt/DerivedActor.h"

#include <limits>
#include <memory>
#include <string>

namespace CollabText::Crdt {

namespace {
/// A one-shot allocator for a seed operation: the derived actor, a counter that
/// starts at 1 on every peer, and no persistence. Not a device identity and not
/// a production allocator path — see seed_actor() for why determinism requires
/// both of those.
class SeedCounter final : public IdentityAllocator {
public:
    explicit SeedCounter(const ActorId &actor) : m_actor(actor) {}

    [[nodiscard]] Result<uint64_t> reserve_run(uint64_t count) override {
        if (count == 0)
            return Error(ErrorCode::invalid_identity, "a run reservation must cover one value");
        if (m_next == 0 || count - 1 > (std::numeric_limits<uint64_t>::max() - 1) - m_next)
            return Error(ErrorCode::counter_exhausted, "seed counter exhausted");
        const uint64_t base = m_next;
        m_next = base + count;
        return base;
    }
    void observe(uint64_t counter) override {
        if (counter >= m_next) m_next = counter + 1;
    }
    [[nodiscard]] uint64_t peek() const noexcept override { return m_next; }
    [[nodiscard]] const ActorId &actor() const noexcept override { return m_actor; }

private:
    ActorId m_actor;
    uint64_t m_next = 1;
};
}  // namespace

ActorId seed_actor(const std::string& content) {
    return derived_actor("collabtext/seed-actor/v1", content);
}

Operation op_for_seed(const std::string& content) {
    // A seed operation is reproducible rather than durable: two peers that read
    // the same seed file must compute byte-identical operations. So it is minted
    // under a content-derived actor with a volatile counter, not a device's
    // durable one. The allocator starts at 1 on every peer, which is what makes
    // the two operations identical.
    auto allocator = std::make_shared<SeedCounter>(seed_actor(content));
    Buffer seed_buffer(allocator);
    return seed_buffer.apply_local_edit({{0, 0}}, {content});
}

} // namespace CollabText::Crdt
