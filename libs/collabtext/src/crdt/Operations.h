#pragma once

#include "crdt/Clock.h"
#include "crdt/Locator.h"
#include "crdt/UndoMap.h"

#include <string>
#include <variant>
#include <vector>

namespace CollabText::Crdt {

// Operation types for sync
struct EditOperation {
    Lamport timestamp;        // Lamport clock value when edit was created
    Global version;           // Causal dependencies (version vector at time of edit)
    std::vector<std::pair<uint32_t, uint32_t>> ranges;  // (start, end) byte offsets in visible text
    std::vector<std::string> new_text;                   // One replacement string per range

    // For remote application: the fragments that were inserted, with their
    // locators and origins, so the remote can place them correctly.
    struct InsertedFragment {
        Lamport origin;
        Locator locator;
        std::string content;
        uint32_t length;
    };
    std::vector<InsertedFragment> inserted_fragments;

    // Timestamps of characters that were deleted by this edit.
    std::vector<Lamport> deleted_timestamps;

    // When inserting in the middle of a fragment, the local side splits
    // the fragment and gives the second half a new locator. Remote replicas
    // need to apply the same split to stay convergent.
    struct SplitRelocation {
        Lamport fragment_origin;  // origin of the fragment being split
        uint32_t split_offset;    // character offset within fragment where split happens
        uint32_t fragment_length; // total character length of the fragment being split
        Locator new_locator;      // new locator for the second half
    };
    std::vector<SplitRelocation> split_relocations;
};

struct UndoOperation {
    Lamport timestamp;
    Global version;
    std::vector<UndoMapKey> undo_keys;     // Characters to hide/show via undo map
    std::vector<UndoMapKey> undelete_keys;  // Characters to un-delete/re-delete via deleted flag
    bool is_redo = false;                   // True if this is a redo operation
};

using Operation = std::variant<EditOperation, UndoOperation>;

} // namespace CollabText::Crdt
