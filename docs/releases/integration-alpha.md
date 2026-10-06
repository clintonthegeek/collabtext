# collabtext 0.2.0-alpha.2 — position-capable integration alpha

**Status: prepared, not published.** No tag exists on any remote, no package has
been published, `master` has not been merged or touched, and no consumer has been
contacted. Publication is a separate action requiring an explicit instruction, and
the checklist produced by the preparation script is deliberately left unexecuted.

This is an **alpha**, not a supported release and not a completed reliability
release. It does not have crash recovery, persistent undo/redo, durable pins, or
GC. What it can and cannot do is stated in the installed capability manifest
`<collabtext/ALPHA_CAPABILITIES.md>`, which is versioned separately from this
document and is the authority.

Two candidates are described here:

| Candidate | Version | Tag | What it adds |
|---|---|---|---|
| package alpha | `0.2.0-alpha.1` | `v0.2.0-alpha.1` | the installable package, pins, checksums, publication checklist |
| **position alpha (this release)** | **`0.2.0-alpha.2`** | `v0.2.0-alpha.2` | **`CTPT/1` declared a supported serialized token version**, with retained golden bytes and a reader fixture |

Neither tag exists on any remote. Neither release has been published.

## Candidate

| | |
|---|---|
| Package version | `0.2.0-alpha.2` (SemVer prerelease) |
| **Candidate SHA (position alpha)** | _filled in by the preparation commit; see `docs/reports/vnext-task-log.md` (E07b) and the generated `alpha-candidate.json`_ |
| **Candidate SHA (package alpha, superseded)** | `bdbae5c3d310f2388b1d445a254b4ac35474d215` — tag `v0.2.0-alpha.1` proposed and **not created** |
| Branch | `feat/fuguemax-placement` |
| Proposed tag | `v0.2.0-alpha.2` — **not created** |
| Destination remote | recorded only (`origin`); this task contacted no remote |
| Capability manifest | version 9, installed beside the headers |
| Serialized formats present | `CTBO/0002`, `CTLO/0002`, `CTGV/0001`, `CTGP/0001`, `CTPT/1` |
| **Supported for consumer persistence** | **`CTPT/1` only** |
| Retained token goldens | `libs/collabtext/tests/fixtures/position-tokens/ctpt-v1-supported.json`, 16 cases, all four token kinds |
| Toolchain | GCC 16.2.1, CMake 4.4.3; libzstd 1.5.7, Qt6 6.11.2, OpenSSL 3.6.4 present but undiscovered by the Core consumer |

The packaged content — headers, `ALPHA_CAPABILITIES.md`, exported CMake config,
`libcollabtext_core.a` — is identical at this SHA and at the later commit that
records this table. Only this document and the task registry changed after
verification.

### How the package candidate was verified

All five gate runs completed at `bdbae5c`, in this worktree, from clean
configuration each time:

| Gate | Mode | Result |
|---|---|---|
| `--gate package` | Debug | PASS, 11 steps, manifest v8 |
| `--gate package` | Release | PASS, 11 steps, manifest v8 |
| `--gate package` | ASan (Debug + ASan/UBSan, leak detection on) | PASS, 11 steps; no sanitizer or leak report |
| `--gate baseline` | Debug | PASS, 18 probe rows, 17 pass |
| `--gate baseline` | Release | PASS, 18 probe rows, 17 pass |

Each gate configures a producer, installs to prefix A, **moves** it to prefix B,
builds a separate consumer against B with Qt/zstd/OpenSSL made undiscoverable,
verifies the consumer compiles and links nothing from the source or build tree,
runs the probes, and refuses an unknown component. Evidence is under
`build-consumer-verify/e07a-*`; per-run `results.json` records the revision, mode,
gate, compiler and manifest version.

Separately, the documented clean-clone build was exercised for real: the
repository was cloned to a fresh directory, checked out at
`bdbae5c3d310f2388b1d445a254b4ac35474d215`, configured, built, installed, and
consumed by a separate CMake project against the install prefix with public
headers only. It prints `text=aXb byte_offset=1 token_bytes=104`.

### How the position candidate was verified

_filled in by the preparation commit from the retained run results._

### Why `0.2.0-alpha.2` and not `2.0.0`

The reliability workstream is internally called "v2", and its branch names carry
"reliability-v2". **That is a workstream label, not a package major version.**
`2.0.0` would advertise an API break nobody has evaluated for compatibility.

The break that actually happened is already a hard cut inside `0.x`: `Locator` was
removed, and the operation codec moved from version 1 to version 2. Version 1
frames are refused rather than read.

The workstream label "v2" is also not the reason the token became supported.
`CTPT/1` was supported because its representation is independent of placement and
its meaning survived S03 — see below — not because of a version number.

