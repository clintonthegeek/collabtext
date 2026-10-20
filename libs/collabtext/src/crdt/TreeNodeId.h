#pragma once

#include <atomic>
#include <compare>
#include <cstdint>
#include <optional>
#include <utility>

namespace CollabText::Crdt {

struct TreeNamespace {
    uint64_t value = 0;
    constexpr bool valid() const noexcept { return value != 0; }
    auto operator<=>(const TreeNamespace&) const = default;
};

struct NodeId {
    TreeNamespace tree;
    uint64_t local = 0;
    constexpr bool valid() const noexcept { return tree.valid() && local != 0; }
    auto operator<=>(const NodeId&) const = default;
};

/// Checked state-local node identity allocator. Copies intentionally retain
/// their namespace and next counter; each derived state owns its own copy.
class NodeIdAllocator {
public:
    static std::optional<NodeIdAllocator> create() noexcept {
        static std::atomic<uint64_t> next_namespace{1};
        uint64_t candidate = next_namespace.load(std::memory_order_relaxed);
        while (candidate != 0 && candidate != UINT64_MAX) {
            if (next_namespace.compare_exchange_weak(
                    candidate, candidate + 1, std::memory_order_relaxed,
                    std::memory_order_relaxed))
                return NodeIdAllocator{TreeNamespace{candidate}, 1};
        }
        return std::nullopt;
    }

    constexpr NodeIdAllocator() = default;
    constexpr NodeIdAllocator(TreeNamespace tree, uint64_t next_local) noexcept
        : m_tree(tree), m_next_local(next_local) {}

    std::optional<NodeId> allocate() noexcept {
        if (!m_tree.valid() || m_exhausted || m_next_local == 0)
            return std::nullopt;
        const NodeId id{m_tree, m_next_local};
        if (m_next_local == UINT64_MAX) {
            m_exhausted = true;
        } else {
            ++m_next_local;
        }
        return id;
    }

    constexpr TreeNamespace tree_namespace() const noexcept { return m_tree; }
    constexpr uint64_t next_local() const noexcept { return m_next_local; }
    constexpr bool exhausted() const noexcept { return m_exhausted; }

private:
    TreeNamespace m_tree;
    uint64_t m_next_local = 1;
    bool m_exhausted = false;
};

struct NoNodeId {
    constexpr bool valid() const noexcept { return true; }
};

/// Empty policy used by ordinary SumTrees. Its [[no_unique_address]] ID and
/// allocator members add no per-node or per-tree state.
struct NoNodeIds {
    using Id = NoNodeId;
    constexpr Id allocate() noexcept { return {}; }
    constexpr bool valid() const noexcept { return true; }
};

/// Stable node-ID policy for a FragmentState lineage.
class StableNodeIds {
public:
    using Id = NodeId;

    static std::optional<StableNodeIds> create() noexcept {
        auto allocator = NodeIdAllocator::create();
        if (!allocator) return std::nullopt;
        return StableNodeIds{*allocator};
    }

    constexpr StableNodeIds() = default;
    explicit constexpr StableNodeIds(NodeIdAllocator allocator) noexcept
        : m_allocator(std::move(allocator)) {}

    Id allocate() noexcept {
        const auto next = m_allocator.allocate();
        if (!next) {
            m_failed = true;
            return {};
        }
        return *next;
    }

    bool valid() const noexcept {
        return !m_failed && m_allocator.tree_namespace().valid();
    }
    TreeNamespace tree_namespace() const noexcept {
        return m_allocator.tree_namespace();
    }

private:
    NodeIdAllocator m_allocator;
    bool m_failed = false;
};

} // namespace CollabText::Crdt
