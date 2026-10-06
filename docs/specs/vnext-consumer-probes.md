# The eight consumer probes: typed fixtures for WP1–WP8

**Task:** W01. **Status:** specification and fixtures only — **not** evidence that
the engine passes any probe. **Binding requirements:**
[W1–W6](vnext-external-position-contract.md). **Task lane:**
[early consumer integration](../roadmap/2026-10-reliability/00-consumer-integration.md).

Machine-readable fixtures live in
`libs/collabtext/tests/fixtures/external-position/` and are checked by
`tools/validate-consumer-fixtures.py`. Provenance, including artifacts that could
not be verified, is in
[the provenance report](../reports/vnext-consumer-probe-provenance.md).

## 0. What these labels are, and what they are not

The consumer numbers its probes **1–8** in one table. `WP1`–`WP8` are **our**
permanent identifiers, assigned in the early-consumer-lane amendment and reused
by W02/W03/E05 so a result matrix can name a case across revisions. The mapping
is fixed by this document and must not be renumbered.

Two distinctions in this document are load-bearing, and the fixtures carry them
as machine-readable tags rather than prose:

- **`consumer_asserted` / `wave_protocol_normative`** — an expectation the
  consumer stated, cited to a file and line.
- **`roadmap_inference`** — an expectation *we* derived from W1–W6 because the
  consumer did not state one. These are legitimate contract requirements, but
  attributing them to the consumer would be a fabricated citation.

A third tag, **`third_party_observation`**, is attached to measurements of
Automerge, yrs, Loro and Yjs. Those are the consumer's observations of *other
engines*. They are **never** expected results for collabtext and are never
satisfied by a collabtext run.

## 1. Result vocabulary

W02 records one machine-readable outcome per case. W01 defines the vocabulary
so a later task cannot quietly invent a passing state:

| Outcome | Meaning | Counts as feature evidence? |
|---|---|---|
| `pass` | The named assertion held. | Yes |
| `known_failure` | The exact defect the allowlist names reproduced. | **No** |
| `unsupported` | The capability does not exist in this build. | **No** — capability information |
| `error` | Abort, signal, assertion failure, malformed result, or mutation. | No, and **fatal** |

`unsupported` and `known_failure` are how an honest baseline stays green. Neither
satisfies a strict gate, and a suite that reports zero selected cases is an
`error`, never a pass.

## 2. Offsets, identity, and scope

W2 fixes the units, and they are **our** decision — no external document states
them (`NOT_STATED_EXTERNALLY`, provenance report §3):

- **Text offsets are UTF-8 bytes on Unicode scalar boundaries.** An offset that
  lands inside a multi-byte sequence, or between the two halves of a surrogate
  pair in UTF-16 terms, is invalid input, not a nearby offset.
- **List positions are element indices**, distinct from byte offsets. A list
  index is never reinterpreted as a text offset.
- Both are distinct from **Qt UTF-16 positions**, which a Qt adapter must convert
  explicitly. Silent reinterpretation is a defect.

Every token is scoped to `(document, stream, primitive, epoch)`. A token resolved
against a different member of that tuple is a **typed error**, never a value
(W2, `consumer_asserted`).

## 3. WP1 — Affinity under insertion

