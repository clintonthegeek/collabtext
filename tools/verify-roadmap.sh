#!/usr/bin/env bash
# Configure, build selected CTest targets, and retain a reproducible local log.
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "Usage: $0 {debug|release|asan} 'CTest regex'" >&2
    exit 2
fi
mode=$1
test_regex=$2
case "$mode" in
    debug) build_type=Debug; sanitizers=OFF ;;
    release) build_type=Release; sanitizers=OFF ;;
    asan) build_type=Debug; sanitizers=ON ;;
    *) echo "Unknown verification mode: $mode" >&2; exit 2 ;;
esac
jobs=${COLLABTEXT_VERIFY_JOBS:-2}
if [[ ! "$jobs" =~ ^[1-9][0-9]*$ ]]; then
    echo "COLLABTEXT_VERIFY_JOBS must be a positive integer" >&2
    exit 2
fi

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
build_dir="$repo_root/build-roadmap-$mode"
run_dir="$build_dir/verification/$(date -u +%Y%m%dT%H%M%SZ)-$$"
mkdir -p "$run_dir"
exec > >(tee "$run_dir/run.log") 2>&1
trap 'result=$?; printf "%s\n" "$result" > "$run_dir/exit-code.txt"; echo "Verification exit: $result; evidence: $run_dir"' EXIT

printf 'Root: %s\nMode: %s\nRegex: %s\nWorkers: %s\n' "$repo_root" "$mode" "$test_regex" "$jobs"
cmake -S "$repo_root" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE="$build_type" \
    -DCOLLABTEXT_ENABLE_SANITIZERS="$sanitizers" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

# Ask CTest to apply its own regex dialect; do not approximate it with Python.
ctest --test-dir "$build_dir" -R "$test_regex" --show-only=json-v1 > "$run_dir/selected-tests.json"
python3 - "$repo_root" "$build_dir" "$run_dir" "$mode" "$test_regex" "$jobs" <<'PY'
import json
from pathlib import Path
import subprocess
import sys

root, build, run, mode, regex, jobs = sys.argv[1:]
run = Path(run)
tests = json.loads((run / 'selected-tests.json').read_text())['tests']
if not tests:
    print('ERROR: CTest selection matched zero tests', file=sys.stderr)
    sys.exit(3)
names = [t['name'] for t in tests]
(run / 'targets.txt').write_text('\n'.join(names) + '\n')
cache = {}
for line in (Path(build) / 'CMakeCache.txt').read_text().splitlines():
    if line and not line.startswith(('#', '//')) and '=' in line and ':' in line.split('=', 1)[0]:
        key, value = line.split('=', 1)
        cache[key.split(':', 1)[0]] = value
revision = subprocess.run(['git', '-C', root, 'rev-parse', 'HEAD'], capture_output=True, text=True)
compiler = cache['CMAKE_CXX_COMPILER']
compiler_version = subprocess.check_output([compiler, '--version'], text=True)
compile_commands = Path(build) / 'compile_commands.json'
examples = json.loads(compile_commands.read_text())
example = next((entry for entry in examples if entry['file'].endswith('/crdt/Buffer.cpp')), examples[0])
if mode == 'asan' and '-fsanitize=address,undefined' not in example.get('command', ' '.join(example.get('arguments', []))):
    print('ERROR: selected build does not instrument the core', file=sys.stderr)
    sys.exit(4)
metadata = dict(root=root, revision=revision.stdout.strip() if revision.returncode == 0 else 'unversioned',
                mode=mode, regex=regex, jobs=int(jobs), test_count=len(names), tests=names,
                compiler=compiler, compiler_version=compiler_version,
                representative_compile_command=example, compile_commands=str(compile_commands),
                configuration={k: v for k, v in cache.items()
                               if k.startswith('CMAKE_CXX_FLAGS') or k in
                               {'CMAKE_BUILD_TYPE', 'COLLABTEXT_ENABLE_SANITIZERS'}},
                commands=[['cmake', '-S', root, '-B', build,
                           f'-DCMAKE_BUILD_TYPE={cache["CMAKE_BUILD_TYPE"]}',
                           f'-DCOLLABTEXT_ENABLE_SANITIZERS={cache.get("COLLABTEXT_ENABLE_SANITIZERS", "OFF")}',
                           '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'],
                          ['cmake', '--build', build, '--parallel', jobs, '--target', *names],
                          ['ctest', '--test-dir', build, '-R', regex, '--parallel', jobs,
                           '--output-on-failure', '--no-tests=error']],
                test_environment={'QT_QPA_PLATFORM': 'offscreen'})
(run / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
print(f'Selected {len(names)} tests; compiler: {compiler_version.splitlines()[0]}')
PY

# Repository convention: each CTest test and its build target share a name.
mapfile -t targets < "$run_dir/targets.txt"
cmake --build "$build_dir" --parallel "$jobs" --target "${targets[@]}"
QT_QPA_PLATFORM=offscreen ctest --test-dir "$build_dir" -R "$test_regex" \
    --parallel "$jobs" --output-on-failure --no-tests=error
