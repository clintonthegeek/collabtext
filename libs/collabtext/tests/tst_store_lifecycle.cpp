#include <QTemporaryDir>
#include <QTest>

#include "transport/StreamSync.h"

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