**Consumer scenario** (`wave ask2:139`, verbatim): *"`ab"`, anchor at 1, insert
`"X"` at 1. Are `Left` and `Right` distinguishable?"*

**Expectation** (`wave devs-our-asks:52-53`, `consumer_asserted`): a boundary at
offset 1 in `ab`, with `X` inserted at 1, resolves **left → 1**, **right → 2**,
giving `aXb`. Today both resolve to 2 (`F7`).

The consumer also asked for "an exhaustive sweep" but **did not name its corpus,
index set, or scenarios** (`NOT_STATED_EXTERNALLY`). We therefore fix the sweep
ourselves, and the fixture set is the specification:

- every scalar boundary of every corpus text, in both affinities;
- empty text, single scalar, start, end, and end-of-concurrent-insert;
- `shared_token_concurrent_boundary_inserts`: the **same minted token** resolved
  on both replicas under every arrival order (W2 — convergence compares one
  token, not two separately minted ones).

The case name `boundary_affinity_ab_insert_X` is the contract's named case and
is implemented exactly.

## 4. WP2 — Deleted referent, by attachment kind

**Consumer scenario** (`wave ask2:140`): anchor mid-text, delete the anchored
character, 400+ trials, report the ratio of typed failure to silent relocation.

**Consumer requirement** (`wave devs-our-asks:32`, `consumer_asserted`, R3):
resolution failure must be **detectable and reported, never silent**.
"Silent relocation to end-of-document is data corruption in a shared document."

The consumer did **not** specify collapse placement, a fallback offset, or
whether undo restores a referent (`NOT_STATED_EXTERNALLY`). W2 supplies the rule,
and it is deliberately split by attachment kind — W2 forbids inferring universal
failure from every deletion (`00-consumer-integration.md`):

| Attachment kind | Requirement | Source |
|---|---|---|
| **Caret** (`caret`) | A surviving boundary resolves with an offset. Deleting a *neighbouring* character does not destroy a caret. | W2, `roadmap_inference` |
| **Caret, referent deleted** | Resolves **with an explicit `collapsed` marker** — a position plus the fact that collapse occurred. | W2 `consumer_asserted` (must report collapse) + exact placement S04a |
| **Strict content** (`strict`) | Surfaces `detached` or `unavailable`. **Never** a bare relocated integer. | W2, `consumer_asserted` |

**Resolved by S04a:** the exact fixture now mints an actual gap a/b before
b, selecting the following item for strict attachment. Deleting b yields caret
byte 1 with explicit collapse; strict resolution yields detached with no usable
offset. A valid selective undo restoring b makes the **same** token resolve at
byte 1; redo detaches. This replaces the pending placement/restoration entries.
The former a/c pair enclosed still-visible b and was not a caret gap.
[vnext-boundary-semantics.md](vnext-boundary-semantics.md) specifies the rules;
these choices are tagged roadmap_inference and model-checked, with the native
implementation complete in S04b and installed strict certification delivered by
W03 (WP1-WP5/WP7 plus the no-GC containment smoke). WP8's strict token likewise
now selects the actually deleted b, rather than the surviving a.

## 5. WP3 — Deleted containing block

**Consumer scenario** (`wave ask2:141`): "Deleted containing block. One level up."

**Normative outcome** (`wave 0001-protocol:666-667`, `wave_protocol_normative`):
a missing block resolves as **`Gone`**; an unknown non-empty token resolves as
**`Stale`** while retaining its original bytes.

**Ownership** (`roadmap_inference`, and it is a division of labour, not a shared
obligation): the **consumer** checks block existence in its own block map; the
**engine** independently enforces token scope before any item lookup and must
not assert or abort. The block-existence check is a Wave-side map lookup, not an
engine feature, and the engine has no business knowing a block exists.

## 6. WP4 — Scalar-boundary minting

**Consumer scenario** (`wave ask2:142`): anchor at `index == len`, and at
`index > len`.

**Consumer requirement** (`wave devs-our-asks:58-59`, `consumer_asserted`): a
reply attached to the end of a block needs a **real token** there, comparable
with tokens minted later — not the global `Anchor::max()` sentinel.

Required cases, all `consumer_asserted` as *requirements* with fixture-specified
exact values:

| Case | Expected |
|---|---|
| offset `0`, nonempty text | real scoped token |
| offset `== len`, nonempty text | real scoped token, **not** a sentinel |
| empty document, offset `0` | real scoped token with scope + affinity |
| offset `len + 1` | typed rejection (`off_end`) |
| offset inside a multi-byte scalar | typed rejection (`not_scalar_boundary`) |
| non-BMP text (e.g. U+1D11E) | token at each boundary; byte offsets, not UTF-16 |
| distinct list indices | typed element-index tokens; never byte offsets |

A token at end must compare consistently against a token minted later (W2).

## 7. WP5 — Token codec and round-trip

**Consumer scenario** (`wave ask2:143`): encode → persist → reopen in a fresh
process → remote edit → decode → resolve.

The consumer promised "the exact expected results we need" (`wave ask2:204-205`)
and **never wrote them** (`NOT_STATED_EXTERNALLY`). W1/W2 supply the contract:

| Case | Expected | Source |
|---|---|---|
| exact byte round-trip | `encode(decode(b)) == b`, canonically | W1, `roadmap_inference` |
| every one-byte trailing suffix | rejected, all 256 | W1 (explicit rejection), `roadmap_inference` on the exact 256 |
| truncation at every length | rejected | W1, `roadmap_inference` |
| unknown version byte | typed `unsupported_version` | W1, `consumer_asserted` |
| invalid enum / unsupported affinity | typed rejection | W1, `consumer_asserted` |
| foreign context | typed rejection, no mutation | W1 + W2, `consumer_asserted` |
| decode must not pin or mutate | document state unchanged after every rejection | W1, `consumer_asserted` |

Trailing-byte rejection is specified as **256 cases** because that is the number
the consumer measured against Loro (`wave ask2:143`), and it is the shape of
defect worth excluding. `decode` returning `optional`-style typed failure, never
an exception, is Markoff's stated preference and W1's requirement.

## 8. WP6 — Split-paste interleave

**Consumer scenario** (`wave ask2:144`): two peers paste a paragraph as multiple
inserts, interleaved.

The consumer stated the **defect and no target result** (`NOT_STATED_EXTERNALLY`).
Measured against collabtext `046d375`: `abc` + `XYZ` interleaves to `aXbYcZ` —
convergent but each passage destroyed (`F8`). The consumer classifies this as a
**quality** repair, not divergence.

**Expectation** (`roadmap_inference`, from the S01 histories and the task lane):
each pasted passage survives as a **contiguous run**. The consumer did not choose
a tie rule; S01 specifies the native actor/root tie for these fixtures, producing
exactly `abcXYZ`. The fixtures assert both that exact string and contiguity, tagged
as roadmap_inference. The full S02 interpreter must implement this chosen order.
Ownership: S01/S02/S03, with S06 enumerating the combined histories. W02/W03
report `known_failure` until S03 lands; **equality of final text alone is not
sufficient evidence.**

## 9. WP7 — Wrong-context resolution

**Consumer scenario** (`wave ask2:145`): resolve a token minted in one block
against a different block in the same document.

**Expectation** (`wave ask2:148-150`, `consumer_asserted`, and the consumer's
most emphatic requirement): **a typed error, never an abort and never a
fabricated plausible number.** The consumer names three ordinary bug causes — a
bad migration, a replay bug, a corrupted block id — and states a desktop abort is
worse than a detectable wrong answer.

Every axis of the scope tuple is a distinct case: wrong document, wrong stream,
wrong block, wrong primitive kind, and wrong epoch. Each expects a typed error,
**no state mutation, no assert, and no abort.** W03 runs these in isolated child
processes and records any signal as an `error`.

`missing_list_id_is_not_an_end_anchor` is the IdList half: a lookup miss returns a
typed not-found result, and `insert_after`/`remove_at` must refuse rather than
assert (`IdList.cpp:40`'s assert is compiled out under `NDEBUG`).

## 10. WP8 — Retention, pins, and reclamation

**Consumer scenario** (`wave ask2:146`): three **questions**, not one assertion —
what does `gc` do to stored positions, is there a retention or pin API, and what
happens with `gc: false`. Its only finding is "Directly informs W3"; there is
**no third-party data for this probe at all.**

Nearest binding requirement (`wave devs-our-asks:32`, R3, `consumer_asserted`):
anchors must survive GC **or** failure must be detectable and reported, never
silent. The consumer **asks whether** a pin API exists; it does **not** require
one (`NOT_STATED_EXTERNALLY`).

W3 supplies the contract, and its first clause is the one most easily got wrong:

> **Storing bytes in an external log is not notification that the engine must
> retain their referents.**

So the no-GC baseline case asserts only that reclamation is **disabled** and
containment remains active. It **cannot** count as certified retention, and a
stored token is **not** a pin. Durable pin registration, release, recovery,
device-retirement survival, and epoch translation are owned by S05 and
G01/G06/G08, gated on `external_pin_recovery_before_gc` and
`retire_device_preserves_live_reply_pin`.

## 11. Ownership

| Probe | Early executable owner | Strict / final owner |
|---|---|---|
| WP1 | S04a model, S04b production, W03 installed | S04, S06 after order + recovery |
| WP2 | S04a/S04b, W03 | S04 recovery, S05 retention + undo policy |
| WP3 | W02 adapter baseline | W03 installed strict gate |
| WP4 | S04a/S04b, W03 | S04 after recovery |
| WP5 | S04b/W03 codec + fresh-process replay | S04 store lifecycle; S05/G06/G08 byte preservation |
| WP6 | S01/S02/S03, S06 | W02/W03 report `known_failure` until repaired |
| WP7 | S04a/S04b, W03 | S04; isolated workers in W03 |
| WP8 | W02 no-GC baseline only | S05/G01/G06/G08; E05 final installed run |

## 12. What W01 does not establish

This task **specifies cases**. It supplies no engine change, no anchor or
ordering behaviour, and no evidence that any probe passes. In particular:

- S04a has settled the pending offset/restoration rules in §4 with an independent
  model. Native behavior/token bytes are implemented by S04b; W03 certifies
  their installed adapter and fresh-process replay.
- No fixture here is an executable regression against production code. W02
  supplies the executable adapter and result matrix; W03 promotes WP1-WP5/WP7 to
  a strict installed gate and adds the fresh-process replay and the independent
  Markdown consumer.
- Third-party observations are attributed historical measurements, reproduced
  from the consumer's report. They are not re-measured here, and the harnesses
  the report cites could not be located (§ provenance report).