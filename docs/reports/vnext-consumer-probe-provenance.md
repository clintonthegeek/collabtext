# W01 provenance: the eight consumer probes

**Task:** W01. **Date:** 2026-10-05. **Engine evaluated:** collabtext `046d375`
(`master`). **Reliability work:** branch `roadmap/reliability-v2`, unmerged.

This report records where every probe expectation came from, and — the part that
matters more — **what could not be verified**. Requirements are in
[`vnext-consumer-probes.md`](../specs/vnext-consumer-probes.md); fixtures are in
`libs/collabtext/tests/fixtures/external-position/`.

## 1. Source documents

| Path | Date | Standing |
|---|---|---|
| `wave/docs/handoff/2026-10-04-collabtext-devs-our-asks.md` | 2026-10-04 | **Superseded** by ask2, but the source of requirements R1–R6 |
| `wave/docs/handoff/2026-10-04-collabtext-ask2-sequencing-and-probes.md` | 2026-10-04 | Current. Source of the eight probe scenarios |
| `wave/docs/reference-notes/reports/2026-10-04-crdt-alternatives-automerge-yrs-loro.md` | 2026-10-04 | Current. Third-party measurements |
| `wave/docs/specs/0001-wave-0.1-protocol.md` | — | Normative Document/Position obligations (§5.2) |
| `Markoff/docs/handoff/2026-05-08-collabtext-joint-design-positions.md` | 2026-05-08 | Position requirements, **not** the joint-design outcome |

All paths are under `/home/clinton/dev/`. These are read-only research inputs.
No fixture, test or tool in this repository requires them: the corpus is
self-contained and validated offline.

## 2. The probe identifiers are ours

The consumer numbers its probes **1–8** in a single table. `WP1`–`WP8` are our
permanent ids, introduced by the early-consumer-lane amendment so a W02/W03/E05
result matrix can name a case across revisions. The mapping is fixed in
`vnext-consumer-probes.md` §0.

A second distinction matters just as much: in the consumer's table, the column
beside each scenario is titled **"What it found"**, and every entry in it is an
observation about **Automerge, yrs or Loro**. Those are third-party measurements.
They are reproduced in our fixtures as attributed historical observations and are
**never** expected results for collabtext.

## 3. What the consumer did *not* state

These are recorded because filling them in silently is how a fixture ends up
citing a requirement that does not exist.

| Item | Status |
|---|---|
| Offset units for collabtext positions (byte / scalar / UTF-16) | **NOT STATED.** No external document specifies a unit. W2 fixes UTF-8 bytes on scalar boundaries for text, element indices for lists. This is our decision. |
| WP1's "exhaustive sweep" corpus, index set, scenarios | **NOT STATED.** The scenario says only "every base text × every anchor index × several scenarios". We define the corpus. |
| WP5 expected results | **NOT STATED.** ask2:204-205 offers "the exact expected results we need" and never writes them. W1/W2 supply the contract. |
| WP6's intended merged result | **NOT STATED.** Only the defect. Contiguity-per-passage is our inference from the S01 histories. |
| WP8 expected results | **NOT STATED.** Three questions, no answers. No external document requires a pin API — the consumer *asks whether one exists*. |
| WP2 collapse placement; whether undo restores a referent | **NOT STATED.** Tagged `design_pending`, owner S04a. We did not invent a fallback offset. |
| WP3: engine-side or consumer-side block check | **NOT STATED.** The split we assign is our inference, recorded as such. |
| WP2/WP7 error type names | **NOT STATED.** Only "typed error, not abort or fabricate a plausible number". |
| Yjs version | **NOT STATED** in any of the four documents. |
| Licences | No licence is named in any of the four documents. GPLv3 for collabtext appears only in the consumer's comparison sentence (`alternatives:119-120`); `LICENSE.txt` in this repository is GPLv3. |

## 4. Artifacts that could not be verified

The consumer's report states its harnesses are "under `/tmp/opencode/`" and cites
exact source locations. **Those harnesses were not found.**

