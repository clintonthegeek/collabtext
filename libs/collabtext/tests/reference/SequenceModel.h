#pragma once
// Independent sequence reference interpreter (roadmap task S02).
//
// It computes the item order the specification calls for. It deliberately does
// NOT use production ordering, Locator, Identity allocation, Buffer, IdList,
// Fragment, anchor or codec helpers: the point is to have a second opinion that
// cannot inherit the defect it is checking. BoundaryModel is likewise independent
// and consumes the order this file produces.
//
// The rule, in one paragraph. Every item records the item it was inserted after
// (its *parent*; START when at the document start). Those parent links form a
// tree that depends only on the operations, never on arrival order. The
// document's order is the pre-order walk of that tree, and the children of one
// parent are ordered by two rules in this priority:
//
//   1. A group inserted immediately *before* a sibling group precedes it. This
//      is what makes reverse typing reverse: the third character typed before
//      the second is a child of START whose right boundary is the second, and
//      must come before it.
//   2. Otherwise (both groups at the same position, typically concurrent) the
//      groups are ordered by their first inserted item, counter first and actor
//      bytes second -- and the whole group moves together.
//
// Rule 2 is the F8 repair. Production atomizes a same-locator run into single
// scalars and orders them by per-scalar identity, which interleaves two
// concurrent three-character pastes into `aXbYcZ`. Ordering whole groups by
// first-item identity keeps each paste contiguous (`abcXYZ`), which is what the
// S01 fixtures require and what prose needs.
//
// Undo hides the owner's group without removing it, so identity and relative
// order of retained items survive; redo reveals it again.

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace SequenceReference {

inline constexpr std::string_view kStart = "START";
inline constexpr std::string_view kEnd = "END";

/// `actor/counter`, the fixture's identity spelling.
struct ItemId {
    std::string actor;
    uint64_t counter = 0;
    bool operator==(const ItemId&) const = default;
};

inline std::optional<ItemId> parse_id(std::string_view text) {
    const auto slash = text.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 >= text.size()) return std::nullopt;
    uint64_t counter = 0;
    for (size_t at = slash + 1; at < text.size(); ++at) {
        const char c = text[at];
        if (c < '0' || c > '9') return std::nullopt;
        if (counter > (UINT64_MAX - 9) / 10) return std::nullopt;
        counter = counter * 10 + uint64_t(c - '0');
    }
    return ItemId{std::string(text.substr(0, slash)), counter};
}

inline std::string format_id(const ItemId& id) {
    return id.actor + "/" + std::to_string(id.counter);
}

/// Counter first, then actor bytes -- the S01 branch rule. `actor_bytes` maps a
/// fixture alias to its 128-bit value so the comparison is over the same bytes
/// production would compare, not over alias spellings.
class IdentityOrder {
    std::map<std::string, std::string> actor_bytes_;
public:
    IdentityOrder() = default;
    explicit IdentityOrder(std::map<std::string, std::string> bytes) : actor_bytes_(std::move(bytes)) {}
    bool operator()(const ItemId& a, const ItemId& b) const {
        if (a.counter != b.counter) return a.counter < b.counter;
        const auto left = actor_bytes_.find(a.actor);
        const auto right = actor_bytes_.find(b.actor);
        if (left != actor_bytes_.end() && right != actor_bytes_.end() && left->second != right->second)
            return left->second < right->second;
        return a.actor < b.actor;
    }
};

struct Operation {
    enum class Kind { insert, remove, undo, redo };
    std::string id;
    Kind kind = Kind::insert;
    std::vector<std::string> after;
    std::string between_left = std::string(kStart);
    std::string between_right = std::string(kEnd);
    std::vector<std::pair<std::string, std::string>> items; // id, value
    std::vector<std::string> targets;                       // remove
    std::string target;                                     // undo / redo
};

struct Item {
    std::string id;
    std::string value;
    std::string parent = std::string(kStart);
    std::string group;
    bool removed = false;   // deleted by any replica
    bool undone = false;    // hidden by its owner's undo
    bool visible() const { return !removed && !undone; }
};

struct ReplayReport {
    std::size_t integrated = 0;
    std::size_t deferred = 0;   // arrived before a causal prerequisite
    std::size_t drains = 0;     // deferral passes that made progress
    std::size_t duplicate = 0;
    bool complete = false;      // every operation in the order was integrated
};

