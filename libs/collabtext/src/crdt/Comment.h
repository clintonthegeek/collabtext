#pragma once
#include "crdt/StreamSync.h"
#include "crdt/Anchor.h"
#include <cstdint>
#include <optional>
#include <string>

namespace CollabText::Crdt {

struct Comment {
    std::string id;
    uint16_t    replica_id  = 0;
    uint64_t    seq         = 0;
    std::string timestamp;
    std::string author;
    std::string author_name;
    std::string body;
    Anchor      range_start;
    Anchor      range_end;
    bool        resolved    = false;
};

StreamEntry              comment_to_entry(const Comment& comment);
std::optional<Comment>   comment_from_entry(const StreamEntry& entry);

} // namespace CollabText::Crdt
