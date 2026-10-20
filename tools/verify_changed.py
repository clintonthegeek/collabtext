"""Fail-closed changed-path verification, with one aggregate build traversal."""
import argparse
import fnmatch
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import tempfile

ROOT = Path(__file__).resolve().parent.parent
PACKAGING = r'(collabtext_.*|tst_.*)'
BROAD = r'(collabtext_.*|tst_.*)'
# Frozen measurement evidence, not shipped source or correctness executables.
# Enumerate this archive explicitly: new benchmark code still needs a route.
PERFORMANCE_EVIDENCE = [
    'aggregate.py', 'bulk-limit.log', 'bulk_limit.cpp', 'configure.log',
    'core-flags.txt', 'environment.json', 'progress.jsonl', 'public-1.err',
    'public-1.jsonl', 'public_api.cpp', 'recovery-512.log', 'recovery-2048.log',
    'repeat.py', 'run-recorded.py', 'summary.json',
    *[f'native-{n}.log' for n in range(1, 6)],
    *[f'public-{n}.log' for n in range(2, 6)],
]
# First match wins. Regexes describe complete families, not accidental prefixes.
ROUTES = [
    ('benchmarks/BenchmarkRunner.cpp', r'tst_(boundary_model|positions|position_token_reader|sequence_differential|idlist.*|operation_validation)'),
    ('benchmarks/traces/*.json', 'tst_verification_tools'),
    ('tools/run_performance_matrix.py', 'tst_verification_tools'),
    ('tools/test_run_performance_matrix.py', 'tst_verification_tools'),
    ('docs/reports/2026-10-19-x01-baseline/raw/*', 'tst_verification_tools'),
    ('docs/reports/2026-10-19-x02-profile/raw/*', 'tst_verification_tools'),
    ('docs/reports/2026-10-19-x07a/raw/*', 'tst_verification_tools'),
    ('tests/performance-model/incremental-model.*', 'tst_incremental_model'),
    ('libs/collabtext/tests/tst_incremental_model.cpp', 'tst_incremental_model'),
    ('libs/collabtext/tests/tst_unicode_seek.cpp', 'tst_unicode_seek'),
    *[(f'docs/reports/2026-10-19-performance/{name}', 'tst_verification_tools')
      for name in PERFORMANCE_EVIDENCE],
    ('.gitignore', ''), ('*.md', ''), ('docs/*.json', ''),
    ('docs/roadmap/*/*.py', 'tst_verification_tools'),
    ('tools/verify-consumer-package.sh', PACKAGING),
    # E07a/E07b candidate preparation configures, builds, installs, consumes and
    # checksums a staged tree, so a change here is a packaging change. Left
    # unrouted it fails the whole diff, which is the correct default.
    ('tools/prepare-integration-alpha.sh', PACKAGING),
    ('tools/verify*', 'tst_verification_tools'),
    ('tools/verification-env.sh', 'tst_verification_tools'),
    ('tools/test_verify_changed.py', 'tst_verification_tools'),
    ('tools/*sequence_fixtures.py', 'tst_sequence_fixtures'),
    # W01 specifies consumer-probe cases; its fixtures and validator are data and
    # Python, checked by the validator's own --self-test rather than by CTest.
    ('tools/validate-consumer-fixtures.py', ''),
    # The branch workflow invokes the installed producer/consumer gate, so a change
    # to it is a packaging change. Left unrouted it fails the whole diff, which is
    # the correct default: an unknown path must never turn into a docs-only pass.
    ('.github/workflows/*', PACKAGING),
    # The dogfood editor integration has a real offscreen open/edit/reopen test;
    # keep the existing widget and viewport regressions beside it.
    ('app/collabedit/*', r'tst_(editor_recovery|scroll_stability|collab_plain_text_edit)'),
    ('libs/collabtext/CMakeLists.txt', PACKAGING), ('CMakeLists.txt', PACKAGING),
    ('cmake/*', PACKAGING), ('tests/consumer-smoke/*', r'(tst_markoff_core|tst_wave_core|'+PACKAGING+')'),
    ('tests/wave-core/*', 'tst_marker_model'),
    ('tests/markoff-core/*', 'tst_counter_gap_model'),
    ('libs/collabtext/include/*', PACKAGING),
    ('libs/collabtext/src/crdt/IdList*', r'tst_(idlist.*|anchor.*|sequence_.*|boundary_.*|operation_validation|evaluation_regressions)'),
    ('libs/collabtext/src/IdListEngine.cpp', r'(tst_idlist.*|collabtext_alpha_consumer)'),
    ('libs/collabtext/src/crdt/Fragment.h', r'tst_(buffer|summary_laws|sumtree|unicode_seek|positions|convergence|sequence_.*|crdt_indexes)'),
    ('libs/collabtext/src/crdt/Buffer*', r'tst_(buffer|anchor.*|sequence_.*|boundary_.*|edits_since|gc|undomap|convergence|serialization.*|decode_validation|operation_validation|fuzz|evaluation_regressions|behavior_fixtures|unicode_seek|crdt_indexes)'),
    ('libs/collabtext/src/identity/*', r'tst_(actor_identity|identity.*|presence_manager|sha256|clock|operation_validation)'),
    ('libs/collabtext/src/codec/*', r'tst_(utf8|decode_validation|serialization.*|operation_validation|anchor.*)'),
    ('libs/collabtext/src/storage/*', r'tst_(durable_file|store_fault_harness|actor_identity|segment.*|opstream.*)'),
    ('libs/collabtext/src/core/*', r'tst_(sha256|segment.*|serialization.*)'),
    ('libs/collabtext/src/transport/*', r'tst_(opstream.*|stream_sync|filesync|segment.*|durable_file)'),
    ('libs/collabtext/src/ui/*', r'tst_(comment|chat_message|multicursor|collab_plain_text_edit|presence_manager|identity_widgets|scroll_stability)'),
    # Shared clocks, queues, trees and operation types affect both engines.
    ('libs/collabtext/src/crdt/*', BROAD),
    ('libs/collabtext/src/*.cpp', BROAD),
    ('libs/collabtext/tests/reference/SequenceModel.h', 'tst_sequence_model'),
    ('libs/collabtext/tests/reference/FugueMaxModel.h', 'tst_fuguemax_model'),
    ('libs/collabtext/tests/reference/Boundary*.h', r'tst_(boundary|sequence)_model'),
    # This test target is only generated in the opt-in diagnostics build; normal
    # changed-path verification runs the always-available Buffer/position suites.
    ('libs/collabtext/tests/tst_perf_diagnostics.cpp', r'tst_(buffer|positions|perf_diagnostics)'),
    ('libs/collabtext/tests/fixtures/sequence-semantics/*', r'tst_(sequence|fuguemax)_model'),
    ('libs/collabtext/tests/tst_sequence_model.cpp', 'tst_sequence_model'),
    ('libs/collabtext/tests/tst_*.cpp', 'OWN_TEST'),
    ('libs/collabtext/tests/fixtures/sequence-semantics/*', r'tst_(sequence_.*|boundary_.*)'),
    ('libs/collabtext/tests/fixtures/external-position/*', r'tst_(boundary_model|anchor.*|idlist_anchor)'),
    ('libs/collabtext/tests/*', BROAD),
]
PACKAGE_PATHS = ('CMakeLists.txt', 'libs/collabtext/CMakeLists.txt', 'cmake/*',
                 'tests/consumer-smoke/*', 'libs/collabtext/include/*',
                 '.github/workflows/*', 'tools/verify-consumer-package.sh',
                 'tools/prepare-integration-alpha.sh')


