#include "crdt/SequenceOrder.h"

#include "crdt/Fragment.h"
#include "crdt/IdList.h"

#include <algorithm>
#include <unordered_map>

namespace CollabText::Crdt::SequenceOrder {
namespace {

/// A position in the flat item sequence: fragment index and offset within it.
struct Pos {
    size_t frag = 0;
    uint32_t off = 0;
    bool operator==(const Pos&) const = default;
    auto operator<=>(const Pos&) const = default;
};

using MaybeId = std::optional<Identity>;

/// Per-actor sorted run starts, so an item identity resolves to its fragment in
/// O(log F). Built once per call; a fragment's items are consecutive counters of
/// one actor, and no two fragments of an actor overlap.
template <class Run>
class Index {
public:
    explicit Index(const std::vector<Run>& frags) : m_frags(frags) {
        for (size_t i = 0; i < frags.size(); ++i)
            m_by_actor[frags[i].origin.actor].emplace_back(frags[i].origin.counter, i);
        for (auto& [_, runs] : m_by_actor) std::sort(runs.begin(), runs.end());
    }

    std::optional<Pos> find(const Identity& id) const {
        const auto found = m_by_actor.find(id.actor);
        if (found == m_by_actor.end()) return std::nullopt;
        const auto& runs = found->second;
        auto at = std::upper_bound(runs.begin(), runs.end(), id.counter,
                                   [](uint64_t c, const auto& run) { return c < run.first; });
        if (at == runs.begin()) return std::nullopt;
        --at;
        const Run& f = m_frags[at->second];
        if (id.counter >= f.origin.counter + f.length) return std::nullopt;
        return Pos{at->second, static_cast<uint32_t>(id.counter - f.origin.counter)};
    }

private:
    const std::vector<Run>& m_frags;
    std::unordered_map<ActorId, std::vector<std::pair<uint64_t, size_t>>, ActorIdHash> m_by_actor;
};

template <class Run>
class Walker {
public:
    explicit Walker(const std::vector<Run>& frags) : m_f(frags), m_ix(frags) {}

    const Index<Run>& index() const { return m_ix; }
    bool at_end(Pos p) const { return p.frag >= m_f.size(); }
    Identity id(Pos p) const { return m_f[p.frag].item_at(p.off); }
    Link link(Pos p) const { return m_f[p.frag].link_at(p.off); }
    Pos next(Pos p) const {
        if (p.off + 1 < m_f[p.frag].length) return {p.frag, p.off + 1};
        return {p.frag + 1, 0};
    }

    /// The ancestor-or-self of `x` that is a direct child of `ancestor` (the root
    /// when empty), or nothing when `x` is not a strict descendant of it.
    ///
    /// Climbs a fragment at a time: the items of a run are a chain, so every item
    /// before `x` in its own fragment is an ancestor, and only the run's first
    /// item has a parent elsewhere.
    std::optional<Pos> top_child(Pos x, const MaybeId& ancestor) const {
        Pos cur = x;
        for (size_t guard = 0; guard <= m_f.size() + 1; ++guard) {
            const Run& f = m_f[cur.frag];
            if (ancestor && f.origin.actor == ancestor->actor &&
                ancestor->counter >= f.origin.counter &&
                ancestor->counter < f.origin.counter + cur.off) {
                return Pos{cur.frag, static_cast<uint32_t>(ancestor->counter - f.origin.counter) + 1};
            }
            if (f.link.parent == ancestor) return Pos{cur.frag, 0};
            if (!f.link.parent) return std::nullopt;
            const auto up = m_ix.find(*f.link.parent);
            if (!up) return std::nullopt;
            cur = *up;
        }
        return std::nullopt;   // a cycle: only a corrupt history can produce one
    }

    bool descendant(Pos x, const MaybeId& ancestor) const { return top_child(x, ancestor).has_value(); }

    /// First position at or after `from` that is neither `root` nor inside its
    /// subtree. Whole fragments are skipped: once an item is in the subtree, so is
    /// every later item of its run.
    Pos skip_subtree(Pos from, const Identity& root) const {
        Pos pos = from;
        while (!at_end(pos)) {
            if (id(pos) == root || descendant(pos, root)) pos = {pos.frag + 1, 0};
            else break;
        }
        return pos;
    }

