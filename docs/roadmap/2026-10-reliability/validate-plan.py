#!/usr/bin/env python3
"""Validate documentation task dependencies and show structurally ready work."""
from pathlib import Path
import argparse
import json
import re
import sys

root = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--ready', action='store_true')
args = parser.parse_args()
data = json.loads((root / 'TASKS.json').read_text())
tasks = data['tasks']
by_id = {t['id']: t for t in tasks}
errors = []
if len(by_id) != len(tasks):
    errors.append('Duplicate task IDs')
allowed = {'not_started', 'in_progress', 'blocked', 'complete', 'deferred'}
seen = set()
for task in tasks:
    tid = task['id']
    if task['status'] not in allowed:
        errors.append(f'{tid}: invalid status')
    for key in ('title', 'files', 'steps', 'acceptance', 'verification', 'addresses'):
        if not task.get(key):
            errors.append(f'{tid}: empty {key}')
    for dep in task['depends_on']:
        if dep not in by_id:
            errors.append(f'{tid}: missing dependency {dep}')
        elif dep not in seen:
            errors.append(f'{tid}: not in serial dependency order ({dep})')
    seen.add(tid)

visiting, done = set(), set()
def visit(tid):
    if tid in visiting:
        errors.append(f'Dependency cycle at {tid}')
        return
    if tid in done:
        return
    visiting.add(tid)
    for dep in by_id[tid]['depends_on']:
        if dep in by_id:
            visit(dep)
    visiting.remove(tid)
    done.add(tid)
for tid in by_id:
    visit(tid)

# Check the sequencing amendment as a transitive contract. A seemingly small
# edge added later must not silently put consumer integration behind recovery.
def ancestors(tid, visited=None):
    visited = set() if visited is None else visited
    for dep in by_id.get(tid, {}).get('depends_on', []):
        if dep in by_id and dep not in visited:
            visited.add(dep)
            ancestors(dep, visited)
    return visited

for tid, contract in data.get('early_consumer_gates', {}).items():
    if tid not in by_id:
        errors.append(f'Early consumer gate names missing task {tid}')
        continue
    inherited = ancestors(tid)
    for dep in contract.get('required_ancestors', []):
        if dep not in inherited:
            errors.append(f'{tid}: missing required consumer prerequisite {dep}')
    for dep in contract.get('forbidden_ancestors', []):
        if dep in inherited:
            errors.append(f'{tid}: consumer sequencing regressed behind {dep}')

probe_owners = data.get('consumer_probe_owners', {})
if probe_owners:
    if set(probe_owners) != {f'WP{i}' for i in range(1, 9)}:
        errors.append('Consumer probe ownership must cover exactly WP1-WP8')
    for probe, owners in probe_owners.items():
        if not owners or 'W01' not in owners or 'E05' not in owners:
            errors.append(f'{probe}: missing fixture or final acceptance owner')
        for owner in owners:
            if owner not in by_id:
                errors.append(f'{probe}: missing owning task {owner}')

headings = []
task_blocks = {}
for doc in sorted(root.glob('[0-9][0-9]-*.md')):
    contents = doc.read_text()
    # A task ID is two digits, optionally followed by a lowercase letter for a
    # split subtask (P06a, P06b). Splitting a task is explicitly supported by
    # EXECUTION.md, so the plan reader has to accept it.
    matches = list(re.finditer(r'^## ([A-Z][0-9]{2}[a-z]?) — ', contents, re.M))
    headings.extend(match.group(1) for match in matches)
    for index, match in enumerate(matches):
        end = matches[index + 1].start() if index + 1 < len(matches) else len(contents)
        task_blocks[match.group(1)] = contents[match.start():end]
if sorted(headings) != sorted(by_id):
    errors.append('Phase Markdown task IDs do not match registry')
for task in tasks:
    tid = task['id']
    block = task_blocks.get(tid, '')
    expected = [f'## {tid} — {task["title"]}',
                f'**Depends on:** {", ".join(task["depends_on"]) or "none"}',
                f'**Coverage:** {", ".join(task["addresses"])}', task['verification']]
    expected.extend(f'- `{path}`' for path in task['files'])
    expected.extend(f'{index}. {step}' for index, step in enumerate(task['steps'], 1))
    expected.extend(f'- {criterion}' for criterion in task['acceptance'])
    if task.get('gate'):
        expected.append(f'**Design/decision gate:** {task["gate"]}')
    if any(line not in block for line in expected):
        errors.append(f'{tid}: Markdown and registry descriptions disagree')
coverage = {x for task in tasks for x in task['addresses']}
for item in [f'F{i}' for i in range(1, 11)] + [f'R{i}' for i in range(1, 15)]:
    if item not in coverage:
        errors.append(f'Missing coverage: {item}')
consumer_requirements = set(data.get('consumer_requirements', []))
consumer_coverage = set()
for task in tasks:
    requirements = set(task.get('consumer_requirements', []))
    consumer_coverage.update(requirements)
    if requirements - consumer_requirements:
        errors.append(f'{task["id"]}: unknown consumer requirement')
    if requirements and 'docs/specs/vnext-external-position-contract.md' not in task['files']:
        errors.append(f'{task["id"]}: missing normative external-position scope')
for item in sorted(consumer_requirements - consumer_coverage):
    errors.append(f'Missing consumer coverage: {item}')
if consumer_requirements and not (root / data.get('consumer_contract', '')).is_file():
    errors.append('Missing consumer contract')
for doc in root.glob('*.md'):
    for target in re.findall(r'\]\(([^)]+)\)', doc.read_text()):
        if target.startswith(('https://', 'http://', '#')):
            continue
        target = target.split('#', 1)[0]
        if not (doc.parent / target).exists():
            errors.append(f'{doc.name}: missing local link {target}')
if errors:
    print('\n'.join(errors), file=sys.stderr)
    sys.exit(1)
print(f'Valid: {len(tasks)} tasks; acyclic graph; serial order; all F1–F10/R1–R14 covered.')
if consumer_requirements:
    print(f'Consumer coverage: {", ".join(sorted(consumer_requirements))}; normative contract exists.')
if data.get('early_consumer_gates'):
    print('Early consumer sequencing: transitive prerequisites and exclusions checked; WP1-WP8 owners exist.')
if args.ready:
    ready = [t for t in tasks if t['status'] == 'not_started'
             and all(by_id[d]['status'] == 'complete' for d in t['depends_on'])]
    for t in ready:
        print(f'{t["id"]}: {t["title"]}')
        if t.get('gate'):
            print(f'  Gate still requires evidence: {t["gate"]}')
