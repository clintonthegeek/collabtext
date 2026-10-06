# collabtext 0.2.0-alpha.1 — integration alpha

**Status: prepared, not published.** No tag exists on any remote, no package has
been published, `master` has not been merged or touched, and no consumer has been
contacted. Publication is a separate action requiring an explicit instruction, and
the checklist produced by the preparation script is deliberately left unexecuted.

This is an **alpha**, not a supported release and not a completed reliability
release. It does not have crash recovery, persistent undo/redo, durable pins, or
GC. What it can and cannot do is stated in the installed capability manifest
`<collabtext/ALPHA_CAPABILITIES.md>`, which is versioned separately from this
document and is the authority.

## Candidate

| | |
|---|---|
| Package version | `0.2.0-alpha.1` (SemVer prerelease) |
| **Candidate SHA** | _filled in by the preparation commit; see `docs/reports/vnext-task-log.md` (E07a) and the generated `alpha-candidate.json`_ |
| Branch | `feat/fuguemax-placement` |
| Proposed tag | `v0.2.0-alpha.1` — **not created** |
| Destination remote | recorded only (`origin`); this task contacted no remote |
| Capability manifest | version 8, installed beside the headers |
| Serialized formats present | `CTBO/0002`, `CTLO/0002`, `CTGV/0001`, `CTGP/0001`, `CTPT/1` |

### Why `0.2.0-alpha.1` and not `2.0.0`

The reliability workstream is internally called "v2", and its branch names carry
"reliability-v2". **That is a workstream label, not a package major version.**
`2.0.0` would advertise an API break nobody has evaluated for compatibility.

The break that actually happened is already a hard cut inside `0.x`: `Locator` was
removed, and the operation codec moved from version 1 to version 2. Version 1
frames are refused rather than read.

`0.2.0-alpha.1` is a prerelease and is therefore **older than `0.2.0`**. One
CMake consequence, stated precisely because it is easy to guess wrong:
`project(VERSION)` accepts only a numeric version, so the installed
`collabtext-config-version.cmake` reports `0.2.0` and **a `find_package` request
cannot name the prerelease label**. Ask for no version or for `0.2.0`; read
`COLLABTEXT_VERSION_STRING` from the installed config for the full label. Pin with
the commit SHA or the tag.

## What this candidate contains

Available, with executable evidence behind every line — the manifest names the
test for each:

- **Core**: persistent 128-bit actor identity, checked 64-bit counters, opaque
  immutable operations, and installed/exported component targets that need
  nothing but a C++20 compiler. No Qt, no zstd, no crypto provider.
- **Text (`Buffer`) and structural list (`IdList`)** local editing, read, undo and
  redo; per-operation outcomes (applied / duplicate / deferred / rejected) on the
  list facade.
- **Scoped positions**: immutable text boundaries and strict attachments, list
  boundaries and elements, exact affinity, explicit collapse, typed
  wrong-context failures, and a binary `CTPT/1` codec with strict decoding.
- **FugueMax passage order** (S03): concurrent runs stay contiguous for forward
  and reverse typing, and a peer editing inside a typed run does not split it.
- **The installed package**: relocatable install, negative discovery for Qt/zstd/
  OpenSSL, and independent consumers for both the Wave-shaped and Markdown-shaped
  probe fixtures.

### Explicitly unavailable in this candidate

Stated so a consumer designs around them rather than discovering them:

- **Anchors across a durable reopen.** Live positions work within a process and
  across fresh processes given retained operation bytes. They have not been
  certified under a real store recovery cycle — that is S04, pending the L-series.
- **Crash recovery, persistent undo/redo** — L02–L08, pending.
- **Durable external pins, checkpoints, GC.** `collect_garbage()` and `compact()`
  return zero and retain everything. Physical GC is disabled on purpose.
- **A supported persisted position token.** `CTPT/1` is real, versioned, and
  round-trips exactly, and it is still **experimental**. Do not store it as a
  durable reference, and do not substitute a stored tree link for it.
- **A supported store or checkpoint format.** None exists.

### Baseline probe results at this candidate

The eight consumer probes (WP1–WP8) run against the installed package through a
consumer-owned block map. At this candidate:

| Probe | Result |
|---|---|
| WP1 boundary affinity, all scalar boundaries, both affinities, two replicas | strict pass |
| WP2 caret collapse, strict referent detachment, undo/redo restoration | strict pass |
| WP3 deleted containing block (consumer-owned check) | strict pass |
| WP4 exact start/end/empty boundaries, off-end and non-scalar refusal | strict pass |
| WP5 token round-trip, all trailing suffixes/truncations, fresh-process replay | strict pass |
| WP6 concurrent paste/typing passage contiguity | strict pass (`abcXYZ`) |
| WP7 wrong document/stream/primitive/epoch | strict pass |
| WP8 no-GC containment smoke | pass |
| WP8 durable pin register/lookup/release | **`unsupported`** — owned by S05/G01/G06/G08 |

The pre-S03 WP6 known-failure exception has been removed. There is no known
failure in the baseline matrix at this candidate; the remaining gap is WP8's
durable pins, which is unimplemented rather than broken.

## Pinning this candidate

**A local worktree cannot be fetched.** If the candidate commit is not reachable
from a remote, no consumer can clone it, and pinning it by SHA will not help
them. Check reachability before telling anyone the SHA is usable:

