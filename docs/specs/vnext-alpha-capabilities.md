# Position capability staging

Status after native S04b implementation: scoped live boundaries, strict attachments,
list elements and CTPT version 1 codec/replay are implemented. Acceptance results
are recorded in TASKS.json. The installed authoritative capability manifest is
`libs/collabtext/include/collabtext/ALPHA_CAPABILITIES.md` version 5. No alpha has
been published and no persisted token version has been approved for release.

| Obligation | Evidence now / implementation owner |
|---|---|
| Exact affinity, scoped empty/end and units | S04a model and native S04b exact/differential checks |
| Caret collapse and strict referent restoration | Native deletion/undo/redo and competing-delete checks |
| Scope rejection and missing list-value lookup | Native scoped typed API and full-width/duplicate list-value cases |
| Binary token version / codec / replay process | Native CTPT v1 goldens, strict decoder and fresh-process original-token replay |
| Installed strict probe pass | W03 after S04b; no pass claimed now |
| Full repaired sequence ordering | S02 model and S03 integration remain pending |
| Actual file/session reopen, persistent undo | L06/L08 and S04 remain pending |
| Durable external pins, checkpoint translation, physical GC | S05/G-series remain pending; physical GC disabled |

Token decoding and resolution create no retention obligation automatically. A
future supported persisted-token version is frozen only with actual codec/golden
reader evidence and the approved alpha manifest. Core remains C++20, Qt-free,
plaintext/list and transport-agnostic; Qt conversion and Wave JSON wrapping remain
adapter responsibilities. No external release or consumer changes are made here.
