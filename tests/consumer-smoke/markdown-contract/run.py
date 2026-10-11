#!/usr/bin/env python3
"""Exact Markdown-fixture gate. No allowlist: a case passes or the gate fails.

A worker exit is evidence, not a verdict. Expectations here are hand-written
independently of worker output, so the worker cannot classify its own result.
"""
import argparse
import hashlib
import json
from pathlib import Path
import resource
import subprocess
import sys


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--worker', required=True)
    parser.add_argument('--cases', required=True)
    parser.add_argument('--revision', required=True)
    parser.add_argument('--mode', required=True, choices=['debug', 'release', 'asan'])
    parser.add_argument('--output', required=True)
    parser.add_argument('--inject-bad-expectation', action='store_true')
    parser.add_argument('--inject-abnormal-worker', action='store_true')
    args = parser.parse_args()
    output = Path(args.output); output.mkdir(parents=True, exist_ok=True)
    specification = json.loads(Path(args.cases).read_text())
    rows = specification['cases']
    if specification['schema'] != 'collabtext-markdown-probe-cases/1' or not rows:
        raise ValueError('missing/versioned cases')
    if len({row['id'] for row in rows}) != len(rows):
        raise ValueError('duplicate case IDs')
    declared = sorted({f for row in rows for f in row.get('formats', [])})
    def family(prefix):
        value = next((f for f in declared if f.startswith(prefix)), None)
        if value is None:
            raise ValueError(f'missing {prefix} format declaration')
        return value
    results = []
    for index, row in enumerate(rows):
        expected = row['expected']
        if args.inject_bad_expectation and index == 0:
            expected = {'deliberately_wrong': True}
        worker_case = 'abnormal' if args.inject_abnormal_worker and index == 0 else row['worker']
        command = [args.worker, worker_case]
        result = dict(case_id=row['id'], capability=row['capability'], requirement=row['requirement'],
                      formats=row['formats'], seed=specification['seed'], case_spec_sha256=digest(args.cases),
                      expected=expected, observed=None, outcome='error', producer_revision=args.revision,
                      command=command, exit_status=None, signal=None)
        try:
            child = subprocess.run(command, capture_output=True, text=True, timeout=20, preexec_fn=no_core)
            result['exit_status'] = child.returncode if child.returncode >= 0 else None
            result['signal'] = -child.returncode if child.returncode < 0 else None
            (output / (row['id'] + '.stdout.log')).write_text(child.stdout)
            (output / (row['id'] + '.stderr.log')).write_text(child.stderr)
            result['stdout_log'] = row['id'] + '.stdout.log'
            result['stderr_log'] = row['id'] + '.stderr.log'
            if child.returncode != 0:
                result['diagnostic'] = 'worker failed or signalled'
            else:
                data = json.loads(child.stdout)
                if (set(data) != {'schema', 'case', 'observed'}
                        or data['schema'] != 'collabtext-markdown-worker/1'
                        or data['case'] != worker_case):
                    raise ValueError('invalid worker result/schema/case')
                observed = data['observed']; result['observed'] = observed
                if observed == expected:
                    result['outcome'] = 'pass'
                else:
                    result['diagnostic'] = 'unexpected assertion mismatch'
        except (OSError, ValueError, subprocess.TimeoutExpired) as error:
            result['diagnostic'] = str(error)
        result['required_strict'] = True
        result['accepted'] = result['outcome'] == 'pass'
        results.append(result)
        print(f"{row['id']}: {result['outcome']}")
    passed = all(row['accepted'] for row in results)
    report = dict(schema='collabtext-markdown-probe-results/1', producer_revision=args.revision,
                  mode=args.mode, seed=specification['seed'], case_spec_sha256=digest(args.cases),
                  formats={'text': family('CTBO'), 'position': family('CTPT')},
                  cases=results, case_count=len(results),
                  strict_pass_count=sum(row['outcome'] == 'pass' for row in results),
                  exit_status=0 if passed else 1)
    (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    return report['exit_status']


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (ValueError, KeyError, OSError, StopIteration) as error:
        print(f'markdown probe runner: {error}', file=sys.stderr)
        sys.exit(2)