# Wave-contract probe harness (W01)

Roadmap task **W01**. This directory holds the *harness contract* for the eight
consumer probes. It is a contract, not a runnable check.

## What is here

- `probe_contract.h` — the typed vocabulary the probes are stated in: probe ids,
  offset units, attachment kinds, resolution states, typed rejection codes, the
  `pass`/`known_failure`/`unsupported`/`error` outcome set, and one recorded case.

## What is deliberately not here

No `main`, no engine call, no assertion that passes. W01 **specifies cases**; it
supplies no evidence that collabtext passes any of them, and a green run here
before an adapter exists would be the exact failure this roadmap keeps auditing
for. The fixtures these cases are stated in live in
`libs/collabtext/tests/fixtures/external-position/` and are checked by
`python3 tools/validate-consumer-fixtures.py`.

Two rules are encoded in the type rather than left to review:

- `PositionResolution::well_formed()` rejects a resolution that carries both an
  offset and an error, or neither. `Resolved` with no offset is how "relocate to
  end-of-document" becomes typable by accident.
- `Collapsed` is distinct from `Resolved`, so a boundary that moved because its
  surroundings moved has to say so.

## Who implements it

| Task | What it adds |
|---|---|
| **W02** | The consumer-owned `Document` adapter: block-id map, UTF-8 body access, local/remote operation routing, capability-aware forwarding. Runs all eight probes as a **baseline**, where `known_failure` and `unsupported` are recorded honestly and neither is treated as passing. |
| **S04a/S04b** | The live public API behind it: scoped minting, affinity, typed resolution, the token codec, and the unresolved `design_pending` collapse rule. |
| **W03** | Strict mode: WP1–WP5 and WP7 must pass, run against the *installed* package, with wrong-object and malformed-token cases in isolated child processes where any signal is an `error`. |

WP6 stays `known_failure` until S03 repairs ordering, and WP8's no-GC case stays
a baseline-only observation until S05/G06/G08 provide durable retention. Neither
may be reported as certified.

## Why the vocabulary is fixed here

Each of these strings already appears in a checked-in fixture, and the fixture
validator rejects a malformed one. Fixing them in one header means W02, S04b and
W03 cannot each invent their own spelling of "wrong context" or "collapsed" and
then compare against a fixture that says something else.

## Provenance

The scenarios are the consumer's; the identifiers, typed results and ownership
are ours. See
[`docs/specs/vnext-consumer-probes.md`](../../../docs/specs/vnext-consumer-probes.md)
and
[`docs/reports/vnext-consumer-probe-provenance.md`](../../../docs/reports/vnext-consumer-probe-provenance.md).
In particular, the harnesses the consumer's report cites could not be located, so
no third-party figure in those fixtures was reproduced here.