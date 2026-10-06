# Position capability staging

Status after E07a: package alpha candidate `0.2.0-alpha.1` is **prepared, not
published**. Scoped live boundaries, strict attachments, list elements and the
CTPT version 1 codec/replay are implemented. Acceptance results are recorded in
TASKS.json. The installed authoritative capability manifest is
`libs/collabtext/include/collabtext/ALPHA_CAPABILITIES.md` version 8, which now
also carries the serialized-format table. No alpha has been published and **no
persisted token version has been approved for release**; that decision is E07b's.

| Obligation | Evidence now / implementation owner |
|---|---|
| Exact affinity, scoped empty/end and units | S04a model and native S04b exact/differential checks |
| Caret collapse and strict referent restoration | Native deletion/undo/redo and competing-delete checks |
| Scope rejection and missing list-value lookup | Native scoped typed API and full-width/duplicate list-value cases |
| Binary token version / codec / replay process | Native CTPT v1 goldens, strict decoder and fresh-process original-token replay. **Experimental; not approved for persistence.** |
| Installed baseline probe pass | W02: all eight probes through an installed-only consumer map. The pre-S03 WP6 known failure is gone; only WP8 durable pins remain `unsupported` |
| Installed strict probe pass | W03: WP1-WP5/WP7 plus the no-GC containment smoke, strict in Debug/Release/ASan through an installed consumer block map, plus a second independent Markdown consumer (exact bytes, optional transport routing, caret collapse apart from strict loss) |
| Concurrent-paste and concurrent-typing ordering | applied in S03: FugueMax placement; every author's run stays contiguous, forward and reverse (docs/specs/vnext-sequence-algorithm.md §0) |
| Package alpha candidate prepared | E07a: `tools/prepare-integration-alpha.sh`, version metadata in both `project()` calls, `docs/releases/integration-alpha.md`, pin-by-SHA instructions, checksummed staged artifacts, unexecuted publication checklist |
| Actual file/session reopen, persistent undo | L06/L08 and S04 remain pending |
| Durable external pins, checkpoint translation, physical GC | S05/G-series remain pending; physical GC disabled |

Token decoding and resolution create no retention obligation automatically. A
future supported persisted-token version is frozen only with actual codec/golden
reader evidence and the approved alpha manifest. Core remains C++20, Qt-free,
plaintext/list and transport-agnostic; Qt conversion and Wave JSON wrapping remain
adapter responsibilities. No external release or consumer changes are made here.

The wire versions this candidate claims (`CTBO/0002`, `CTLO/0002`, `CTGV/0001`,
`CTGP/0001`, `CTPT/1`) are checked against the codecs by
`tools/prepare-integration-alpha.sh`, so a release note cannot drift away from the
code it describes without failing the preparation step. Note that
`docs/specs/vnext-operation-codec.md` still describes the pre-bump version 1 frame
table; E04 owns that correction and the codecs are the authority.
