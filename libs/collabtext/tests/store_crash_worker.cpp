// store_crash_worker.cpp — a child process that dies mid-write.
//
// Roadmap task V07. The worker runs the harness's durable write sequence and
// terminates at a named hook point, so a test can assert what a *process* crash
// leaves behind: user-space buffers are gone, and whatever reached the
// filesystem is either the old content or the new content, never a torn
// record.
//
// What this does not prove: that the data survived. Losing a process does not
// lose the page cache, so nothing here says anything about power loss or a
// machine that stops. That evidence needs a real fsync and a real power-loss
// test, which is L01/L02's job. The worker says so on stderr before it dies.
//
// Usage:
//   store_crash_worker <dir> <stream-name> <point|none> [records] [phase]
//
// <point> is a point name such as "write.after" or "rename.before"; "none"
// means run to completion and exit 0. <phase> is "append" (the default) or
// "seal": the rename, directory-sync, seal, cursor and checkpoint points are
// only reachable while sealing or publishing, so a crash at one of those needs
// the records written first and the fault armed afterwards.

#include "StoreFaultHarness.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace CollabText::StoreFault;

namespace {

struct NamedPoint {
    FaultPoint point;
    const char* name;
};

const std::vector<NamedPoint>& named_points() {
    static const std::vector<NamedPoint> points = {
        {FaultPoint::WriteBefore, "write.before"},
        {FaultPoint::WriteAfter, "write.after"},
        {FaultPoint::FileSyncBefore, "file_sync.before"},
        {FaultPoint::FileSyncAfter, "file_sync.after"},
        {FaultPoint::RenameBefore, "rename.before"},
        {FaultPoint::RenameAfter, "rename.after"},
        {FaultPoint::DirSyncBefore, "dir_sync.before"},
        {FaultPoint::DirSyncAfter, "dir_sync.after"},
        {FaultPoint::SealBefore, "seal.before"},
        {FaultPoint::SealAfter, "seal.after"},
        {FaultPoint::CursorPublishBefore, "cursor_publish.before"},
        {FaultPoint::CursorPublishAfter, "cursor_publish.after"},
        {FaultPoint::CheckpointPublishBefore, "checkpoint_publish.before"},
        {FaultPoint::CheckpointPublishAfter, "checkpoint_publish.after"},
    };
    return points;
}

FaultPoint parse_point(const std::string& name, bool* found) {
    for (const auto& entry : named_points()) {
        if (name == entry.name) {
            *found = true;
            return entry.point;
        }
    }
    *found = false;
    return FaultPoint::WriteBefore;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
                     "usage: %s <dir> <stream-name> <point|none> [records-before]\n",
                     argv[0]);
        return 2;
    }
    const std::string dir = argv[1];
    const std::string name = argv[2];
    const std::string point_name_arg = argv[3];
    const int records = argc > 4 ? std::atoi(argv[4]) : 2;
    const std::string phase = argc > 5 ? argv[5] : "append";
    const bool seal_phase = phase == "seal";

    bool found = false;
    const FaultPoint point = parse_point(point_name_arg, &found);
    if (point_name_arg != "none" && !found) {
        std::fprintf(stderr, "unknown fault point: %s\n", point_name_arg.c_str());
        return 2;
    }

    FaultPlan plan;
    DurableWriter writer(dir, name, &plan);

    // Steady state first, with nothing armed: some records written and fsynced
    // normally, so the crash point lands on a stream that already has durable
    // content.
    for (int i = 0; i < std::max(1, records); ++i) {
        if (!writer.append("stable record " + std::to_string(i))) {
            std::fprintf(stderr, "append failed: %s\n", writer.last_error().c_str());
            return 3;
        }
    }

    std::fprintf(stderr,
                 "phase=%s terminating at %s after %d durable records; process death "
                 "is not power-loss evidence\n",
                 phase.c_str(), point_name_arg.c_str(), records);
    std::fflush(stderr);

    // Arm the fault now. In the append phase the next append trips it; in the
    // seal phase the seal or publication does.
    if (found) plan.arm(point, FaultAction::Abort, /*skip=*/0);

    // With no fault armed, run the whole sequence regardless of phase.
    if (found && !seal_phase) {
        if (!writer.append("the record that never finished")) {
            std::fprintf(stderr, "append failed: %s\n", writer.last_error().c_str());
            return 3;
        }
        std::fprintf(stderr, "expected to abort before this line\n");
        return 4;
    }

    if (!writer.seal(1)) {
        std::fprintf(stderr, "seal failed: %s\n", writer.last_error().c_str());
        return 5;
    }
    if (!writer.publish_cursor(2)) {
        std::fprintf(stderr, "cursor publish failed: %s\n", writer.last_error().c_str());
        return 6;
    }
    if (!writer.publish_checkpoint(1, 2)) {
        std::fprintf(stderr, "checkpoint publish failed: %s\n",
                     writer.last_error().c_str());
        return 7;
    }
    std::fprintf(stderr, "worker completed normally\n");
    return 0;
}
