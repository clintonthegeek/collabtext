// tst_actor_identity.cpp — P02 acceptance for persistent actor identity,
// writer leases, and checked counters.
//
// Each test here corresponds to a way the previous identity handling could lose
// or duplicate an operation. Nothing is asserted about timing or performance;
// the failure modes are logical.

#include <QDir>
#include <QTemporaryDir>
#include <QTest>

#include "TestSeed.h"
#include "TraceHarness.h"
#include "collabtext/ActorId.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>

using namespace CollabText;
using CollabText::Test::announce_seed;
using CollabText::Test::effective_seed;
namespace fs = std::filesystem;

namespace {

/// A deterministic randomness source, so a collision is a reproducible input
/// rather than a lucky draw.
ActorId::Source fixed_source(std::array<uint8_t, ActorId::kBytes> bytes) {
    return [bytes](std::string_view) -> Result<std::array<uint8_t, ActorId::kBytes>> {
        return bytes;
    };
}

/// A randomness source that always fails, standing in for a platform adapter
/// with no entropy available.
ActorId::Source failing_source(std::string_view purpose) {
    return [purpose](std::string_view) -> Result<std::array<uint8_t, ActorId::kBytes>> {
        return Error(ErrorCode::random_source_unavailable,
                     std::string("no entropy for ") + std::string(purpose));
    };
}

/// Distinct bytes for a given seed, so two devices can be told apart.
std::array<uint8_t, ActorId::kBytes> source_bytes(uint8_t seed) {
    std::array<uint8_t, ActorId::kBytes> bytes{};
    bytes[0] = seed;
    return bytes;
}

std::filesystem::path write_state(const fs::path& dir, const std::string& body) {
    fs::create_directories(dir);
    const fs::path path = dir / "replica.state";
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << body;
    file.close();
    return path;
}

}  // namespace

class TestActorIdentity : public QObject {
    Q_OBJECT

private slots:
    void unreadable_existing_state_link_is_never_replaced() {
        for (const bool loop : {false, true}) {
            QTemporaryDir temp;
            const auto dir = fs::path(temp.path().toStdString());
            const auto path = dir / "replica.state";
            fs::create_symlink(loop ? path : dir / "missing-target", path);
            int generated = 0;
            ActorId::Source source = [&](std::string_view)
                -> Result<std::array<uint8_t, ActorId::kBytes>> {
                ++generated;
                return source_bytes(0x77);
            };
            const auto opened = ReplicaState::open(dir, source);
            QVERIFY(!opened.ok());
            QCOMPARE(generated, 0);
            QVERIFY(fs::is_symlink(fs::symlink_status(path)));
        }
    }

    void fresh_reservations_never_issue_the_zero_identity_sentinel() {
        QTemporaryDir dir;
        auto replica = ReplicaState::open(fs::path(dir.path().toStdString()));
        QVERIFY(replica.ok());
        const auto operation = replica.value().next_operation("text", "doc", 1);
        QVERIFY(operation.ok());
        QCOMPARE(operation.value().counter, uint64_t(1));
        CheckedCounter zero(0);
        QVERIFY(!zero.reserve().ok());
    }

    void closed_and_moved_from_owners_cannot_reserve_operations() {
        QTemporaryDir dir;
        auto opened = ReplicaState::open(fs::path(dir.path().toStdString()));
        QVERIFY(opened.ok());
        auto moved = std::move(opened).value();
        QVERIFY(!opened.value().next_operation("text", "doc", 1).ok());
        QVERIFY(moved.next_operation("text", "doc", 1).ok());
        QVERIFY(moved.close().ok());
        QVERIFY(moved.close().ok());
        QVERIFY(!moved.next_operation("text", "doc", 1).ok());
    }

    void mutable_private_state_has_one_owner_even_for_different_documents() {
        QTemporaryDir dir;
        const fs::path path(dir.path().toStdString());
        auto first = ReplicaState::open(path);
        QVERIFY(first.ok());
        auto second = ReplicaState::open(path);
        QVERIFY2(!second.ok(), "two owners can currently issue the same actor/stream/counter");
        QCOMPARE(second.error().code(), ErrorCode::writer_conflict);
    }

    void duplicate_identity_directives_are_rejected() {
        QTemporaryDir dir;
        const fs::path path(dir.path().toStdString());
        write_state(path, "collabtext-replica-state 1\nactor 11111111111111111111111111111111\n"
                          "actor 22222222222222222222222222222222\n");
        QVERIFY(!ReplicaState::open(path).ok());
    }

