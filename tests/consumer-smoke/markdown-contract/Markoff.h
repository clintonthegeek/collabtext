#pragma once
// Markoff-shaped consumer composition over the installed engine.
//
// Deliberately independent of the Wave-shaped fixture next door: it shares no
// adapter, no expectation table and no support header with it, because the point
// of a second consumer is that it reaches the same library a different way. What
// this file owns is the Markdown bytes, the block map and the transport. What the
// engine owns is the characters inside one block, and nothing else -- it has no
// concept of a document, a block, or Markdown.
//
// There is deliberately no Wave event log, no engine-owned block tree, no Qt
// widget and no DocumentSession/file store here. Transport is an optional
// interface the consumer supplies; with nothing attached, edits stay local.

#include <collabtext/CrdtEngine.h>
#include <collabtext/Serialization.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace Markoff {
using namespace CollabText;

inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class T> T checked(Result<T> result) {
    if (!result.ok()) throw std::runtime_error(std::string(to_string(result.error().code())) + ": " + std::string(result.error().message()));
    return std::move(result).value();
}

/// A consumer's own durable-allocator obligation. The library hands out no
/// identities by itself, so this is where the "which actor am I" fact lives.
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

/// One block in the consumer's sibling map. `kind`, `stream` and `original` are
/// consumer facts; the engine below is handed `stream` and never learns anything
/// else.
struct Block {
    uint64_t id = 0;
    std::string kind;
    std::string stream;
    std::string original;
    std::unique_ptr<CrdtEngine> engine;
    /// Already-encoded opaque operation frames, in emission order. The consumer
    /// queues and routes these; it never inspects a payload field.
    std::vector<std::string> outbox;
};

/// Optional. Anything that can carry an opaque frame, including nothing at all.
class Transport {
public:
    virtual ~Transport() = default;
    virtual void deliver(std::string_view bytes) = 0;
};

class InMemoryWire final : public Transport {
    std::vector<std::string> frames_;
public:
    void deliver(std::string_view bytes) override { frames_.emplace_back(bytes); }
    size_t frame_count() const noexcept { return frames_.size(); }
    const std::vector<std::string>& frames() const noexcept { return frames_; }
};

class Document {
    std::string id_;
    uint8_t actor_;
    uint64_t epoch_;
    std::string trailer_;          // the bytes after the final block, kept exactly
    std::vector<uint64_t> order_;  // consumer block order, not an engine concept
    std::map<uint64_t, Block> blocks_;
    Transport* transport_ = nullptr;

    static std::string classify(std::string_view body) {
        if (body.starts_with("#")) return "heading";
        if (body.starts_with("- ") || body.starts_with("* ")) return "list";
        return "paragraph";
    }

public:
    /// Blocks exist and hold their bytes, but nothing has been handed to an
    /// engine yet. This is the shape a replica takes before initial sync: it can
    /// receive the document's frames without having minted any of its own.
    static Document open(std::string markdown, std::string id, uint8_t actor, uint64_t epoch = 1) {
        Document out;
        out.id_ = std::move(id);
        out.actor_ = actor;
        out.epoch_ = epoch;
        if (markdown.ends_with("\n")) { markdown.pop_back(); out.trailer_ = "\n"; }
        std::vector<std::string> bodies;
        if (!markdown.empty()) {
            for (;;) {
                const auto next = markdown.find("\n\n");
                if (next == std::string::npos) { bodies.push_back(std::move(markdown)); break; }
                bodies.push_back(markdown.substr(0, next));
                markdown.erase(0, next + 2);
            }
        }
        uint64_t next_id = 1;
        for (auto& body : bodies) {
            Block block;
            block.id = next_id;
            block.kind = classify(body);
            block.stream = "block-" + std::to_string(next_id);
            block.original = std::move(body);
            block.engine = checked(CrdtEngine::create(std::make_shared<Counter>(actor),
                                                      PositionScope{out.id_, block.stream, Primitive::text, epoch}));
            out.order_.push_back(next_id);
            out.blocks_.emplace(next_id, std::move(block));
            // Attach before seeding: a seed insert is itself a local operation
            // the consumer would otherwise have to special-case.
            out.attach(*out.blocks_.at(next_id).engine);
            ++next_id;
        }
        return out;
    }

    /// Blocks are separated by a blank line; a single trailing newline is the
    /// document's own and is kept separately rather than glued to the last
    /// block, so `render` can rebuild the input byte for byte.
    static Document parse(std::string markdown, std::string id, uint8_t actor, uint64_t epoch = 1) {
        Document out = open(std::move(markdown), std::move(id), actor, epoch);
        out.seed();
        return out;
    }

