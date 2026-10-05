"""Validate S01 histories and exact semantic oracles, not production CRDT output."""
import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIXTURES = ROOT / 'libs/collabtext/tests/fixtures/sequence-semantics'
MAX_COUNTER = (1 << 64) - 1


def require(condition, message):
    if not condition:
        raise ValueError(message)


def identity(value, actors):
    actor, counter = value.split('/')
    require(actor in actors, 'unknown actor')
    require(counter.isdecimal() and 0 < int(counter) < MAX_COUNTER, 'invalid counter')
    return actor, int(counter)


def validate(case):
    require(case['schema'] == 'collabtext-sequence-fixture/1', 'unsupported schema')
    kind = case['primitive']
    require(kind in ('text', 'list'), 'invalid primitive')
    actors = case['actors']
    require(len(set(actors.values())) == len(actors), 'duplicate actors')
    for value in actors.values():
        require(len(value) == 32 and int(value, 16) > 0, 'invalid actor bytes')
    context = case['context']
    require(context['document'] == case['id'] and context['stream'] == kind
            and context['epoch'] == 1, 'invalid context')
    require(case['offset_unit'] == ('utf8_bytes' if kind == 'text' else 'list_index'), 'wrong units')
    items, owners, operations = {}, {}, {}
    all_ids = set()

    def add_item(item, operation):
        identity(item['id'], actors)
        require(item['id'] not in items, 'duplicate item identity')
        value = item['value']
        if kind == 'text':
            require(isinstance(value, str) and len(value) == 1, 'item is not one scalar')
            value.encode('utf-8', errors='strict')
        else:
            require(type(value) is int and 0 <= value <= MAX_COUNTER, 'invalid list value')
        items[item['id']] = value
        owners[item['id']] = operation

    for item in case['initial']:
        add_item(item, None)
        all_ids.add(item['id'])
    causal = {}
    for event in case['operations']:
        eid = event['id']
        actor, counter = identity(eid, actors)
        require(eid not in all_ids, 'duplicate operation identity')
        parents = event['after']
        require(len(set(parents)) == len(parents) and all(p in operations for p in parents),
                'missing or duplicate causal dependency')
        causal[eid] = set(parents)
        for parent in parents:
            causal[eid].update(causal[parent])
            require(counter > identity(parent, actors)[1], 'causal counter does not advance')
        for previous in operations:
            if identity(previous, actors)[0] == actor:
                require(previous in causal[eid], 'actor history omits its predecessor')
        if event['kind'] == 'insert':
            left, right = event['between']
            require(left == 'START' or left in items, 'unknown left neighbor')
            require(right == 'END' or right in items, 'unknown right neighbor')
            for neighbor in (left, right):
                owner = owners.get(neighbor)
                require(owner is None or owner in causal[eid], 'neighbor was not causally observed')
            require(bool(event['items']), 'empty insert')
            for item in event['items']:
                item_actor, item_counter = identity(item['id'], actors)
                require(item_actor == actor, 'inserted item has foreign actor')
                require(item['id'] not in all_ids, 'reused identity')
                require(item_counter <= counter, 'item beyond operation interval')
                add_item(item, eid)
                all_ids.add(item['id'])
            if kind == 'text':
                counters = [identity(i['id'], actors)[1] for i in event['items']]
                require(counters == list(range(counter - len(counters), counter)), 'invalid text interval')
            else:
                require(len(event['items']) == 1 and event['items'][0]['id'] == eid,
                        'list origin must be its insertion identity')
        elif event['kind'] == 'delete':
            require(event['targets'] and len(set(event['targets'])) == len(event['targets']), 'bad delete')
            for target in event['targets']:
                require(target in items, 'delete names unknown item')
                require(owners[target] is None or owners[target] in causal[eid], 'unobserved deletion')
        elif event['kind'] in ('undo', 'redo'):
            target = event['target']
            require(target in causal[eid] and operations[target]['kind'] in ('insert', 'delete'), 'bad undo target')
            require(identity(target, actors)[0] == actor, 'undo targets a peer edit')
        else:
            raise ValueError('unknown event kind')
        operations[eid] = event
        all_ids.add(eid)
    for arrival in case['arrival_orders']:
        require(len(arrival) == len(operations) and set(arrival) == set(operations), 'arrival is not a permutation')
    require(case['arrival_orders'], 'missing arrivals')
    expected = case['expected']
    order, hidden = expected['order'], expected['hidden']
    require(len(set(order)) == len(order) and len(set(hidden)) == len(hidden), 'duplicate expected item')
    require(not set(order).intersection(hidden) and set(order + hidden) == set(items), 'incomplete expected state')
    if kind == 'text':
        require(expected['text'] == ''.join(items[i] for i in order), 'wrong expected text')
        boundaries = {0}
        offset = 0
        for i in order:
            offset += len(items[i].encode('utf-8'))
            boundaries.add(offset)
    else:
        require(expected['ids'] == [items[i] for i in order], 'wrong expected list')
        boundaries = set(range(len(order) + 1))
    for token in case['tokens']:
        require(token['context'] == context and token['affinity'] in ('left', 'right'), 'invalid token scope/affinity')
        require(all(p in operations for p in token['after']), 'missing mint causal state')
        for neighbor in token['between']:
            require(neighbor in ('START', 'END') or neighbor in items, 'unknown token neighbor')
            owner = owners.get(neighbor)
            require(owner is None or any(owner == p or owner in causal[p] for p in token['after']),
                    'token references unobserved item')
        placement = expected['positions'][token['id']]
        require(type(placement['offset']) is int and placement['offset'] in boundaries
                and placement['state'] in ('resolved', 'collapsed'), 'bad exact position')
    token_ids = [token['id'] for token in case['tokens']]
    require(len(set(token_ids)) == len(token_ids), 'duplicate token identity')
    for comparison in expected['comparisons']:
        a, b = comparison['left'], comparison['right']
        require(a in token_ids and b in token_ids, 'comparison names unknown token')
        x, y = (expected['positions'][t]['offset'] for t in (a, b))
        relation = 'less' if x < y else 'greater' if x > y else 'equal'
        require(comparison['order'] == relation, 'wrong token comparison')
    if 'missing_list_lookup' in case:
        require(kind == 'list' and case['missing_list_lookup']['value'] not in expected['ids']
                and case['missing_list_lookup']['outcome'] == 'not_found', 'invalid missing-list oracle')
    initial_boundaries = {0}
    current = 0
    for item in case['initial']:
        current += len(item['value'].encode('utf-8')) if kind == 'text' else 1
        initial_boundaries.add(current)
    for invalid in case.get('invalid_mint_offsets', []):
        require(invalid not in initial_boundaries and invalid >= 0, 'declared invalid mint is legal')
    return len(operations), len(case['arrival_orders'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', nargs='?', type=Path, default=FIXTURES)
    args = parser.parse_args()
    paths = sorted(args.directory.glob('*.json'))
    require(bool(paths), 'zero fixtures')
    totals = [0, 0]
    for path in paths:
        require(path.stat().st_size < 128 * 1024, 'fixture too large')
        count = validate(json.loads(path.read_text()))
        totals = [a + b for a, b in zip(totals, count)]
    print(f'{len(paths)} fixtures: {totals[0]} operations, {totals[1]} arrival permutations; exact semantic oracles valid.')


if __name__ == '__main__':
    main()
