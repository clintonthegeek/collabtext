# Position capability staging

Status after S04a: a scoped boundary specification and an independent executable
model exist. They are design evidence, not implemented public positions. The
installed authoritative manifest remains
`libs/collabtext/include/collabtext/ALPHA_CAPABILITIES.md`; S04a does not promote
any of its runtime capabilities.

| Obligation | Evidence now / implementation owner |
|---|---|
| Exact affinity, scoped empty/end and units | S04a supplied-order model; production S04b |
| Caret collapse and strict referent restoration | S04a exact fixtures/model; production S04b |
| Scope rejection and missing list-value lookup | S04a model plus existing typed legacy list miss; full scoped API S04b |
| Binary token version / codec / replay process | Candidate format specified; implementation S04b |
| Installed strict probe pass | W03 after S04b; no pass claimed now |
| Full repaired sequence ordering | S02 model and S03 integration remain pending |
| Actual file/session reopen, persistent undo | L06/L08 and S04 remain pending |
| Durable external pins, checkpoint translation, physical GC | S05/G-series remain pending; physical GC disabled |

Token decoding and resolution create no retention obligation automatically. A
future supported persisted-token version is frozen only with actual codec/golden
reader evidence and the approved alpha manifest. Core remains C++20, Qt-free,
plaintext/list and transport-agnostic; Qt conversion and Wave JSON wrapping remain
adapter responsibilities. No external release or consumer changes are made here.
