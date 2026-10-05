"""Validate the W01 consumer-probe fixtures: schema, causality, exact oracles.

This tool specifies cases. It proves nothing about engine behaviour: a passing
run means the fixtures are internally consistent and their expectations are
derivable from the histories, not that collabtext passes WP1-WP8. W02 supplies
the executable adapter and result matrix.

Run ``python3 tools/validate-consumer-fixtures.py --self-test`` to confirm this
validator actually rejects malformed input. Every rejection below was reached by
deliberate mutation; see ``MUTATIONS`` at the bottom of this file.
"""
import argparse
import copy
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIXTURES = ROOT / 'libs/collabtext/tests/fixtures/external-position'
MAX_COUNTER = (1 << 64) - 1
PROBES = {f'WP{i}' for i in range(1, 9)}
SOURCES = {'consumer_asserted', 'wave_protocol_normative', 'roadmap_inference'}
EXTERNAL = {'consumer_asserted', 'wave_protocol_normative'}
EXTERNAL = re.compile(r'(wave|alternatives|Markoff|0001-protocol|positions:)')
INTERNAL = re.compile(
    r'(W[1-6]\b|vnext-|00-consumer-integration|EXECUTION|README|'
    r'\b[PSGALWX]E?\d+[a-z]?\b|\bF\d+\b|\bR\d+\b)')
ATTRIBUTION = 'reported_by_wave_not_reproduced_here'
RESOLVED_KINDS = {'resolved', 'comparison'}
UNRESOLVED_KINDS = {'detached', 'collapsed', 'gone', 'stale', 'not_found',
                    'design_pending'}
