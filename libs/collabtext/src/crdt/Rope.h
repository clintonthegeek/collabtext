#pragma once

#include "crdt/SumTree.h"
#include <cstdint>
#include <string>
#include <string_view>

namespace CollabText::Crdt {

// ============================================================================
// Chunk — a piece of text stored in the Rope
// ============================================================================

#ifdef COLLABTEXT_TEST_ROPE
static constexpr uint32_t CHUNK_MAX_BYTES = 16;
#else
static constexpr uint32_t CHUNK_MAX_BYTES = 128;
#endif

struct ChunkSummary {
    uint32_t bytes = 0;

    static ChunkSummary zero() { return {0}; }
    void add_summary(const ChunkSummary& other) {
        bytes += other.bytes;
    }
};

/// Seek by byte offset in a Rope.
struct ByteOffset {
    uint32_t value = 0;

    static ByteOffset zero() { return {0}; }
    void add_summary(const ChunkSummary& s) { value += s.bytes; }

    auto operator<=>(const ByteOffset&) const = default;
    bool operator==(const ByteOffset&) const = default;
};

struct Chunk {
    using Summary = ChunkSummary;

    std::string text;

    Chunk() = default;
    explicit Chunk(std::string t) : text(std::move(t)) {}

    ChunkSummary summary() const {
        return {static_cast<uint32_t>(text.size())};
    }
};

// ============================================================================
// Rope — text storage backed by SumTree<Chunk>
// ============================================================================

#ifdef COLLABTEXT_TEST_ROPE
static constexpr std::size_t ROPE_B = 2;
#else
static constexpr std::size_t ROPE_B = 6;
#endif

class Rope {
public:
    Rope() = default;

    /// Create a rope from a string.
    static Rope from(std::string_view text) {
        Rope r;
        r.push_str(text);
        return r;
    }

    /// Append text. Splits into chunks at CHUNK_MAX_BYTES boundaries.
    void push_str(std::string_view text) {
        while (!text.empty()) {
            uint32_t len = std::min(static_cast<uint32_t>(text.size()), CHUNK_MAX_BYTES);
            // Don't split in the middle of a UTF-8 character
            len = adjust_to_char_boundary(text, len);
            m_tree.push_item(Chunk(std::string(text.substr(0, len))));
            text.remove_prefix(len);
        }
    }

    /// Append another rope.
    void append(Rope other) {
        m_tree.push_tree(std::move(other.m_tree));
    }

    /// Total byte length.
    uint32_t len() const {
        return m_tree.summary().bytes;
    }

    bool empty() const { return m_tree.empty() || len() == 0; }

    /// Extract all text as a string.
    std::string to_string() const {
        std::string result;
        result.reserve(len());
        m_tree.for_each([&](const Chunk& chunk) {
            result += chunk.text;
        });
        return result;
    }

    /// Extract a substring [start, start+length).
    std::string substr(uint32_t start, uint32_t length) const {
        std::string result;
        result.reserve(length);

        uint32_t pos = 0;
        m_tree.for_each([&](const Chunk& chunk) {
            uint32_t chunk_end = pos + static_cast<uint32_t>(chunk.text.size());
            if (pos < start + length && chunk_end > start) {
                uint32_t from = (start > pos) ? start - pos : 0;
                uint32_t to = std::min(static_cast<uint32_t>(chunk.text.size()),
                                       start + length - pos);
                result += chunk.text.substr(from, to - from);
            }
            pos = chunk_end;
        });
        return result;
    }

    /// Slice: extract bytes [0, offset) as a new rope, advance past them.
    /// Uses cursor for O(log n) operation.
    Rope slice_to(uint32_t offset) {
        if (offset == 0) return {};
        if (offset >= len()) {
            Rope result;
            std::swap(result.m_tree, m_tree);
            return result;
        }

        auto cursor = m_tree.cursor<ByteOffset>();
        cursor.seek({0}, Bias::Left);

        Rope result;
        // Take whole chunks before the offset
        auto prefix_tree = cursor.slice({offset});

        // The cursor is now at the chunk containing offset
        // Check if we need to split a chunk
        if (cursor.item() && cursor.position().value < offset) {
            uint32_t split_within = offset - cursor.position().value;
            auto& chunk = *cursor.item();
            uint32_t adj = adjust_to_char_boundary(chunk.text, split_within);
            if (adj > 0 && adj < chunk.text.size()) {
                // Split chunk
                prefix_tree.push_item(Chunk(chunk.text.substr(0, adj)));
                // Build suffix: remainder of this chunk + rest
                SumTree<Chunk, ROPE_B> suffix_tree;
                suffix_tree.push_item(Chunk(chunk.text.substr(adj)));
                cursor.next();
                suffix_tree.push_tree(cursor.suffix());
                m_tree = std::move(suffix_tree);
                result.m_tree = std::move(prefix_tree);
                return result;
            }
        }

        auto suffix_tree = cursor.suffix();
        m_tree = std::move(suffix_tree);
        result.m_tree = std::move(prefix_tree);
        return result;
    }

    /// Access the underlying tree (for RopeBuilder and cursor operations).
    const SumTree<Chunk, ROPE_B>& tree() const { return m_tree; }
    SumTree<Chunk, ROPE_B>& tree() { return m_tree; }

private:
    SumTree<Chunk, ROPE_B> m_tree;

    /// Adjust byte length backward to a UTF-8 character boundary.
    static uint32_t adjust_to_char_boundary(std::string_view text, uint32_t len) {
        if (len >= text.size()) return static_cast<uint32_t>(text.size());
        // Walk backward from len to find a char boundary
        while (len > 0 && (static_cast<unsigned char>(text[len]) & 0xC0) == 0x80) {
            len--;
        }
        return len;
    }
};

} // namespace CollabText::Crdt
