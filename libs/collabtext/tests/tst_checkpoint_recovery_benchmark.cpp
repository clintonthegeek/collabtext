#include <QTemporaryDir>

#include <collabtext/DocumentSession.h>
#include <collabtext/Serialization.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace CollabText;

namespace {
constexpr size_t kRepeats = 5;

bool copy_tree(const fs::path &from, const fs::path &to) {
  std::error_code error;
  fs::copy(from, to, fs::copy_options::recursive, error);
  return !error;
}

int fail(const Error &error) {
  std::cerr << "checkpoint recovery benchmark failed: " << error.message();
  if (const auto context = error.context())
    std::cerr << " (" << *context << ')';
  std::cerr << '\n';
  return 1;
}

Result<uint64_t> measure_open(DocumentSessionConfig config,
                              const std::string &expected_text) {
  const auto started = std::chrono::steady_clock::now();
  auto opened = DocumentSession::open(std::move(config));
  const auto finished = std::chrono::steady_clock::now();
  if (!opened.ok()) return opened.error();
  auto session = std::move(opened).value();
  if (session->text() != expected_text || !session->ready()) {
    (void)session->close();
    return Error(ErrorCode::malformed,
                 "recovery benchmark branches did not reconstruct equivalent text");
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      finished - started).count();
  auto closed = session->close();
  if (!closed.ok()) return closed.error();
  return static_cast<uint64_t>(elapsed);
}

uint64_t median(std::vector<uint64_t> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}
} // namespace

int main(int argc, char **argv) {
  uint64_t operation_count = 512;
  if (argc > 1) {
    try {
      size_t consumed = 0;
      operation_count = std::stoull(argv[1], &consumed);
      if (consumed != std::string(argv[1]).size() || operation_count == 0 ||
          operation_count > 50000)
        throw std::invalid_argument("operation count must be 1..50000");
    } catch (const std::exception &error) {
      std::cerr << error.what() << '\n';
      return 2;
    }
  }
  QTemporaryDir temporary;
  if (!temporary.isValid()) {
    std::cerr << "unable to create temporary benchmark directory\n";
    return 1;
  }
  const fs::path root = temporary.path().toStdString();
  DocumentSessionConfig source_config{root / "source-shared",
                                      root / "source-device",
                                      "checkpoint-benchmark-doc",
                                      "checkpoint-benchmark-stream", 1};
  source_config.checkpoint_store = root / "source-checkpoints";
  source_config.validate_checkpoint_candidate =
      [](std::string_view shareable,
         const std::optional<std::string> &local) -> Result<void> {
    if (shareable.empty() || !local)
      return Error(ErrorCode::malformed, "incomplete benchmark checkpoint");
    return {};
  };

  auto source_open = DocumentSession::open(source_config);
  if (!source_open.ok()) return fail(source_open.error());
  auto source = std::move(source_open).value();
  for (uint64_t i = 0; i < operation_count; ++i) {
    const auto end = source->visible_length();
    auto edit = source->apply_local({{end, end}}, {"x"});
    if (!edit.ok()) return fail(edit.error());
  }
  const auto expected_text = source->text();
  SessionCheckpointProviderState provider;
  provider.canonical_pin_projection_hash =
      "e3b0c44298fc1c149afbf4c8996fb924"
      "27ae41e4649b934ca495991b7852b855";
  auto published = source->publish_checkpoint(provider);
  if (!published.ok()) return fail(published.error());
  auto closed = source->close();
  if (!closed.ok()) return fail(closed.error());

  const fs::path checkpoint_root = root / "checkpoint-branch";
  const fs::path full_root = root / "full-replay-branch";
  const auto copy_branch = [&](const fs::path &branch) {
    std::error_code error;
    fs::create_directories(branch, error);
    if (error) return false;
    return copy_tree(source_config.shared_store, branch / "shared") &&
           copy_tree(source_config.private_state, branch / "device") &&
           copy_tree(source_config.checkpoint_store, branch / "checkpoints");
  };
  if (!copy_branch(checkpoint_root) || !copy_branch(full_root)) {
    std::cerr << "unable to clone benchmark recovery branches\n";
    return 1;
  }

  auto checkpoint_config = source_config;
  checkpoint_config.shared_store = checkpoint_root / "shared";
  checkpoint_config.private_state = checkpoint_root / "device";
  checkpoint_config.checkpoint_store = checkpoint_root / "checkpoints";
  auto full_config = source_config;
  full_config.shared_store = full_root / "shared";
  full_config.private_state = full_root / "device";
  full_config.checkpoint_store.clear();
  full_config.validate_checkpoint_candidate = {};

  std::vector<uint64_t> checkpoint_ns;
  std::vector<uint64_t> full_replay_ns;
  checkpoint_ns.reserve(kRepeats);
  full_replay_ns.reserve(kRepeats);
  for (size_t repeat = 0; repeat < kRepeats; ++repeat) {
    if ((repeat & 1u) == 0) {
      auto checkpoint = measure_open(checkpoint_config, expected_text);
      if (!checkpoint.ok()) return fail(checkpoint.error());
      checkpoint_ns.push_back(checkpoint.value());
      auto full = measure_open(full_config, expected_text);
      if (!full.ok()) return fail(full.error());
      full_replay_ns.push_back(full.value());
    } else {
      auto full = measure_open(full_config, expected_text);
      if (!full.ok()) return fail(full.error());
      full_replay_ns.push_back(full.value());
      auto checkpoint = measure_open(checkpoint_config, expected_text);
      if (!checkpoint.ok()) return fail(checkpoint.error());
      checkpoint_ns.push_back(checkpoint.value());
    }
  }

  const auto checkpoint_median = median(checkpoint_ns);
  const auto full_median = median(full_replay_ns);
  std::cout << "checkpoint_recovery_benchmark"
            << " operations=" << operation_count
            << " text_bytes=" << expected_text.size()
            << " repeats=" << kRepeats
            << " checkpoint_median_ns=" << checkpoint_median
            << " full_replay_median_ns=" << full_median
            << " checkpoint_to_full_ratio="
            << static_cast<double>(checkpoint_median) /
                   static_cast<double>(full_median)
            << '\n';
  return 0;
}