`0.2.0-alpha.2` is a prerelease and is therefore **older than `0.2.0`**. One
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
- **`CTPT/1` as a supported token format** (new in this candidate): 16 retained
  golden cases covering all four token kinds, an independent literal §4 reader in
  the test suite, and a per-case replay that re-mints every golden on the current
  engines.
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
- **A token that survives everything.** `CTPT/1` is supported as *bytes*, which is
  a promise about reading stored tokens — not about pinning, reclamation, epoch
  changes, `local_clear()` or store recovery. The full list is in "What this
  candidate guarantees, exactly". Do not substitute a stored tree link
  (`parent`, `side`, `right_origin`) for a token; that is the mistake the token
  format exists to prevent.
- **A supported store or checkpoint format.** None exists.

### Baseline probe results at this candidate

Measured at `bdbae5c` through the installed package, Debug and Release, both
identical: 18 probe rows, 17 pass, 1 `unsupported`.

| Probe | Result |
|---|---|
| WP1 boundary affinity, all scalar boundaries, both affinities, two replicas | pass (3 cases) |
| WP2 caret collapse, strict referent detachment, undo/redo restoration | pass (3 cases) |
| WP3 deleted containing block (consumer-owned check) | pass |
| WP4 exact start/end/empty boundaries, off-end and non-scalar refusal | pass (3 cases) |
| WP5 token round-trip, all trailing suffixes/truncations, fresh-process replay | pass (3 cases) |
| WP6 concurrent paste/typing passage contiguity | pass, observed `abcXYZ` |
| WP7 wrong document/stream/primitive/epoch | pass (2 cases) |
| WP8 no-GC containment smoke | pass |
| WP8 durable pin register/lookup/release | **`unsupported`**, owner S05/G01/G06/G08 |

The pre-S03 WP6 known-failure exception is gone. There is **no known failure**
in the baseline matrix at this candidate: the single non-pass is an
unimplemented capability recorded as `unsupported`, not a defect.

### Strict live-position gate

The baseline above is deliberately permissive: it accepts the exact capability
state of each probe. The **strict** gate is a separate set that refuses
`known_failure` and `unsupported` for the live-position capabilities:

| Case set | What it requires |
|---|---|
| WP1 | exact affinity, all scalar boundaries, both affinities, two independent replicas |
| WP2 | explicit collapse for a caret, typed `detached` for a strict referent, undo and redo restore it |
| WP3 | the consumer's own deleted-containing-block check returning `Gone` before any engine lookup |
| WP4 | exact start/end/empty boundaries; off-end and interior-of-scalar offsets refused |
| WP5 | token canonical round-trip, all truncations, all 256 trailing bytes, malformed/foreign refusal, original-token replay in a **fresh process** |
| WP7 | wrong document, stream, primitive and epoch each refused with no mutation |
| WP8 | the no-GC containment smoke only — **never** counted as certified reclamation |
| Markdown consumer | a second, independent fixture: exact Markdown bytes, optional transport routing, and caret collapse shown apart from strict attachment loss |

The results at this candidate are recorded under "How the position candidate was
verified". A strict row that is `unsupported` or `known_failure` fails the gate;
a missing row fails it too, rather than shrinking the set.

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

This `main.cpp` is the example above, compiled and run against the clean-clone
install. It is the whole shape of a consumer: supply an allocator, edit, take a
position, keep the token.

```cpp
#include <collabtext/ActorId.h>
#include <collabtext/CrdtEngine.h>
#include <collabtext/Engine.h>
#include <collabtext/Identity.h>
#include <collabtext/Serialization.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>

// The Core package does not persist identity for you: you supply an allocator.
class Counter final : public CollabText::Crdt::IdentityAllocator {
public:
    explicit Counter(CollabText::ActorId actor) : m_actor(actor) {}
    CollabText::Result<uint64_t> reserve_run(uint64_t count) override {
        return m_counter.reserve_run(count, "consumer");
    }
    void observe(uint64_t counter) override { (void)m_counter.observe(counter, "consumer"); }
    uint64_t peek() const noexcept override { return m_counter.peek(); }
    const CollabText::ActorId &actor() const noexcept override { return m_actor; }
private:
    CollabText::ActorId m_actor;
    CollabText::CheckedCounter m_counter;
};

int main() {
    using namespace CollabText;
    std::array<uint8_t, ActorId::kBytes> bytes{};
    bytes[ActorId::kBytes - 1] = 1;
    auto allocator = std::make_shared<Counter>(ActorId::from_bytes(bytes));

    auto created = CrdtEngine::create(allocator, {"doc", "block", Primitive::text, 1});
    if (!created.ok()) return 1;
    auto& engine = *created.value();

    engine.insert(0, "ab");                            // editing uses UTF-16 units
    auto left = engine.boundary_at(1, Affinity::left); // positions use UTF-8 bytes
    if (!left.ok()) return 2;
    auto token = encode_position(left.value());        // library-owned CTPT/1 bytes
    if (!token.ok()) return 3;

    engine.insert(1, "X");
    auto resolved = engine.resolve(left.value());
    if (!resolved.ok()) return 4;
    std::printf("%s %d %zu\n", engine.text().c_str(),
                static_cast<int>(resolved.value().byte_offset), token.value().size());
    return 0;
}
```

