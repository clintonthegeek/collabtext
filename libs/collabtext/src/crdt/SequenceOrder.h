#pragma once
// FugueMax placement over a document-ordered fragment vector (roadmap task S03).
//
// Two operations, both pure functions of the fragments and the incoming link:
//
//   mint_link   what link a new element must carry so that it lands immediately
//               after a given element (the visible predecessor of an edit).
//   integrate   where an element carrying a link goes, and put it there.
//
// `integrate` is the ONLY place a position is decided. A local edit mints a link
// and then integrates it through the same code a remote replica runs, so the
// originating replica and its peers cannot disagree about where an operation's
// items went.
//
// The rules are Algorithm 1 of the Fugue paper plus the FugueMax modifications;
// see Placement.h. In traversal terms: an element is a child of `parent` on `side`;
// the list is, for each node, its left children (with their subtrees), the node,
// then its right children (with their subtrees). Same-side siblings are ordered by
// identity; right siblings are ordered first by the REVERSE list order of their
// right origins. A proven append shortcut handles right children at END whose
// parent is already the final item; all other placements use the general
// whole-vector algorithm. The slow route remains available for differential tests.

#include "crdt/Placement.h"

#include <collabtext/Engine.h>

#include <optional>
#include <string>
#include <vector>

namespace CollabText::Crdt {
struct Fragment;
struct IdListEntry;
}

namespace CollabText::Crdt::SequenceOrder {

/// What a document-ordered run must provide. `Fragment` is a run of one actor's
/// consecutive items; `IdListEntry` is a run of length one. The algorithm is the
/// same for both, which is the point: Buffer and IdList cannot disagree about
/// ordering because there is only one ordering.
///
///   origin            Identity of the first item
///   length            number of items (constant 1 for a list entry)
///   link              placement of the first item
///   item_at(k)        identity of item k
///   link_at(k)        placement of item k (k > 0: right child of item k-1)
///   split_off(k)      cut before item k and return the tail (never called for
///                     a run of length one)

/// The link for a new element that is to sit immediately after `left_origin` (the
/// document start when empty). `left_origin` must be an element of `runs`.
template <class Run>
[[nodiscard]] Link mint_link(const std::vector<Run>& runs,
                             const std::optional<Identity>& left_origin);

/// Insert `incoming` at the position its link determines, splitting a run if the
/// position falls inside one. Returns false, leaving `runs` untouched, if the link
/// names an element that is not in `runs`.
template <class Run>
[[nodiscard]] bool integrate(std::vector<Run>& runs, Run incoming);

/// Force the general placement algorithm. Kept beside the fast route so the
/// equivalence test can compare both on the same input corpus.
template <class Run>
[[nodiscard]] bool integrate_slow(std::vector<Run>& runs, Run incoming);

/// Per-thread path counts; these are diagnostic and never affect document state.
struct IntegrationCounters {
    uint64_t append_fast = 0;
    uint64_t general = 0;
};
[[nodiscard]] IntegrationCounters integration_counters();
void reset_integration_counters();

/// Index of the run holding `id`, with the item's offset in it.
struct ItemPos {
    size_t run = 0;
    uint32_t offset = 0;
};
template <class Run>
[[nodiscard]] std::optional<ItemPos> find_item(const std::vector<Run>& runs,
                                               const Identity& id);

/// Structural invariants of a document-ordered run list, for tests and simulators:
/// every link names elements that are present, a right child follows its parent, a
/// left child precedes it and has it as its right origin, and every right origin
/// lies after the run that names it. Returns a description of the first violation.
/// This is a necessary condition, not a proof of the placement rule -- the
/// differential tests against the reference model are the proof.
template <class Run>
[[nodiscard]] std::optional<std::string> check_consistency(const std::vector<Run>& runs);

extern template Link mint_link<Fragment>(const std::vector<Fragment>&, const std::optional<Identity>&);
extern template bool integrate<Fragment>(std::vector<Fragment>&, Fragment);
extern template bool integrate_slow<Fragment>(std::vector<Fragment>&, Fragment);
extern template std::optional<ItemPos> find_item<Fragment>(const std::vector<Fragment>&, const Identity&);
extern template Link mint_link<IdListEntry>(const std::vector<IdListEntry>&, const std::optional<Identity>&);
extern template bool integrate<IdListEntry>(std::vector<IdListEntry>&, IdListEntry);
extern template bool integrate_slow<IdListEntry>(std::vector<IdListEntry>&, IdListEntry);
extern template std::optional<ItemPos> find_item<IdListEntry>(const std::vector<IdListEntry>&, const Identity&);
extern template std::optional<std::string> check_consistency<Fragment>(const std::vector<Fragment>&);
extern template std::optional<std::string> check_consistency<IdListEntry>(const std::vector<IdListEntry>&);

}  // namespace CollabText::Crdt::SequenceOrder