class Document {
    bool list_ = false;
    IdentityOrder identity_;
    std::map<std::string, Item> items_;
    std::map<std::string, std::string> item_group_;                  // item -> owning operation
    std::map<std::string, std::string> group_parent_;
    std::map<std::string, std::string> group_before_;               // group -> right boundary at mint
    std::map<std::string, std::vector<std::string>> children_;      // parent -> groups
    std::map<std::string, std::vector<std::string>> group_items_;   // group -> its items
    std::set<std::string> undone_;
    std::vector<std::string> deferred_;

    const std::vector<std::string>& group_items(const std::string& group) const {
        static const std::vector<std::string> empty;
        auto found = group_items_.find(group);
        return found == group_items_.end() ? empty : found->second;
    }
    const std::vector<std::string>& children_of(const std::string& parent) const {
        static const std::vector<std::string> empty;
        auto found = children_.find(parent);
        return found == children_.end() ? empty : found->second;
    }

    /// True when `id` is one of `group`'s items or sits under one of them. This
    /// is how "inserted immediately before" is recognised without positions.
    bool in_span(const std::string& group, const std::string& id) const {
        for (const auto& item : group_items(group)) {
            if (item == id) return true;
            auto at = items_.find(id);
            while (at != items_.end() && at->second.parent != std::string(kStart)) {
                if (at->second.parent == item) return true;
                at = items_.find(at->second.parent);
            }
        }
        return false;
    }

    /// `group` comes before any group inserted immediately before it.
    void emit_before(const std::string& parent, const std::string& group,
                     std::vector<std::string>& out, std::set<std::string>& done) const {
        auto sorted = children_of(parent);
        std::sort(sorted.begin(), sorted.end(), [&](const std::string& a, const std::string& b) {
            return identity_(first_item(a), first_item(b));
        });
        for (const auto& other : sorted) {
            if (done.count(other)) continue;
            const auto& before = group_before_.at(other);
            if (before != std::string(kEnd) && in_span(group, before)) emit_before(parent, other, out, done);
        }
        if (done.insert(group).second) out.push_back(group);
    }

    ItemId first_item(const std::string& group) const {
        const auto& items = group_items(group);
        const auto parsed = items.empty() ? std::nullopt : parse_id(items.front());
        return parsed ? *parsed : ItemId{group, 0};
    }

    std::vector<std::string> ordered_groups(const std::string& parent) const {
        auto groups = children_of(parent);
        // Only groups whose right boundary is not a sibling participate in the
        // precedence pass; the rest are appenders and keep identity order.
        std::vector<std::string> appenders;
        for (const auto& group : groups) {
            const auto& before = group_before_.at(group);
            if (before == std::string(kEnd)) { appenders.push_back(group); continue; }
            auto owning = item_group_.find(before);
            if (owning == item_group_.end() || group_parent_.at(owning->second) != parent) {
                appenders.push_back(group);
            }
        }
        std::sort(appenders.begin(), appenders.end(), [&](const std::string& a, const std::string& b) {
            return identity_(first_item(a), first_item(b));
        });
        std::vector<std::string> out;
        std::set<std::string> done;
        for (const auto& group : appenders) emit_before(parent, group, out, done);
        // A cycle or a missing reference must not silently drop an operation.
        for (const auto& group : groups) emit_before(parent, group, out, done);
        return out;
    }

    void walk(const std::string& parent, std::vector<std::string>& out) const {
        for (const auto& group : ordered_groups(parent)) {
            for (const auto& id : group_items(group)) {
                out.push_back(id);
                walk(id, out);
            }
        }
    }

public:
    Document(bool list, IdentityOrder identity)
        : list_(list), identity_(std::move(identity)) {}

    /// The fixture's initial items are already observed through the seed. Each is
    /// its own single-item group appended at the end, so the seed is a document
    /// state and not a local edit each replica would have to mint.
    void seed(const std::vector<std::pair<std::string, std::string>>& initial) {
        for (const auto& [id, value] : initial) {
            const std::string group = "seed:" + id;
            Item item;
            item.id = id; item.value = value;
            item.parent = std::string(kStart);
            item.group = group;
            items_.emplace(id, item);
            item_group_[id] = group;
            group_parent_[group] = std::string(kStart);
            group_before_[group] = std::string(kEnd);
            group_items_[group] = {id};
            children_[std::string(kStart)].push_back(group);
        }
    }

    std::size_t deferred_count() const { return deferred_.size(); }
    const std::vector<std::string>& deferred() const { return deferred_; }

