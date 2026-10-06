# Position capability staging

Status after native S04b implementation: scoped live boundaries, strict attachments,
list elements and CTPT version 1 codec/replay are implemented. Acceptance results
are recorded in TASKS.json. The installed authoritative capability manifest is
`libs/collabtext/include/collabtext/ALPHA_CAPABILITIES.md` version 7. No alpha has
been published and no persisted token version has been approved for release.

| Obligation | Evidence now / implementation owner |
|---|---|
| Exact affinity, scoped empty/end and units | S04a model and native S04b exact/differential checks |
| Caret collapse and strict referent restoration | Native deletion/undo/redo and competing-delete checks |
| Scope rejection and missing list-value lookup | Native scoped typed API and full-width/duplicate list-value cases |
| Binary token version / codec / replay process | Native CTPT v1 goldens, strict decoder and fresh-process original-token replay |
| Installed baseline probe pass | W02: all eight probes through an installed-only consumer map; exact WP6 known failure and WP8 unsupported pins remain explicit |
| Installed strict probe pass | W03: WP1-WP5/WP7 plus the no-GC containment smoke, strict in Debug/Release/ASan through an installed consumer block map, plus a second independent Markdown consumer (exact bytes, optional transport routing, caret collapse apart from strict loss) |
| Concurrent-paste and concurrent-typing ordering | applied in S03: FugueMax placement; every author's run stays contiguous, forward and reverse (docs/specs/vnext-sequence-algorithm.md §0) |
| Actual file/session reopen, persistent undo | L06/L08 and S04 remain pending |
| Durable external pins, checkpoint translation, physical GC | S05/G-series remain pending; physical GC disabled |

Token decoding and resolution create no retention obligation automatically. A
future supported persisted-token version is frozen only with actual codec/golden
reader evidence and the approved alpha manifest. Core remains C++20, Qt-free,
plaintext/list and transport-agnostic; Qt conversion and Wave JSON wrapping remain
adapter responsibilities. No external release or consumer changes are made here.
