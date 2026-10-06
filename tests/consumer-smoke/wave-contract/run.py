#!/usr/bin/env python3
"""Exact installed-consumer gate. A worker exit is evidence, not a verdict."""
import argparse
import hashlib
import json
from pathlib import Path
import resource
import subprocess
import sys

LIVE_PROBES = {'WP1', 'WP2', 'WP3', 'WP4', 'WP5', 'WP7'}

def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--worker', required=True)
    parser.add_argument('--cases', required=True)
    parser.add_argument('--fixtures', required=True)
    parser.add_argument('--manifest', required=True)
    parser.add_argument('--revision', required=True)
    parser.add_argument('--mode', required=True, choices=['debug', 'release', 'asan'])
    parser.add_argument('--gate', required=True, choices=['baseline', 'live'])
    parser.add_argument('--output', required=True)
    parser.add_argument('--inject-bad-expectation', action='store_true')
    parser.add_argument('--inject-abnormal-worker', action='store_true')
    args = parser.parse_args()
    output = Path(args.output); output.mkdir(parents=True, exist_ok=True)
    specification = json.loads(Path(args.cases).read_text())
    rows = specification['cases']
    if specification['schema'] != 'collabtext-installed-probe-cases/1' or not rows:
        raise ValueError('missing/versioned cases')
    if len({row['id'] for row in rows}) != len(rows):
        raise ValueError('duplicate case IDs')
    if {row['probe'] for row in rows} != {f'WP{i}' for i in range(1, 9)}:
        raise ValueError('all eight probes must be present')
    fixtures = {p.stem: p for p in Path(args.fixtures).glob('wp*.json')}
    if {row['fixture'] for row in rows} != set(fixtures):
        raise ValueError('every W01 fixture must have an executed case')
    manifest_text = Path(args.manifest).read_text()
    version = next(line.removeprefix('**Version:** ').strip() for line in manifest_text.splitlines() if line.startswith('**Version:**'))
    results = []
    for index, row in enumerate(rows):
        fixture = fixtures[row['fixture']]
        source = json.loads(fixture.read_text())
        if source['probe'] != row['probe'] or source['schema'] != 'collabtext-consumer-probe/1':
            raise ValueError('fixture probe/schema mismatch')
        expected = row['expected']
        if args.inject_bad_expectation and index == 0:
            expected = {'deliberately_wrong': True}
        worker_case = 'abnormal' if args.inject_abnormal_worker and index == 0 else row['worker']
        command = [args.worker, worker_case]
        result = dict(case_id=row['id'], probe=row['probe'], capability=row['capability'],
                      formats=row['formats'], seed=specification['seed'], fixture_sha256=digest(fixture),
                      fixture_id=source['id'], expected=expected, observed=None, outcome='error',
                      producer_revision=args.revision, exit_status=None, signal=None, command=command)
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
                if set(data) != {'schema', 'case', 'observed'} or data['schema'] != 'collabtext-installed-worker/1' or data['case'] != worker_case:
                    raise ValueError('invalid worker result/schema/case')
                observed = data['observed']; result['observed'] = observed
                if observed == expected:
                    result['outcome'] = 'pass'
                else:
                    exception = row.get('baseline_exception')
                    if exception and observed == exception['observed']:
                        if exception['outcome'] not in {'known_failure', 'unsupported'} or not exception['owner']:
                            raise ValueError('invalid exact baseline exception')
                        # An unavailable capability needs the installed manifest's
                        # explicit statement, never a successful substitute path.
                        if exception['outcome'] == 'unsupported' and not any('GC, pins, checkpoints' in line and 'not implemented' in line for line in manifest_text.splitlines()):
                            raise ValueError('unsupported pins disagree with installed manifest')
                        result.update(outcome=exception['outcome'], exception_owner=exception['owner'],
                                      exception_formats=exception['formats'], baseline_expected=exception['observed'])
                    else:
                        result['diagnostic'] = 'unexpected assertion mismatch'
        except (OSError, ValueError, subprocess.TimeoutExpired) as error:
            result['diagnostic'] = str(error)
        result['required_strict'] = args.gate == 'live' and (row['probe'] in LIVE_PROBES or row['capability'] == 'gc_disabled')
        result['accepted'] = result['outcome'] == 'pass' or (not result['required_strict'] and result['outcome'] in {'known_failure', 'unsupported'})
        # Even live mode retains the complete eight-probe matrix; its pending
        # ordering/pins rows are information, never counted as strict passes.
        results.append(result)
        print(f"{row['probe']} {row['id']}: {result['outcome']}")
    passed = all(row['accepted'] for row in results)
    strict = [row for row in results if row['required_strict']]
    if args.gate == 'live' and ({row['probe'] for row in strict} - {'WP8'} != LIVE_PROBES or not strict):
        passed = False
    report = dict(schema='collabtext-installed-probe-results/1', producer_revision=args.revision,
                  mode=args.mode, gate=args.gate, seed=specification['seed'], capability_manifest_version=version,
                  capability_manifest_sha256=digest(args.manifest), case_spec_sha256=digest(args.cases),
                  formats={'position': 'CTPT/1 experimental unpublished', 'text': 'CTBO/1', 'list': 'CTLO/1'},
                  cases=results, case_count=len(results), strict_pass_count=sum(row['outcome']=='pass' for row in strict),
                  exit_status=0 if passed else 1)
    (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    return report['exit_status']

if __name__ == '__main__':
    try:
        sys.exit(main())
    except (ValueError, KeyError, OSError, StopIteration) as error:
        print(f'probe runner: {error}', file=sys.stderr)
        sys.exit(2)