Persist `token`'s bytes, not the C++ fields. The allocator above is volatile by
construction: a production allocator must persist the actor and the counter
**before** handing out an identity, or it will reissue after a restart.

Ask for no version, or for `0.2.0`. Read `<collabtext/ALPHA_CAPABILITIES.md>`
from the install prefix before designing against anything.

## Running the probes against a candidate

The same script that produced the evidence is in the repository, and needs no
network, no sibling checkout and no temporary harness:

```sh
# The package gate: install, relocate, and an independent consumer.
tools/verify-consumer-package.sh --mode debug   --gate package --components core
tools/verify-consumer-package.sh --mode release --gate package --components core
tools/verify-consumer-package.sh --mode asan    --gate package --components core

# The strict live-position gate, required for a position-capable candidate.
tools/verify-consumer-package.sh --mode debug   --gate live    --components core
tools/verify-consumer-package.sh --mode release --gate live    --components core
tools/verify-consumer-package.sh --mode asan    --gate live    --components core
```

The preparation script is told which gate the evidence must come from, so a
package-only candidate cannot be passed off as a position one:

```sh
tools/prepare-integration-alpha.sh --gate live \
    --evidence <dir from each live run above>
```

It additionally refuses a candidate whose manifest declares `CTPT/1` supported
without the retained golden fixture being present and covering all four token
kinds. A supported-format promise with nothing behind it fails the run.

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

**`CTPT/1` position tokens are supported for consumer persistence, from
`0.2.0-alpha.2`.** This is the one format in this candidate with a reader
obligation attached. From this candidate forward a supported release either keeps
reading these bytes or ships a checked translation, reports a version it cannot
read as `unsupported_version` (distinct from `malformed`), and never remints a
stored token to make a resolve succeed. The C++ API may break freely; that does
not excuse reinterpreting stored tokens.

`CTBO/0002`, `CTLO/0002`, `CTGV/0001` and `CTGP/0001` remain the alpha's
*interchange* formats: versioned, checked, strict-decoding, and what an installed
consumer actually exchanges operations over. They are usable between peers pinned
to the same candidate. They are not storage formats, and **no reader obligation
has started for them** — an operation you store yourself may not be readable by a
later release.

There is nothing to migrate: no production data, store or history exists, and no
pre-vNext or earlier experimental token is supported or will be converted.

## Known documentation drift at this candidate

Recorded here rather than left to be discovered:

- `docs/specs/vnext-operation-codec.md` still says "Version is 0001" in its frame
  table and describes `CTBO`/`CTLO` as version 1. The implemented versions are
  `CTBO/0002` and `CTLO/0002`; version 1 is refused. E04 owns the status-claim
  sweep that corrects it. The codecs are the authority, not that paragraph.
  `tools/prepare-integration-alpha.sh` now fails a candidate whose release notes
  disagree with the codecs, so this drift cannot survive a preparation run — but
  the spec text itself is still stale.
- `ALPHA_CAPABILITIES.md` and `docs/USING_VNEXT.md` were corrected in this
  candidate (they claimed version 1 codecs, and `USING_VNEXT` still described
  concurrent insertion as interleaving character-by-character after S03 repaired
  it). The consumer probe runner also recorded hardcoded `CTBO/1`/`CTLO/1` while
  its own case file declared `CTBO/2`/`CTLO/2`; the retained results in this
  candidate correctly report `CTBO/2` and `CTLO/2`.

## What this candidate guarantees, exactly

Stated positively, because the "not available" list above reads as if less is
working than is.

**Guaranteed at `0.2.0-alpha.2`:**

- Local text (`Buffer`) and structural list (`IdList`) editing, reading, and
  in-memory undo/redo.
- Scoped live positions: exact affinity, start/end/empty boundaries, UTF-8 scalar
  boundaries, explicit collapse, and typed failure for the wrong
  document/stream/primitive/epoch.
- `CTPT/1` as a supported serialized token, with retained golden bytes.
- Tokens round-trip through encode/decode exactly, including all truncations and
  all 256 trailing suffixes, and the *original* bytes resolve in a fresh process
  given operations the consumer retained.
- Concurrent insertion keeps each author's run contiguous, forward and reverse.
- An installable, relocatable Core package with no Qt, zstd or crypto dependency,
  and independent consumers that prove it.
- Physical GC disabled: `collect_garbage()` and `compact()` retain everything, so
  nothing a token references can disappear from under it today.

