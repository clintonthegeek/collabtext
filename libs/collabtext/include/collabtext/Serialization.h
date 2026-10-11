/// Checked, independently versioned wire codecs. No C++ layout is wire data.
/// On failure retain original bytes; decoding never advances a cursor/coverage.
/// Success validates owned payload data, not state-dependent applicability.
#pragma once
#include <collabtext/Operations.h>
#include <collabtext/IdListOperations.h>
#include <string>
#include <string_view>

namespace CollabText::Crdt {
/// Terminal unused counter interval, protocol metadata rather than a document op.
/// Encoding/decoding does not reserve counters or certify durable gap publication.
struct CounterGap {
    ActorId actor;
    uint64_t first = 0;
    uint64_t count = 0;
    Global prior;
};

/// CTBO/CTLO version 0002; CTGV/CTGP version 0001. Frames contain bounded
/// JSON and fixed-width hex integers; use the checked codecs, not manual parsing.
/// Default limits: 8 MiB frame, 4 MiB string, 32 nesting, 100,000 JSON values,
/// 1,024-byte keys, 512 actors. Unknown/newer formats remain caller-owned.
Result<std::string> encode_operation(const Operation& op);
Result<Operation> decode_operation(std::string_view bytes);
Result<std::string> encode_idlist_operation(const IdListOperation& op);
Result<IdListOperation> decode_idlist_operation(std::string_view bytes);
Result<std::string> encode_global(const Global& global);
Result<Global> decode_global(std::string_view bytes);
Result<std::string> encode_counter_gap(const CounterGap& gap);
Result<CounterGap> decode_counter_gap(std::string_view bytes);
} // namespace CollabText::Crdt
