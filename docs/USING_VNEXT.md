# Using the collabtext adoption snapshot

Start with the [Wave and Markoff adoption handoff](handoff/consumer-adoption.md).
It settles persistence ownership, coordinates, affinity, callbacks, failures,
initial lifecycle policy, and the gate for merging this branch into master.

The source branch is **`feat/fuguemax-placement`**, in
`.worktrees/reliability-v2`. The annotated adoption tag is
**`collabtext-adoption-2026-10-11`**. Pin its resolved commit SHA, rather than a
moving branch or the package label alone. This is an adoption snapshot; the
package still reports **`0.2.0-alpha.2`**, and its C++ API/ABI remains alpha.
A local tag must be distributed separately before other repositories can fetch it.

The [installed capability manifest](../libs/collabtext/include/collabtext/ALPHA_CAPABILITIES.md)
describes supported formats and component limits. `CTPT/1` position-token bytes
have a supported-reader obligation. Operation/history/store/checkpoint formats
remain candidate-bound without an equivalent cross-version storage commitment.

## Build and consume

A headless Core-only producer:

```sh
cmake -S . -B build-adoption-core \
  -DCMAKE_BUILD_TYPE=Release \
  -DCOLLABTEXT_BUILD_QT=OFF \
  -DCOLLABTEXT_BUILD_APPS=OFF \
  -DCOLLABTEXT_BUILD_TESTS=OFF \
  -DCOLLABTEXT_BUILD_BENCHMARKS=OFF \
  -DCOLLABTEXT_BUILD_FILE_TRANSPORT=OFF \
  -DCMAKE_INSTALL_PREFIX=/tmp/collabtext-adoption-install
cmake --build build-adoption-core -j 4
cmake --install build-adoption-core
```

Use the installed package's lowercase name:

```cmake
find_package(collabtext CONFIG REQUIRED COMPONENTS core)
target_link_libraries(my_consumer PRIVATE CollabText::Core)
```

| Component | Target | Dependency |
|---|---|---|
| `core` | `CollabText::Core` | C++20 |
| `file-transport` | `CollabText::FileTransport` | zstd |
| `qt` | `CollabText::QtAdapter`, `CollabText::Widgets` | Qt6 |

Enable optional producer components and request them explicitly in the consumer.
`DocumentSession` belongs to FileTransport. The installed `Core` exposes checked
text/list facades, opaque operations and positions, pin projection, and membership
coordination; consumers using it own durable allocation and persistence.
The internal concrete StreamSync/ReplicaAllocator and certificate-construction
headers are not installed. Do not add `src/` include paths to reach them.

Read `COLLABTEXT_VERSION_STRING` for the full package label. CMake's numeric
version is `0.2.0`, which does not distinguish alpha snapshots. Build only from the
pinned candidate and record its SHA beside the package label.

## Essential adoption rules

- Use checked `try_*` methods and inspect every encode/decode/apply result.
- Text-facade edits use UTF-16 units; scoped text positions and session edits use
  UTF-8 bytes at Unicode scalar boundaries. Centralize conversion.
- Obtain scoped engines with `create(allocator, PositionScope)` for durable
  positions. A session currently exposes legacy anchors, not that scoped API.
- Persist original binary `encode_position` bytes. A token is not a pin. A strict
  reference that reports `detached` must not be retargeted to a nearby character.
- Keep callbacks synchronous on an owner thread; schedule mutations after
  callback dispatch. Select one publisher for each operation.
- Core input acceptance is not a durable recovery point. Retain deferred/rejected
  envelopes; commit delivery only after recovery is guaranteed.
- Session replay and persistent text undo exist. Consumer-owned list undo uses
  history snapshots. Neither path automatically certifies an arbitrary external
  consumer's canonical history or transaction handling.
- Begin with retained history and physical reclamation disabled. Source-level
  certified GC/checkpoint/pin composition exists, but it requires validated
  consumer infrastructure and is not a turnkey installed GC service.

The handoff contains the detailed error table, consumer-specific guidance,
installed-test references, and the exact master merge checklist.

## Verification

The repository's relocated-package checks demonstrate the installed contracts;
the actual consumer integrations need their own acceptance suites as well.

```sh
./tools/verify-consumer-package.sh --mode release --gate package --components core,file-transport,qt
./tools/verify-consumer-package.sh --mode release --gate live --components core
```

[Installed smoke example](../tests/consumer-smoke/main.cpp),
[Wave-shaped adapter](../tests/consumer-smoke/wave-contract/Adapter.h), and
[Markoff-shaped adapter](../tests/consumer-smoke/markdown-contract/Markoff.h)
are executable references. Their volatile Core allocators are test helpers,
not production durability implementations.
