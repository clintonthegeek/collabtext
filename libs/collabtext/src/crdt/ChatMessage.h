#pragma once
#include <collabtext/Anchor.h>
#include "transport/StreamSync.h"
#include "crdt/Anchor.h"
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
    std::optional<Anchor> anchor;
    /// Scoped CTPT bytes; retained unchanged for retry if causal state is pending.
    std::optional<std::string> position_token;
};

StreamEntry              chat_message_to_entry(const ChatMessage& msg);
std::optional<ChatMessage> chat_message_from_entry(const StreamEntry& entry);
Result<ChatMessage>        chat_message_from_entry(const StreamEntry& entry,
                                                    const PositionScope& expected_scope);

} // namespace CollabText::Crdt