def route(paths):
    """Return per-file regexes and packaging requirement, refusing unknown code."""
    routed = {}
    unknown = []
    package = False
    for path in paths:
        package |= any(fnmatch.fnmatchcase(path, p) for p in PACKAGE_PATHS)
        for pattern, regex in ROUTES:
            if fnmatch.fnmatchcase(path, pattern):
                if regex == 'OWN_TEST':
                    regex = Path(path).stem
                routed[path] = regex
                break
        else:
            unknown.append(path)
    if unknown:
        raise ValueError('Unrouted changed files (add an explicit route or use --all):\n  '
                         + '\n  '.join(unknown))
    return routed, package


def changed_paths(root, baseline=None):
    args = ['git', '-C', str(root), 'diff', '--name-only', '-z', baseline or 'HEAD', '--']
    result = subprocess.run(args, check=True, capture_output=True)
    paths = result.stdout.split(b'\0')
    if baseline is None:
        result = subprocess.run(['git', '-C', str(root), 'ls-files', '-z', '--others',
                                 '--exclude-standard'], check=True, capture_output=True)
        paths += result.stdout.split(b'\0')
    return sorted({os.fsdecode(p) for p in paths if p})


def command(args, **kwargs):
    print('+', ' '.join(map(str, args)), flush=True)
    return subprocess.run(list(map(str, args)), check=True, **kwargs)