**Explicitly *not* guaranteed, and each has an owner:**

| Not guaranteed | Owner | What a consumer gets instead |
|---|---|---|
| Crash recovery | L02–L08 | nothing recovered; a reopen loses the document |
| Persistent undo/redo across reopen | L06 | in-memory only |
| Durable external pins | S05/G01/G06/G08 | storing a token is not a pin; WP8 is `unsupported` |
| Physical reclamation | G06–G08 | disabled; nothing is reclaimed, so nothing breaks |
| Checkpoint serialization / translation | G04/G05 | no checkpoint format exists |
| Epoch-change translation | G01/G02 | `wrong_epoch` |
| Anchors across a durable reopen | S04 (needs L08) | fresh-process replay of self-retained operations only |
| Enumerated proof of combined sequence/undo histories | S06 | fixtures and 240 seeded differential traces, not exhaustion |

**What the consumer owns** (library behaviour does not extend into it):

- **Block structure and existence.** The engine scopes a position by
  (document, stream, primitive, epoch); it does not know what a "block" is. A
  deleted containing block is the consumer's check, and it must run *before*
  engine lookup — `WP3` tests this in the installed fixture.
- **Formatting and block-level annotations.** Deliberately out of scope; a consumer
  layers it over character CRDTs.
- **Encoding token bytes for its own transport.** `CTPT/1` is binary and may
  contain NUL. A JSON log must ASCII-wrap it (base64url under its own version
  prefix); raw binary through UTF-8 conversion loses data.
- **Interpreting collapse.** The library reports `lost_left`/`lost_right`; whether
  that is acceptable for a caret and unacceptable for a strict reference is the
  consumer's policy.
- **Authorization, event logs and transport choice.** A successful decode grants
  no permission and a CRDT timestamp is not an authorization.

**Strict reference versus caret** — the distinction a consumer must implement,
with both branches in `docs/USING_VNEXT.md`:

```cpp
auto caret  = engine.boundary_at(1, Affinity::left);              // TextBoundary
auto strict = engine.attachment_at(1, Affinity::left,             // TextAttachment
                                   ReferentSide::preceding);
```

A caret keeps resolving after its neighbour is deleted, possibly with
`collapsed()` set. A strict reference returns typed `detached` with **no offset**,
because there is no honest nearby position for it. Never resolve a strict
reference to a nearest offset: that turns a deleted character into a live one,
which is the failure the typed result exists to prevent.

## Saved reply draft for the user to relay — NOT SENT

Written per E07b step 5 so a consumer conversation can happen on the user's
terms. **Nothing was sent, and no consumer repository was touched.** Draft:

> Subject: collabtext position alpha `0.2.0-alpha.2` — usable live positions, one
> supported token format, no recovery yet
>
> We've prepared an alpha you can pin by commit SHA. It is an alpha: the C++ API
> may change between candidates, so pin the SHA and rebuild rather than pinning a
> tag or a symbol.
>
> What it does that you asked about: scoped live positions for text and list
> primitives, with exact left/right affinity, explicit start/end/empty boundaries,
> UTF-8 scalar-boundary positions, and typed failures for the wrong document,
> stream, primitive or epoch instead of a fallback offset. Concurrent insertion is
> repaired — each author's run stays contiguous for forward and reverse typing —
> so a pasted paragraph no longer interleaves character by character.
>
> `CTPT` version 1 position tokens are now a **supported serialized format**. The
> golden bytes for all four token kinds are retained in the repository and a
> reader fixture keeps them working, so a later supported release will keep reading
> them or provide a translation. Store `encode_position`'s bytes; they are binary
> and may contain NUL, so wrap them (base64url is fine) rather than passing them
> through UTF-8 conversion.
>
> What it does **not** do yet, so you can plan around it: no crash recovery, no
> persistent undo/redo across a reopen, no durable external pins, and no garbage
> collection. Storing a token is not a pin. Physical GC is disabled, so nothing is
> reclaimed today and a stored token cannot be pulled out from under you yet — but
> that changes when reclamation lands, and a pin register is on the roadmap.
> Anchors are not yet certified across a durable reopen; the same bytes resolve in
> a fresh process given operations you retained yourself.
>
> Block structure, existence checks, Markdown/formatting, the event log and
> transport remain yours. A deleted containing block is your check to make before
> asking us to resolve anything.
>
> Nothing has been published yet — no tag, no release, no package registry entry —
> so pin the SHA only once the branch is reachable. Tell us if you want it
> published and we will prepare it for your approval first.

## Publication checklist

`tools/prepare-integration-alpha.sh` writes `PUBLICATION-CHECKLIST.md` next to the
staged archive, containing the exact proposed tag, target SHA, annotation text,
destination remote, and the verification commands a separately authorized
publication would run. **It is a proposal and is left unexecuted.** Remote CI
status is recorded as pending until it has actually run.