```sh
git ls-remote origin | grep <candidate-sha>     # absent => not reachable
```

Once the branch is pushed, a **full commit SHA is a complete pin by itself** — no
tag is required, and a SHA never moves:

```sh
git clone --branch feat/fuguemax-placement <remote-url> collabtext
cd collabtext
git checkout <candidate-sha>      # the full 40-character SHA
```

After a separately authorized publication, the immutable annotated tag
`v0.2.0-alpha.1` pins the same commit. Prefer the SHA for reproducibility: a tag
is a name that can be (wrongly) moved, while a SHA cannot be moved without
rewriting history that every other clone would reject.

Never pin `master` for this line. The histories diverged at `046d375` and
`master` does not contain any of this.

## Build, install, and consume from a clean clone

Source accessibility requirements: a C++20 compiler and CMake ≥ 3.19 for Core. Qt6
(≥ 6.8) only for the `qt` component, zstd only for `file-transport`. Nothing from
the build machine's paths is recorded in the installation.

```sh
git clone --branch feat/fuguemax-placement <remote-url> collabtext
cd collabtext
git checkout <candidate-sha>

# Producer. Core needs nothing but the compiler.
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCOLLABTEXT_BUILD_TESTS=OFF \
    -DCOLLABTEXT_BUILD_APPS=OFF \
    -DCOLLABTEXT_BUILD_QT=OFF \
    -DCOLLABTEXT_BUILD_FILE_TRANSPORT=OFF
cmake --build build -j "$(nproc)"
cmake --install build --prefix /opt/collabtext
```

A consumer, in a **separate project outside the source tree**:

```sh
cmake -S my-consumer -B consumer-build -DCMAKE_PREFIX_PATH=/opt/collabtext
cmake --build consumer-build -j "$(nproc)"
```

```cmake
cmake_minimum_required(VERSION 3.21)
project(my_consumer CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CollabText CONFIG REQUIRED COMPONENTS core)
add_executable(my_consumer main.cpp)
target_link_libraries(my_consumer PRIVATE CollabText::Core)
```

Ask for no version, or for `0.2.0`. Read
`<collabtext/ALPHA_CAPABILITIES.md>` from the install prefix before designing
against anything.

## Running the probes against a candidate

The same script that produced the evidence is in the repository, and needs no
network, no sibling checkout and no temporary harness:

```sh
tools/verify-consumer-package.sh --mode debug   --gate package  --components core
tools/verify-consumer-package.sh --mode release --gate package  --components core
tools/verify-consumer-package.sh --mode asan    --gate package  --components core
tools/verify-consumer-package.sh --mode debug   --gate live     --components core
```

It configures a producer, installs to prefix A, **moves** it to prefix B, builds a
separate consumer against B with Qt/zstd/OpenSSL made undiscoverable, and refuses
to pass on an unsupported case or a zero-test selection. Each run retains a
versioned `results.json`.

To reproduce the candidate itself:

```sh
tools/prepare-integration-alpha.sh \
    --evidence <dir from each of the three package runs above>
```

It refuses to produce a candidate unless the retained results name the exact
candidate commit, refuses a dirty tree or the wrong branch, verifies that the
wire versions named in the release notes match the codecs, consumes the staged
tree with an independent consumer, and writes a deterministic archive with
checksums. It performs no network access and cannot push, tag, release or merge.

## Stability, stated separately for API and for formats

These are two different promises and conflating them is how an alpha breaks
somebody's storage.

**C++ API/ABI is unstable between candidates.** Headers, signatures and types may
change without notice or deprecation. Pin the SHA and rebuild; do not pin a
symbol.

**No serialized format is supported for durable consumer storage at this
candidate.** `CTBO/0002`, `CTLO/0002`, `CTGV/0001` and `CTGP/0001` are the alpha's
interchange formats: versioned, checked, strict-decoding, and what an installed
consumer actually exchanges operations over. They are usable between peers pinned
to the same candidate. They are not storage formats, and no reader obligation has
started for them. `CTPT/1` is additionally *experimental* and is not approved for
consumer persistence.

A reader obligation starts only when a specific format version is declared
supported in an approved alpha: from that candidate forward, that reader and its
golden bytes are retained, or a checked translation is provided. Publishing this
package alpha does not retroactively certify any format, and it does not oblige a
future release to keep reading obsolete prototype readers. Later supported
releases must keep the readers of formats they declared supported — API
instability is not an excuse to silently reinterpret stored bytes.

There is nothing to migrate: no production data, store or history exists.

## Known documentation drift at this candidate

Recorded here rather than left to be discovered:

- `docs/specs/vnext-operation-codec.md` still says "Version is 0001" in its frame
  table and describes `CTBO`/`CTLO` as version 1. The implemented versions are
  `CTBO/0002` and `CTLO/0002`; version 1 is refused. E04 owns the status-claim
  sweep that corrects it. The codecs are the authority, not that paragraph.
- `docs/USING_VNEXT.md` is corrected in this candidate; this section is the one
  remaining stale statement about wire versions.

## Publication checklist

`tools/prepare-integration-alpha.sh` writes `PUBLICATION-CHECKLIST.md` next to the
staged archive, containing the exact proposed tag, target SHA, annotation text,
destination remote, and the verification commands a separately authorized
publication would run. **It is a proposal and is left unexecuted.** Remote CI
status is recorded as pending until it has actually run.