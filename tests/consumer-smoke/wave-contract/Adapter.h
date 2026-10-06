#pragma once
// Consumer-owned composition. No Wave code, engine internals, Qt or session.
#include <collabtext/CrdtEngine.h>
#include <collabtext/IdListEngine.h>
#include <collabtext/Serialization.h>
#include <array>
#include <map>
#include <optional>
#include <type_traits>
#include <stdexcept>
#include <vector>
namespace Consumer {
using namespace CollabText;
inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class T> T checked(Result<T> result) {
    if (!result.ok()) throw std::runtime_error(std::string(to_string(result.error().code())) + ": " + std::string(result.error().message()));
    return std::move(result).value();
}
template<class T> std::string refusal(Result<T> result) {
    require(!result.ok(), "expected refusal, received usable value");
    return std::string(to_string(result.error().code()));
}
class Counter final : public Crdt::IdentityAllocator {
    ActorId who;
    CheckedCounter counter;
public:
    explicit Counter(uint8_t n) {
        std::array<uint8_t,16> bytes{}; bytes[15] = n;
        who = ActorId::from_bytes(bytes);
    }
    Result<uint64_t> reserve_run(uint64_t n) override { return counter.reserve_run(n); }
    void observe(uint64_t n) override { (void)counter.observe(n); }
    uint64_t peek() const noexcept override { return counter.peek(); }
    const ActorId& actor() const noexcept override { return who; }
};
struct Resolution {
    std::string state; // Resolved, Collapsed, Stale, Gone
    std::optional<uint32_t> offset;
    std::string error;
    bool lost_left = false, lost_right = false;
};
struct Block {
    std::shared_ptr<Counter> allocator;
    std::unique_ptr<CrdtEngine> engine;
    std::vector<std::string> emitted;
    unsigned changes = 0;
    Block(std::string document, std::string stream, uint8_t actor, uint64_t epoch)
        : allocator(std::make_shared<Counter>(actor)),
          engine(checked(CrdtEngine::create(allocator,{std::move(document),std::move(stream),Primitive::text,epoch}))) {
        engine->setOnLocalOp([this](const auto& op) { emitted.push_back(checked(Crdt::encode_operation(op))); });
        engine->setOnChange([this] { ++changes; });
    }
};
class Document {
    std::string id;
    uint8_t actor;
    uint64_t epoch;
public:
    std::map<std::string,std::unique_ptr<Block>> blocks;
    Document(std::string id, uint8_t actor, uint64_t epoch=1) : id(std::move(id)),actor(actor),epoch(epoch) {}
    Block& add(const std::string& name) {
        require(!blocks.contains(name), "duplicate consumer block");
        auto block=std::make_unique<Block>(id,name,actor,epoch);
        auto& value=*block; blocks.emplace(name,std::move(block)); return value;
    }
    void erase(const std::string& name) { blocks.erase(name); }
    bool receive(const std::string& name, std::string_view bytes) {
        auto found=blocks.find(name); if(found==blocks.end()) return false;
        return found->second->engine->applyRemoteOp(checked(Crdt::decode_operation(bytes)));
    }
    Resolution resolve(const std::string& name, std::string_view original) const {
        auto found=blocks.find(name);
        if(found==blocks.end()) return {"Gone",{},"missing_block"}; // precedes decode/lookup
        auto decoded=decode_position(original);
        if(!decoded.ok()) return {"Stale",{},std::string(to_string(decoded.error().code()))};
        return std::visit([&](const auto& token)->Resolution {
            using T=std::decay_t<decltype(token)>;
            if constexpr (std::is_same_v<T,TextBoundary> || std::is_same_v<T,TextAttachment>) {
                auto result=found->second->engine->resolve(token);
                if(!result.ok()) return {"Stale",{},std::string(to_string(result.error().code()))};
                auto p=result.value();
                return {p.collapsed()?"Collapsed":"Resolved",p.byte_offset,"",p.lost_left,p.lost_right};
            } else return {"Stale",{},"wrong_primitive"};
        },decoded.value());
    }
};
inline std::unique_ptr<IdListEngine> list(uint8_t actor=1) {
    return checked(IdListEngine::create(std::make_shared<Counter>(actor),{"doc","structure",Primitive::list,1}));
}
} // namespace Consumer
