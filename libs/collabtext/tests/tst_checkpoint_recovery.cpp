#include "storage/CheckpointStore.h"
#include "core/Sha256.h"

#include <QTemporaryDir>
#include <QtTest>

#include "TestActor.h"

#include <filesystem>
#include <fstream>
#include <array>

using namespace CollabText;
using namespace CollabText::Storage;
namespace fs = std::filesystem;

namespace {
std::string checkpoint(std::string document) {
  CheckpointShareableCore core;
  core.context = {std::move(document), "main", 1, "",
                  CheckpointEngineKind::Text};
  core.pins.canonical_projection_hash = sha256_hex("");
  core.translations.supported_token_versions = {1};
  auto bytes = encode_checkpoint_core(core);
  if (!bytes) qFatal("failed to construct a checkpoint fixture");
  return std::move(bytes).value();
}

Result<void> accept_checkpoint(std::string_view,
                               const std::optional<std::string> &) {
  return {};
}
}

class CheckpointRecoveryTest final : public QObject {
  Q_OBJECT
private slots:
  void publishesAndRecoversTypedCore() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const auto actor = Test::actor(111);
    auto store = CheckpointStore::open(temp.path().toStdString());
    QVERIFY(store);
    auto empty = store.value().recover(actor, accept_checkpoint);
    QVERIFY(empty);
    QVERIFY(!empty.value());