def configure(build, mode, targets=()):
    args = ['cmake', '-S', ROOT, '-B', build,
            '-DCMAKE_BUILD_TYPE=' + ('Release' if mode == 'release' else 'Debug'),
            '-DCOLLABTEXT_ENABLE_SANITIZERS=' + ('ON' if mode == 'asan' else 'OFF'),
            '-DCOLLABTEXT_BUILD_BENCHMARKS=OFF',
            '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
            '-DCOLLABTEXT_VERIFY_TARGETS=' + ';'.join(targets)]
    cache = (build / 'CMakeCache.txt').read_text() if (build / 'CMakeCache.txt').exists() else ''
    launcher = next((line.split('=', 1)[1] for line in cache.splitlines()
                     if line.startswith('CMAKE_CXX_COMPILER_LAUNCHER:')), '')
    if os.getenv('COLLABTEXT_USE_CCACHE', '1') == '1' and shutil.which('ccache'):
        if not launcher or 'ccache' in launcher:
            args.append('-DCMAKE_CXX_COMPILER_LAUNCHER=' + shutil.which('ccache'))
    command(args)


def cache_values(build):
    path = build / 'CMakeCache.txt'
    if not path.exists():
        return {}
    return {line.split(':', 1)[0]: line.split('=', 1)[1]
            for line in path.read_text().splitlines()
            if not line.startswith(('#', '//')) and ':' in line and '=' in line}


