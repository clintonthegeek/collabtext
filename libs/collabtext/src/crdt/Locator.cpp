#include "crdt/Locator.h"
#include <cassert>
#include <limits>

namespace CollabText::Crdt {

static constexpr uint64_t DMIN = 0;
static constexpr uint64_t DMAX = std::numeric_limits<uint64_t>::max();

// Bias the midpoint to lean toward whichever bound is closer to a boundary.
// For appends (lo near some value, hi = DMAX): lean toward lo, giving ~65536
// appends per depth level. For prepends (lo = DMIN, hi near some value):
// lean toward hi, giving the same ~65536 prepends per depth level.
static uint64_t biased_mid(uint64_t lo, uint64_t hi) {
    assert(lo < hi);
    uint64_t gap = hi - lo;
    if (gap == 1) return lo;  // No integer strictly between; caller must descend

    // Detect prepend pattern: lo is close to DMIN relative to hi.
    // Use hi-biased allocation (step from hi downward) to prevent
    // depth explosion for prepend-heavy editing.
    bool prepend_bias = (lo < (hi >> 1));

    if (prepend_bias) {
        uint64_t step = gap >> 48;
        if (step == 0) step = 1;
        return hi - step;
    } else {
        uint64_t step = gap >> 48;
        if (step == 0) step = 1;
        return lo + step;
    }
}

Locator Locator::min() {
    return Locator(std::vector<uint64_t>{DMIN});
}

Locator Locator::max() {
    return Locator(std::vector<uint64_t>{DMAX});
}

Locator Locator::between(const Locator &lo, const Locator &hi) {
    assert(lo <= hi);  // Allow equal; algorithm descends to next digit level

    // Walk through digit levels. At each level we try to find a value
    // strictly between lo's digit and hi's digit at that level.
    // If the digits are adjacent (or equal), we descend to the next level.

    auto lo_digit = [&](size_t i) -> uint64_t {
        return i < lo.m_digits.size() ? lo.m_digits[i] : DMIN;
    };
    auto hi_digit = [&](size_t i) -> uint64_t {
        return i < hi.m_digits.size() ? hi.m_digits[i] : DMAX;
    };

    std::vector<uint64_t> result;
    size_t maxDepth = lo.m_digits.size() + hi.m_digits.size() + 2;

    for (size_t level = 0; level < maxDepth; ++level) {
        uint64_t ld = lo_digit(level);
        uint64_t hd = hi_digit(level);

        if (ld == hd) {
            // Same digit at this level — must descend
            result.push_back(ld);
            continue;
        }

        // ld < hd at this level. But we need to be careful:
        // if lo has more digits below and we pick ld, we'd be <= lo.
        // If we can pick ld + 1 < hd, or a biased midpoint, we're done.

        // Check if lo has trailing digits that make it > (result..., ld, 0, 0, ...)
        bool lo_has_more = (level + 1 < lo.m_digits.size());

        if (lo_has_more) {
            // lo = (result..., ld, ...) with more digits.
            // We need our result > lo, so we can't use ld (lo has trailing digits > 0).
            // Try ld + 1. If ld + 1 < hd, use biased_mid(ld, hd) which is >= ld + 1.
            if (ld + 1 < hd) {
                // There's room above ld+1 up to hd-1
                uint64_t mid = biased_mid(ld, hd);
                result.push_back(mid);
                return Locator(result);
            }
            // ld + 1 == hd. We can't fit between them at this level.
            // We need to descend below ld: result = (result..., ld, between(lo_rest, MAX))
            result.push_back(ld);
            // Continue descending. lo's next digit is lo_digit(level+1),
            // hi's next digit is effectively DMAX since hd > ld means
            // anything with prefix (result..., ld, ...) is < hi.
            // Now we're finding between lo_rest and {DMAX}.
            for (size_t sub = level + 1; ; ++sub) {
                uint64_t sub_lo = lo_digit(sub);
                uint64_t sub_hi = DMAX;

                if (sub_lo < sub_hi) {
                    bool sub_lo_has_more = (sub + 1 < lo.m_digits.size());
                    uint64_t effective_lo = sub_lo_has_more ? sub_lo : sub_lo;
                    if (sub_lo_has_more) {
                        if (sub_lo + 1 <= sub_hi) {
                            uint64_t mid = biased_mid(sub_lo, sub_hi);
                            result.push_back(mid);
                            return Locator(result);
                        }
                        result.push_back(sub_lo);
                        continue;
                    }
                    uint64_t mid = biased_mid(sub_lo, sub_hi);
                    result.push_back(mid);
                    return Locator(result);
                }
                // sub_lo == sub_hi == DMAX shouldn't happen since lo < hi
                result.push_back(sub_lo);
            }
        }

        // lo has no more digits. (result..., ld) == lo at this depth.
        // Need a value strictly > ld and strictly < hd.
        if (ld + 1 < hd) {
            uint64_t mid = biased_mid(ld, hd);
            result.push_back(mid);
            return Locator(result);
        }
        // ld + 1 == hd: no integer strictly between at this level.
        // Descend below ld. Since lo has no more digits,
        // (..., ld, X) where X > 0 is strictly > lo and strictly < hi.
        result.push_back(ld);
        result.push_back(biased_mid(DMIN, DMAX));
        return Locator(result);
    }

    // Should never reach here for valid inputs
    assert(false && "between() failed to find a position");
    return lo;
}

std::strong_ordering Locator::operator<=>(const Locator &other) const {
    size_t len = std::max(m_digits.size(), other.m_digits.size());
    for (size_t i = 0; i < len; ++i) {
        uint64_t a = i < m_digits.size() ? m_digits[i] : DMIN;
        uint64_t b = i < other.m_digits.size() ? other.m_digits[i] : DMIN;
        if (a < b) return std::strong_ordering::less;
        if (a > b) return std::strong_ordering::greater;
    }
    return std::strong_ordering::equal;
}

} // namespace CollabText::Crdt
