#include <collabtext/CrdtEngine.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

using namespace CollabText;
using namespace CollabText::Crdt;

class CheckedAllocator final : public IdentityAllocator {
public:
    explicit CheckedAllocator(uint64_t actor) {
        std::array<uint8_t, 16> bytes{};
        for (int i = 15; i >= 8; --i) {
            bytes[static_cast<size_t>(i)] = static_cast<uint8_t>(actor);
            actor >>= 8;
        }
        actor_ = ActorId::from_bytes(bytes);
    }
    Result<uint64_t> reserve_run(uint64_t count) override { return counter_.reserve_run(count); }
    void observe(uint64_t value) override {
        if (!counter_.observe(value)) std::terminate();
    }
    uint64_t peek() const noexcept override { return counter_.peek(); }
    const ActorId& actor() const noexcept override { return actor_; }
private:
    ActorId actor_;
    CheckedCounter counter_;
};

int main() {
    const std::string text(100'000, 'b');
    for (uint64_t sample = 0; sample < 25; ++sample) {
        auto created = CrdtEngine::create(
            std::make_shared<CheckedAllocator>(sample + 1),
            {"perf", "body", Primitive::text, 1});
        if (!created) return 2;
        auto engine = std::move(created).value();
        const auto start = std::chrono::steady_clock::now();
        auto inserted = engine->try_insert(0, text);
        const auto stop = std::chrono::steady_clock::now();
        if (!inserted || engine->text() != text) return 3;
        std::cout << std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count()
                  << '\n';
    }
}