def configuration_needed(build, mode):
    values = cache_values(build)
    expected = {'CMAKE_BUILD_TYPE': 'Release' if mode == 'release' else 'Debug',
                'COLLABTEXT_ENABLE_SANITIZERS': 'ON' if mode == 'asan' else 'OFF',
                'COLLABTEXT_BUILD_BENCHMARKS': 'OFF'}
    if any(values.get(key) != value for key, value in expected.items()):
        return True
    if os.getenv('COLLABTEXT_USE_CCACHE', '1') == '1' and shutil.which('ccache'):
        if not values.get('CMAKE_CXX_COMPILER_LAUNCHER'):
            return True
    stamp = (build / 'CMakeCache.txt').stat().st_mtime_ns
    inputs = [ROOT / 'CMakeLists.txt', *ROOT.glob('libs/**/CMakeLists.txt'),
              *ROOT.glob('app/**/CMakeLists.txt'), *ROOT.glob('cmake/*')]
    return any(p.is_file() and p.stat().st_mtime_ns > stamp for p in inputs)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=('debug', 'release', 'asan'), default='debug')
    parser.add_argument('--since')
    group = parser.add_mutually_exclusive_group()
    group.add_argument('--all', action='store_true', help='All correctness tests, including long simulation')
    group.add_argument('--target', help='Explicit CTest regex; benchmarks are not CTest tests')
    parser.add_argument('--thorough', action='store_true', help='Include the long realistic simulation')
    parser.add_argument('--package-gate', action='store_true')
    parser.add_argument('--explain', action='store_true')
    args = parser.parse_args(argv)
    start = time.monotonic()
    result = {'schema': 'collabtext-changed-verification/1', 'mode': args.mode}
    run = None
    try:
        jobs = int(os.getenv('COLLABTEXT_VERIFY_JOBS', str(os.cpu_count() or 1)))
        if jobs < 1:
            raise ValueError('COLLABTEXT_VERIFY_JOBS must be positive')
        paths = [] if args.all or args.target else changed_paths(ROOT, args.since)
        if not paths and not args.all and not args.target:
            raise ValueError('No changes to route; use --all or --target')
        routes, package = ({}, False) if args.all or args.target else route(paths)
        regexes = sorted({r for r in routes.values() if r})
        regex = '.' if args.all else args.target if args.target else '^(' + '|'.join(regexes) + ')$'
        result.update(paths=paths, routes=routes, regex=regex, jobs=jobs)
        for path, selected in routes.items():
            print(f'{path} -> {selected or "documentation (no build)"}')
        if args.explain:
            print('Regex:', regex)
            print('Package gate:', package or args.package_gate)
            return 0
        if not regexes and not args.all and not args.target:
            print('Documentation only; no engine build needed.')
            return 0
        build = ROOT / ('build-roadmap-' + args.mode)
        evidence = build / 'verification'
        evidence.mkdir(parents=True, exist_ok=True)
        run = Path(tempfile.mkdtemp(prefix=time.strftime('%Y%m%dT%H%M%SZ', time.gmtime()) + '-changed-', dir=evidence))
        if configuration_needed(build, args.mode):
            configure(build, args.mode)
        selection = command(['ctest', '--test-dir', build, '--show-only=json-v1', '-R', regex],
                            capture_output=True, text=True)
        tests = json.loads(selection.stdout)['tests']
        tests = [t for t in tests if 'benchmark' not in t['name'].lower()]
        if not (args.all or args.thorough or args.target):
            tests = [t for t in tests if t['name'] != 'tst_realistic']
        names = [t['name'] for t in tests]
        if not names:
            raise ValueError('CTest selection matched zero runnable correctness tests')
        # Every non-documentation file must select at least one actual test.
        # Catch stale exact test names instead of trusting a nonempty union.
        for path, expression in routes.items():
            if expression:
                import re
                if not any(re.fullmatch(expression, name) for name in names):
                    raise ValueError(f'Route selects no runnable tests: {path} -> {expression}')
        targets = names.copy()
        if package or args.all:
            targets.append('collabtext_public_headers_core')
            if 'COLLABTEXT_BUILD_QT:BOOL=ON' in (build / 'CMakeCache.txt').read_text():
                targets.append('collabtext_public_headers_qt')
        print(f'Selected {len(names)} correctness tests; benchmarks excluded.', flush=True)
        if cache_values(build).get('COLLABTEXT_VERIFY_TARGETS') != ';'.join(targets):
            configure(build, args.mode, targets)
        if args.mode == 'asan':
            entries = json.loads((build / 'compile_commands.json').read_text())
            core = next(e for e in entries if e['file'].endswith('/crdt/Buffer.cpp'))
            flags = core.get('command', ' '.join(core.get('arguments', [])))
            if '-fsanitize=address,undefined' not in flags:
                raise ValueError('ASan mode does not instrument the core')
        result.update(tests=names, test_count=len(names), targets=targets)
        command(['cmake', '--build', build, '--parallel', jobs, '--target', 'collabtext_verify_selected'])
        exact = '^(' + '|'.join(names) + ')$'
        environment = dict(os.environ, QT_QPA_PLATFORM='offscreen')
        command(['ctest', '--test-dir', build, '-R', exact, '--parallel', jobs,
                 '--output-on-failure', '--no-tests=error'], env=environment)
        if package or args.package_gate:
            command([ROOT / 'tools/verify-consumer-package.sh', '--mode', args.mode,
                     '--gate', 'package', '--components', 'core', '--producer-build', build])
        result['exit_status'] = 0
        print('PASS: selected correctness tests passed.')
        return 0
    except (ValueError, subprocess.CalledProcessError, OSError) as error:
        result.update(exit_status=3, error=str(error))
        print('FAILED:', error, file=sys.stderr)
        return 3
    finally:
        result['wall_seconds'] = round(time.monotonic() - start, 3)
        if run:
            (run / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
            print(f'Wall time: {result["wall_seconds"]}s; evidence: {run}')


if __name__ == '__main__':
    sys.exit(main())