    const auto bytes = checkpoint("document-a");
    auto published = store.value().publish(bytes, {}, actor);
    QVERIFY(published);
    QCOMPARE(published.value().generation, uint64_t{1});
    QCOMPARE(published.value().checkpoint_id, sha256_hex(bytes));
    auto recovered = store.value().recover(
        actor, accept_checkpoint,
        CheckpointContext{"document-a", "main", 1, "",
                          CheckpointEngineKind::Text});
    QVERIFY(recovered);
    QVERIFY(recovered.value());
    QCOMPARE(recovered.value()->shareable_bytes, bytes);
    QCOMPARE(recovered.value()->checkpoint_id, sha256_hex(bytes));
    QCOMPARE(recovered.value()->generation, uint64_t{1});
    QVERIFY(!recovered.value()->local_bytes);
  }

  void restartRecoversShareableAndLocalFilesUnderOneWriterLease() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const auto actor = Test::actor(115);
    const CheckpointContext context{"restart-document", "main", 1, "",
                                    CheckpointEngineKind::Text};
    const auto shareable = checkpoint(context.document);
    CheckpointLocalCore local;
    local.owner = {actor, sha256_hex(shareable)};
    local.writer = {actor, context.document, context.stream, context.epoch, 1, 0};
    auto local_bytes = encode_checkpoint_local_core(local, context, shareable);
    QVERIFY(local_bytes);
    {
      auto store = CheckpointStore::open(temp.path().toStdString());
      QVERIFY(store);
      QVERIFY(store.value().publish(shareable, local_bytes.value(), actor));
      auto competing_writer = CheckpointStore::open(temp.path().toStdString());
      QVERIFY(!competing_writer);
      QCOMPARE(competing_writer.error().code(), ErrorCode::writer_conflict);
    }

    auto restarted = CheckpointStore::open(temp.path().toStdString());
    QVERIFY(restarted);
    auto recovered = restarted.value().recover(actor, accept_checkpoint, context);
    QVERIFY(recovered);
    QVERIFY(recovered.value());
    QCOMPARE(recovered.value()->shareable_bytes, shareable);
    QCOMPARE(recovered.value()->local_bytes,
             std::optional<std::string>(local_bytes.value()));
  }

  void localCompanionMustMatchShareableOwnerFrontiersAndHistory() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const auto actor = Test::actor(116);
    const CheckpointContext context{"bound-local-document", "main", 1, "",
                                    CheckpointEngineKind::Text};
    CheckpointShareableCore core;
    core.context = context;
    core.causal.version.observe(Crdt::Identity(actor, 4));
    core.coverage.operation[actor] = 4;
    core.coverage.delivery[actor] = 9;
    core.pins.canonical_projection_hash = sha256_hex("");
    core.translations.supported_token_versions = {1};
    const auto shareable = encode_checkpoint_core(core);
    QVERIFY(shareable);

    CheckpointLocalCore local;
    local.owner = {actor, sha256_hex(shareable.value())};
    const auto identity = Crdt::Identity(actor, 3);
    local.history.entries.push_back(
        {identity, {identity}, {}, {}, 42});
    local.history.cursor = 1;
    local.writer = {actor, context.document, context.stream, context.epoch, 5, 9};
    auto local_bytes = encode_checkpoint_local_core(
        local, context, shareable.value());
    QVERIFY(local_bytes);

    auto store = CheckpointStore::open(temp.path().toStdString());
    QVERIFY(store);
    QVERIFY(store.value().publish(shareable.value(), local_bytes.value(), actor));

    auto stale_counter = local;
    stale_counter.writer.next_identity_counter = 4;
    auto stale_counter_bytes = encode_checkpoint_local_core(
        stale_counter, context, shareable.value());
    QVERIFY(stale_counter_bytes);
    auto counter_rejected = store.value().publish(
        shareable.value(), stale_counter_bytes.value(), actor);
    QVERIFY(!counter_rejected);
    QCOMPARE(counter_rejected.error().code(), ErrorCode::counter_regression);

    auto stale_delivery = local;
    stale_delivery.writer.durable_delivery_frontier = 8;
    auto stale_delivery_bytes = encode_checkpoint_local_core(
        stale_delivery, context, shareable.value());
    QVERIFY(stale_delivery_bytes);
    auto delivery_rejected = store.value().publish(
        shareable.value(), stale_delivery_bytes.value(), actor);
    QVERIFY(!delivery_rejected);
    QCOMPARE(delivery_rejected.error().code(), ErrorCode::corrupt_record);

    auto uncovered_history = local;
    const auto uncovered = Crdt::Identity(actor, 5);
    uncovered_history.history.entries[0].group_id = uncovered;
    uncovered_history.history.entries[0].operation_ids = {uncovered};
    auto uncovered_bytes = encode_checkpoint_local_core(
        uncovered_history, context, shareable.value());
    QVERIFY(uncovered_bytes);
    auto history_rejected = store.value().publish(
        shareable.value(), uncovered_bytes.value(), actor);
    QVERIFY(!history_rejected);
    QCOMPARE(history_rejected.error().code(), ErrorCode::corrupt_record);

    auto recovered = store.value().recover(actor, accept_checkpoint, context);
    QVERIFY(recovered);
    QVERIFY(recovered.value());
    QCOMPARE(recovered.value()->generation, uint64_t{1});
    QCOMPARE(recovered.value()->shareable_bytes, shareable.value());
    QCOMPARE(recovered.value()->local_bytes,
             std::optional<std::string>(local_bytes.value()));
  }

  void failedPublicationKeepsPreviousManifestAndCorruptNewestFallsBack() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const auto actor = Test::actor(112);
    bool fail_manifest_replace = false;
    StepHook hook;
    hook.fail = [&](Step step, const fs::path &path) -> std::optional<Error> {
      if (fail_manifest_replace && step == Step::Replace &&
          path.filename().string().starts_with("manifest-"))
        return Error(ErrorCode::io_error, "injected manifest replace failure");
      return {};
    };
    auto store = CheckpointStore::open(temp.path().toStdString(), &hook);
    QVERIFY(store);
    const auto old_bytes = checkpoint("old-document");
    auto old = store.value().publish(old_bytes, {}, actor);
    QVERIFY(old);

    const auto new_bytes = checkpoint("new-document");
    fail_manifest_replace = true;
    auto failed = store.value().publish(new_bytes, {}, actor);
    QVERIFY(!failed);
    fail_manifest_replace = false;
    auto after_failure = store.value().recover(actor, accept_checkpoint);
    QVERIFY(after_failure);
    QVERIFY(after_failure.value());
    QCOMPARE(after_failure.value()->shareable_bytes, old_bytes);

    auto newest = store.value().publish(new_bytes, {}, actor);
    QVERIFY(newest);
    const auto newest_manifest = fs::path(temp.path().toStdString()) / "manifest-0";
    {
      std::ofstream corrupt(newest_manifest, std::ios::binary | std::ios::trunc);
      QVERIFY(corrupt.good());
      corrupt << "damaged manifest";
    }
    auto manifest_fallback = store.value().recover(actor, accept_checkpoint);
    QVERIFY(manifest_fallback);
    QVERIFY(manifest_fallback.value());
    QCOMPARE(manifest_fallback.value()->shareable_bytes, old_bytes);
    fs::remove(newest_manifest);
    QVERIFY(fs::create_directory(newest_manifest));
    auto unreadable_manifest_fallback = store.value().recover(actor, accept_checkpoint);
    QVERIFY(unreadable_manifest_fallback);
    QVERIFY(unreadable_manifest_fallback.value());
    QCOMPARE(unreadable_manifest_fallback.value()->shareable_bytes, old_bytes);
    QVERIFY(fs::remove(newest_manifest));

    // Re-publish the second generation, then corrupt its data file. The
    // alternating prior manifest must still provide a complete fallback.
    newest = store.value().publish(new_bytes, {}, actor);
    QVERIFY(newest);
    auto reject_new_candidate = [&](std::string_view candidate,
                                    const std::optional<std::string> &) -> Result<void> {
      if (candidate == new_bytes)
        return Error(ErrorCode::wrong_epoch,
                     "candidate is outside the provider's accepted epoch state");
      return {};
    };
    auto semantic_fallback = store.value().recover(actor, reject_new_candidate);
    QVERIFY(semantic_fallback);
    QVERIFY(semantic_fallback.value());
    QCOMPARE(semantic_fallback.value()->shareable_bytes, old_bytes);

    const auto newest_path = fs::path(temp.path().toStdString()) /
        ("shareable-" + sha256_hex(new_bytes));
    {
      std::ofstream corrupt(newest_path, std::ios::binary | std::ios::trunc);
      QVERIFY(corrupt.good());
      corrupt << "damaged";
    }
    auto fallback = store.value().recover(actor, accept_checkpoint);
    QVERIFY(fallback);
    QVERIFY(fallback.value());
    QCOMPARE(fallback.value()->shareable_bytes, old_bytes);
    QCOMPARE(fallback.value()->generation, uint64_t{1});
  }

  void everyManifestPublicationFailureRecoversAnOldOrNewCompleteBundle() {
    const std::array<Step, 5> steps{Step::Write, Step::FileSync, Step::Close,
                                    Step::Replace, Step::DirSync};
    for (const auto failed_step : steps) {
      QTemporaryDir temp;
      QVERIFY(temp.isValid());
      const auto actor = Test::actor(114);
      bool inject = false;
      bool manifest_renamed = false;
      StepHook hook;
      hook.fail = [&](Step step, const fs::path &path) -> std::optional<Error> {
        const bool manifest_path = path.filename().string().starts_with("manifest-");
        const bool manifest_directory_sync =
            step == Step::DirSync && manifest_renamed;
        if (inject && step == failed_step &&
            (manifest_path || manifest_directory_sync))
          return Error(ErrorCode::io_error, "injected atomic manifest failure");
        return {};
      };
      hook.observe = [&](Step step, const fs::path &path) {
        if (step == Step::Replace &&
            path.filename().string().starts_with("manifest-"))
          manifest_renamed = true;
      };
      auto store = CheckpointStore::open(temp.path().toStdString(), &hook);
      QVERIFY(store);
      const auto old_bytes = checkpoint("stable-before-publication");
      QVERIFY(store.value().publish(old_bytes, {}, actor));
      const auto new_bytes = checkpoint("candidate-during-publication");
      manifest_renamed = false;
      inject = true;
      const auto publish = store.value().publish(new_bytes, {}, actor);
      QVERIFY(!publish);

      auto recovered = store.value().recover(actor, accept_checkpoint);
      QVERIFY(recovered);
      QVERIFY(recovered.value());
      if (failed_step == Step::DirSync) {
        QCOMPARE(recovered.value()->shareable_bytes, new_bytes);
        QCOMPARE(recovered.value()->generation, uint64_t{2});
      } else {
        QCOMPARE(recovered.value()->shareable_bytes, old_bytes);
        QCOMPARE(recovered.value()->generation, uint64_t{1});
      }
    }
  }

  void artifactFailuresNeverAdvanceTheManifest() {
    const std::array<Step, 5> steps{Step::Write, Step::FileSync, Step::Close,
                                    Step::Replace, Step::DirSync};
    for (const auto failed_step : steps) {
      QTemporaryDir temp;
      QVERIFY(temp.isValid());
      const auto actor = Test::actor(116);
      bool inject = false;
      bool artifact_renamed = false;
      StepHook hook;
      hook.fail = [&](Step step, const fs::path &path) -> std::optional<Error> {
        const bool artifact_path = path.filename().string().starts_with("shareable-");
        const bool artifact_directory_sync = step == Step::DirSync && artifact_renamed;
        if (inject && step == failed_step &&
            (artifact_path || artifact_directory_sync))
          return Error(ErrorCode::io_error, "injected checkpoint artifact failure");
        return {};
      };
      hook.observe = [&](Step step, const fs::path &path) {
        if (step == Step::Replace &&
            path.filename().string().starts_with("shareable-"))
          artifact_renamed = true;
      };
      auto store = CheckpointStore::open(temp.path().toStdString(), &hook);
      QVERIFY(store);
      const auto old_bytes = checkpoint("artifact-stable-state");
      QVERIFY(store.value().publish(old_bytes, {}, actor));
      const auto candidate = checkpoint("artifact-not-yet-published");
      artifact_renamed = false;
      inject = true;
      QVERIFY(!store.value().publish(candidate, {}, actor));

      auto recovered = store.value().recover(actor, accept_checkpoint);
      QVERIFY(recovered);
      QVERIFY(recovered.value());
      QCOMPARE(recovered.value()->shareable_bytes, old_bytes);
      QCOMPARE(recovered.value()->generation, uint64_t{1});
    }
  }

  void manifestSlotsRollOverAndKeepThePreviousCompleteGeneration() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const auto actor = Test::actor(117);
    auto store = CheckpointStore::open(temp.path().toStdString());
    QVERIFY(store);
    const auto first = checkpoint("generation-one");
    const auto second = checkpoint("generation-two");
    const auto third = checkpoint("generation-three");
    auto first_publish = store.value().publish(first, {}, actor);
    QVERIFY(first_publish);
    QCOMPARE(first_publish.value().generation, uint64_t{1});
    auto second_publish = store.value().publish(second, {}, actor);
    QVERIFY(second_publish);
    QCOMPARE(second_publish.value().generation, uint64_t{2});
    auto third_publish = store.value().publish(third, {}, actor);
    QVERIFY(third_publish);
    QCOMPARE(third_publish.value().generation, uint64_t{3});
    QVERIFY(fs::exists(fs::path(temp.path().toStdString()) / "manifest-0"));
    QVERIFY(fs::exists(fs::path(temp.path().toStdString()) / "manifest-1"));

    const auto third_artifact = fs::path(temp.path().toStdString()) /
        ("shareable-" + sha256_hex(third));
    QVERIFY(fs::remove(third_artifact));
    auto fallback = store.value().recover(actor, accept_checkpoint);
    QVERIFY(fallback);
    QVERIFY(fallback.value());
    QCOMPARE(fallback.value()->shareable_bytes, second);
    QCOMPARE(fallback.value()->generation, uint64_t{2});
    QVERIFY(fs::exists(fs::path(temp.path().toStdString()) /
                       ("shareable-" + sha256_hex(first))));
  }

  void rejectsWrongContextAndCompanionBinding() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const auto actor = Test::actor(113);
    auto store = CheckpointStore::open(temp.path().toStdString());
    QVERIFY(store);
    const auto bytes = checkpoint("document-a");
    auto published = store.value().publish(bytes, {}, actor);
    QVERIFY(published);
    auto wrong_context = store.value().recover(
        actor, accept_checkpoint,
        CheckpointContext{"document-b", "main", 1, "",
                          CheckpointEngineKind::Text});
    QVERIFY(!wrong_context);

    CheckpointLocalCore local;
    local.owner = {actor, sha256_hex("wrong checkpoint")};
    local.writer = {actor, "document-a", "main", 1, 1, 0};
    auto local_bytes = encode_checkpoint_local_core(
        local, CheckpointContext{"document-a", "main", 1, "",
                                 CheckpointEngineKind::Text}, bytes);
    QVERIFY(!local_bytes);
    QVERIFY(!store.value().publish(bytes, std::string("not-a-local-checkpoint"), actor));

    const CheckpointContext context{"document-a", "main", 1, "",
                                    CheckpointEngineKind::Text};
    local.owner = {actor, sha256_hex(bytes)};
    local.writer = {actor, context.document, context.stream, context.epoch, 1, 0};
    auto valid_local = encode_checkpoint_local_core(local, context, bytes);
    const auto local_error = valid_local
        ? std::string{} : std::string(valid_local.error().message());
    QVERIFY2(valid_local, local_error.c_str());
    auto with_local = store.value().publish(bytes, valid_local.value(), actor);
    QVERIFY(with_local);
    auto recovered = store.value().recover(actor, accept_checkpoint, context);
    QVERIFY(recovered);
    QVERIFY(recovered.value());
    QCOMPARE(recovered.value()->local_bytes,
             std::optional<std::string>(valid_local.value()));

    const auto other_shareable = checkpoint("another-document");
    local.owner.shareable_checkpoint_hash = sha256_hex(other_shareable);
    const CheckpointContext other_context{"another-document", "main", 1, "",
                                          CheckpointEngineKind::Text};
    local.writer.document = other_context.document;
    auto foreign_local = encode_checkpoint_local_core(
        local, other_context, other_shareable);
    QVERIFY(foreign_local);
    QVERIFY(!store.value().publish(bytes, foreign_local.value(), actor));
  }
};

QTEST_GUILESS_MAIN(CheckpointRecoveryTest)
#include "tst_checkpoint_recovery.moc"
