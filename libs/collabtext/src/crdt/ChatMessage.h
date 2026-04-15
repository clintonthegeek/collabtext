#pragma once
#include "crdt/StreamSync.h"
#include <cstdint>
#include <optional>
#include <string>

namespace CollabText::Crdt {

struct ChatMessage {
    std::string id;
    uint16_t    replica_id  = 0;
    uint64_t    seq         = 0;
    std::string timestamp;
    std::string author;
    std::string author_name;
    std::string body;
};

StreamEntry              chat_message_to_entry(const ChatMessage& msg);
std::optional<ChatMessage> chat_message_from_entry(const StreamEntry& entry);

} // namespace CollabText::Crdt