    /// Integrate one operation. Returns false when a causal prerequisite has not
    /// arrived; the caller defers and retries rather than inventing history.
    bool integrate(const Operation& op) {
        for (const auto& parent : op.after) {
            if (applied_.count(parent) == 0) return false;
        }
        switch (op.kind) {
            case Operation::Kind::insert: {
                const std::string& parent = op.between_left;
                if (parent != std::string(kStart) && !items_.count(parent)) return false;
                group_parent_[op.id] = parent;
                group_before_[op.id] = op.between_right;
                auto& ids = group_items_[op.id];
                for (const auto& [id, value] : op.items) {
                    Item item;
                    item.id = id; item.value = value; item.parent = parent; item.group = op.id;
                    items_.emplace(id, item);
                    item_group_[id] = op.id;
                    ids.push_back(id);
                }
                children_[parent].push_back(op.id);
                break;
            }
            case Operation::Kind::remove: {
                for (const auto& id : op.targets) {
                    auto found = items_.find(id);
                    if (found == items_.end()) return false;
                    found->second.removed = true;
                }
                removed_ops_.insert(op.id);
                break;
            }
            case Operation::Kind::undo: {
                // The target names the original *operation*, not an item, so the
                // group is looked up by operation id.
                if (!group_items_.count(op.target)) return false;
                for (const auto& id : group_items(op.target))
                    if (auto item = items_.find(id); item != items_.end()) item->second.undone = true;
                undone_.insert(op.target);
                undone_ops_.insert(op.id);
                break;
            }
            case Operation::Kind::redo: {
                // Redo must clear the per-item flag, not just the record of it:
                // leaving the flag set is how an undone insert stays invisible
                // after it has been restored.
                undone_.erase(op.target);
                if (group_items_.count(op.target))
                    for (const auto& id : group_items(op.target))
                        if (auto item = items_.find(id); item != items_.end()) item->second.undone = false;
                undone_ops_.insert(op.id);
                break;
            }
        }
        applied_.insert(op.id);
        return true;
    }

    /// Deliver `order`, deferring anything whose prerequisites are missing and
    /// draining afterwards. Duplicates are counted, never reapplied.
    ReplayReport replay(const std::vector<Operation>& operations, const std::vector<std::string>& order) {
        std::map<std::string, const Operation*> by_id;
        for (const auto& op : operations) by_id[op.id] = &op;
        ReplayReport report;
        std::vector<std::string> pending;
        for (const auto& id : order) {
            const Operation* op = by_id.find(id) == by_id.end() ? nullptr : by_id.at(id);
            if (op == nullptr) continue;
            if (applied_.count(id)) { ++report.duplicate; continue; }
            if (integrate(*op)) { ++report.integrated; continue; }
            pending.push_back(id);
            ++report.deferred;
        }
        // Drain: an operation whose parent arrived later becomes applicable. A
        // full pass that integrates nothing means the remaining operations are
        // genuinely not deliverable from this order.
        for (bool progress = true; !pending.empty() && progress;) {
            progress = false;
            for (size_t at = 0; at < pending.size();) {
                const Operation* op = by_id.at(pending[at]);
                if (integrate(*op)) { pending.erase(pending.begin() + static_cast<ptrdiff_t>(at)); ++report.integrated; progress = true; ++report.drains; }
                else ++at;
            }
        }
        deferred_ = pending;
        report.complete = pending.empty() && report.integrated + report.duplicate == order.size();
        return report;
    }

    std::vector<std::string> order() const {
        std::vector<std::string> out;
        walk(std::string(kStart), out);
        return out;
    }
    std::vector<std::string> hidden() const {
        std::vector<std::string> out;
        for (const auto& [id, item] : items_) if (!item.visible()) out.push_back(id);
        return out;
    }
    std::vector<std::string> visible_order() const {
        std::vector<std::string> out;
        for (const auto& id : order()) if (items_.at(id).visible()) out.push_back(id);
        return out;
    }
    bool text_primitive() const { return !list_; }
    const std::map<std::string, Item>& items() const { return items_; }

    /// Visible UTF-8 text for the text primitive. List primitives have no text;
    /// their visible order is their content.
    std::string text() const {
        std::string out;
        for (const auto& id : visible_order()) out += items_.at(id).value;
        return out;
    }

private:
    std::set<std::string> applied_;
    std::set<std::string> removed_ops_;
    std::set<std::string> undone_ops_;
};

} // namespace SequenceReference