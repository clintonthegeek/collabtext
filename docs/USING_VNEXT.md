# Using the collabtext alpha package

This is the consumer-facing guide for the alpha on branch `roadmap/reliability-v2`.
It is an alpha: not a supported release, not published, and expected to change its
API between candidates. What it can and cannot do is stated in
`<collabtext/ALPHA_CAPABILITIES.md>`, which is installed beside the headers and
versioned independently of this document. **Read that before designing against this**. Scoped live positions are implemented;
store recovery, persistent undo and durable pins remain pending.

## What is installed

| Component | Imported target | Needs | Built when |
|---|---|---|---|
| Core | `CollabText::Core` | C++20 only | always |
| File transport | `CollabText::FileTransport` | zstd | `COLLABTEXT_BUILD_FILE_TRANSPORT=ON` |
| Qt adapter | `CollabText::QtAdapter` | Qt6 Core/Gui | `COLLABTEXT_BUILD_QT=ON` |
| Qt widgets | `CollabText::Widgets` | Qt6 Widgets | `COLLABTEXT_BUILD_QT=ON` |

`CollabText::CollabText` exists in the build tree as an aggregate for the
repository's own tests and apps. It is deliberately **not** exported: a consumer
picks the components it needs, and picking nothing extra is the point.

Headers install to `<prefix>/include/collabtext/`. Nothing from `src/` is
installed, and no installed file refers to a build machine's paths.

## Build, install, consume

```sh
# 1. Producer. Core only needs nothing but a C++20 compiler.
cmake -S collabtext -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/opt/collabtext
cmake --build build -j "$(nproc)"
cmake --install build

# 2. Consumer. A separate project, outside the source tree.
cmake -S my-consumer -B consumer-build \
  -DCMAKE_PREFIX_PATH=/opt/collabtext
cmake --build consumer-build -j "$(nproc)"
```

`my-consumer/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.21)
project(my_consumer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CollabText CONFIG REQUIRED COMPONENTS core)

add_executable(my_consumer main.cpp)
target_link_libraries(my_consumer PRIVATE CollabText::Core)
```

The [installed consumer](../tests/consumer-smoke/main.cpp) is a complete working
example of allocator construction, text/list editing and checked operation
exchange. It uses an explicitly volatile allocator for a single-process smoke
check. Production callers supply a `Crdt::IdentityAllocator` that persists
reservations before returning them; the Core package does not supply a complete
recovered document session yet.

The operation exchange uses the checked public codecs:

```cpp
#include <collabtext/Serialization.h>

CollabText::Result<CollabText::Crdt::Operation>
read_operation(std::string_view retained_record) {
    auto decoded = CollabText::Crdt::decode_operation(retained_record);
    if (!decoded.ok()) return decoded.error();
    return std::move(decoded).value();
}
```

Encoding also returns `Result<std::string>`: check `ok()` before `value()`.
Keep the original operation if encoding fails and the original record if decoding
fails. A failure does not permit committing a reader cursor or advancing coverage.
The wire is `CTBO0001:` / `CTLO0001:` plus bounded JSON; `CTGV` encodes sparse
vectors and `CTGP` encodes terminal unused intervals. Every integer is an exact
sixteen-digit hex string. Old bare JSON is refused with `unknown_format`; a newer
version returns `unsupported_version`. Gap decoding does not publish the gap or
advance coverage. See [the codec contract](specs/vnext-operation-codec.md).

Ask for components you were given:

```cmake
find_package(CollabText CONFIG REQUIRED COMPONENTS core file-transport qt)
target_link_libraries(my_consumer PRIVATE
    CollabText::Core CollabText::FileTransport CollabText::Widgets)
```

Requesting a component the producer was not built with is a configure error
naming the option that would enable it — not an empty target you discover at link
time.

## A Core-only consumer proves nothing about optional components

`Core` has no link dependencies at all. A consumer that only ever asks for Core
tells you nothing about whether the optional components link. The check that
matters configures the consumer with those dependencies made undiscoverable:

```sh
cmake -S my-consumer -B consumer-build \
  -DCMAKE_PREFIX_PATH=/opt/collabtext \
  -DCMAKE_DISABLE_FIND_PACKAGE_Qt6=ON \
  -DCMAKE_DISABLE_FIND_PACKAGE_ZSTD=ON \
  -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON
```

`tools/verify-consumer-package.sh` does this, plus a relocation check, for all
three build modes. Read its `--help`.

## Conventions you have to honour

- **Text offsets are UTF-16 code units.** `CrdtEngine::insert`, `remove` and
  `length` all speak UTF-16 units. `IdListEngine` has no offset space; it is
  addressed by anchor only.
