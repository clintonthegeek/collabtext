#!/usr/bin/env python3
"""Build and run a checked, paired CollabText performance matrix."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import statistics
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent
BASELINE = "d04f151b3a70b161b72c33a8de6895efbd1c13e2"
METHODS = [
    "single_replica_throughput", "single_replica_large_doc", "edit_patterns",
    "edit_sizes", "tombstone_degradation", "tombstone_degradation_large",
    "multi_replica_convergence", "undo_redo_cycle", "undo_stack_depth",
    "fragment_proliferation", "memory_growth", "reconnect_sync_cost",
]
QUANTILES = (("p50_ns", 0.50), ("p95_ns", 0.95), ("p99_ns", 0.99))


def statistic(samples):
    ordered = sorted(samples)
    result = {"mean_ns": statistics.fmean(ordered), "n": len(ordered)}
    for name, q in QUANTILES:
        result[name] = ordered[min(len(ordered) - 1, int(q * (len(ordered) - 1)))]
    return result


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def capture(command, **kwargs):
    return subprocess.check_output(command, text=True, **kwargs).strip()


def validate_trace(trace_data):
    if trace_data.get("schema") != "collabtext-benchmark-trace/1":
        raise RuntimeError("unsupported workload trace schema")
    dimensions = trace_data.get("dimensions", {})
    if trace_data.get("seed") != 42:
        raise RuntimeError("this harness is pinned to trace seed 42")
    if dimensions.get("text_bytes") != [1000, 10000, 100000, 1000000]:
        raise RuntimeError("trace text size grid does not match the checked harness")
    if dimensions.get("replacement_calls") != 200 or dimensions.get("query_calls") != 50:
        raise RuntimeError("trace call windows do not match the checked harness")
    if dimensions.get("codec_calls") != 100 or dimensions.get("bulk_calls") != 5:
        raise RuntimeError("trace codec/bulk windows do not match the checked harness")
    if dimensions.get("list_entries") != [100, 1000, 5000] or dimensions.get("causal_actors") != [1, 32, 128, 512]:
        raise RuntimeError("trace list/causal frontier grid does not match the checked harness")
    return dimensions


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-root", type=Path, required=True,
                        help="source snapshot of the pinned baseline commit")
    parser.add_argument("--baseline-sha", default=BASELINE)
    parser.add_argument("--candidate-root", type=Path, default=ROOT)
    parser.add_argument("--output", type=Path, required=True,
                        help="new directory for builds, raw runs and aggregates")
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--smoke", action="store_true",
                        help="one quick public harness run per build; not evidence of a win")
    args = parser.parse_args()
    baseline_root = args.baseline_root.resolve()
    candidate_root = args.candidate_root.resolve()
    output = args.output.resolve()
    if args.repeats < 1:
        parser.error("--repeats must be positive")
    if baseline_root == candidate_root or not (baseline_root / "CMakeLists.txt").is_file():
        parser.error("--baseline-root must be an extracted source snapshot")
    if not (candidate_root / "benchmarks/BenchmarkRunner.cpp").is_file():
        parser.error("candidate source must contain benchmarks/BenchmarkRunner.cpp")
    if output.exists():
        parser.error("output directory must not already exist")
    output.mkdir(parents=True)
    cache = ROOT / ".cache/ccache"
    cache.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, CCACHE_DIR=str(cache), QT_QPA_PLATFORM="offscreen")
    trace = candidate_root / "benchmarks/traces/workloads-v1.json"
    harness = candidate_root / "benchmarks/BenchmarkRunner.cpp"
    trace_data = json.loads(trace.read_text())
    dimensions = validate_trace(trace_data)
    meta = {
        "schema": "collabtext-performance-matrix/1",
        "baseline_sha": args.baseline_sha,
        "baseline_source": str(baseline_root),
        "candidate_sha": capture(["git", "-C", str(candidate_root), "rev-parse", "HEAD"]),
        "candidate_status": capture(["git", "-C", str(candidate_root), "status", "--short"]),
        "candidate_diff_sha256": hashlib.sha256(
            capture(["git", "-C", str(candidate_root), "diff", "HEAD", "--"]).encode()
        ).hexdigest(),
        "trace_sha256": sha256(trace),
        "harness_sha256": sha256(harness),
        "matrix_runner_sha256": sha256(Path(__file__).resolve()),
        "candidate_input_sha256": {
            "superproject_cmake": sha256(candidate_root / "CMakeLists.txt"),
            "library_cmake": sha256(candidate_root / "libs/collabtext/CMakeLists.txt"),
            "harness": sha256(harness),
            "trace": sha256(trace),
        },
        "host": platform.platform(),
        "cpu": capture(["lscpu"]),
        "compiler": capture(["c++", "--version"]),
        "filesystem": capture(["stat", "-f", "-c", "%T", str(output)]),
        "governor": next((p.read_text().strip() for p in
                          [Path("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")]
                          if p.is_file()), None),
        "build_flags": "-O3 -DNDEBUG; C++20; COLLABTEXT_ENABLE_SANITIZERS=OFF",
        "profile": "quick-smoke" if args.smoke else "full-paired-matrix",
        "repeats": 1 if args.smoke else args.repeats,
        "native_methods": [] if args.smoke else METHODS,
        "commands": [],
    }

    def run(label, command, timeout=600, log_path=None):
        before = Path("/proc/loadavg").read_text().strip()
        started = time.time()
        log_path = log_path or (output / (label + ".log"))
        status = None
        timed_out = False
        with log_path.open("w") as log:
            try:
                result = subprocess.run(command, cwd=ROOT, env=env, stdout=log,
                                        stderr=subprocess.STDOUT, timeout=timeout)
                status = result.returncode
            except subprocess.TimeoutExpired:
                timed_out = True
                status = "timeout"
        record = {
            "label": label, "command": list(map(str, command)),
            "exit_status": status, "timed_out": timed_out,
            "wall_seconds": time.time() - started,
            "load_before": before, "load_after": Path("/proc/loadavg").read_text().strip(),
            "log": str(log_path),
        }
        meta["commands"].append(record)
        (output / "environment.json").write_text(json.dumps(meta, indent=2) + "\n")
        if status != 0:
            raise RuntimeError(f"{label} failed ({status}); see {log_path}")
        return record

    def configure_and_build(label, source):
        build = output / ("build-" + label)
        run(label + "-configure", [
            "cmake", "-S", source, "-B", build,
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCOLLABTEXT_BUILD_TESTS=OFF",
            "-DCOLLABTEXT_BUILD_BENCHMARKS=ON",
            "-DCOLLABTEXT_BUILD_QT=OFF",
            "-DCOLLABTEXT_BUILD_APPS=OFF",
            "-DCOLLABTEXT_BUILD_FILE_TRANSPORT=ON",
            "-DCOLLABTEXT_ENABLE_SANITIZERS=OFF",
            "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache",
        ], timeout=180)
        targets = ["collabtext_core", "collabtext_filetransport", "tst_benchmark",
                   "tst_checkpoint_recovery_benchmark"]
        if label == "candidate":
            targets.append("collabtext_performance_benchmark")
        run(label + "-build", ["cmake", "--build", build, "--target", *targets, "-j16"])
        ctest_file = build / "libs/collabtext/CTestTestfile.cmake"
        registered = ctest_file.read_text() if ctest_file.exists() else ""
        if "benchmark" in registered.lower():
            raise RuntimeError(f"{label} registered an opt-in benchmark with CTest")
        return build

    def compile_baseline_harness(build):
        binary = output / "benchmark-baseline"
        source = baseline_root / "libs/collabtext"
        run("baseline-harness-compile", [
            "ccache", "c++", "-std=c++20", "-O3", "-DNDEBUG",
            "-I", source / "include",
            # The checked harness may depend on post-baseline, header-only
            # diagnostics. Their disabled inline implementation lets the same
            # harness compile against the pinned baseline without copying new
            # engine code or changing its library binary.
            "-I", candidate_root / "libs/collabtext/src", "-I", source / "src",
            harness, build / "libs/collabtext/libcollabtext_core.a",
            "-pthread", "-o", binary,
        ], timeout=180)
        return binary

    def read_public(path):
        rows = [json.loads(line) for line in path.read_text().splitlines()]
        headers = [r for r in rows if r.get("record") == "header"]
        cases = [r for r in rows if r.get("record") == "case"]
        samples = [r for r in rows if r.get("record") == "sample"]
        limits = [r for r in rows if r.get("record") == "limit"]
        if len(headers) != 1 or headers[0].get("schema") != "collabtext-performance-samples/1":
            raise RuntimeError(f"invalid public harness header: {path}")
        expected_profile = "quick" if args.smoke else "full"
        if headers[0].get("profile") != expected_profile or headers[0].get("seed") != trace_data["seed"]:
            raise RuntimeError(f"profile/seed differs from workload trace: {path}")
        by_name = {}
        for sample in samples:
            by_name.setdefault(sample["workload"], []).append(sample["latency_ns"])
        names = [case["workload"] for case in cases]
        if len(names) != len(set(names)) or set(names) != set(by_name):
            raise RuntimeError(f"missing or duplicate public workload records: {path}")
        for case in cases:
            actual = len(by_name[case["workload"]])
            if actual != case["samples"] or case["attempted"] != actual or case["accepted"] != actual:
                raise RuntimeError(f"public work accounting mismatch: {case}")
            if case["refused"] or case["no_op"]:
                raise RuntimeError(f"unexpected refused/no-op work: {case}")
            name = case["workload"]
            expected_samples = (
                5 if args.smoke and (name.startswith("public_replace_") or
                                     name.startswith("byte_replace_") or
                                     name.startswith("unicode_append_")) else
                10 if args.smoke and name.startswith(("codec_encode_", "codec_decode_")) else
                1 if args.smoke and name.startswith("bulk_") else
                5 if args.smoke and ("boundary_" in name or "attachment_" in name or "marker_resolve_" in name or
                                     name.startswith(("list_identity_", "list_anchor_",
                                                      "list_projection_", "list_single_insert_"))) else
                dimensions["replacement_calls"] if name.startswith(("public_replace_", "byte_replace_", "unicode_append_")) else
                dimensions["query_calls"] if ("boundary_" in name or "attachment_" in name or "marker_resolve_" in name or
                                               name.startswith(("list_identity_", "list_anchor_",
                                                                "list_projection_", "list_single_insert_"))) else
                dimensions["codec_calls"] if name.startswith(("codec_encode_", "codec_decode_")) else
                dimensions["bulk_calls"] if name.startswith("bulk_") else None
            )
            if expected_samples is None or actual != expected_samples:
                raise RuntimeError(f"trace sample count mismatch for {name}: {actual}")
        if len(limits) != 1 or limits[0] != {
                "record": "limit", "bytes": 1000000, "outcome": "limit_exceeded",
                "text_empty": True, "allocator_unchanged": True}:
            raise RuntimeError(f"default import refusal contract failed: {path}")
        pairs = {}
        for case in cases:
            if case["workload"].startswith("public_replace_"):
                byte_name = case["workload"].replace("public_replace_", "byte_replace_", 1)
                other = next((item for item in cases if item["workload"] == byte_name), None)
                if not other or case["operation_fnv1a64"] != other["operation_fnv1a64"]:
                    raise RuntimeError(f"public/byte trace operation mismatch: {case['workload']}")
            pairs[case["workload"]] = {
                "samples_ns": by_name[case["workload"]],
                "case": {k: v for k, v in case.items() if k != "record"},
            }
        return pairs

    def read_native(path):
        if "Totals: 14 passed, 0 failed, 0 skipped" not in path.read_text():
            raise RuntimeError(f"native benchmark did not report all 12 selected cases passing: {path}")
        rows = []
        for line in path.read_text().splitlines():
            if line.startswith('{"name":'):
                rows.append(json.loads(line))
        if not rows:
            raise RuntimeError(f"native benchmark produced no structured rows: {path}")
        return {row["name"]: row for row in rows}

    def read_recovery(path):
        match = re.search(
            r"checkpoint_recovery_benchmark operations=(\d+) text_bytes=(\d+) repeats=(\d+) "
            r"checkpoint_median_ns=(\d+) full_replay_median_ns=(\d+) "
            r"checkpoint_to_full_ratio=([0-9.eE+-]+)", path.read_text())
        if not match:
            raise RuntimeError(f"invalid recovery benchmark output: {path}")
        keys = ("operations", "text_bytes", "repeats", "checkpoint_median_ns",
                "full_replay_median_ns", "checkpoint_to_full_ratio")
        parsed = dict(zip(keys, match.groups()))
        for key in keys[:-1]:
            parsed[key] = int(parsed[key])
        parsed[keys[-1]] = float(parsed[keys[-1]])
        if parsed["operations"] not in (512, 2048) or parsed["text_bytes"] != parsed["operations"] or parsed["repeats"] != 5:
            raise RuntimeError(f"recovery fixture dimensions changed: {parsed}")
        return parsed

    try:
        if args.baseline_sha != BASELINE:
            raise RuntimeError(f"baseline must remain pinned to {BASELINE}")
        baseline_build = configure_and_build("baseline", baseline_root)
        candidate_build = configure_and_build("candidate", candidate_root)
        baseline_bin = compile_baseline_harness(baseline_build)
        candidate_bin = candidate_build / "libs/collabtext/collabtext_performance_benchmark"
        datasets = {"baseline": {}, "candidate": {}}
        run_count = 1 if args.smoke else args.repeats
        for repeat in range(1, run_count + 1):
            order = ("baseline", "candidate") if repeat % 2 else ("candidate", "baseline")
            for label in order:
                build = baseline_build if label == "baseline" else candidate_build
                binary = baseline_bin if label == "baseline" else candidate_bin
                suffix = str(repeat)
                public_log = output / f"{label}-public-{suffix}.jsonl"
                run(f"{label}-public-{suffix}", [binary, *(["--quick"] if args.smoke else [])],
                    timeout=300, log_path=public_log)
                datasets[label].setdefault("public", []).append(read_public(public_log))
                if args.smoke:
                    continue
                native_log = output / f"{label}-native-{suffix}.log"
                run(f"{label}-native-{suffix}",
                    [build / "libs/collabtext/tst_benchmark", *METHODS], log_path=native_log)
                datasets[label].setdefault("native", []).append(read_native(native_log))
                for count in (512, 2048):
                    recovery_log = output / f"{label}-recovery-{count}-{suffix}.log"
                    run(f"{label}-recovery-{count}-{suffix}",
                        [build / "libs/collabtext/tst_checkpoint_recovery_benchmark", str(count)],
                        timeout=600, log_path=recovery_log)
                    datasets[label].setdefault(f"recovery-{count}", []).append(
                        read_recovery(recovery_log))

        for repeat in range(run_count):
            before, after = datasets["baseline"]["public"][repeat], datasets["candidate"]["public"][repeat]
            if set(before) != set(after):
                raise RuntimeError("baseline/candidate public case sets differ")
            for name in before:
                left, right = before[name]["case"], after[name]["case"]
                if left["samples"] != right["samples"] or left["attempted"] != right["attempted"]:
                    raise RuntimeError(f"paired public work counts differ: {name}")
                if (left["result_fnv1a64"], left["operation_fnv1a64"]) != (
                        right["result_fnv1a64"], right["operation_fnv1a64"]):
                    raise RuntimeError(f"public result or encoded operation differs: {name}")
            if not args.smoke:
                left, right = datasets["baseline"]["native"][repeat], datasets["candidate"]["native"][repeat]
                if set(left) != set(right):
                    raise RuntimeError("baseline/candidate native cases differ")
                for name in left:
                    for field in ("work_items", "source_edits", "remote_applications",
                                  "visible_bytes", "deleted_bytes"):
                        if left[name].get(field) != right[name].get(field):
                            raise RuntimeError(f"native state/work mismatch for {name}: {field}")

        aggregate = {
            "schema": "collabtext-performance-aggregate/1",
            "baseline_sha": args.baseline_sha,
            "candidate_sha": meta["candidate_sha"],
            "trace_sha256": meta["trace_sha256"],
            "profile": meta["profile"],
            "paired_runs": run_count,
            "correctness": "baseline/candidate state and operation fingerprints match",
            "cases": {},
        }
        public_names = list(datasets["baseline"]["public"][0])
        for name in public_names:
            entry = {}
            for label in ("baseline", "candidate"):
                run_stats = [statistic(datasets[label]["public"][i][name]["samples_ns"])
                             for i in range(run_count)]
                entry[label] = run_stats
            entry["candidate_over_baseline"] = {
                key: [entry["candidate"][i][key] / entry["baseline"][i][key]
                      for i in range(run_count)]
                for key, _ in (("mean_ns", 0), *QUANTILES)
            }
            aggregate["cases"][name] = entry

        if not args.smoke:
            for group in ("native", "recovery-512", "recovery-2048"):
                aggregate["cases"][group] = {}
                case_names = (datasets["baseline"][group][0].keys()
                              if group == "native" else ["checkpoint_open"])
                for name in case_names:
                    if group == "native":
                        base_values = [
                            datasets["baseline"]["native"][r][name]["ns_per_work_item"]
                            for r in range(run_count)]
                        cand_values = [
                            datasets["candidate"]["native"][r][name]["ns_per_work_item"]
                            for r in range(run_count)]
                        aggregate["cases"][group][name] = {
                            "baseline": base_values, "candidate": cand_values,
                            "candidate_over_baseline": [c / b for b, c in zip(base_values, cand_values)],
                        }
                    else:
                        for field in ("checkpoint_median_ns", "full_replay_median_ns",
                                      "checkpoint_to_full_ratio"):
                            base_values = [datasets["baseline"][group][r][field]
                                           for r in range(run_count)]
                            cand_values = [datasets["candidate"][group][r][field]
                                           for r in range(run_count)]
                            aggregate["cases"][group][field] = {
                                "baseline": base_values, "candidate": cand_values,
                                "candidate_over_baseline": [c / b for b, c in zip(base_values, cand_values)],
                            }
        (output / "aggregate.json").write_text(json.dumps(aggregate, indent=2) + "\n")
        meta["matrix_status"] = "smoke_pass" if args.smoke else "paired_measurements_complete"
        (output / "environment.json").write_text(json.dumps(meta, indent=2) + "\n")
        print(json.dumps({"status": meta["matrix_status"], "output": str(output),
                          "cases": len(public_names), "paired_runs": run_count}, indent=2))
        return 0
    except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
        meta["matrix_status"] = "failed"
        meta["failure"] = str(error)
        (output / "environment.json").write_text(json.dumps(meta, indent=2) + "\n")
        print(f"FAILED: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
