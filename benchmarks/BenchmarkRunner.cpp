#include "crdt/Buffer.h"

#include <collabtext/CrdtEngine.h>
#include <collabtext/IdListEngine.h>
#include <collabtext/Serialization.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace CollabText;
using namespace CollabText::Crdt;
using Clock = std::chrono::steady_clock;
constexpr uint32_t workload_seed = 42;

template<class T>
T take(Result<T> result) {
    if (!result) throw std::runtime_error(std::string(result.error().message()));
    return std::move(result).value();
}

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

class CheckedAllocator final : public IdentityAllocator {
public:
    explicit CheckedAllocator(uint64_t actor) {
        std::array<uint8_t, 16> bytes{};
        for (int i = 15; i >= 8; --i) {
            bytes[static_cast<size_t>(i)] = static_cast<uint8_t>(actor);
            actor >>= 8;
        }
        actor_ = ActorId::from_bytes(bytes);
    }
    Result<uint64_t> reserve_run(uint64_t count) override {
        return counter_.reserve_run(count);
    }
    void observe(uint64_t value) override {
        require(counter_.observe(value).ok(), "allocator observe failed");
    }
    uint64_t peek() const noexcept override { return counter_.peek(); }
    const ActorId& actor() const noexcept override { return actor_; }
private:
    ActorId actor_;
    CheckedCounter counter_;
};

std::unique_ptr<CrdtEngine> make_text(uint64_t actor = 1) {
    return take(CrdtEngine::create(std::make_shared<CheckedAllocator>(actor),
                                   {"perf", "body", Primitive::text, 1}));
}

std::vector<uint64_t> samples;
uint64_t case_attempts = 0;
uint64_t case_accepted = 0;
uint64_t case_refused = 0;
uint64_t case_noop = 0;
std::string case_name;
int edit_samples = 200;
int query_samples = 50;
int codec_samples = 100;
int bulk_samples = 5;

