"""Repeat the measured workload against an already-built Release tree.

See ../2026-10-19-performance-evaluation.md for interpretation and build commands.
The output directory must be new, so recorded evidence cannot be overwritten.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument("repo", type=Path)
parser.add_argument("build", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
repo, build, output = (p.resolve() for p in (args.repo, args.build, args.output))
output.mkdir(parents=True, exist_ok=False)
source = Path(__file__).resolve().parent
env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
methods = [
    "single_replica_throughput", "single_replica_large_doc", "edit_patterns",
    "edit_sizes", "tombstone_degradation", "tombstone_degradation_large",
    "multi_replica_convergence", "undo_redo_cycle", "undo_stack_depth",
    "fragment_proliferation", "memory_growth", "reconnect_sync_cost",
]

# Finish compilation before any timed process starts.
for name in ("public_api", "bulk_limit"):
    subprocess.run([
        "c++", "-std=c++20", "-O3", "-DNDEBUG",
        "-I", str(repo / "libs/collabtext/include"),
        "-I", str(repo / "libs/collabtext/src"), str(source / (name + ".cpp")),
        str(build / "libs/collabtext/libcollabtext_core.a"), "-pthread",
        "-o", str(output / name),
    ], check=True)

def capture(argv):
    return subprocess.check_output(argv, text=True).strip()

meta = {
    "revision": capture(["git", "-C", str(repo), "rev-parse", "HEAD"]),
    "tracked_diff": capture(["git", "-C", str(repo), "diff", "HEAD", "--stat"]),
    "platform": platform.platform(), "cpu": capture(["lscpu"]),
    "compiler": capture(["c++", "--version"]),
    "filesystem": capture(["stat", "-f", "-c", "%T", str(output)]),
    "harness_sha256": hashlib.sha256((source / "public_api.cpp").read_bytes()).hexdigest(),
    "core_flags": (build / "libs/collabtext/CMakeFiles/collabtext_core.dir/flags.make").read_text(),
    "methods": methods, "repeats": 5, "commands": [],
}

def run(label, argv):
    before = Path("/proc/loadavg").read_text().strip()
    start = time.time()
    with (output / (label + ".log")).open("w") as log:
        result = subprocess.run(argv, env=env, stdout=log,
                                stderr=subprocess.STDOUT, timeout=300)
    row = {"label": label, "argv": argv, "exit_status": result.returncode,
           "wall_seconds": time.time() - start, "load_before": before,
           "load_after": Path("/proc/loadavg").read_text().strip()}
    meta["commands"].append(row)
    (output / "environment.json").write_text(json.dumps(meta, indent=2) + "\n")
    print(json.dumps(row), flush=True)
    result.check_returncode()

for repeat in range(1, 6):
    run(f"native-{repeat}", [str(build / "libs/collabtext/tst_benchmark"), *methods])
    run(f"public-{repeat}", [str(output / "public_api")])
    if repeat == 1:
        (output / "public-1.jsonl").write_bytes((output / "public-1.log").read_bytes())
for count in (512, 2048):
    run(f"recovery-{count}", [str(build / "libs/collabtext/tst_checkpoint_recovery_benchmark"), str(count)])
run("bulk-limit", [str(output / "bulk_limit")])
subprocess.run(["python3", str(source / "aggregate.py"), str(output)], check=True)