    /// Put each block's own bytes into its engine as ordinary local edits, which
    /// is what makes them ordinary operations a peer can be sent.
    void seed() {
        for (auto id : order_) {
            auto& block = at(id);
            if (!block.original.empty()) block.engine->insert(0, block.original);
        }
    }

    /// The consumer's own routing. It encodes what the engine emitted and hands
    /// the bytes on; it reads no payload field, because there are none to read.
    void attach(CrdtEngine& engine) {
        engine.setOnLocalOp([this, &engine](const Crdt::Operation& op) {
            const auto frame = checked(Crdt::encode_operation(op));
            blocks_.at(block_of(engine)).outbox.push_back(frame);
            if (transport_ != nullptr) transport_->deliver(frame);
        });
    }

    uint64_t block_of(const CrdtEngine& engine) const {
        for (const auto& entry : blocks_)
            if (entry.second.engine.get() == &engine) return entry.first;
        throw std::runtime_error("engine is not one of this document's blocks");
    }

    void set_transport(Transport* transport) { transport_ = transport; }

    size_t block_count() const noexcept { return blocks_.size(); }
    std::vector<std::string> kinds() const {
        std::vector<std::string> out;
        for (auto id : order_) out.push_back(blocks_.at(id).kind);
        return out;
    }

    Block& at(uint64_t id) {
        auto found = blocks_.find(id);
        require(found != blocks_.end(), "no such consumer block");
        return found->second;
    }
    const Block& at(uint64_t id) const {
        auto found = blocks_.find(id);
        require(found != blocks_.end(), "no such consumer block");
        return found->second;
    }

    /// Block lifecycle is the consumer's, not the engine's.
    void erase(uint64_t id) {
        auto found = blocks_.find(id);
        require(found != blocks_.end(), "no such consumer block");
        order_.erase(std::remove(order_.begin(), order_.end(), id), order_.end());
        blocks_.erase(found);
    }

    std::string body(uint64_t id) const { return at(id).engine->text(); }

    /// Exact bytes: the surviving block bodies rejoined with the same separator,
    /// plus the same trailing newline this document was parsed with.
    std::string render() const {
        std::string out;
        for (size_t n = 0; n < order_.size(); ++n) {
            if (n) out += "\n\n";
            out += blocks_.at(order_[n]).engine->text();
        }
        return out + trailer_;
    }

    /// `byte` is a UTF-8 byte offset at a scalar boundary; editing is in UTF-16
    /// code units. The asymmetry is the engine's documented one, not this
    /// fixture's, and these bodies stay ASCII where the two are compared.
    std::string mint(uint64_t id, uint32_t byte, Affinity affinity, bool strict) const {
        auto found = blocks_.find(id);
        require(found != blocks_.end(), "no such consumer block");
        auto& engine = *found->second.engine;
        if (strict)
            return checked(encode_position(checked(engine.attachment_at(byte, affinity, ReferentSide::following))));
        return checked(encode_position(checked(engine.boundary_at(byte, affinity))));
    }

    /// The consumer's state vocabulary. Deletion is answered from the block map,
    /// before any decode or item lookup, and a collapsed caret keeps its offset:
    /// collapse is a usable answer, not an error, and it is never reported as an
    /// intact strict attachment.
    Resolution resolve(uint64_t id, std::string_view original) const {
        auto found = blocks_.find(id);
        if (found == blocks_.end()) return {"Gone", {}, "missing_block"};
        auto decoded = decode_position(original);
        if (!decoded.ok()) return {"Stale", {}, std::string(to_string(decoded.error().code()))};
        return std::visit([&](const auto& token) -> Resolution {
            using T = std::decay_t<decltype(token)>;
            if constexpr (std::is_same_v<T, TextBoundary> || std::is_same_v<T, TextAttachment>) {
                auto result = found->second.engine->resolve(token);
                if (!result.ok()) return {"Stale", {}, std::string(to_string(result.error().code()))};
                auto p = result.value();
                return {p.collapsed() ? "Collapsed" : "Resolved", p.byte_offset, "", p.lost_left, p.lost_right};
            } else {
                return {"Stale", {}, "wrong_primitive"};
            }
        }, decoded.value());
    }

    /// Apply a transported frame to a named block. The bytes are decoded by the
    /// library's checked codec; the consumer neither trusts nor edits them.
    bool receive(uint64_t id, std::string_view bytes) {
        auto found = blocks_.find(id);
        if (found == blocks_.end()) return false;
        const auto outcome = found->second.engine->apply_remote(
            checked(Crdt::decode_operation(bytes)));
        return outcome.kind() == ApplyOutcomeKind::applied ||
               outcome.kind() == ApplyOutcomeKind::duplicate;
    }
};

} // namespace Markoff
