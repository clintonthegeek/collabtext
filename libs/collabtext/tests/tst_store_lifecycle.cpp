#include <QTemporaryDir>
#include <QTest>

#include "crdt/Buffer.h"
#include "crdt/IdList.h"
#include "crdt/IdentityAllocator.h"
#include "transport/StreamSync.h"
#include <collabtext/Serialization.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "TestActor.h"

using namespace CollabText;
using namespace CollabText::Crdt;
namespace fs = std::filesystem;

namespace {
void copy_tree(const fs::path &from, const fs::path &to) {
  fs::create_directories(to);
  for (const auto &entry : fs::recursive_directory_iterator(from)) {
    const auto rel = fs::relative(entry.path(), from);
    const auto target = to / rel;
    if (entry.is_directory()) fs::create_directories(target);
    else if (entry.is_regular_file()) {
      fs::create_directories(target.parent_path());
      fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing);
    }
  }
}

std::vector<fs::path> sealed_segments(const fs::path &dir) {
  std::vector<fs::path> files;
  for (const auto &entry : fs::directory_iterator(dir))
    if (entry.is_regular_file() && entry.path().filename().string().ends_with(".seg.zst"))
      files.push_back(entry.path());
  std::sort(files.begin(), files.end());
  return files;
}

fs::path stream_path(const fs::path &root, const ActorId &actor,
                     const std::string &stream, uint64_t epoch) {
  std::string escaped;
  static constexpr char hex[] = "0123456789abcdef";
  for (unsigned char c : stream) {
    escaped.push_back(hex[c >> 4]); escaped.push_back(hex[c & 15]);
  }
  return root / "replicas" / actor.to_string() / "log" / "streams" /
         escaped / std::to_string(epoch);
}
}

class TestStoreLifecycle final : public QObject {
  Q_OBJECT
private slots:
  void buffer_and_idlist_partial_arrival_is_incomplete_until_older_segments_arrive() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const fs::path base = tmp.path().toStdString();
    const fs::path producer_root = base / "producer";
    const fs::path receiver_root = base / "receiver";
    const auto producer = CollabText::Test::actor(95);
    const StreamScope text_scope{"matrix:text", 1};
    const StreamScope list_scope{"matrix:list", 1};

    Crdt::Buffer source_text(CollabText::Test::test_counter(95));
    auto text_first = source_text.try_apply_local_edit({{0, 0}}, {"seed"});
    QVERIFY(text_first.ok());
    auto text_first_bytes = Crdt::encode_operation(text_first.value());
    QVERIFY(text_first_bytes.ok());
    Crdt::IdList source_list(CollabText::Test::test_counter(95));
    auto list_first = source_list.try_insert_after(Crdt::Anchor::min(), 101);
    QVERIFY(list_first.ok());
    auto list_first_bytes = Crdt::encode_idlist_operation(list_first.value());
    QVERIFY(list_first_bytes.ok());

    {
      FileOpStream writer(producer_root, producer);
      QVERIFY(writer.start().ok());
      QVERIFY(writer.register_stream(text_scope).ok());
      QVERIFY(writer.register_stream(list_scope).ok());
      QVERIFY(writer.push({{text_scope.stream, text_scope.epoch, producer, 1},
                           text_first_bytes.value()}, PushDurability::durable).ok());
      QVERIFY(writer.push({{list_scope.stream, list_scope.epoch, producer, 1},
                           list_first_bytes.value()}, PushDurability::durable).ok());
      QVERIFY(writer.close().ok());
    }

    auto text_second = source_text.try_apply_local_edit({{4, 4}}, {"!"});
    QVERIFY(text_second.ok());
    auto text_second_bytes = Crdt::encode_operation(text_second.value());
    QVERIFY(text_second_bytes.ok());
    auto list_second = source_list.try_insert_after(source_list.anchor_at_index(0), 202);
    QVERIFY(list_second.ok());
    auto list_second_bytes = Crdt::encode_idlist_operation(list_second.value());
    QVERIFY(list_second_bytes.ok());
    {
      FileOpStream writer(producer_root, producer);
      QVERIFY(writer.start().ok());
      QVERIFY(writer.register_stream(text_scope).ok());
      QVERIFY(writer.register_stream(list_scope).ok());
      QVERIFY(writer.push({{text_scope.stream, text_scope.epoch, producer, 2},
                           text_second_bytes.value()}, PushDurability::durable).ok());
      QVERIFY(writer.push({{list_scope.stream, list_scope.epoch, producer, 2},
                           list_second_bytes.value()}, PushDurability::durable).ok());
      QVERIFY(writer.close().ok());
    }

