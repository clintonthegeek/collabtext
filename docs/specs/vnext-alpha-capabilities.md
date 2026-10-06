# Position capability staging

Status after E07b: package alpha candidate `0.2.0-alpha.2` is **prepared, not
published**. Scoped live boundaries, strict attachments, list elements and the
CTPT version 1 codec/replay are implemented. Acceptance results are recorded in
TASKS.json. The installed authoritative capability manifest is
`libs/collabtext/include/collabtext/ALPHA_CAPABILITIES.md` version 9, which now
also carries the serialized-format table. No alpha has been published. **E07b
declares `CTPT/1` a supported token version** — the reader manifest is below.

| Obligation | Evidence now / implementation owner |
|---|---|
| Exact affinity, scoped empty/end and units | S04a model and native S04b exact/differential checks |
| Caret collapse and strict referent restoration | Native deletion/undo/redo and competing-delete checks |
| Scope rejection and missing list-value lookup | Native scoped typed API and full-width/duplicate list-value cases |
| Binary token version / codec / replay process | Native CTPT v1 goldens, strict decoder and fresh-process original-token replay |
| **Supported token version, retained bytes, reader fixture** | **E07b**: `tests/fixtures/position-tokens/ctpt-v1-supported.json` (16 cases, all four kinds) and `tst_position_token_reader` — independent literal §4 reader, retention round-trip, exhaustive truncation/trailing-byte/negative-version coverage, and per-case replay against the current engines |
| Installed baseline probe pass | W02: all eight probes through an installed-only consumer map. The pre-S03 WP6 known failure is gone; only WP8 durable pins remain `unsupported` |
| Installed strict probe pass | W03: WP1-WP5/WP7 plus the no-GC containment smoke, strict in Debug/Release/ASan through an installed consumer block map, plus a second independent Markdown consumer (exact bytes, optional transport routing, caret collapse apart from strict loss) |
| Concurrent-paste and concurrent-typing ordering | applied in S03: FugueMax placement; every author's run stays contiguous, forward and reverse (docs/specs/vnext-sequence-algorithm.md §0) |
| Package alpha candidate prepared | E07a: `tools/prepare-integration-alpha.sh`, version metadata, `docs/releases/integration-alpha.md`, pin-by-SHA instructions, checksummed staged artifacts, unexecuted publication checklist |
| Anchors across a durable reopen | S04 pending L08; the token survives engine destruction and fresh-process replay, which is not a recovered store |
| Actual file/session reopen, persistent undo | L06/L08 and S04 remain pending |
| Durable external pins, checkpoint translation, physical GC | S05/G-series remain pending; physical GC disabled |

## Supported token reader manifest

Declared by E07b. This is the whole obligation; it is short because the format
is one self-contained record with no optional sections.

**`CTPT` version 1, binary.** First supported in `0.2.0-alpha.2`. Layout is
`docs/specs/vnext-boundary-semantics.md` §4 and is unchanged.

A later supported release must, for stored version-1 bytes:

1. keep a reader for this layout, or provide a checked translation from it to the
   layout it replaces;
2. keep resolving the same logical position for the same document, stream, epoch,
   kind, affinity and referents — the stored bytes carry meaning, not layout;
3. report a stored version it cannot read as `unsupported_version`, which is
   already distinguishable from `malformed`;
4. never remint a stored token to make a resolve succeed.

What a supported version does **not** buy a consumer, and must not be read as:

- **No pin.** Storing bytes creates no retention obligation. Physical GC is
  disabled, so nothing is reclaimed and nothing can go missing yet.
- **No reclamation survival.** When S05/G06/G08 enable physical GC, a live
  registered pin must keep its referent, or a boundary stub / checked translation
  must keep the original bytes resolving. That work does not exist.
- **No epoch translation.** A token minted in another epoch reports `wrong_epoch`.
  G01/G02 own epoch membership and translation.
- **No store recovery.** The token survives engine destruction and fresh-process
  replay of operations the consumer itself retained. L05/L08 own recovered
  stores, and S04 certifies the composition.
- **No reset survival.** `local_clear()` invalidates position history, including
  for empty-edge tokens; stored bytes then report `history_unavailable`.

**Representation evidence.** The token's fields are logical — scope strings, kind,
affinity, attachment selection, two fences of item identities, a referent for
elements, and a mint frontier of integrated heads. No tree link, side, parent,
right origin or fractional locator appears in any field. `AnchorCodec.cpp` was
written by S04b and is byte-identical before and after S03's placement repair,
which changed `CTBO`/`CTLO` to version 0002 and left the token untouched; the
S04b golden bytes still assert unchanged. `tst_position_token_reader` keeps both
facts executable.

**Change policy.** A change to the meaning of stored version-1 bytes is a new
version with a translation, not a different version 1. Alpha C++ API/ABI
instability does not excuse reinterpreting stored tokens; the reader obligation
outlives the API.

**Not covered.** No pre-vNext or earlier experimental token is supported, and
none will be converted.

Token decoding and resolution create no retention obligation automatically. Core
remains C++20, Qt-free, plaintext/list and transport-agnostic; Qt conversion and
Wave JSON wrapping remain adapter responsibilities. No external release or
consumer changes are made here.

The wire versions this candidate claims (`CTBO/0002`, `CTLO/0002`, `CTGV/0001`,
`CTGP/0001`, `CTPT/1`) are checked against the codecs by
`tools/prepare-integration-alpha.sh`, so a release note cannot drift away from
the code it describes without failing the preparation step. The supported token
declaration is checked against the retained fixture by the same script. Note that
`docs/specs/vnext-operation-codec.md` still describes the pre-bump version 1
frame table; E04 owns that correction and the codecs are the authority.