| Referenced artifact | Cited at | Status |
|---|---|---|
| Probe harnesses under `/tmp/opencode/` | `alternatives:13` | **NOT FOUND.** The directory exists and is large, but contains no identifiable Automerge/yrs/Loro probe harness. `probe.cpp`/`probe2.cpp`/`probe3.cpp` there are unrelated SQLite/GTFS timing code. |
| `loro-internal/src/state.rs:2122-2152` | `alternatives:45-51` | **CANNOT BE VERIFIED** — file absent. |
| `crates/loro/tests/contracts/cursor_recovery.rs:164-171` | `alternatives:45-51` | **CANNOT BE VERIFIED** — file absent. |
| yrs source / version `0.28` | `alternatives:35` | **UNVERIFIABLE.** Only a vendored `libyrs.h` exists; no checkout, no `Cargo.toml`, no version string. |
| Automerge source | — | **PARTIAL.** `/tmp/opencode/am-src/` with `LICENSE` (MIT) and `rust/Cargo.toml`. Version fields unread. |
| Loro source / version `1.16.2`, `loro.js@0.3.0` | `alternatives:34`, `:67` | **UNVERIFIABLE.** Only extracted snippets (`loro_panic_test.rs`, `loro_cursor_recovery.rs`, …). No checkout, no `Cargo.toml`. |

**Consequence, stated plainly:** every third-party figure in our fixtures —
400/400, 1000/1000, 256/256, the 84-case sweep, exit 134 — is the consumer's
reported observation, carried with `attribution:
reported_by_wave_not_reproduced_here`. None was re-measured, and the fixture
validator *rejects* an observation that does not carry that attribution.

One path the consumer cites is wrong in a way worth recording: it points at
`docs/reports/2026-10-03-evaluation-artifacts/probes.cpp` twice, relative to
collabtext rather than to wave. That file exists and contains **collabtext's own**
probes — 13 blocks, of which exactly one (the `ab`/`X` affinity case) is probe 1
of the eight. A fixture author following that citation would find one block and
stop.

## 5. Unverified citations into our own source

The consumer's documents cite collabtext source lines: `Buffer.cpp:347-365`,
`:367-393`, `:371`, `:392`, `IdList.cpp:283`, `:295`, `:40`, `Operations.h:27-35`.
These describe revision `046d375`. They were **not** re-verified against that
revision during W01, and the line numbers may have drifted since. They are treated
as consumer-asserted descriptions of a defect, not as verified references. The
defects themselves are independently recorded as `F7` (anchor affinity) and `F8`
(concurrent interleave) in `docs/2026-10-03-library-evaluation.md`, which is
present in this repository.

## 6. Adapter assumptions we recorded

- The consumer owns the canonical log, block identity, permissions, formatting and
  the block map; the engine sees one text `Buffer` per block and an optional
  `IdList`, each mapped to an explicit document/stream identity.
- The consumer will **not** call `collect_garbage()` / `compact()` while F1/F2/F3
  stand, and will revisit if the G-series certifies them. WP8's no-GC case
  reflects that policy; it cannot certify retention.
- Tokens are expected to survive engine restart and re-resolve against a changed
  document (WP5), stored inside the consumer's own versioned envelope.
- The consumer implements against a `Document`-shaped seam, not `Buffer`, and has
  offered to write its own adapter.
- IdList payloads are `uint64` while consumer block ids may be strings; a
  collision-checked canonical mapping is required. Truncating or hashing an id
  into the list is forbidden (W5).

## 7. What was copied, and what was not

**Copied into this repository:** nothing verbatim. The scenarios are short enough
to restate, and each fixture records its own citation, so a reader can check the
claim without the original file.

**Not copied:** the consumer harnesses — they do not exist to copy. No fixture
requires `/tmp`, a sibling checkout, a network download, or another engine's
toolchain. `tools/validate-consumer-fixtures.py --self-test` and a corpus
validation run offline, which is the point: the alternative report's evidence is
the clearest example of a claim that cannot be re-checked, and repeating that
shape here would have imported the problem.

## 8. Verification performed for W01

```sh
python3 tools/validate-consumer-fixtures.py --self-test
python3 tools/validate-consumer-fixtures.py
```

- 14 fixtures, 32 operations, 28 tokens, 58 typed expectations, WP1–WP8 covered.
- 31 corruption kinds applied 351 times across the corpus; **all rejected**.
- The corpus was validated twice and the normalized histories (seed, order,
  hidden set, arrival orders, positions) compared: identical. Every fixture pins
  seed `20261005`.
- The self-test was itself checked for the ability to fail: disabling the
  causality rule made it report the corruptions it had been accepting, and
  restoring the rule returned it to green.

This is evidence about the **fixtures and the validator**. It is not evidence
that collabtext passes any probe.