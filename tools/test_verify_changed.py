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

    def test_boundary_model_change_is_narrow_and_fixture_changes_are_executable(self):
        routes, package = verify.route(['libs/collabtext/tests/reference/BoundaryModel.h',
                                       'libs/collabtext/tests/reference/BoundaryApiContract.h'])
        self.assertEqual(set(routes.values()), {'tst_boundary_model'})
        self.assertFalse(package)
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