uint64_t fnv1a(std::string_view bytes) {
    uint64_t hash = 14695981039346656037ull;
    for (unsigned char byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string hex64(uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}

void begin_case(std::string name) {
    case_name = std::move(name);
    samples.clear();
    case_attempts = case_accepted = case_refused = case_noop = 0;
}

template<class F>
auto timed(F&& function) {
    ++case_attempts;
    const auto start = Clock::now();
    auto result = function();
    const auto finish = Clock::now();
    ++case_accepted;
    samples.push_back(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count()));
    return result;
}

void report_case(uint64_t visible_bytes, uint64_t prior_edits,
                 uint64_t fragments, uint64_t tombstones,
                 std::string_view result_digest, std::string_view operation_digest = {}) {
    for (size_t i = 0; i < samples.size(); ++i) {
        std::cout << "{\"record\":\"sample\",\"workload\":\"" << case_name
                  << "\",\"sample\":" << i << ",\"latency_ns\":" << samples[i]
                  << ",\"work_items\":1,\"work_unit\":\"call\"}\n";
    }
    std::cout << "{\"record\":\"case\",\"workload\":\"" << case_name
              << "\",\"samples\":" << samples.size()
              << ",\"attempted\":" << case_attempts
              << ",\"accepted\":" << case_accepted
              << ",\"refused\":" << case_refused
              << ",\"no_op\":" << case_noop
              << ",\"visible_bytes\":" << visible_bytes
              << ",\"prior_edits\":" << prior_edits
              << ",\"structure_available\":" << (fragments != 0 ? "true" : "false")
              << ",\"fragments\":";
    if (fragments) std::cout << fragments; else std::cout << "null";
    std::cout << ",\"tombstone_fragments\":";
    if (fragments) std::cout << tombstones; else std::cout << "null";
    std::cout << ",\"result_fnv1a64\":\"" << result_digest << "\""
              << ",\"operation_fnv1a64\":\"" << operation_digest << "\"}\n";
}

void seed_text(CrdtEngine& engine, std::string& expected, size_t bytes,
               size_t undo_budget = 0) {
    expected.assign(bytes, 'a');
    if (undo_budget)
        require(engine.set_max_undo_bytes(undo_budget).ok(), "undo budget configuration failed");
    take(engine.try_insert(0, expected));
}

void public_edits(size_t bytes, int prior_edits) {
    auto engine = make_text();
    std::string expected;
    seed_text(*engine, expected, bytes,
              bytes >= 1'000'000 ? 256u * 1024 * 1024 : 0);
    std::mt19937 random(workload_seed);
    for (int i = 0; i < prior_edits; ++i) {
        const size_t position = random() % bytes;
        const std::string replacement(1, expected[position] == 'x' ? 'y' : 'x');
        take(engine->try_replace(static_cast<int>(position), 1, replacement));
        expected[position] = replacement[0];
    }

    begin_case("public_replace_" + std::to_string(bytes) + "B_history_" +
               std::to_string(prior_edits));
    Operation last;
    for (int i = 0; i < edit_samples; ++i) {
        const size_t position = random() % bytes;
        const std::string replacement(1, expected[position] == 'x' ? 'y' : 'x');
        last = take(timed([&] {
            return engine->try_replace(static_cast<int>(position), 1, replacement);
        }));
        expected[position] = replacement[0];
    }
    require(engine->text() == expected, "public edit result mismatch");
    const auto frame = take(encode_operation(last));
    report_case(expected.size(), prior_edits, 0, 0, hex64(fnv1a(expected)),
                hex64(fnv1a(frame)));
}

void byte_edits(size_t bytes, int prior_edits) {
    Buffer engine(std::make_shared<CheckedAllocator>(1));
    if (bytes >= 1'000'000)
        require(engine.set_max_undo_bytes(256u * 1024 * 1024).ok(),
                "byte control undo budget configuration failed");
    std::string expected(bytes, 'a');
    take(engine.try_apply_local_edit({{0, 0}}, {expected}));
    std::mt19937 random(workload_seed);
    for (int i = 0; i < prior_edits; ++i) {
        const uint32_t position = static_cast<uint32_t>(random() % bytes);
        const std::string replacement(1, expected[position] == 'x' ? 'y' : 'x');
        take(engine.try_apply_local_edit({{position, position + 1}}, {replacement}));
        expected[position] = replacement[0];
    }

    begin_case("byte_replace_" + std::to_string(bytes) + "B_history_" +
               std::to_string(prior_edits));
    Operation last;
    for (int i = 0; i < edit_samples; ++i) {
        const uint32_t position = static_cast<uint32_t>(random() % bytes);
        const std::string replacement(1, expected[position] == 'x' ? 'y' : 'x');
        last = take(timed([&] {
            return engine.try_apply_local_edit({{position, position + 1}}, {replacement});
        }));
        expected[position] = replacement[0];
    }
    require(engine.text() == expected, "byte control result mismatch");
    const auto frame = take(encode_operation(last));
    report_case(expected.size(), prior_edits, engine.fragment_count(),
                engine.tombstone_count(), hex64(fnv1a(expected)), hex64(fnv1a(frame)));
}

void boundary_reads(int prior_edits) {
    auto engine = make_text();
    std::string expected;
    seed_text(*engine, expected, 100'000);
    std::mt19937 random(42);
    for (int i = 0; i < prior_edits; ++i) {
        const size_t position = random() % expected.size();
        const std::string replacement(1, expected[position] == 'x' ? 'y' : 'x');
        take(engine->try_replace(static_cast<int>(position), 1, replacement));
        expected[position] = replacement[0];
    }

    begin_case("boundary_mint_varied_history_" + std::to_string(prior_edits));
    std::vector<TextBoundary> tokens;
    for (int i = 0; i < query_samples; ++i) {
        const uint32_t offset = static_cast<uint32_t>(random() % (expected.size() + 1));
        auto token = take(timed([&] { return engine->boundary_at(offset, Affinity::left); }));
        require(take(engine->resolve(token)).byte_offset == offset,
                "boundary mint resolution mismatch");
        tokens.push_back(std::move(token));
    }
    require(engine->text() == expected, "boundary fixture text mismatch");
    report_case(expected.size(), prior_edits, 0, 0, hex64(fnv1a(expected)));

    begin_case("boundary_resolve_varied_history_" + std::to_string(prior_edits));
    std::string resolved;
    for (size_t i = 0; i < tokens.size(); ++i) {
        const auto result = take(timed([&] { return engine->resolve(tokens[i]); }));
        resolved += std::to_string(result.byte_offset) + ",";
    }
    report_case(expected.size(), prior_edits, 0, 0, hex64(fnv1a(resolved)));

    begin_case("attachment_mint_varied_history_" + std::to_string(prior_edits));
    std::vector<TextAttachment> attachments;
    for (int i = 0; i < query_samples; ++i) {
        const uint32_t offset = 1 + static_cast<uint32_t>(random() % expected.size());
        auto token = take(timed([&] {
            return engine->attachment_at(offset, Affinity::left, ReferentSide::preceding);
        }));
        require(take(engine->resolve(token)).byte_offset == offset,
                "attachment mint resolution mismatch");
        attachments.push_back(std::move(token));
    }
    report_case(expected.size(), prior_edits, 0, 0, hex64(fnv1a(expected)));

    begin_case("attachment_resolve_varied_history_" + std::to_string(prior_edits));
    resolved.clear();
    for (const auto& attachment : attachments) {
        const auto result = take(timed([&] { return engine->resolve(attachment); }));
        resolved += std::to_string(result.byte_offset) + ",";
    }
    report_case(expected.size(), prior_edits, 0, 0, hex64(fnv1a(resolved)));
}

void interleaved_position_reads() {
    auto engine = make_text();
    std::string expected;
    seed_text(*engine, expected, 100'000);
    std::mt19937 random(workload_seed + 669);
    begin_case("interleaved_boundary_mint_after_edit_100KB");
    for (int i = 0; i < query_samples; ++i) {
        const size_t position = random() % expected.size();
        const std::string replacement(1, expected[position] == 'x' ? 'y' : 'x');
        take(engine->try_replace(static_cast<int>(position), 1, replacement));
        expected[position] = replacement[0];
        const uint32_t offset = static_cast<uint32_t>(random() % (expected.size() + 1));
        const auto token = take(timed([&] {
            return engine->boundary_at(offset, i % 2 ? Affinity::left : Affinity::right);
        }));
        require(take(engine->resolve(token)).byte_offset == offset,
                "interleaved boundary mismatch");
    }
    report_case(expected.size(), 50, 0, 0, hex64(fnv1a(expected)));

    const auto marker = take(engine->try_insert_marker(50'000)).marker;
    begin_case("interleaved_marker_resolve_after_edit_100KB");
    for (int i = 0; i < query_samples; ++i) {
        const size_t position = random() % expected.size();
        const std::string replacement(1, expected[position] == 'x' ? 'y' : 'x');
        take(engine->try_replace(static_cast<int>(position), 1, replacement));
        expected[position] = replacement[0];
        const auto result = take(timed([&] { return engine->resolve(marker); }));
        require(result.byte_offset == 50'000, "interleaved marker offset mismatch");
    }
    report_case(expected.size(), 100, 0, 0, hex64(fnv1a(expected)));
}

void unicode_edits() {
    auto engine = make_text();
    std::string expected;
    for (int i = 0; i < 10'000; ++i) expected += "a😀é";
    take(engine->try_insert(0, expected));
    require(expected.size() == 70'000 && engine->length() == 40'000,
            "unicode seed dimensions differ");
    begin_case("unicode_append_70000_utf8_bytes");
    for (int i = 0; i < edit_samples; ++i) {
        take(timed([&] { return engine->try_insert(40'000 + 2 * i, "😀"); }));
        expected += "😀";
    }
    require(engine->text() == expected &&
                engine->length() == static_cast<int>(40'000 + 2 * edit_samples),
            "unicode edit result mismatch");
    report_case(expected.size(), 0, 0, 0, hex64(fnv1a(expected)));
}

void list_reads(size_t count) {
    auto engine = take(IdListEngine::create(std::make_shared<CheckedAllocator>(1),
                                            {"perf", "blocks", Primitive::list, 1}));
    Anchor last = Anchor::min();
    for (size_t i = 0; i < count; ++i)
        last = Anchor{idlist_op_identity(take(engine->try_insert_after(last, 7)))};
    const auto entries = take(engine->visible_entries());
    require(entries.size() == count && entries.back().identity == last.position,
            "list seed mismatch");

    begin_case("list_identity_lookup_" + std::to_string(count));
    std::string resolved;
    for (int i = 0; i < query_samples; ++i) {
        const auto index = (static_cast<size_t>(i) * (count - 1)) /
                           static_cast<size_t>(query_samples - 1);
        const auto& expected = entries[index];
        auto value = take(timed([&] { return engine->element_of(expected.identity); }));
        const auto position = take(engine->resolve(value));
        require(position.element_index ==
                    static_cast<uint32_t>(index),
                "identity lookup mismatch");
        resolved += std::to_string(position.element_index) + ",";
    }
    report_case(0, count, engine->entry_count(), engine->tombstone_count(),
                hex64(fnv1a(resolved)));

    begin_case("list_anchor_lookup_" + std::to_string(count));
    resolved.clear();
    for (int i = 0; i < query_samples; ++i) {
        const auto index = (static_cast<size_t>(i) * (count - 1)) /
                           static_cast<size_t>(query_samples - 1);
        const auto& expected = entries[index];
        const auto anchor = take(timed([&] { return engine->try_anchor_of(expected.identity); }));
        require(anchor.position == expected.identity, "identity anchor mismatch");
        resolved += std::to_string(anchor.position.counter) + ",";
    }
    report_case(0, count, engine->entry_count(), engine->tombstone_count(),
                hex64(fnv1a(resolved)));

    begin_case("list_projection_" + std::to_string(count));
    for (int i = 0; i < query_samples; ++i) {
        auto projection = take(timed([&] { return engine->visible_entries(); }));
        require(projection.size() == count && projection.back().identity == last.position,
                "list projection mismatch");
    }
    report_case(0, count, engine->entry_count(), engine->tombstone_count(),
                hex64(fnv1a(resolved)));

    begin_case("list_single_insert_after_aged_" + std::to_string(count));
    std::string operation_frames;
    for (int i = 0; i < query_samples; ++i) {
        auto operation = take(timed([&] {
            return engine->try_insert_after(last, static_cast<uint64_t>(7));
        }));
        last = Anchor{idlist_op_identity(operation)};
        operation_frames += take(encode_idlist_operation(operation));
        require(engine->size() == count + static_cast<uint32_t>(i + 1),
                "list mutation result mismatch");
    }
    report_case(0, count, engine->entry_count(), engine->tombstone_count(),
                hex64(fnv1a(operation_frames)), hex64(fnv1a(operation_frames)));
}

enum class BulkKind { insert, remove, replace, undo };

std::string bulk_name(BulkKind kind) {
    switch (kind) {
    case BulkKind::insert: return "insert";
    case BulkKind::remove: return "remove";
    case BulkKind::replace: return "replace";
    case BulkKind::undo: return "undo_after_remove";
    }
    throw std::runtime_error("unknown bulk workload");
}

void bulk_edits(size_t bytes, BulkKind kind) {
    const size_t repeat_count = static_cast<size_t>(bulk_samples);
    const std::string text(bytes, 'b');
    const std::string original(bytes, 'a');
    std::vector<std::unique_ptr<CrdtEngine>> engines;
    engines.reserve(repeat_count);
    for (size_t i = 0; i < repeat_count; ++i) {
        auto engine = make_text();
        if (bytes >= 1'000'000)
            require(engine->set_max_undo_bytes(256u * 1024 * 1024).ok(),
                    "bulk undo budget configuration failed");
        if (kind != BulkKind::insert) {
            take(engine->try_insert(0, original));
            if (kind == BulkKind::undo)
                take(engine->try_remove(0, static_cast<int>(bytes)));
        }
        engines.push_back(std::move(engine));
    }
    begin_case("bulk_" + bulk_name(kind) + "_" + std::to_string(bytes) + "B");
    for (auto& engine : engines) {
        if (kind == BulkKind::insert) {
            take(timed([&] { return engine->try_insert(0, text); }));
            require(engine->text() == text, "bulk insert result mismatch");
        } else if (kind == BulkKind::remove) {
            take(timed([&] { return engine->try_remove(0, static_cast<int>(bytes)); }));
            require(engine->text().empty(), "bulk remove result mismatch");
        } else if (kind == BulkKind::replace) {
            take(timed([&] { return engine->try_replace(0, static_cast<int>(bytes), text); }));
            require(engine->text() == text, "bulk replace result mismatch");
        } else {
            const auto operation = take(timed([&] { return engine->try_undo(); }));
            require(operation.has_value() && engine->text() == original,
                    "bulk undo result mismatch");
        }
    }
    report_case(kind == BulkKind::remove ? 0 : bytes, 0, 0, 0,
                hex64(fnv1a(engines.back()->text())));
}

void default_import_limit() {
    auto allocator = std::make_shared<CheckedAllocator>(17);
    auto engine = take(CrdtEngine::create(allocator, {"perf", "limit", Primitive::text, 1}));
    const auto before = allocator->peek();
    const auto result = engine->try_insert(0, std::string(1'000'000, 'a'));
    require(!result && result.error().code() == ErrorCode::limit_exceeded,
            "default one-megabyte insert did not return the measured refusal");
    require(engine->text().empty() && allocator->peek() == before,
            "refused default import changed text or allocated identity");
    std::cout << "{\"record\":\"limit\",\"bytes\":1000000,"
                 "\"outcome\":\"limit_exceeded\",\"text_empty\":true,"
                 "\"allocator_unchanged\":true}\n";
}

void codec_cases() {
    for (int actors : {1, 32, 128, 512}) {
        auto receiver = make_text(999);
        for (int actor = 1; actor < actors; ++actor) {
            auto source = make_text(actor);
            const auto op = take(source->try_insert(0, "s"));
            require(receiver->apply_remote(op).kind() == ApplyOutcomeKind::applied,
                    "codec frontier setup failed");
        }
        take(receiver->try_insert(actors - 1, "z"));
        const auto operation = take(receiver->try_insert(actors, "q"));
        const auto frame = take(encode_operation(operation));
        require(receiver->text() == std::string(static_cast<size_t>(actors - 1), 's') + "zq",
                "codec frontier fixture mismatch");
        begin_case("codec_encode_actors_" + std::to_string(actors));
        for (int i = 0; i < codec_samples; ++i) {
            require(take(timed([&] { return encode_operation(operation); })) == frame,
                    "noncanonical encode result");
        }
        report_case(actors + 1, 0, 0, 0, hex64(fnv1a(receiver->text())), hex64(fnv1a(frame)));
        begin_case("codec_decode_actors_" + std::to_string(actors));
        for (int i = 0; i < codec_samples; ++i) {
            const auto decoded = take(timed([&] { return decode_operation(frame); }));
            require(op_identity(decoded) == op_identity(operation), "decoded identity mismatch");
        }
        report_case(actors + 1, 0, 0, 0, hex64(fnv1a(receiver->text())), hex64(fnv1a(frame)));
    }
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--quick") {
            edit_samples = 5;
            query_samples = 5;
            codec_samples = 10;
            bulk_samples = 1;
        } else if (argc != 1) {
            throw std::runtime_error("usage: collabtext_performance_benchmark [--quick]");
        }
        std::cout << "{\"record\":\"header\",\"schema\":\"collabtext-performance-samples/1\","
                  << "\"profile\":\"" << (edit_samples == 5 ? "quick" : "full")
                  << "\",\"seed\":" << workload_seed
                  << ",\"percentile\":\"lower_order_statistic_floor_q_n_minus_1\"}\n";
        for (size_t bytes : {1'000u, 10'000u, 100'000u, 1'000'000u}) {
            public_edits(bytes, 0);
            byte_edits(bytes, 0);
        }
        for (int history : {500, 3'000}) {
            public_edits(10'000, history);
            byte_edits(10'000, history);
        }
        unicode_edits();
        boundary_reads(0);
        boundary_reads(1'000);
        interleaved_position_reads();
        for (size_t count : {100u, 1'000u, 5'000u}) list_reads(count);
        for (size_t bytes : {1'000u, 100'000u, 1'000'000u}) {
            bulk_edits(bytes, BulkKind::insert);
            bulk_edits(bytes, BulkKind::remove);
            bulk_edits(bytes, BulkKind::replace);
            if (bytes <= 100'000) bulk_edits(bytes, BulkKind::undo);
        }
        codec_cases();
        default_import_limit();
    } catch (const std::exception& error) {
        std::cerr << "benchmark correctness failure: " << error.what() << '\n';
        return 1;
    }
}
