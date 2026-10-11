// libFuzzer entry point for the checked decoding boundary. Build with
// COLLABTEXT_BUILD_FUZZERS=ON and seed with the V08 fixture directory.
#include <collabtext/Serialization.h>

#include "crdt/OperationValidation.h"
#include "crdt/SidecarManifest.h"
#include "crdt/SegmentFormat.h"
#include "crdt/SegmentReader.h"
#include "crdt/StreamSerialization.h"
#include "crdt/ZstdUtil.h"
#include "core/Sha256.h"
#include "storage/CheckpointCodec.h"
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
    (void)CollabText::Storage::decode_membership_checkpoint_bundle(bytes);
    CollabText::Storage::MembershipEvent membership_event;
    membership_event.kind = size != 0 && (data[0] & 1)
        ? CollabText::Storage::MembershipEvent::Kind::Retired
        : CollabText::Storage::MembershipEvent::Kind::Enrolled;
    membership_event.evidence.assign(bytes);
    (void)CollabText::Storage::decode_membership_transition_evidence(
        membership_event);
    // Checkpoint parsing accepts at most 256 MiB in production. Keep libFuzzer
    // inputs much smaller while exercising both independent artifact magics.
    const auto shareable = CollabText::Storage::decode_checkpoint(
        bytes, CollabText::Storage::CheckpointKind::Shareable);
    if (shareable) {
        const auto core = CollabText::Storage::decode_checkpoint_core(shareable.value());
        if (core) {
            (void)CollabText::Storage::encode_checkpoint_core(core.value());
            const auto local_actor = CollabText::ActorId::parse(
                "ffffffffffffffffffffffffffffffff");
            if (local_actor) {
                const auto shareable_hash = CollabText::sha256_hex(bytes);
                CollabText::Storage::CheckpointLocalCore local;
                local.owner = {local_actor.value(), shareable_hash};
                local.writer = {local_actor.value(), core.value().context.document,
                    core.value().context.stream, core.value().context.epoch, 1, 0};
                const auto local_bytes =
                    CollabText::Storage::encode_checkpoint_local_core(
                        local, core.value().context, bytes);
                if (local_bytes) {
                    const auto local_artifact =
                        CollabText::Storage::decode_checkpoint(
                            local_bytes.value(),
                            CollabText::Storage::CheckpointKind::Local);
                    if (local_artifact)
                        (void)CollabText::Storage::decode_checkpoint_local_core(
                            local_artifact.value(), local_actor.value(),
                            shareable_hash, core.value().context);
                }
            }
        }
        if (core && !core.value().translations.supported_token_versions.empty()) {
            auto descriptor = CollabText::Storage::MembershipDescriptor{};
            descriptor.epoch = core.value().context.epoch;
            descriptor.parent_hash = core.value().context.parent_descriptor_hash;
            if (descriptor.epoch > 1)
                descriptor.transition_event_hash =
                    CollabText::sha256_hex("fuzz transition event");
            descriptor.base_checkpoint_hash = CollabText::sha256_hex(bytes);
            descriptor.base_frontiers.push_back({
                core.value().context.stream, core.value().coverage.operation,
                core.value().coverage.delivery});
            for (const auto &[actor, _] : core.value().coverage.operation)
                if (core.value().coverage.delivery.contains(actor))
                    descriptor.members.push_back(actor);
            if (descriptor.members.empty()) {
                const auto fallback = CollabText::ActorId::parse(
                    "ffffffffffffffffffffffffffffffff");
                if (fallback) {
                    descriptor.members.push_back(fallback.value());
                    descriptor.base_frontiers.front().operation.emplace(
                        fallback.value(), 0);
                    descriptor.base_frontiers.front().delivery.emplace(
                        fallback.value(), 0);
                }
            }
            descriptor.supported_token_versions =
                core.value().translations.supported_token_versions;
            descriptor.translation_manifest_hash =
                core.value().translations.manifest_hash;
            auto sealed = CollabText::Storage::seal_membership_descriptor(
                std::move(descriptor));
            if (sealed)
                (void)CollabText::Storage::validate_checkpoint_artifact_membership(
                    bytes, sealed.value());
        }
        for (const auto& section : shareable.value().sections) {
            if (section.id == 1)
                (void)CollabText::Storage::decode_checkpoint_context(section);
            else if (section.id == 2)
                (void)CollabText::Storage::decode_checkpoint_engine(section);
            else if (section.id == 3)
                (void)CollabText::Storage::decode_checkpoint_causal(section);
            else if (section.id == 6) {
                const auto context = CollabText::Storage::decode_checkpoint_context(
                    shareable.value().sections[0]);
                if (context)
                    (void)CollabText::Storage::decode_checkpoint_pins(section,
                                                                     context.value());
            }
            else if (section.id == 7 && shareable.value().sections.size() == 7) {
                const auto context = CollabText::Storage::decode_checkpoint_context(
                    shareable.value().sections[0]);
                if (context) {
                    const auto pins = CollabText::Storage::decode_checkpoint_pins(
                        shareable.value().sections[5], context.value());
                    if (pins)
                        (void)CollabText::Storage::decode_checkpoint_translations(
                            section, context.value(), pins.value());
                }
            }
        }
    }
    (void)CollabText::Storage::decode_checkpoint(
        bytes, CollabText::Storage::CheckpointKind::Local);
    // Exercise the bounded typed section parsers directly too, including when
    // the input is a payload rather than a complete valid artifact.
    (void)CollabText::Storage::decode_checkpoint_context({1, 1, std::string(bytes)});
    (void)CollabText::Storage::decode_checkpoint_engine({2, 2, std::string(bytes)});
    (void)CollabText::Storage::decode_checkpoint_causal({3, 1, std::string(bytes)});
    (void)CollabText::Storage::decode_checkpoint_deferred(
        {4, 1, std::string(bytes)},
        CollabText::Storage::CheckpointContext{"fuzz", "main", 1, "",
            CollabText::Storage::CheckpointEngineKind::Text},
        CollabText::Crdt::Global{});
    (void)CollabText::Storage::decode_checkpoint_coverage({5, 1, std::string(bytes)});
    (void)CollabText::Storage::decode_checkpoint_pins(
        {6, 1, std::string(bytes)},
        CollabText::Storage::CheckpointContext{"doc", "main", 1, "",
            CollabText::Storage::CheckpointEngineKind::Text});
    (void)CollabText::Storage::decode_checkpoint_translations(
        {7, 1, std::string(bytes)},
        CollabText::Storage::CheckpointContext{"doc", "main", 1, "",
            CollabText::Storage::CheckpointEngineKind::Text},
        CollabText::Storage::CheckpointShareableCore::Pins{});
    const auto fuzz_owner = CollabText::ActorId{};
    (void)CollabText::Storage::decode_checkpoint_local_owner({1, 1, std::string(bytes)});
    (void)CollabText::Storage::decode_checkpoint_local_history(
        {2, 1, std::string(bytes)}, fuzz_owner);
    (void)CollabText::Storage::decode_checkpoint_local_writer(
        {3, 1, std::string(bytes)},
        CollabText::Storage::CheckpointContext{"doc", "main", 1, "",
            CollabText::Storage::CheckpointEngineKind::Text});
    return 0;
}
