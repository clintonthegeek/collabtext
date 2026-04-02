#pragma once

#include <cstdint>
#include <compare>
#include <vector>

namespace CollabText::Crdt {

/// Fractional position identifier for ordering characters in the CRDT.
///
/// Each Locator is a variable-length sequence of uint64_t "digits" that
/// defines a position in an infinitely divisible space. Comparison is
/// lexicographic over the digit sequence.
///
/// The `between()` factory uses a biased-midpoint algorithm (>> 48 bias)
/// that keeps depth shallow for sequential appends (depth 1 for thousands
/// of appends) while still handling adversarial interleaving gracefully.
class Locator {
public:
    Locator() = default;
    explicit Locator(std::vector<uint64_t> digits) : m_digits(std::move(digits)) {}
    explicit Locator(uint64_t single) : m_digits{single} {}

    /// Smallest possible locator.
    static Locator min();

    /// Largest possible locator.
    static Locator max();

    /// Produce a Locator strictly between `lo` and `hi`.
    /// Precondition: lo < hi.
    static Locator between(const Locator &lo, const Locator &hi);

    const std::vector<uint64_t> &digits() const { return m_digits; }
    size_t depth() const { return m_digits.size(); }

    std::strong_ordering operator<=>(const Locator &other) const;
    bool operator==(const Locator &other) const = default;

private:
    std::vector<uint64_t> m_digits;
};

} // namespace CollabText::Crdt