- **An identity is unique within one (document, stream).** The same counter in two
  streams is two unrelated facts. Do not mint identifiers from your own string
  ids and assume they are globally unique.
- **You supply the allocator.** Engines retain a shared `Crdt::IdentityAllocator`;
  the interface itself does not persist anything. Persist the 128-bit actor and the 64-bit counter before you
  use an operation, or you will reissue identities after a restart.
- **Persist library-owned position bytes, not `Anchor` or `Locator` fields.**
  Those legacy layouts remain reserved for evolution.
- **Do not call `collect_garbage()` or `compact()` expecting reclamation.** They
  return zero and retain everything.

## Scoped positions

```cpp
auto created = CollabText::CrdtEngine::create(allocator,
    {"document-id", "block-id", CollabText::Primitive::text, 1});
if (!created.ok()) return created.error();
auto& engine = *created.value();
engine.insert(0, "ab"); // editing uses UTF-16 units
auto left = engine.boundary_at(1, CollabText::Affinity::left); // UTF-8 bytes
if (!left.ok()) return left.error();
auto bytes = CollabText::encode_position(left.value());
if (!bytes.ok()) return bytes.error();
engine.insert(1, "X");
auto resolved = engine.resolve(left.value()); // byte_offset == 1
```

Right affinity at that same original gap resolves to byte 2. Carets expose
`lost_left`/`lost_right` and `collapsed()`. A strict `attachment_at` selects a
preceding/following item; hiding that selected item returns `detached` with no
usable offset. Selective undo can restore the same original token. Resolve also
reports wrong document/stream/primitive/epoch, awaiting mint state or unavailable
history explicitly. `local_clear()` invalidates history even for empty-edge
tokens. Allocator-only engines can edit but have no position scope.

`encode_position` returns library-owned **binary** CTPT v1 bytes; persist those
bytes, not C++ fields. `decode_position_as<TextBoundary>` refuses another token
kind. A decoder owns its value independently of input lifetime and rejects every
trailing byte. Wave's JSON adapter must ASCII-wrap bytes (for example base64url);
raw binary must not pass through UTF-8 conversion. Wave owns block existence and
its UI's stale/gone interpretation. Position queries create no pins.

No alpha has been published. This version is experimental until approved release
preparation freezes its supported-reader obligation. See
[the position contract](specs/vnext-boundary-semantics.md) for exact guarantees
and the retained-order assumption that S02/S03 still certify.

## What is not implemented

Summarised here; the manifest is authoritative and versioned.

- **Ordering repair.** Two peers inserting at the same position interleave
  character-by-character. Convergent, and wrong for prose.
- **Persistent undo**, **crash recovery**, **GC/pins/checkpoints**, **epoch on the
  operation**.

## Verifying an installation

```sh
# Producer, install to A, move to B, consumer against B with Qt/zstd/OpenSSL
# discovery disabled. This is the check: the move is what proves the consumer
# resolved against the installation and not the source tree.
./tools/verify-consumer-package.sh --mode release --gate package \
    --components core,file-transport,qt

# The consumer fixture itself: two engines converge over the wire codec, undo and
# redo behave, and every installed header compiles alone as the only library
# include. Built against an install, never against this checkout.
cmake -S tests/consumer-smoke -B /tmp/consumer \
    -DCMAKE_PREFIX_PATH=<prefix>
cmake --build /tmp/consumer -j "$(nproc)"
ctest --test-dir /tmp/consumer --output-on-failure --no-tests=error
```

## Reporting a discrepancy

The manifest has a "verified by" column naming an executable test for each
claim. If reality disagrees with the manifest, that is a bug in one of the two —
say which. A capability with no test behind it should say `not implemented`, not
`probably works`.

## Opaque operations (P06a step 6)

`Crdt::Operation` and `Crdt::IdListOperation` are immutable, independently
retainable values. Copying them shares read-only ownership, so a callback may
keep a copy for delivery after the engine returns or is destroyed. Obtain them
from the engine or public decoder, use the identity accessors, and transmit
through the public encode functions. Concrete edit/list/undo payload structs and
`std::get`/`std::visit` access are no longer public. No consumer can mutate an
emitted operation. An empty or moved-from value is rejected before application.

P06b replaces the old unversioned schema with checked version 1 codecs. The
public encode/decode functions return `Result` and preserve exact 64-bit values.
Document/stream/epoch operation envelopes remain P07 work. Failed records must
be retained and progress blocked; full transactional file recovery is not yet
certified. Live scoped positions are implemented as described above.