ALL_KINDS = RESOLVED_KINDS | UNRESOLVED_KINDS | {
    'typed_error', 'no_silent_relocation', 'replica_agreement', 'codec_roundtrip',
    'reject_trailing_suffixes', 'reject_truncation', 'state_unchanged_on_reject',
    'fresh_process_replay', 'passage_contiguous', 'unspecified_ordering',
    'evidence_rule', 'no_fabricated_offset', 'no_mutation_no_abort',
    'no_assert_no_abort', 'consumer_owned_check', 'mutation_refused',
    'scoped_affinity_token', 'unit_declaration', 'reclamation_disabled',
    'not_a_pin', 'requires_pin', 'provenance_note',
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def split_identity(value, actors):
    require(isinstance(value, str) and '/' in value, 'malformed identity')
    actor, counter = value.split('/', 1)
    require(actor in actors, 'unknown actor')
    require(counter.isdecimal() and 0 < int(counter) < MAX_COUNTER,
            'invalid counter')
    return actor, int(counter)


def boundaries_of(case, order):
    """Scalar-boundary offsets available in the converged text/list."""
    edges = {0}
    running = 0
    values = case['_values']
    if case['primitive'] == 'text':
        for item in order:
            running += len(values[item].encode('utf-8'))
            edges.add(running)
    else:
        for _ in order:
            running += 1
            edges.add(running)
    return edges


def boundary_range(case, order, token):
    """Offsets a boundary between the token's neighbours could legitimately take.

    ``lo`` is the end of the left neighbour, ``hi`` the start of the right one.
    When nothing was inserted at the boundary the two coincide and the offset is
    fully determined by the history. When content *was* inserted there, every
    offset in between is affinity-dependent, and the rule that picks one belongs
    to S04a/S04b. This tool constrains the range and refuses to invent the rule.
    """
    values = case['_values']

    def width(item):
        if item == 'END':
            return 0
        return len(values[item].encode('utf-8')) if case['primitive'] == 'text' else 1

    def start_of(target):
        running = 0
        for item in order:
            if item == target:
                return running
            running += width(item)
        if target == 'END':
            return running
        raise KeyError(target)

    left, right = token['between']
    lo = 0 if left == 'START' else start_of(left) + width(left)
    hi = start_of(right)
    return lo, hi


def validate_expectation(case, spec):
    require(isinstance(spec, dict), 'expectation is not an object')
    require(isinstance(spec.get('id'), str) and spec['id'], 'expectation lacks id')
    require(spec.get('kind') in ALL_KINDS, 'unknown expectation kind')
    source = spec.get('source')
    require(source in SOURCES, 'expectation lacks a provenance source')
    citation = spec.get('citation')
    require(isinstance(citation, str) and citation.strip(), 'expectation lacks citation')
    # A derived expectation must not be attributed to the consumer, and a
    # consumer-asserted one must point at the document that said it. This is the
    # guard against a fabricated citation, which is the failure mode that makes a
    # provenance report worthless.
    if source == 'roadmap_inference':
        require(INTERNAL.search(citation),
                'inference must cite the internal requirement it derives from')
    else:
        require(EXTERNAL.search(citation),
                'consumer-asserted expectation must cite an external document')
    token = spec.get('token')
    if token is not None:
        require(token in case['_tokens'], 'expectation names an unknown token')
    if spec['kind'] == 'resolved':
        require(token in case['_tokens'], 'resolved expectation needs a token')
        require(type(spec.get('offset')) is int, 'resolved expectation needs an offset')
        require(type(spec.get('collapsed')) is bool,
                'resolved expectation must state collapsed explicitly')
        placement = case['expected']['positions'].get(token)
        require(placement is not None, 'resolved token has no expected position')
        require(placement['offset'] == spec['offset']
                and placement['state'] == 'resolved', 'expectation contradicts position')
    if spec['kind'] == 'comparison':
        left, right = spec.get('left'), spec.get('right')
        require(left in case['_tokens'] and right in case['_tokens'],
                'comparison names an unknown token')
        placements = case['expected']['positions']
        require(left in placements and right in placements,
                'compared tokens need positions')
        a, b = placements[left]['offset'], placements[right]['offset']
        relation = 'less' if a < b else 'greater' if a > b else 'equal'
        require(spec.get('order') == relation, 'comparison contradicts positions')
    if spec['kind'] == 'design_pending':
        require(isinstance(spec.get('pending_rule'), str) and spec['pending_rule'],
                'design_pending needs a named pending rule')
        require(isinstance(spec.get('owner'), str) and re.fullmatch(r'[A-Z]\d+[a-z]?', spec['owner']),
                'design_pending needs a single owning task')
        require(spec.get('note'), 'design_pending must say what is unresolved')
    if spec['kind'] == 'typed_error':
        require(isinstance(spec.get('error'), str) and spec['error'],
                'typed_error needs an error code')
    if spec['kind'] == 'passage_contiguous':
        passage = spec.get('passage')
        require(isinstance(passage, str) and passage, 'passage_contiguous needs a passage')
        require(passage in case['expected']['text'],
                'declared passage is absent from the expected text')


def validate(case):
    require(case['schema'] == 'collabtext-consumer-probe/1', 'unsupported schema')
    require(isinstance(case.get('id'), str) and re.fullmatch(r'[a-z0-9-]+', case['id']),
            'fixture id must be kebab-case')
    probe = case.get('probe')
    require(probe in PROBES, 'invalid probe id')
    kind = case['primitive']
    require(kind in ('text', 'list'), 'invalid primitive')
    require(case['offset_unit'] == ('utf8_bytes' if kind == 'text' else 'list_index'),
            'wrong offset units for primitive')
    require(case['id'].startswith(probe.lower()), 'fixture id must start with its probe')

    actors = case['actors']
    require(isinstance(actors, dict) and actors, 'missing actors')
    require(len(set(actors.values())) == len(actors), 'duplicate actor bytes')
    for label, value in actors.items():
        require(isinstance(value, str) and len(value) == 32
                and int(value, 16) > 0, 'invalid actor bytes')
        require(label, 'empty actor label')

    context = case['context']
    require(context['document'] == case['id'], 'context document must be the fixture id')
    require(isinstance(context['stream'], str) and context['stream'], 'invalid stream')
    require(type(context['epoch']) is int and context['epoch'] > 0, 'invalid epoch')

    seed = case.get('seed')
    require(type(seed) is int and seed > 0, 'fixture must pin a positive seed')

    # ---- items and operations -------------------------------------------------
    values, owners, operations, all_ids = {}, {}, {}, set()
    causal = {}

    def add_item(item, operation):
        split_identity(item['id'], actors)
        require(item['id'] not in values, 'duplicate item identity')
        value = item['value']
        if kind == 'text':
            require(isinstance(value, str) and len(value) == 1, 'item is not one scalar')
            require(len(value.encode('utf-8')) > 0, 'item is not encodable')
        else:
            require(type(value) is int and 0 <= value <= MAX_COUNTER, 'invalid list value')
        values[item['id']] = value
        owners[item['id']] = operation

    for item in case['initial']:
        add_item(item, None)
        all_ids.add(item['id'])

    for event in case['operations']:
        eid = event['id']
        actor, counter = split_identity(eid, actors)
        require(eid not in all_ids, 'duplicate operation identity')
        require(eid not in operations, 'duplicate operation id')
        parents = event['after']
        require(len(set(parents)) == len(parents)
                and all(p in operations for p in parents),
                'missing or duplicate causal dependency')
        causal[eid] = set(parents)
        for parent in parents:
            causal[eid].update(causal[parent])
            require(counter > split_identity(parent, actors)[1],
                    'causal counter does not advance')
        for previous in operations:
            if split_identity(previous, actors)[0] == actor:
                require(previous in causal[eid], 'actor history omits its predecessor')
        if event['kind'] == 'insert':
            left, right = event['between']
            require(left == 'START' or left in values, 'unknown left neighbour')
            require(right == 'END' or right in values, 'unknown right neighbour')
            for neighbour in (left, right):
                owner = owners.get(neighbour)
                require(owner is None or owner in causal[eid],
                        'neighbour was not causally observed')
            require(bool(event['items']), 'empty insert')
            for item in event['items']:
                item_actor, item_counter = split_identity(item['id'], actors)
                require(item_actor == actor, 'inserted item has a foreign actor')
                require(item['id'] not in all_ids, 'reused identity')
                add_item(item, eid)
                all_ids.add(item['id'])
            if kind == 'text':
                # A text insert allocates a contiguous run of scalar identities
                # below its own operation counter.
                for item in event['items']:
                    require(split_identity(item['id'], actors)[1] < counter,
                            'item beyond its operation interval')
                counters = [split_identity(i['id'], actors)[1] for i in event['items']]
                require(counters == list(range(counters[0], counters[0] + len(counters))),
                        'text item identities are not a contiguous interval')
            else:
                # A list element's identity *is* its insertion operation, so the
                # two counters are equal by construction and the interval rule
                # above does not apply.
                require(len(event['items']) == 1
                        and event['items'][0]['id'] == eid,
                        'list origin must be its insertion identity')
        elif event['kind'] == 'delete':
            require(event['targets'] and len(set(event['targets'])) == len(event['targets']),
                    'bad delete targets')
            for target in event['targets']:
                require(target in values, 'delete names an unknown item')
                owner = owners[target]
                require(owner is None or owner in causal[eid], 'unobserved deletion')
        else:
            raise ValueError('unknown event kind')
        operations[eid] = event
        all_ids.add(eid)

    # ---- arrival orders -------------------------------------------------------
    require(case['arrival_orders'], 'missing arrival orders')
    for arrival in case['arrival_orders']:
        require(len(arrival) == len(operations)
                and len(set(arrival)) == len(arrival)
                and set(arrival) == set(operations),
                'arrival is not a permutation of the operations')
        seen = set()
        for eid in arrival:
            require(all(parent in seen for parent in operations[eid]['after']),
                    'arrival order violates causality')
            seen.add(eid)

    # ---- exact converged oracle ----------------------------------------------
    expected = case['expected']
    order, hidden = expected['order'], expected['hidden']
    require(len(set(order)) == len(order) and len(set(hidden)) == len(hidden),
            'duplicate expected item')
    require(not set(order).intersection(hidden), 'item is both visible and hidden')
    require(set(order + hidden) == set(values), 'incomplete expected state')
    if kind == 'text':
        require(expected['text'] == ''.join(values[i] for i in order),
                'expected text contradicts the expected order')
    else:
        require(expected['ids'] == [values[i] for i in order],
                'expected ids contradict the expected order')

    case['_values'] = values
    edges = boundaries_of(case, order)

    # ---- tokens ---------------------------------------------------------------
    tokens = {}
    for token in case['tokens']:
        tid = token['id']
        require(tid not in tokens, 'duplicate token identity')
        require(token['affinity'] in ('left', 'right'), 'invalid token affinity')
        require(all(p in operations for p in token['after']),
                'token names a missing mint operation')
        observed = set()
        for parent in token['after']:
            observed.add(parent)
            observed.update(causal[parent])
        for neighbour in token['between']:
            require(neighbour in ('START', 'END') or neighbour in values,
                    'unknown token neighbour')
            owner = owners.get(neighbour)
            require(owner is None or owner in observed,
                    'token references an unobserved item')
        tokens[tid] = token
    case['_tokens'] = tokens

    kinds = case.get('attachment_kinds', {})
    require(set(kinds) <= set(tokens), 'attachment kind names an unknown token')
    for tid, attachment in kinds.items():
        require(attachment in ('caret', 'strict'), 'invalid attachment kind')

    # A token's offset must follow from its neighbours, not merely be restated.
    for tid, token in tokens.items():
        lo, hi = boundary_range(case, order, token)
        require(lo <= hi, 'token neighbours are inverted in the converged order')
        placement = expected['positions'].get(tid)
        if placement is None:
            continue
        require(type(placement['offset']) is int and placement['offset'] in edges,
                'bad exact position')
        require(placement['state'] in ('resolved', 'collapsed'), 'bad position state')
        require(lo <= placement['offset'] <= hi,
                'expected position falls outside the boundary its neighbours allow')

    for comparison in expected['comparisons']:
        left, right = comparison['left'], comparison['right']
        require(left in tokens and right in tokens, 'comparison names an unknown token')
        a, b = expected['positions'][left]['offset'], expected['positions'][right]['offset']
        relation = 'less' if a < b else 'greater' if a > b else 'equal'
        require(comparison['order'] == relation, 'wrong token comparison')

    # ---- declared-invalid requests must really be invalid ----------------------
    for request in case['invalid_requests']:
        rkind = request.get('kind')
        require(rkind in ('mint_offset', 'mint_index', 'wrong_scope',
                          'token_bytes', 'missing_list_id', 'unknown_token_in_live_block'),
                'unknown invalid-request kind')
        require(isinstance(request.get('error'), str) and request['error'],
                'invalid request needs an expected error code')
        if rkind in ('mint_offset', 'mint_index'):
            field = 'offset' if rkind == 'mint_offset' else 'index'
            value = request[field]
            require(type(value) is int and value >= 0, 'invalid mint value')
            require(value not in edges,
                    'declared invalid mint is actually a legal boundary')

    # ---- expectations ---------------------------------------------------------
    specs = case['expectations']
    require(specs, 'fixture declares no expectation')
    seen_ids = set()
    for spec in specs:
        require(spec['id'] not in seen_ids, 'duplicate expectation id')
        seen_ids.add(spec['id'])
        validate_expectation(case, spec)

    # Every resolved expectation must be backed by a position. The converse is not
    # required: a sweep fixture legitimately pins many offsets in its oracle while
    # asserting a typed result for a few of them. What must never happen is a
    # token that is silently left out of both.
    resolved = {s['token'] for s in specs if s['kind'] == 'resolved'}
    require(resolved <= set(expected['positions']),
            'resolved expectation has no expected position')
    require(set(expected['positions']) <= set(tokens),
            'expected position names an unknown token')
    for tid in tokens:
        if tid not in expected['positions']:
            require(any(s.get('token') == tid for s in specs
                        if s['kind'] in UNRESOLVED_KINDS),
                    'unpositioned token needs an explicit unresolved expectation')

    # ---- provenance and ownership ---------------------------------------------
    for observation in case['third_party_observations']:
        require(observation.get('attribution') == ATTRIBUTION,
                'third-party observation must be attributed, never a native result')
        require(observation.get('library'), 'observation lacks a library')
        require(observation.get('version'), 'observation lacks a version')
        require(observation.get('claim'), 'observation lacks a claim')
        require(EXTERNAL.search(observation.get('citation', '')),
                'third-party observation must cite the report it came from')

    ownership = case['ownership']
    for stage in ('early', 'strict'):
        owners = ownership.get(stage)
        require(isinstance(owners, list) and owners
                and all(re.fullmatch(r'(W\d+|[A-Z]\d+[a-z]?|E\d+[a-z]?)', o)
                        for o in owners),
                f'invalid {stage} ownership')

    if 'missing_list_lookup' in case:
        require(kind == 'list', 'missing_list_lookup requires a list fixture')
        lookup = case['missing_list_lookup']
        require(lookup['outcome'] == 'not_found', 'invalid missing-list oracle')
        require(lookup['value'] not in expected['ids'],
                'declared missing list id is present')

    return len(operations), len(tokens), len(specs)


def load(path):
    return json.loads(path.read_text(encoding='utf-8'))


def corpus(directory=FIXTURES):
    paths = sorted(Path(directory).glob('*.json'))
    require(bool(paths), 'zero fixtures')
    for path in paths:
        require(path.stat().st_size < 128 * 1024, f'fixture too large: {path.name}')
    return paths


def validate_corpus(directory=FIXTURES):
    paths = corpus(directory)
    totals = [0, 0, 0]
    probes = set()
    for path in paths:
        case = load(path)
        require(case.get('id') == path.stem, f'fixture id does not match filename: {path.name}')
        counts = validate(case)
        totals = [a + b for a, b in zip(totals, counts)]
        probes.add(case['probe'])
    require(probes == PROBES,
            f'probe coverage is incomplete: missing {sorted(PROBES - probes)}')
    return len(paths), totals


# ---------------------------------------------------------------------------
# Self-test: mutate a valid fixture and require that this validator notices.
# A validator that cannot fail is worse than none, so each mutation below is a
# defect this tool exists to catch.
# ---------------------------------------------------------------------------
def _first(case, probe=None):
    for path in corpus():
        case = load(path)
        if probe is None or case['probe'] == probe:
            return case
    raise AssertionError('no fixture found')


MUTATIONS = []


def mutation(name):
    def register(fn):
        MUTATIONS.append((name, fn))
        return fn
    return register


@mutation('unsupported schema version')
def _(case):
    case['schema'] = 'collabtext-consumer-probe/2'


@mutation('unknown probe id')
def _(case):
    case['probe'] = 'WP9'


@mutation('offset unit contradicts primitive')
def _(case):
    case['offset_unit'] = 'list_index' if case['primitive'] == 'text' else 'utf8_bytes'


@mutation('duplicate actor identity')
def _(case):
    labels = list(case['actors'])
    case['actors'][labels[1]] = case['actors'][labels[0]]


@mutation('context document is not the fixture id')
def _(case):
    case['context']['document'] = 'somewhere-else'


@mutation('missing causal parent')
def _(case):
    for event in case['operations']:
        if event['after']:
            event['after'].append('A/9999')
            return case
    return None


@mutation('reused item identity')
def _(case):
    case['operations'][0]['items'][0]['id'] = case['operations'][0]['id']


@mutation('unobserved neighbour')
def _(case):
    # Point an insert at an item created by an operation outside its transitive
    # causal closure, which is exactly the "neighbour was not causally observed"
    # rule. Replacing a neighbour with START/END would be a legal alternative
    # history, not a corruption.
    closure = {item['id'] for item in case['initial']}
    for event in case['operations']:
        observed = set(closure)
        for parent in event['after']:
            observed.add(parent)
        if event['kind'] == 'insert' and event['after']:
            for candidate in case['operations']:
                for item in candidate.get('items', []):
                    if item['id'] not in observed:
                        event['between'][1] = item['id']
                        return case
        closure = observed
        for item in event.get('items', []):
            closure.add(item['id'])
    return None


@mutation('wrong expected text')
def _(case):
    if case['primitive'] != 'text':
        return None
    case['expected']['text'] = case['expected']['text'] + '!'
    return case


@mutation('wrong expected ids')
def _(case):
    if case['primitive'] != 'list':
        return None
    case['expected']['ids'] = list(case['expected']['ids']) + [0]
    return case


@mutation('incomplete expected state')
def _(case):
    case['expected']['hidden'] = []


@mutation('expected position contradicts token neighbours')
def _(case):
    for placement in case['expected']['positions'].values():
        placement['offset'] += 1
        for spec in case['expectations']:
            if spec['kind'] == 'resolved' and spec.get('token') in case['expected']['positions']:
                spec['offset'] = placement['offset']
        return case
    return None


@mutation('position outside the scalar boundary set')
def _(case):
    for placement in case['expected']['positions'].values():
        placement['offset'] = 9999
        return case
    return None


@mutation('resolved expectation contradicts the position')
def _(case):
    for spec in case['expectations']:
        if spec['kind'] == 'resolved':
            spec['offset'] += 1
            return case
    return None


@mutation('declared-invalid mint is actually legal')
def _(case):
    case['invalid_requests'].append(
        {'kind': 'mint_index' if case['primitive'] == 'list' else 'mint_offset',
         'index' if case['primitive'] == 'list' else 'offset': 0,
         'error': 'off_end'})
    return case


@mutation('arrival order is not a permutation')
def _(case):
    if not case['operations']:
        return None
    case['arrival_orders'][0] = case['arrival_orders'][0][:-1]
    return case


@mutation('arrival order violates causality')
def _(case):
    if not case['operations']:
        return None
    # Place a child operation before one of its parents.
    for event in case['operations']:
        if event['after']:
            order = [event['id']] + [e['id'] for e in case['operations']
                                      if e['id'] != event['id']]
            case['arrival_orders'][0] = order
            return case
    return None


@mutation('expectation without a provenance source')
def _(case):
    case['expectations'][0].pop('source')


@mutation('inference attributed to the consumer')
def _(case):
    spec = case['expectations'][0]
    spec['source'] = 'roadmap_inference'
    spec['citation'] = 'wave devs-our-asks:32'


@mutation('consumer assertion without an external citation')
def _(case):
    spec = case['expectations'][0]
    spec['source'] = 'consumer_asserted'
    spec['citation'] = 'W2'


@mutation('unknown expectation kind')
def _(case):
    case['expectations'][0]['kind'] = 'probably_fine'


@mutation('design_pending without an owner')
def _(case):
    case['expectations'] = [s for s in case['expectations'] if s['kind'] != 'resolved']
    case['expected']['positions'] = {}
    case['expected']['comparisons'] = []
    case['tokens'] = [t for t in case['tokens']
                      if any(s.get('token') == t['id'] for s in case['expectations'])]
    case['attachment_kinds'] = {}
    case['expectations'].append({
        'id': 'invented-pending', 'kind': 'design_pending', 'pending_rule': 'whatever',
        'source': 'roadmap_inference', 'citation': 'W2'})
    return case


@mutation('third-party observation un-attributed')
def _(case):
    case['third_party_observations'] = [{
        'library': 'Loro', 'version': '1.16.2', 'claim': 'x',
        'citation': 'wave ask2:139'}]


@mutation('third-party observation cited to nothing')
def _(case):
    case['third_party_observations'] = [{
        'library': 'Loro', 'version': '1.16.2', 'claim': 'x',
        'citation': 'measured locally', 'attribution': ATTRIBUTION}]


@mutation('missing ownership')
def _(case):
    case['ownership']['strict'] = []


@mutation('unpinned seed')
def _(case):
    case.pop('seed')


@mutation('empty expectation list')
def _(case):
    case['expectations'] = []


@mutation('token affinity is not explicit')
def _(case):
    if not case['tokens']:
        return None
    case['tokens'][0].pop('affinity')
    return case


@mutation('attachment kind names an unknown token')
def _(case):
    case['attachment_kinds'] = {'no-such-token': 'caret'}
    return case


@mutation('unpositioned token with no unresolved expectation')
def _(case):
    if not case['tokens']:
        return None
    case['tokens'].append({'id': 'ghost', 'after': [], 'between': ['START', 'END'],
                           'affinity': 'left'})
    return case


@mutation('missing-list id is actually present')
def _(case):
    if 'missing_list_lookup' not in case:
        return None
    case['missing_list_lookup']['value'] = case['expected']['ids'][0]
    return case


def self_test():
    """Apply every mutation to every fixture; require each to be rejected.

    A mutation that cannot apply to any fixture is itself a failure: it means
    the self-test silently claims coverage it does not have.
    """
    paths = corpus()
    for path in paths:
        validate(load(path))  # the corpus must be clean to begin with

    survivors = []
    exercised = {name: 0 for name, _ in MUTATIONS}
    for path in paths:
        original = load(path)
        snapshot = json.dumps(original, sort_keys=True)
        for name, mutate in MUTATIONS:
            case = copy.deepcopy(original)
            try:
                produced = mutate(case)
            except (KeyError, IndexError, TypeError, AssertionError):
                continue  # not applicable to this fixture's shape
            if produced is None:
                produced = case
            require(isinstance(produced, dict),
                    f'mutation produced a non-case: {name}')
            # A mutation that changed nothing has not exercised anything. Without
            # this check an in-place mutation returning None would look identical
            # to one that deliberately declined to apply.
            if json.dumps(produced, sort_keys=True) == snapshot:
                continue
            exercised[name] += 1
            try:
                validate(produced)
            except (ValueError, KeyError, TypeError, IndexError, AssertionError):
                continue
            survivors.append(f'{name} [{path.stem}]')

    never = sorted(name for name, hits in exercised.items() if hits == 0)
    if survivors or never:
        if survivors:
            print('self-test FAILED: corruptions were accepted:', file=sys.stderr)
            for item in survivors:
                print(f'  accepted: {item}', file=sys.stderr)
        if never:
            print('self-test FAILED: mutations that never applied:', file=sys.stderr)
            for name in never:
                print(f'  never applied: {name}', file=sys.stderr)
        return 1

    total = sum(exercised.values())
    print(f'self-test: {len(MUTATIONS)} corruption kinds applied {total} times across '
          f'{len(paths)} fixtures; all rejected.')
    print('Corpus clean. This proves the validator rejects malformed fixtures, not '
          'that the engine passes any probe.')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('directory', nargs='?', type=Path, default=FIXTURES)
    parser.add_argument('--self-test', action='store_true',
                        help='confirm this validator rejects malformed fixtures')
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    count, (operations, tokens, specs) = validate_corpus(args.directory)
    print(f'{count} consumer-probe fixtures: {operations} operations, {tokens} tokens, '
          f'{specs} typed expectations; WP1-WP8 covered; exact oracles valid.')
    print('These fixtures specify cases. They are not evidence the engine passes them.')
    return 0


if __name__ == '__main__':
    sys.exit(main())