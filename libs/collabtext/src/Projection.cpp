#include <collabtext/Projection.h>
#include "core/Sha256.h"
#include "crdt/PositionPayload.h"
namespace CollabText {
ProjectionAllocator::ProjectionAllocator(ActorId actor) : m_actor(actor) {}
Result<std::shared_ptr<ProjectionAllocator>> ProjectionAllocator::create(
    const PositionScope& scope, std::string_view adapter, uint32_t version) {
    auto valid = detail::validate_scope(scope, scope.primitive);
    if (!valid) return valid.error();
    if (adapter.empty() || adapter.size()>256 || !version)
        return Error(ErrorCode::malformed, "projection requires a bounded adapter name and nonzero version");
    std::string seed = "collabtext/projection-actor/v1";
    const auto number = [&](uint64_t value, unsigned width) {
        for (unsigned n=width;n;--n) seed.push_back(static_cast<char>(value >> ((n-1)*8)));
    };
    const auto field = [&](std::string_view text) { number(text.size(),4); seed.append(text); };
    field(scope.document);field(scope.stream);
    number(scope.primitive==Primitive::text ? 1 : 2,1);number(scope.epoch,8);
    field(adapter);number(version,4);
    auto actor = ActorId::parse(sha256_hex(seed).substr(0,32));
    if (!actor) return actor.error();
    return std::shared_ptr<ProjectionAllocator>(new ProjectionAllocator(actor.value()));
}
Result<uint64_t> ProjectionAllocator::reserve_run(uint64_t count) { return m_counter.reserve_run(count, "canonical projection"); }
void ProjectionAllocator::observe(uint64_t counter) {
    if (counter == CheckedCounter::kExhausted)
        (void)m_counter.restore(CheckedCounter::kExhausted, "canonical projection");
    else
        (void)m_counter.observe(counter, "canonical projection");
}
uint64_t ProjectionAllocator::peek() const noexcept { return m_counter.peek(); }
const ActorId& ProjectionAllocator::actor() const noexcept { return m_actor; }
}