    void duplicate_stream_directives_are_rejected() {
        QTemporaryDir dir;
        const fs::path path(dir.path().toStdString());
        write_state(path, "collabtext-replica-state 1\nactor 11111111111111111111111111111111\n"
                          "stream text 1\nstream text 99\n");
        QVERIFY(!ReplicaState::open(path).ok());
    }

    void stream_names_cannot_inject_private_state_directives() {
        QTemporaryDir dir;
        auto replica = ReplicaState::open(fs::path(dir.path().toStdString()));
        QVERIFY(replica.ok());
        QVERIFY(!replica.value().next_operation("text\nstream text 0", "doc", 1).ok());
    }


    void initTestCase() {
        announce_seed("tst_actor_identity", "identity", "actor-identity");
    }

    // ---- the seed contract ----

    void default_seed_is_the_documented_fixed_value() {
        QCOMPARE(CollabText::Test::kDefaultTestSeed, 20261003ull);
        QCOMPARE(effective_seed("identity"), effective_seed("identity"));
    }

    // ---- ActorId ----

    void default_actor_id_is_invalid_by_construction() {
        // There is no "actor 1". A default-constructed id cannot be used, which
        // is what makes a silent fallback to a small integer impossible to write.
        const ActorId id;
        QVERIFY(!id.valid());
        QCOMPARE(id.to_string().size(), size_t(32));
        const auto parsed = ActorId::parse(id.to_string());
        QVERIFY(!parsed.ok());
        QCOMPARE(parsed.error().code(), ErrorCode::invalid_identity);
    }

    void generated_actor_ids_are_128_bit_and_valid() {
        const auto id = ActorId::generate();
        QVERIFY2(id.ok(), qPrintable(QString::fromStdString(std::string(id.error().message()))));
        QVERIFY(id.value().valid());
        QCOMPARE(id.value().to_string().size(), size_t(32));

        // Round trip through the canonical text form.
        const auto parsed = ActorId::parse(id.value().to_string());
        QVERIFY(parsed.ok());
        QCOMPARE(parsed.value(), id.value());
    }

    void distinct_devices_get_distinct_actors_offline() {
        // Two devices, never in contact, each with their own private state.
        QTemporaryDir a;
        QTemporaryDir b;
        QVERIFY(a.isValid() && b.isValid());

        const auto first = ReplicaState::open(fs::path(a.path().toStdString()),
                                              fixed_source(source_bytes(0x11)));
        const auto second = ReplicaState::open(fs::path(b.path().toStdString()),
                                               fixed_source(source_bytes(0x22)));
        QVERIFY(first.ok() && second.ok());
        QVERIFY(first.value().actor() != second.value().actor());
        QVERIFY(first.value().actor().valid());
        QVERIFY(second.value().actor().valid());
    }

    void generation_failure_produces_a_typed_error_and_no_state() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const fs::path state = fs::path(dir.path().toStdString());

