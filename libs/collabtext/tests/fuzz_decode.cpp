// libFuzzer entry point for the checked decoding boundary. Build with
// COLLABTEXT_BUILD_FUZZERS=ON and seed with the V08 fixture directory.
#include <collabtext/Serialization.h>

#include "crdt/OperationValidation.h"
#include "crdt/SidecarManifest.h"
#include "crdt/SegmentFormat.h"
#include "crdt/SegmentReader.h"
#include "crdt/StreamSerialization.h"
#include "crdt/ZstdUtil.h"
#include "transport/StreamSync.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    using namespace CollabText::Crdt;
    constexpr size_t kMaxFuzzInput = 1u * 1024u * 1024u;
    if (size > kMaxFuzzInput) return 0;
    const std::string_view bytes(reinterpret_cast<const char*>(data), size);

    auto text = decode_operation(bytes);
    if (text) {
        const Global empty{};
        (void)validate_operation(text.value(), empty);
    }
    auto list = decode_idlist_operation(bytes);
    if (list) {
        const Global empty{};
        (void)validate_idlist_operation(list.value(), empty);
    }
    (void)decode_stream_entry(bytes);
    if (size <= 64u * 1024u) {
        const std::string manifest_json(bytes);
        (void)manifest_from_json(manifest_json);
    }
    (void)decode_global(bytes);
    (void)decode_counter_gap(bytes);
    (void)decode_segment_header(bytes);
    (void)decode_segment_cursor(bytes);
    (void)decode_legacy_peer_ack(bytes, 1);
    (void)decode_sealed_segment(bytes);
    (void)base64_decode(bytes);
    (void)zstd_decompress(bytes, kMaxDecompressedBytes);
    return 0;
}
