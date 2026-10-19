import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location(
    "performance_matrix", ROOT / "tools/run_performance_matrix.py")
matrix = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(matrix)
TRACE = json.loads((ROOT / "benchmarks/traces/workloads-v1.json").read_text())
RAW = ROOT / "docs/reports/2026-10-19-x01-baseline/raw"


class TraceValidationTests(unittest.TestCase):
    def test_checked_trace_is_accepted(self):
        dimensions = matrix.validate_trace(TRACE)
        self.assertEqual(dimensions["query_calls"], 50)

    def test_invalid_trace_dimensions_are_rejected(self):
        for mutate in (
            lambda trace: trace.update(schema="unknown"),
            lambda trace: trace.update(seed=43),
            lambda trace: trace["dimensions"].update(text_bytes=[1000]),
            lambda trace: trace["dimensions"].update(query_calls=49),
            lambda trace: trace["dimensions"].update(causal_actors=[1, 8]),
        ):
            with self.subTest(mutate=mutate):
                trace = copy.deepcopy(TRACE)
                mutate(trace)
                with self.assertRaises(RuntimeError):
                    matrix.validate_trace(trace)

    def test_archived_x01_matrix_is_complete_and_correct(self):
        environment = json.loads((RAW / "environment.json").read_text())
        aggregate = json.loads((RAW / "aggregate.json").read_text())
        self.assertEqual(environment["matrix_status"], "paired_measurements_complete")
        self.assertEqual(environment["baseline_sha"], matrix.BASELINE)
        self.assertEqual(environment["candidate_sha"], "1701d35fe9f88a4071d4fb9e71224cd5d8a429bd")
        self.assertEqual(environment["trace_sha256"], hashlib.sha256(
            (ROOT / "benchmarks/traces/workloads-v1.json").read_bytes()).hexdigest())
        self.assertEqual(len(environment["commands"]), 45)
        self.assertTrue(all(command["exit_status"] == 0 for command in environment["commands"]))
        self.assertEqual(aggregate["paired_runs"], 5)
        public_names = set(aggregate["cases"]) - {"native", "recovery-512", "recovery-2048"}
        self.assertEqual(len(public_names), 54)

        for run in range(1, 6):
            paired = {}
            for label in ("baseline", "candidate"):
                rows = [json.loads(line) for line in
                        (RAW / f"{label}-public-{run}.jsonl").read_text().splitlines()]
                headers = [row for row in rows if row.get("record") == "header"]
                cases = {row["workload"]: row for row in rows if row.get("record") == "case"}
                samples = {}
                for row in rows:
                    if row.get("record") == "sample":
                        samples.setdefault(row["workload"], []).append(row["latency_ns"])
                self.assertEqual(len(headers), 1)
                self.assertEqual(headers[0]["seed"], 42)
                self.assertEqual(set(cases), public_names)
                self.assertEqual(set(samples), public_names)
                for name, case in cases.items():
                    self.assertEqual(case["samples"], len(samples[name]))
                    self.assertEqual(case["attempted"], case["accepted"])
                    self.assertEqual(case["refused"], 0)
                    self.assertEqual(case["no_op"], 0)
                paired[label] = cases
                native = (RAW / f"{label}-native-{run}.log").read_text()
                self.assertIn("Totals: 14 passed, 0 failed, 0 skipped", native)
                for size in (512, 2048):
                    recovery = (RAW / f"{label}-recovery-{size}-{run}.log").read_text()
                    self.assertRegex(recovery, rf"operations={size} .*repeats=5")
            for name in public_names:
                self.assertEqual(
                    (paired["baseline"][name]["result_fnv1a64"],
                     paired["baseline"][name]["operation_fnv1a64"]),
                    (paired["candidate"][name]["result_fnv1a64"],
                     paired["candidate"][name]["operation_fnv1a64"]))


if __name__ == "__main__":
    unittest.main()
