#include <collabtext/Serialization.h>

#include "crdt/Buffer.h"
#include "crdt/SegmentWriter.h"
#include "TestActor.h"

#include <cstdlib>
#include <filesystem>
#include <string>

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const std::filesystem::path dir(argv[1]);
    const std::string crash_point(argv[2]);
    CollabText::Storage::StepHook hook;
    hook.observe = [&](CollabText::Storage::Step step,
                       const std::filesystem::path& path) {
        const std::string name = path.filename().string();
        if (crash_point == "temp_sync" && step == CollabText::Storage::Step::FileSync
            && name.find(".seg.zst.tmp.") != std::string::npos)
            std::_Exit(81);
        if (crash_point == "publish" && step == CollabText::Storage::Step::Replace
            && name.ends_with(".seg.zst"))
            std::_Exit(82);
    };

    auto cfg = CollabText::Crdt::WriterConfig{};
    cfg.step_hook = &hook;
    CollabText::Crdt::SegmentWriter writer(
        dir, CollabText::Crdt::SegmentKind::Ops, cfg);
    if (!writer.start().ok()) return 3;
    CollabText::Crdt::Buffer buffer(CollabText::Test::test_counter(81));
    const auto op = buffer.apply_local_edit({{0, 0}}, {"crash-safe"});
    const auto wire = CollabText::Crdt::encode_operation(op);
    if (!wire.ok()) return 4;
    if (!writer.append(wire.value(),
                       CollabText::Crdt::op_identity(op).counter).ok()) return 5;
    if (!writer.tick(std::chrono::steady_clock::now()).ok()) return 6;
    if (!writer.close().ok()) return 7;
    return 0;
}