        const auto opened = ReplicaState::open(state, failing_source("test"));
        QVERIFY(!opened.ok());
        QCOMPARE(opened.error().code(), ErrorCode::random_source_unavailable);
        // And crucially: no identity was written, so a retry cannot inherit one.
        QVERIFY(!fs::exists(state / "replica.state"));
    }

    void an_all_zero_source_is_rejected_rather_than_minted() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto opened = ReplicaState::open(
            fs::path(dir.path().toStdString()), fixed_source({}));
        QVERIFY(!opened.ok());
        QCOMPARE(opened.error().code(), ErrorCode::invalid_identity);
    }

    void malformed_actor_text_is_rejected() {
        for (const char* text : {"", "abc", "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz",
                                 "00000000000000000000000000000000",
                                 "000000000000000000000000000000001"}) {
            const auto parsed = ActorId::parse(text);
            QVERIFY2(!parsed.ok(), text);
            QVERIFY(parsed.error().code() == ErrorCode::malformed ||
                    parsed.error().code() == ErrorCode::invalid_identity);
        }
    }

    // ---- counter durability and non-reuse ----

    void restart_never_reuses_a_durable_operation_id() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const fs::path state = fs::path(dir.path().toStdString());

        std::vector<uint64_t> first_run;
        {
            auto replica = ReplicaState::open(state, fixed_source(source_bytes(0x33)));
            QVERIFY(replica.ok());
            for (int i = 0; i < 5; ++i) {
                const auto id = replica.value().next_operation("text", "doc", 1);
                QVERIFY(id.ok());
                QCOMPARE(id.value().actor, replica.value().actor());
                first_run.push_back(id.value().counter);
            }
        }
        QCOMPARE(first_run.front(), 1ull);
        QCOMPARE(first_run.back(), 5ull);

        // Restart from the same private state: same actor, no reused counter.
        auto reopened = ReplicaState::open(state, fixed_source(source_bytes(0x99)));
        QVERIFY(reopened.ok());
        QCOMPARE(reopened.value().actor(), ActorId::from_bytes(source_bytes(0x33)));
        for (const uint64_t previous : first_run) {
            const auto id = reopened.value().next_operation("text", "doc", 1);
            QVERIFY(id.ok());
            QVERIFY2(id.value().counter > previous,
                     "restart reissued a durable operation id");
        }
    }

    void counters_are_per_stream() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        auto replica = ReplicaState::open(fs::path(dir.path().toStdString()));
        QVERIFY(replica.ok());

        // Two streams advance independently: the same counter value in both is
        // not a collision, because coverage is per (actor, stream).
        QCOMPARE(replica.value().next_operation("text", "doc", 1).value().counter, 1ull);
        QCOMPARE(replica.value().next_operation("comments", "doc", 1).value().counter, 1ull);
        QCOMPARE(replica.value().next_operation("text", "doc", 1).value().counter, 2ull);
        QCOMPARE(replica.value().next_operation("comments", "doc", 1).value().counter, 2ull);

        // And they persist as separate spaces.
        QCOMPARE(replica.value().high_water("text"), 2ull);
        QCOMPARE(replica.value().high_water("comments"), 2ull);
        QVERIFY(!replica.value().peek_stream("chat").ok());
    }

    void counter_exhaustion_is_an_error_that_mutates_nothing() {
        // Walk a counter to its last value, then confirm exhaustion is typed,
        // non-mutating, and does not wrap into a fresh identity.
        CheckedCounter counter;
        uint64_t issued = 0;
        for (uint64_t step = 0; step < 3; ++step) {
            const auto value = counter.reserve("test");
            QVERIFY(value.ok());
            issued = value.value();
        }
        QCOMPARE(issued, 3ull);
        QCOMPARE(counter.peek(), 4ull);
        QVERIFY(!counter.exhausted());

        CheckedCounter near_end(CheckedCounter::kExhausted - 1);
        QCOMPARE(near_end.reserve("test").value(), CheckedCounter::kExhausted - 1);
        QCOMPARE(near_end.peek(), CheckedCounter::kExhausted);
        QVERIFY(near_end.exhausted());

        const auto failed = near_end.reserve("test");
        QVERIFY(!failed.ok());
        QCOMPARE(failed.error().code(), ErrorCode::counter_exhausted);
        // Unchanged: the exhaustion did not wrap back to zero.
        QCOMPARE(near_end.peek(), CheckedCounter::kExhausted);
        QVERIFY(!near_end.reserve("test").ok());
        QCOMPARE(near_end.peek(), CheckedCounter::kExhausted);
    }

    void observing_a_peer_raises_the_counter_without_going_backwards() {
        CheckedCounter counter;
        QVERIFY(counter.observe(41, "peer").ok());
        QCOMPARE(counter.peek(), 42ull);
        // Behind is normal: the peer has simply not seen everything.
        QVERIFY(counter.observe(3, "peer").ok());
        QCOMPARE(counter.peek(), 42ull);
        // The very last value cannot be raised past.
        QVERIFY(!counter.observe(CheckedCounter::kExhausted, "peer").ok());
        QCOMPARE(counter.peek(), 42ull);
    }

    void restoring_a_backwards_counter_is_refused() {
        CheckedCounter counter(100);
        QVERIFY(counter.restore(50, "state").ok() == false);
        QCOMPARE(counter.peek(), 100ull);
        QVERIFY(counter.restore(100, "state").ok());
        QVERIFY(counter.restore(101, "state").ok());
        QCOMPARE(counter.peek(), 101ull);
    }

    void operation_ids_reserve_before_returning() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const fs::path state = fs::path(dir.path().toStdString());
        {
            auto replica = ReplicaState::open(state);
            QVERIFY(replica.ok());
            const auto id = replica.value().next_operation("text", "doc", 1);
            QVERIFY(id.ok());
        }
        // The reservation is on disk before the caller ever saw the id: a fresh
        // reader of the same directory must already know the next value.
        std::ifstream file(state / "replica.state", std::ios::binary);
        const std::string contents((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
        QVERIFY(contents.find("stream text 2") != std::string::npos);
    }

    // ---- corrupt and hostile state files ----

    void corrupt_state_is_reported_and_never_replaced() {
        for (const char* body : {
                 "collabtext-replica-state 1\n",                        // no actor
                 "collabtext-replica-state 1\nactor nope\n",           // bad actor
                 "collabtext-replica-state 1\nactor 11111111111111111111111111111111\nstream text\n",
                 "collabtext-replica-state 1\nactor 11111111111111111111111111111111\nstream text nine\n",
                 "collabtext-replica-state 1\nactor 11111111111111111111111111111111\nstream text 5 nonsense\n",
                 "not-a-state-file 1\n",
                 "",
             }) {
            QTemporaryDir dir;
            QVERIFY(dir.isValid());
            const fs::path state = fs::path(dir.path().toStdString()) / "sub";
            write_state(state, body);
            const auto opened = ReplicaState::open(state);
            QVERIFY2(!opened.ok(), body);
            QVERIFY(opened.error().code() == ErrorCode::corrupt_record ||
                    opened.error().code() == ErrorCode::malformed ||
                    opened.error().code() == ErrorCode::unknown_format ||
                    opened.error().code() == ErrorCode::invalid_identity);
            // Critically: the file is still there, unchanged. Replacing it would
            // mint a new actor while old operations claim the old one.
            std::ifstream check(state / "replica.state", std::ios::binary);
            const std::string after((std::istreambuf_iterator<char>(check)),
                                    std::istreambuf_iterator<char>());
            QCOMPARE(after, std::string(body));
        }
    }

    void unknown_state_version_is_refused_rather_than_guessed() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const fs::path state = fs::path(dir.path().toStdString()) / "sub";
        write_state(state, "collabtext-replica-state 99\nactor 11111111111111111111111111111111\n");
        const auto opened = ReplicaState::open(state);
        QVERIFY(!opened.ok());
        QCOMPARE(opened.error().code(), ErrorCode::unsupported_version);
    }

    void an_unknown_directive_is_refused_not_skipped() {
        // Skipping it would drop a counter and reissue identifiers later.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const fs::path state = fs::path(dir.path().toStdString()) / "sub";
        write_state(state,
                    "collabtext-replica-state 1\n"
                    "actor 11111111111111111111111111111111\n"
                    "wibble 7\n");
        const auto opened = ReplicaState::open(state);
        QVERIFY(!opened.ok());
        QCOMPARE(opened.error().code(), ErrorCode::malformed);
    }

    void restart_resumes_above_the_high_water_mark() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // The property that matters: an id is never handed out twice, even
        // across a restart, because the reservation is durable before it is
        // returned.
        const fs::path state = fs::path(dir.path().toStdString()) / "sub";
        auto replica = ReplicaState::open(state, fixed_source(source_bytes(0x5A)));
        QVERIFY(replica.ok());
        std::vector<uint64_t> issued;
        for (int i = 0; i < 40; ++i) {
            const auto id = replica.value().next_operation("text", "doc", 1);
            QVERIFY(id.ok());
            issued.push_back(id.value().counter);
        }
        QCOMPARE(issued.front(), 1ull);
        QCOMPARE(issued.back(), 40ull);
        QCOMPARE(replica.value().high_water("text"), 40ull);
        QVERIFY(fs::exists(state / "replica.state"));

        // Reopen: the same actor, and the next id continues where it stopped.
        QVERIFY(replica.value().close().ok());
        auto reopened = ReplicaState::open(state);
        QVERIFY(reopened.ok());
        QCOMPARE(reopened.value().actor(), replica.value().actor());
        QCOMPARE(reopened.value().high_water("text"), 40ull);
        const auto next = reopened.value().next_operation("text", "doc", 1);
        QVERIFY(next.ok());
        QCOMPARE(next.value().counter, 41ull);
        QVERIFY2(std::find(issued.begin(), issued.end(), next.value().counter) == issued.end(),
                 "restart reissued an operation id");
    }

    // ---- writer leases ----

    void two_local_writers_cannot_both_hold_a_stream() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const fs::path state = fs::path(dir.path().toStdString());

        auto first = ReplicaState::open(state / "device-a", fixed_source(source_bytes(0xA1)));
        auto second = ReplicaState::open(state / "device-b", fixed_source(source_bytes(0xB2)));
        QVERIFY(first.ok() && second.ok());

        const auto lease = first.value().acquire_writer("doc", "text");
        QVERIFY(lease.ok());
        QVERIFY(first.value().holds_writer("doc", "text"));

        // The lease is stored per device state directory, so a second device with
        // its own private state must contend for the *same* lease to be a real
        // test. Point both at one directory to model two processes on one machine.
        auto contender = ReplicaState::open(state / "device-a");
        QVERIFY2(!contender.ok(), "two owners acquired the same private state");
        QCOMPARE(contender.error().code(), ErrorCode::writer_conflict);
        QVERIFY(contender.error().message().find("holder") != std::string::npos);
        QVERIFY(first.value().close().ok());
        contender = ReplicaState::open(state / "device-a");
        QVERIFY(contender.ok());
        const auto after_release = contender.value().acquire_writer("doc", "text");
        QVERIFY2(after_release.ok(), "the lease was not released");
        contender.value().release_writer();
    }

    void a_lease_is_per_document_and_stream() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        auto replica = ReplicaState::open(fs::path(dir.path().toStdString()));
        QVERIFY(replica.ok());
        QVERIFY(replica.value().acquire_writer("doc-a", "text").ok());
        // A different stream, and a different document, are different leases.
        QVERIFY(replica.value().acquire_writer("doc-a", "comments").ok());
        QVERIFY(replica.value().acquire_writer("doc-b", "text").ok());
        QVERIFY(!replica.value().acquire_writer("doc-a", "text").ok());
        replica.value().release_writer();
    }

    // ---- identity conflicts ----

    void a_peer_claiming_our_actor_is_reported_not_absorbed() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        auto replica = ReplicaState::open(fs::path(dir.path().toStdString()),
                                          fixed_source(source_bytes(0xC3)));
        QVERIFY(replica.ok());
        for (int i = 0; i < 4; ++i)
            QVERIFY(replica.value().next_operation("text", "doc", 1).ok());

        const ActorId ours = replica.value().actor();
        // Someone else is producing operations under our identity.
        const auto conflict = replica.value().check_claim("text", ours, 2);
        QVERIFY(!conflict.ok());
        QCOMPARE(conflict.error().code(), ErrorCode::actor_conflict);

        // A different actor is not our problem: it is a normal peer.
        QVERIFY(replica.value().check_claim("text", ActorId::from_bytes(source_bytes(0xD4)), 0).ok());
        // An invalid claimant is rejected outright.
        QVERIFY(!replica.value().check_claim("text", ActorId{}, 0).ok());
        // A peer claiming to be us *beyond* our mark means our own state is
        // truncated, which is a local problem and must not be papered over.
        QVERIFY(!replica.value().check_claim("text", ours, 99).ok());
    }

    void an_identical_clone_is_detected_when_it_meets_us() {
        // Two devices that started from the same copied private state. They
        // cannot always be detected before they exchange operations; when they
        // do, it is reported.
        QTemporaryDir shared;
        QVERIFY(shared.isValid());
        const fs::path template_state = fs::path(shared.path().toStdString()) / "template";
        {
            auto original = ReplicaState::open(template_state,
                                               fixed_source(source_bytes(0xE5)));
            QVERIFY(original.ok());
            QVERIFY(original.value().next_operation("text", "doc", 1).ok());
        }
        const fs::path clone = fs::path(shared.path().toStdString()) / "clone";
        fs::copy(template_state, clone, fs::copy_options::recursive);

        auto a = ReplicaState::open(template_state);
        auto b = ReplicaState::open(clone);
        QVERIFY(a.ok() && b.ok());
        QCOMPARE(a.value().actor(), b.value().actor());   // the collision itself

        QVERIFY(a.value().next_operation("text", "doc", 1).ok());   // a issues 1
        QVERIFY(b.value().next_operation("text", "doc", 1).ok());   // b also issues 1
        // A sees an operation claiming to be A, at a counter A already issued.
        QVERIFY(!a.value().check_claim("text", b.value().actor(), 1).ok());
        QVERIFY(!b.value().check_claim("text", a.value().actor(), 1).ok());
    }

    // ---- forking versus copying ----

    void copying_private_state_yields_a_clone_until_it_is_forked_explicitly() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const fs::path state = fs::path(dir.path().toStdString());
        const fs::path copy = fs::path(dir.path().toStdString() + "-copy");

        ActorId original_actor;
        {
            auto replica = ReplicaState::open(state, fixed_source(source_bytes(0xF6)));
            QVERIFY(replica.ok());
            original_actor = replica.value().actor();
            QVERIFY(replica.value().next_operation("text", "doc", 1).ok());
        }
        fs::copy(state, copy, fs::copy_options::recursive);

        // A plain copy of private state still claims the same identity: the copy
        // is a clone, and pretending otherwise would be the bug.
        auto clone = ReplicaState::open(copy);
        QVERIFY(clone.ok());
        QCOMPARE(clone.value().actor(), original_actor);

        // The fix is explicit: fork the identity.
        const auto forked = clone.value().fork_identity("copied private state");
        QVERIFY(forked.ok());
        QVERIFY(forked.value() != original_actor);
        QVERIFY(forked.value().valid());
        QCOMPARE(clone.value().actor(), forked.value());
        // The fork starts a new numbering space, so it cannot collide with the
        // original's operations even at the same counter.
        const auto id = clone.value().next_operation("text", "doc", 1);
        QVERIFY(id.ok());
        QVERIFY(id.value().actor != original_actor);

        // And the fork is durable, so a restart keeps the new identity.
        QVERIFY(clone.value().close().ok());
        auto reopened = ReplicaState::open(copy);
        QVERIFY(reopened.ok());
        QCOMPARE(reopened.value().actor(), forked.value());
        QVERIFY(reopened.value().high_water("text") == 1ull);
    }

    void copying_a_shared_document_copies_no_writer_identity() {
        // The ordinary case: a shared folder holds document state and nothing
        // private. Copying it produces no actor identity at all, so the copy is a
        // document, not a second writer.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const fs::path shared = fs::path(dir.path().toStdString()) / "shared-doc";
        fs::create_directories(shared / "segments");
        {
            std::ofstream file(shared / "segments" / "document.txt");
            file << "content\n";
        }

        const fs::path copy = fs::path(dir.path().toStdString()) / "shared-doc-copy";
        fs::copy(shared, copy, fs::copy_options::recursive);

        // No identity anywhere in the shared store, in either copy.
        for (const auto& entry : fs::recursive_directory_iterator(copy)) {
            const std::string name = entry.path().filename().string();
            QVERIFY2(name.find("replica.state") == std::string::npos,
                     "writer identity leaked into a shared document store");
            QVERIFY2(name.find("writer-") == std::string::npos,
                     "a writer lease leaked into a shared document store");
        }

        // Two devices pointed at the same shared store keep separate identities,
        // because identity lives in each device's private state.
        auto device_a = ReplicaState::open(fs::path(dir.path().toStdString()) / "private-a");
        auto device_b = ReplicaState::open(fs::path(dir.path().toStdString()) / "private-b");
        QVERIFY(device_a.ok() && device_b.ok());
        QVERIFY(device_a.value().actor() != device_b.value().actor());
    }

    // ---- the operation context ----

    void operation_identity_is_actor_plus_checked_counter() {
        OperationId a;
        a.actor = ActorId::from_bytes(source_bytes(0x01));
        a.counter = 41;
        OperationId b;
        b.actor = ActorId::from_bytes(source_bytes(0x01));
        b.counter = 42;
        OperationId c;
        c.actor = ActorId::from_bytes(source_bytes(0x02));
        c.counter = 1;

        // Total order, stable across processes, so ids can key a dedupe map.
        QVERIFY(a < b);
        QVERIFY(b < c);   // actor dominates counter, not the other way round
        QCOMPARE(a, (OperationId{a.actor, 41}));
        QVERIFY(a != c);

        OperationContext context;
        context.document = "doc";
        context.stream = "text";
        context.epoch = 1;
        context.id = a;
        QCOMPARE(context.id.counter, 41ull);
    }
};

QTEST_GUILESS_MAIN(TestActorIdentity)
#include "tst_actor_identity.moc"
