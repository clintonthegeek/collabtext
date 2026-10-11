#pragma once
#include <collabtext/Anchor.h>
#include <collabtext/Engine.h>
#include <memory>
#include <string_view>

namespace CollabText {
/// Disposable deterministic canonical-log projection only. Never a device
/// writer. Start fresh and replay the complete canonical prefix in fixed order.
/// Derivation v1 uses SHA-256 with a library domain and length-prefixed scope,
/// adapter name/version and epoch; the first 128 bits identify one stream actor.
/// Consumers must check actor namespace collisions and version their translator.
/// This does not promise cross-version edit/encoder byte determinism.
class ProjectionAllocator final : public Crdt::IdentityAllocator {
public:
    static Result<std::shared_ptr<ProjectionAllocator>> create(
        const PositionScope&, std::string_view adapter, uint32_t adapter_version);
    Result<uint64_t> reserve_run(uint64_t count) override;
    void observe(uint64_t counter) override;
    uint64_t peek() const noexcept override;
    const ActorId& actor() const noexcept override;
private:
    explicit ProjectionAllocator(ActorId);
    ActorId m_actor;
    CheckedCounter m_counter{1};
};
}
