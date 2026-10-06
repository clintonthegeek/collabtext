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
    # The recorded formats come from the case file, not from literals in this
    # runner. They used to be hardcoded as CTBO/1 and CTLO/1, which the same file
    # declared as CTBO/2 and CTLO/2 -- so every retained result disagreed with the
    # codec it exercised, in the one field a reader would use to identify the
    # wire version. Derived, they cannot drift again; absent, the run fails.
    declared = sorted({f for row in rows for f in row.get('formats', [])})

    def family(prefix):
        found = next((f for f in declared if f.startswith(prefix)), None)
        if found is None:
            raise ValueError(f'no case declares a {prefix} format; declared: {declared}')
        return found
    formats = {
        'position': 'CTPT/1 experimental unpublished' if family('CTPT') == 'CTPT/1' else family('CTPT'),
        'text': family('CTBO'),
        'list': family('CTLO'),
    }
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
        # A case is one worker invocation, or several. The last phase's stdout is
        # the observation; earlier phases must also succeed, so "the mint worked"
        # can never be separated from "the replay worked". An injected abnormal
        # worker replaces the phases outright rather than prepending to them.
        phases = ['abnormal'] if worker_case == 'abnormal' else (row.get('phases') or [worker_case])
        bundle = output / (row['id'] + '.bundle')
        commands = [[args.worker, phase, str(bundle)] if len(phases) > 1 else [args.worker, phase]
                    for phase in phases]
        result = dict(case_id=row['id'], probe=row['probe'], capability=row['capability'],
                      formats=row['formats'], seed=specification['seed'], fixture_sha256=digest(fixture),
                      fixture_id=source['id'], expected=expected, observed=None, outcome='error',
                      producer_revision=args.revision, phases=[], exit_status=None, signal=None)
        try:
            for phase_index, command in enumerate(commands):
                child = subprocess.run(command, capture_output=True, text=True, timeout=20, preexec_fn=no_core)
                phase = dict(name=phases[phase_index], command=command,
                             exit_status=child.returncode if child.returncode >= 0 else None,
                             signal=-child.returncode if child.returncode < 0 else None)
                (output / f"{row['id']}.{phase_index}.stdout.log").write_text(child.stdout)
                (output / f"{row['id']}.{phase_index}.stderr.log").write_text(child.stderr)
                result['phases'].append(phase)
                result['exit_status'] = phase['exit_status']
                result['signal'] = phase['signal']
                if phase['exit_status'] != 0:
                    result['diagnostic'] = f"worker failed or signalled in phase {phase['name']}"
                    break
                data = json.loads(child.stdout)
                if set(data) != {'schema', 'case', 'observed'} or data['schema'] != 'collabtext-installed-worker/1' \
                        or data['case'] != phases[phase_index]:
                    raise ValueError('invalid worker result/schema/phase')
                if phase_index + 1 < len(commands):
                    continue
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
        result['command'] = commands[-1]
        result['stdout_log'] = f"{row['id']}.{len(commands) - 1}.stdout.log"
        result['stderr_log'] = f"{row['id']}.{len(commands) - 1}.stderr.log"
        result['required_strict'] = args.gate == 'live' and (row['probe'] in LIVE_PROBES or row['capability'] == 'gc_disabled')
        result['accepted'] = result['outcome'] == 'pass' or (not result['required_strict'] and result['outcome'] in {'known_failure', 'unsupported'})
        # Even live mode retains the complete eight-probe matrix; its pending
        # ordering/pins rows are information, never counted as strict passes.
        results.append(result)
        print(f"{row['probe']} {row['id']}: {result['outcome']}")
    passed = all(row['accepted'] for row in results)
    strict = [row for row in results if row['required_strict']]
    # Live mode is a strict gate over an exact probe set: WP1-WP5/WP7 plus the
    # no-GC containment smoke, nothing substituted, nothing newly skipped. A
    # missing row fails the gate rather than shrinking the set.
    if args.gate == 'live' and ({row['probe'] for row in strict} - {'WP8'} != LIVE_PROBES
                                or not strict
                                or any(row['outcome'] != 'pass' for row in strict)):
        passed = False
    report = dict(schema='collabtext-installed-probe-results/1', producer_revision=args.revision,
                  mode=args.mode, gate=args.gate, seed=specification['seed'], capability_manifest_version=version,
                  capability_manifest_sha256=digest(args.manifest), case_spec_sha256=digest(args.cases),
                  formats=formats, declared_formats=declared,
                  strict_probes=sorted(LIVE_PROBES), cases=results, case_count=len(results),
                  strict_case_count=len(strict),
                  strict_pass_count=sum(row['outcome']=='pass' for row in strict),
                  unfinished=[{'case_id': row['case_id'], 'probe': row['probe'], 'outcome': row['outcome'],
                               'owner': row.get('exception_owner')} for row in results if row['outcome'] != 'pass'],
                  exit_status=0 if passed else 1)
    (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    return report['exit_status']

if __name__ == '__main__':
    try:
        sys.exit(main())
    except (ValueError, KeyError, OSError, StopIteration) as error:
        print(f'probe runner: {error}', file=sys.stderr)
        sys.exit(2)
