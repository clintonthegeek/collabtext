"""Regression tests for verification coverage and truthful failure exits."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
import contextlib
import io
import json
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('verify_changed', Path(__file__).with_name('verify_changed.py'))
verify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify)


class VerificationTests(unittest.TestCase):
    def test_performance_benchmark_has_correctness_routes_without_ctest_registration(self):
        routes, package = verify.route(['benchmarks/BenchmarkRunner.cpp'])
        selected = next(iter(routes.values()))
        for test in ('tst_boundary_model', 'tst_positions',
                     'tst_sequence_differential', 'tst_idlist_undo',
                     'tst_operation_validation'):
            self.assertRegex(test, '^' + selected + '$')
        self.assertFalse(package)
        self.assertEqual(verify.route(['benchmarks/traces/workloads-v1.json'])[0]
                         ['benchmarks/traces/workloads-v1.json'],
                         'tst_verification_tools')
        self.assertEqual(verify.route(['tools/run_performance_matrix.py'])[0]
                         ['tools/run_performance_matrix.py'],
                         'tst_verification_tools')
        self.assertEqual(verify.route([
            'docs/reports/2026-10-19-x01-baseline/raw/aggregate.json'])[0][
                'docs/reports/2026-10-19-x01-baseline/raw/aggregate.json'],
            'tst_verification_tools')
        self.assertEqual(verify.route([
            'docs/reports/2026-10-19-x02-profile/raw/diagnostics-overhead.json'])[0][
                'docs/reports/2026-10-19-x02-profile/raw/diagnostics-overhead.json'],
            'tst_verification_tools')
        self.assertEqual(verify.route([
            'docs/reports/2026-10-20-x05-initial-paired/raw/aggregate.json'])[0][
                'docs/reports/2026-10-20-x05-initial-paired/raw/aggregate.json'],
            'tst_verification_tools')
        self.assertEqual(verify.route([
            'docs/reports/2026-10-20-x05-exact-run-paired/raw/aggregate.json'])[0][
                'docs/reports/2026-10-20-x05-exact-run-paired/raw/aggregate.json'],
            'tst_verification_tools')

    def test_frozen_performance_archive_does_not_allow_unknown_code(self):
        paths = ['docs/reports/2026-10-19-performance/' + name
                 for name in verify.PERFORMANCE_EVIDENCE]
        routes, package = verify.route(paths)
        self.assertEqual(set(routes.values()), {'tst_verification_tools'})
        self.assertFalse(package)
        for unknown in ('docs/reports/2026-10-19-performance/new.cpp',
                        'docs/reports/2026-10-20-performance/public_api.cpp',
                        'benchmarks/new-component.cpp'):
            with self.subTest(unknown=unknown), self.assertRaisesRegex(ValueError, 'Unrouted'):
                verify.route([paths[0], unknown])

    def test_mixed_documentation_and_unknown_code_fails(self):
        with self.assertRaisesRegex(ValueError, 'Unrouted'):
            verify.route(['docs/note.md', 'new-component/editor.cpp'])

    def test_buffer_routes_anchors_and_regressions(self):
        routes, package = verify.route(['libs/collabtext/src/crdt/Buffer.cpp'])
        regex = next(iter(routes.values()))
        import re
        for test in ('tst_anchor', 'tst_fuzz', 'tst_evaluation_regressions', 'tst_buffer'):
            self.assertRegex(test, '^' + regex + '$')
        self.assertFalse(package)

    def test_fragment_summary_routes_tree_and_summary_tests(self):
        routes, package = verify.route(['libs/collabtext/src/crdt/Fragment.h'])
        regex = next(iter(routes.values()))
        import re
        for test in ('tst_buffer', 'tst_summary_laws', 'tst_sumtree', 'tst_positions'):
            self.assertRegex(test, '^' + regex + '$')
        self.assertFalse(package)

    def test_crdt_index_headers_route_index_and_buffer_regressions(self):
        cases = {
            'libs/collabtext/src/crdt/PersistentAvlMap.h': ('tst_crdt_indexes',),
            'libs/collabtext/src/crdt/OriginIndex.h':
                ('tst_crdt_indexes', 'tst_buffer', 'tst_sequence_semantics', 'tst_gc'),
            'libs/collabtext/src/crdt/DeletionIndex.h':
                ('tst_crdt_indexes', 'tst_buffer', 'tst_sequence_semantics', 'tst_gc'),
            'libs/collabtext/src/crdt/RunDirectory.h':
                ('tst_crdt_indexes', 'tst_buffer', 'tst_sequence_semantics', 'tst_gc'),
        }
        for path, expected in cases.items():
            routes, package = verify.route([path])
            regex = next(iter(routes.values()))
            import re
            for test in expected:
                self.assertRegex(test, '^' + regex + '$')
            self.assertFalse(package)

    def test_sumtree_route_covers_all_engine_tree_users(self):
        routes, package = verify.route(['libs/collabtext/src/crdt/SumTree.h'])
        regex = next(iter(routes.values()))
        import re
        for test in ('tst_sumtree', 'tst_buffer', 'tst_idlist', 'tst_undomap',
                     'tst_persistent_undo', 'tst_opqueue', 'tst_rope_integration',
                     'tst_sequence_semantics'):
            self.assertRegex(test, '^' + regex + '$')
        self.assertFalse(package)

    def test_idlist_routes_whole_family(self):
        routes, _ = verify.route(['libs/collabtext/src/crdt/IdList.cpp'])
        for test in ('tst_idlist', 'tst_idlist_undo', 'tst_idlist_anchor',
                     'tst_idlist_serialization', 'tst_idlist_op_api'):
            self.assertRegex(test, '^' + next(iter(routes.values())) + '$')

    def test_clock_and_codec_are_covered(self):
        for path in ('libs/collabtext/src/crdt/Clock.cpp',
                     'libs/collabtext/src/codec/Utf.cpp',
                     'libs/collabtext/src/CrdtEngine.cpp'):
            self.assertTrue(verify.route([path])[0][path])

    def test_changed_test_routes_itself(self):
        routes, _ = verify.route(['libs/collabtext/tests/tst_identity_allocation_model.cpp'])
        self.assertEqual(next(iter(routes.values())), 'tst_identity_allocation_model')

    def test_benchmark_source_is_explicitly_opt_in_and_excluded_from_ctest(self):
        routes, package = verify.route(['libs/collabtext/tests/tst_benchmark.cpp'])
        self.assertEqual(routes, {'libs/collabtext/tests/tst_benchmark.cpp': ''})
        self.assertFalse(package)
        routes, _ = verify.route(['libs/collabtext/tests/tst_perf_diagnostics.cpp'])
        self.assertEqual(next(iter(routes.values())), 'tst_(buffer|positions|perf_diagnostics)')

    def test_boundary_model_change_is_narrow_and_fixture_changes_are_executable(self):
        # BoundaryModel.h is consumed by two independent model tests since S02:
        # the boundary model itself, and the sequence model that feeds it a total
        # order. The route must stay narrow -- both models, never the engine
        # suites -- and must not demand a package check.
        routes, package = verify.route(['libs/collabtext/tests/reference/BoundaryModel.h',
                                       'libs/collabtext/tests/reference/BoundaryApiContract.h'])
        self.assertEqual(set(routes.values()), {'tst_(boundary|sequence)_model'})
        self.assertFalse(package)
        routes, _ = verify.route(['libs/collabtext/tests/reference/SequenceModel.h'])
        self.assertEqual(set(routes.values()), {'tst_sequence_model'})
        # The S01 fixtures are the oracle of both the S02 interpreter and the S03
        # FugueMax reference, so a fixture edit must run both.
        routes, _ = verify.route(['libs/collabtext/tests/fixtures/sequence-semantics/forward-typing.json'])
        self.assertEqual(set(routes.values()), {'tst_(sequence|fuguemax)_model'})
        routes, _ = verify.route(['libs/collabtext/tests/reference/FugueMaxModel.h'])
        self.assertEqual(set(routes.values()), {'tst_fuguemax_model'})
        routes, package = verify.route(['libs/collabtext/tests/fixtures/external-position/wp2-strict-referent-delete.json',
                                       'libs/collabtext/tests/fixtures/external-position/model/retained-orders.json'])
        for regex in routes.values():
            for test in ('tst_boundary_model','tst_anchor','tst_anchor_codec','tst_idlist_anchor'):
                self.assertRegex(test,'^'+regex+'$')
        self.assertFalse(package)

    def test_public_headers_require_package_and_behavior(self):
        routes, package = verify.route(['libs/collabtext/include/collabtext/Engine.h'])
        self.assertTrue(package)
        self.assertRegex('tst_identity_types', '^' + next(iter(routes.values())) + '$')

    def test_alpha_preparation_and_probes_are_packaging_changes(self):
        # E07a added tools/prepare-integration-alpha.sh and W02's probe runner
        # lives under tests/consumer-smoke. Both build, install and consume a
        # tree, so both must demand the package gate. An unrouted path fails the
        # diff, which is right, but it means a route deleted here turns a
        # packaging change into a hard failure rather than a silent docs-only
        # pass -- so pin the route instead of trusting it.
        for path in ('tools/prepare-integration-alpha.sh',
                     'tests/consumer-smoke/wave-contract/run.py'):
            routes, package = verify.route([path])
            self.assertTrue(package, path)
            self.assertTrue(routes[path], path)
            self.assertRegex('collabtext_core_smoke', '^' + routes[path] + '$')

    def test_documentation_has_no_engine_build(self):
        routes, package = verify.route(['docs/report.md', 'AGENTS.md'])
        self.assertFalse(any(routes.values()))
        self.assertFalse(package)

    def test_invalid_git_baseline_is_not_empty_success(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            subprocess.run(['git', 'init', '-q', root], check=True)
            with self.assertRaises(subprocess.CalledProcessError):
                verify.changed_paths(root, 'nonexistent-revision')

    def test_failure_of_a_build_or_test_is_nonzero(self):
        with tempfile.TemporaryDirectory() as temporary, \
             patch.object(verify, 'ROOT', Path(temporary)), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()), \
             patch.object(verify, 'changed_paths', return_value=['libs/collabtext/src/crdt/Buffer.cpp']), \
             patch.object(verify, 'configuration_needed', return_value=True), \
             patch.object(verify, 'configure', side_effect=subprocess.CalledProcessError(2, ['cmake'])):
            self.assertNotEqual(verify.main([]), 0)
            result = next(Path(temporary).glob('build-roadmap-debug/verification/*/results.json'))
            self.assertNotEqual(json.loads(result.read_text())['exit_status'], 0)

    def test_zero_ctest_selection_fails(self):
        with tempfile.TemporaryDirectory() as temporary, \
             patch.object(verify, 'ROOT', Path(temporary)), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()), \
             patch.object(verify, 'configuration_needed', return_value=False), \
             patch.object(verify, 'command', return_value=subprocess.CompletedProcess([], 0, '{"tests":[]}')):
            self.assertNotEqual(verify.main(['--target', 'does_not_exist']), 0)

    def test_package_missing_option_terminates(self):
        for option in ('--mode', '--gate', '--producer-build'):
            worker = subprocess.run(['bash', str(Path(__file__).with_name('verify-consumer-package.sh')), option],
                                    capture_output=True, timeout=5)
            self.assertNotEqual(worker.returncode, 0)

    def test_missing_option_value_terminates(self):
        for option in ('--since', '--mode', '--target'):
            worker = subprocess.run(['bash', str(Path(__file__).with_name('verify-changed.sh')), option],
                                    capture_output=True, timeout=5)
            self.assertNotEqual(worker.returncode, 0)


if __name__ == '__main__':
    unittest.main()
