import copy
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


if __name__ == "__main__":
    unittest.main()