    const auto text_segments = sealed_segments(stream_path(
        producer_root, producer, text_scope.stream, text_scope.epoch));
    const auto list_segments = sealed_segments(stream_path(
        producer_root, producer, list_scope.stream, list_scope.epoch));
    QCOMPARE(text_segments.size(), size_t(2));
    QCOMPARE(list_segments.size(), size_t(2));
    const auto text_dest = stream_path(receiver_root, producer,
                                       text_scope.stream, text_scope.epoch);
    const auto list_dest = stream_path(receiver_root, producer,
                                       list_scope.stream, list_scope.epoch);
    fs::create_directories(text_dest);
    fs::create_directories(list_dest);
    fs::copy_file(text_segments.back(), text_dest / text_segments.back().filename());
    fs::copy_file(list_segments.back(), list_dest / list_segments.back().filename());

    FileOpStream receiver(receiver_root, CollabText::Test::actor(96));
    QVERIFY(receiver.start().ok());
    QVERIFY(receiver.register_stream(text_scope).ok());
    QVERIFY(receiver.register_stream(list_scope).ok());
    size_t premature_text_callbacks = 0;
    auto text_gap = receiver.replay_all(text_scope, [&](const InboundDelivery &d) -> Result<void> {
      ++premature_text_callbacks;
      auto op = Crdt::decode_operation(d.payload);
      if (!op.ok()) return op.error();
      return {};
    });
    QVERIFY(!text_gap.ok());
    QCOMPARE(premature_text_callbacks, size_t(0));
    size_t premature_list_callbacks = 0;
    auto list_gap = receiver.replay_all(list_scope, [&](const InboundDelivery &d) -> Result<void> {
      ++premature_list_callbacks;
      auto op = Crdt::decode_idlist_operation(d.payload);
      if (!op.ok()) return op.error();
      return {};
    });
    QVERIFY(!list_gap.ok());
    QCOMPARE(premature_list_callbacks, size_t(0));

