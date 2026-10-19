# collabtext

A native C++20 CRDT for collaborative text and ordered structural lists. Text
and IdList share FugueMax sequence ordering. Operations and scoped positions
are opaque public values; consumers own authorization and transport.

The reliability engine line is merged into `master` and tagged
`collabtext-integration-2026-10-19`; the package target is `0.2.0-alpha.5`
(capability manifest v17). Read the
[consumer handoff](docs/handoff/consumer-adoption.md) and
[Wave Core response](docs/handoff/wave-core-response.md) before integration.
Earlier candidate tags are preserved and immutable; the integration tree is
byte-identical to the `collabtext-markoff-core-2026-10-13-alpha5` candidate for
every library, test, fixture and packaging path.

## Use the installed library

[USING_VNEXT.md](docs/USING_VNEXT.md) gives producer/install/consumer commands.
Link `CollabText::Core` for dependency-free text and IdList. Optional
`FileTransport`, `QtAdapter` and `Widgets` add file-backed sessions and Qt UI.
Use installed public headers, never engine/tree/payload headers in `src/`.
The [installed capability manifest](libs/collabtext/include/collabtext/ALPHA_CAPABILITIES.md)
separates implemented capabilities, storage promises and remaining certification.

Core supports checked UTF-16 text edits, scoped UTF-8 boundaries and strict
character attachments, typed zero-width text markers, causal remote outcomes,
compensating undo/redo, bounded consumer-owned undo snapshots and deterministic
disposable projection allocators. Wave keeps its canonical EventStore; Core
requires no second log. Text deletion preserves markers; explicit marker
removal detaches them. Marker tokens survive fresh replay and undo restoration.

CTBO/0002 text/undo and CTBO/0003 marker operations have indefinite archive-reader
obligations. CTPT/1 and CTPT/2 marker tokens have supported persisted readers.
C++ ABI remains alpha: pin the exact candidate SHA and rebuild. Cross-version
store/checkpoint upgrades have separate validation obligations. Initial consumer
adoption retains history and disables physical collection/log pruning.

## Develop and verify

Read [AGENTS.md](AGENTS.md), then the roadmap's
[DIRECTION.md](docs/roadmap/2026-10-reliability/DIRECTION.md).
[TASKS.json](docs/roadmap/2026-10-reliability/TASKS.json) owns exact task status;
[EXECUTION.md](docs/roadmap/2026-10-reliability/EXECUTION.md) defines verification.

```sh
./tools/verify-changed.sh --all --mode debug
./tools/verify-consumer-package.sh --mode release --gate live --components core
```

Debug, Release, leak-enabled ASan/UBSan and relocated installed consumers are
acceptance gates. Empty selections and unknown verification routes fail.
Benchmarks are opt-in standalone tools, separate from correctness tests.
Vector/tree rebuild edits remain linear; the candidate makes no logarithmic
editing or competitive performance claim. Further measured optimization has
explicit X-series tasks and independent ordering oracles.

Historical locator-era specs and discovery reports remain in `docs/` for
provenance. Current public contracts are the vNext specs supplemented by
[wave-core-adoption.md](docs/specs/wave-core-adoption.md). They supersede the
historical API examples and earlier capability limits.

Current Markoff adoption: [ordered list identities, durable counter gaps and CTLO readers](docs/handoff/markoff-core-response.md). Alpha.4 candidate `collabtext-markoff-core-2026-10-11` remains immutable; alpha.5 adds selective local-history group compensation.