    /// Where a right origin sits in list order; the end of the list sorts last.
    /// Nothing means the origin is not in the list at all.
    std::optional<Pos> rank(const MaybeId& right_origin) const {
        if (!right_origin) return Pos{m_f.size() + 1, 0};
        return m_ix.find(*right_origin);
    }

private:
    const std::vector<Run>& m_f;
    Index<Run> m_ix;
};

}  // namespace

template <class Run>
std::optional<ItemPos> find_item(const std::vector<Run>& frags, const Identity& id) {
    const Index<Run> ix(frags);
    if (const auto at = ix.find(id)) return ItemPos{at->frag, at->off};
    return std::nullopt;
}

template <class Run>
Link mint_link(const std::vector<Run>& frags, const std::optional<Identity>& left_origin) {
    Walker<Run> w(frags);
    Pos after{0, 0};
    if (left_origin) {
        const auto at = w.index().find(*left_origin);
        if (!at) return Link{};   // callers pass an element they just found
        after = w.next(*at);
    }
    const MaybeId right = w.at_end(after) ? MaybeId{} : MaybeId{w.id(after)};

    // The left origin has a right child exactly when the element after it is in its
    // subtree (the root has one whenever the list is not empty). Then the new
    // element must go on the LEFT of that element, which is adjacent.
    const bool has_right_child = left_origin ? (right && w.descendant(after, left_origin))
                                             : right.has_value();
    if (!has_right_child) return Link{left_origin, Side::Right, right};
    return Link{right, Side::Left, right};
}

template <class Run>
bool integrate(std::vector<Run>& frags, Run incoming) {
    Pos at;
    {
        Walker<Run> w(frags);
        const Link& link = incoming.link;
        if (link.side == Side::Right) {
            Pos pos{0, 0};
            if (link.parent) {
                const auto parent = w.index().find(*link.parent);
                if (!parent) return false;
                pos = w.next(*parent);
            }
            // The right children of `parent` are consecutive subtrees right after
            // it. Walk them, stopping before the first one the incoming element
            // precedes. FugueMax: later right origin first, then lower identity.
            const auto mine = w.rank(link.right_origin);
            if (!mine) return false;
            while (!w.at_end(pos)) {
                const auto child = w.top_child(pos, link.parent);
                if (!child) break;   // left the parent's subtree
                const auto theirs = w.rank(w.link(*child).right_origin);
                if (!theirs) return false;
                const bool before = *mine > *theirs ||
                                    (*mine == *theirs && incoming.origin < w.id(*child));
                if (before) break;
                pos = w.skip_subtree(pos, w.id(*child));
            }
            at = pos;
        } else {
            // A left child of `parent` goes into the run of left subtrees that ends
            // immediately before `parent`, ordered by identity.
            if (!link.parent) return false;
            const auto parent = w.index().find(*link.parent);
            if (!parent) return false;
            Pos start = *parent;
            while (true) {
                Pos before;
                if (start.off > 0) before = {start.frag, start.off - 1};
                else if (start.frag > 0) before = {start.frag - 1, frags[start.frag - 1].length - 1};
                else break;
                if (!w.descendant(before, link.parent)) break;
                // The last item of a run being in the subtree puts the whole run in it.
                start = {before.frag, 0};
            }
            Pos pos = start;
            while (pos != *parent) {
                const auto child = w.top_child(pos, link.parent);
                if (!child) break;
                if (incoming.origin < w.id(*child)) break;
                pos = w.skip_subtree(pos, w.id(*child));
            }
            at = pos;
        }
    }

    size_t index = at.frag;
    if (at.off > 0) {
        Run tail = frags[at.frag].split_off(at.off);
        frags.insert(frags.begin() + static_cast<ptrdiff_t>(at.frag) + 1, std::move(tail));
        index = at.frag + 1;
    }
    frags.insert(frags.begin() + static_cast<ptrdiff_t>(index), std::move(incoming));
    return true;
}

template <class Run>
std::optional<std::string> check_consistency(const std::vector<Run>& runs) {
    const Index<Run> ix(runs);
    auto where = [&](size_t i) {
        return runs[i].origin.actor.to_string() + "/" + std::to_string(runs[i].origin.counter);
    };
    for (size_t i = 0; i < runs.size(); ++i) {
        if (runs[i].length == 0) return "run " + where(i) + " is empty";
        const Link& link = runs[i].link;
        const Pos here{i, 0};
        const Pos last{i, runs[i].length - 1};
        if (link.parent) {
            const auto parent = ix.find(*link.parent);
            if (!parent) return "run " + where(i) + " attaches to a parent that is not present";
            if (link.side == Side::Right && !(*parent < here))
                return "run " + where(i) + " is a right child placed before its parent";
            if (link.side == Side::Left && !(last < *parent))
                return "run " + where(i) + " is a left child placed after its parent";
            if (link.side == Side::Left && link.right_origin != link.parent)
                return "run " + where(i) + " is a left child whose parent is not its right origin";
        } else if (link.side == Side::Left) {
            return "run " + where(i) + " is a left child of the document root";
        }
        if (link.right_origin) {
            const auto ro = ix.find(*link.right_origin);
            if (!ro) return "run " + where(i) + " has a right origin that is not present";
            if (link.side == Side::Right && !(last < *ro))
                return "run " + where(i) + " has a right origin before itself";
        }
    }
    return std::nullopt;
}

template Link mint_link<Fragment>(const std::vector<Fragment>&, const std::optional<Identity>&);
template bool integrate<Fragment>(std::vector<Fragment>&, Fragment);
template std::optional<ItemPos> find_item<Fragment>(const std::vector<Fragment>&, const Identity&);
template Link mint_link<IdListEntry>(const std::vector<IdListEntry>&, const std::optional<Identity>&);
template bool integrate<IdListEntry>(std::vector<IdListEntry>&, IdListEntry);
template std::optional<ItemPos> find_item<IdListEntry>(const std::vector<IdListEntry>&, const Identity&);
template std::optional<std::string> check_consistency<Fragment>(const std::vector<Fragment>&);
template std::optional<std::string> check_consistency<IdListEntry>(const std::vector<IdListEntry>&);

}  // namespace CollabText::Crdt::SequenceOrder