    fs::copy_file(text_segments.front(), text_dest / text_segments.front().filename());
    fs::copy_file(list_segments.front(), list_dest / list_segments.front().filename());
    Crdt::Buffer recovered_text(CollabText::Test::test_counter(97));
    size_t recovered_text_count = 0;
    auto text_replay = receiver.replay_all(text_scope, [&](const InboundDelivery &d) -> Result<void> {
      auto op = Crdt::decode_operation(d.payload);
      if (!op.ok()) return op.error();
      auto outcomes = recovered_text.apply_ops({op.value()});
      if (outcomes.size() != 1 || outcomes.front().kind() != ApplyOutcomeKind::applied)
        return Error(ErrorCode::malformed, "Buffer matrix operation did not apply");
      ++recovered_text_count;
      return {};
    });
    QVERIFY(text_replay.ok());
    QCOMPARE(recovered_text_count, size_t(2));
    QCOMPARE(recovered_text.text(), std::string("seed!"));
    Crdt::IdList recovered_list(CollabText::Test::test_counter(98));
    size_t recovered_list_count = 0;
    auto list_replay = receiver.replay_all(list_scope, [&](const InboundDelivery &d) -> Result<void> {
      auto op = Crdt::decode_idlist_operation(d.payload);
      if (!op.ok()) return op.error();
      auto outcomes = recovered_list.apply_ops({op.value()});
      if (outcomes.size() != 1 || outcomes.front().kind() != ApplyOutcomeKind::applied)
        return Error(ErrorCode::malformed, "IdList matrix operation did not apply");
      ++recovered_list_count;
      return {};
    });
    QVERIFY(list_replay.ok());
    QCOMPARE(recovered_list_count, size_t(2));
    // Full replay reconstructs state without a durable checkpoint, so it must
    // not create receiver cursors that could later be mistaken for coverage.
    const auto replay_cursors = receiver_root / "local"
        / CollabText::Test::actor(96).to_string() / "read-cursors" / "replay";
    QVERIFY(!fs::exists(replay_cursors));
    const auto recovered_ids = recovered_list.ids();
    QCOMPARE(recovered_ids.size(), size_t(2));
    QVERIFY(std::find(recovered_ids.begin(), recovered_ids.end(), 101)
            != recovered_ids.end());
    QVERIFY(std::find(recovered_ids.begin(), recovered_ids.end(), 202)
            != recovered_ids.end());
    QVERIFY(receiver.close().ok());
  }

  void buffer_and_idlist_operations_recover_from_copied_replica_tree() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const fs::path base = tmp.path().toStdString();
    const fs::path device_a = base / "producer";
    const fs::path device_b = base / "receiver";
    const auto producer = CollabText::Test::actor(91);
    const auto receiver_actor = CollabText::Test::actor(92);
    const StreamScope text_scope{"document:text", 2};
    const StreamScope list_scope{"document:list", 2};

    Crdt::Buffer source_text(CollabText::Test::test_counter(91));
    auto text_op = source_text.try_apply_local_edit({{0, 0}}, {"recover me"});
    QVERIFY(text_op.ok());
    auto text_bytes = Crdt::encode_operation(text_op.value());
    QVERIFY(text_bytes.ok());

    Crdt::IdList source_list(CollabText::Test::test_counter(91));
    auto list_op = source_list.try_insert_after(Crdt::Anchor::min(), 0xabc);
    QVERIFY(list_op.ok());
    auto list_bytes = Crdt::encode_idlist_operation(list_op.value());
    QVERIFY(list_bytes.ok());

    {
      FileOpStream sender(device_a, producer);
      QVERIFY(sender.start().ok());
      QVERIFY(sender.register_stream(text_scope).ok());
      QVERIFY(sender.register_stream(list_scope).ok());
      QVERIFY(sender.push({{text_scope.stream, text_scope.epoch, producer, 1},
                           text_bytes.value()}, PushDurability::durable).ok());
      QVERIFY(sender.push({{list_scope.stream, list_scope.epoch, producer, 1},
                           list_bytes.value()}, PushDurability::durable).ok());

      // The same actor cannot have two concurrent local writers, regardless of
      // which engine or stream is being written.
      FileOpStream competing(device_a, producer);
      auto competing_start = competing.start();
      QVERIFY(!competing_start.ok());
      QCOMPARE(competing_start.error().code(), ErrorCode::writer_conflict);
      QVERIFY(sender.close().ok());
    }

    copy_tree(device_a / "replicas", device_b / "replicas");
    FileOpStream replica(device_b, receiver_actor);
    QVERIFY(replica.start().ok());
    QVERIFY(replica.register_stream(text_scope).ok());
    QVERIFY(replica.register_stream(list_scope).ok());

    Crdt::Buffer recovered_text(CollabText::Test::test_counter(93));
    size_t text_count = 0;
    auto text_replay = replica.replay_all(text_scope, [&](const InboundDelivery &d) -> Result<void> {
      auto decoded = Crdt::decode_operation(d.payload);
      if (!decoded.ok()) return decoded.error();
      auto outcome = recovered_text.apply_ops({decoded.value()});
      if (outcome.size() != 1 || outcome.front().kind() != ApplyOutcomeKind::applied)
        return Error(ErrorCode::malformed, "recovered Buffer operation was not applied");
      ++text_count;
      return {};
    });
    QVERIFY(text_replay.ok());
    QCOMPARE(text_count, size_t(1));
    QCOMPARE(recovered_text.text(), std::string("recover me"));

    Crdt::IdList recovered_list(CollabText::Test::test_counter(94));
    size_t list_count = 0;
    auto list_replay = replica.replay_all(list_scope, [&](const InboundDelivery &d) -> Result<void> {
      auto decoded = Crdt::decode_idlist_operation(d.payload);
      if (!decoded.ok()) return decoded.error();
      auto outcome = recovered_list.apply_ops({decoded.value()});
      if (outcome.size() != 1 || outcome.front().kind() != ApplyOutcomeKind::applied)
        return Error(ErrorCode::malformed, "recovered IdList operation was not applied");
      ++list_count;
      return {};
    });
    QVERIFY(list_replay.ok());
    QCOMPARE(list_count, size_t(1));
    QCOMPARE(recovered_list.ids(), (std::vector<uint64_t>{0xabc}));
    QVERIFY(replica.close().ok());
  }

  void physically_separate_replica_roots_replay_after_restart() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const fs::path base = tmp.path().toStdString();
    const fs::path device_a = base / "device-a";
    const fs::path device_b = base / "device-b";
    const auto author = CollabText::Test::actor(81);
    const StreamScope scope{"lifecycle:text", 4};
    WriterConfig config;
    config.seal_bytes = 1;
    config.flush_bytes = 1;

    {
      FileOpStream sender(device_a, author, config);
      QVERIFY(sender.start().ok());
      QVERIFY(sender.register_stream(scope).ok());
      QVERIFY(sender.push({{scope.stream, scope.epoch, author, 1}, "first"},
                          PushDurability::durable).ok());
      QVERIFY(sender.close().ok());
    }
    {
      FileOpStream sender(device_a, author, config);
      QVERIFY(sender.start().ok());
      QVERIFY(sender.register_stream(scope).ok());
      QVERIFY(sender.push({{scope.stream, scope.epoch, author, 2}, "second"},
                          PushDurability::durable).ok());
      QVERIFY(sender.close().ok());
    }

    const auto source = stream_path(device_a, author, scope.stream, scope.epoch);
    QVERIFY(fs::exists(source));
    std::vector<fs::path> segments;
    for (const auto &entry : fs::directory_iterator(source))
      if (entry.is_regular_file() && entry.path().filename().string().ends_with(".seg.zst"))
        segments.push_back(entry.path());
    std::sort(segments.begin(), segments.end());
    QCOMPARE(segments.size(), size_t(2));

    // A sync client may expose the newest sealed segment before its predecessor.
    const auto destination = stream_path(device_b, author, scope.stream, scope.epoch);
    fs::create_directories(destination);
    fs::copy_file(segments.back(), destination / segments.back().filename());
    FileOpStream receiver(device_b, CollabText::Test::actor(82));
    QVERIFY(receiver.start().ok());
    QVERIFY(receiver.register_stream(scope).ok());
    std::vector<std::string> seen;
    auto partial = receiver.replay_all(scope, [&](const InboundDelivery &d) -> Result<void> {
      seen.push_back(d.payload);
      return {};
    });
    QVERIFY(!partial.ok());
    QVERIFY(seen.empty());

    // Arrival of the missing older segment repairs the same independent tree.
    fs::copy_file(segments.front(), destination / segments.front().filename());
    const auto older_copy = destination / segments.front().filename();
    {
      std::fstream damaged(older_copy, std::ios::in | std::ios::out | std::ios::binary);
      QVERIFY(damaged.good());
      damaged.seekg(0, std::ios::end);
      const auto length = damaged.tellg();
      QVERIFY(length > std::streamoff(16));
      damaged.seekg(length - std::streamoff(1));
      char byte = 0;
      damaged.read(&byte, 1);
      QVERIFY(damaged.good());
      byte ^= 0x40;
      damaged.seekp(length - std::streamoff(1));
      damaged.write(&byte, 1);
      QVERIFY(damaged.good());
    }
    auto corrupted = receiver.replay_all(scope, [&](const InboundDelivery &d) -> Result<void> {
      seen.push_back(d.payload);
      return {};
    });
    QVERIFY(!corrupted.ok());
    QVERIFY(seen.empty());
    fs::copy_file(segments.front(), older_copy, fs::copy_options::overwrite_existing);
    auto recovered = receiver.replay_all(scope, [&](const InboundDelivery &d) -> Result<void> {
      seen.push_back(d.payload);
      return {};
    });
    const std::string recovery_error = recovered.ok()
        ? std::string{} : std::string(recovered.error().message());
    QVERIFY2(recovered.ok(), recovery_error.c_str());
    QCOMPARE(seen, (std::vector<std::string>{"first", "second"}));
    QVERIFY(receiver.close().ok());

    // Restart on device B replays from its own private cursor and gets the
    // retained operation sequence exactly once in the newly opened process.
    FileOpStream restarted(device_b, CollabText::Test::actor(83));
    QVERIFY(restarted.start().ok());
    QVERIFY(restarted.register_stream(scope).ok());
    std::vector<std::string> after_restart;
    auto replayed = restarted.replay_all(scope, [&](const InboundDelivery &d) -> Result<void> {
      after_restart.push_back(d.payload);
      return {};
    });
    QVERIFY(replayed.ok());
    QCOMPARE(after_restart, (std::vector<std::string>{"first", "second"}));
    QVERIFY(restarted.close().ok());
  }
};

QTEST_GUILESS_MAIN(TestStoreLifecycle)
#include "tst_store_lifecycle.moc"
