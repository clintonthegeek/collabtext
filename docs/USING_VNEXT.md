# Using the collabtext alpha package

This is the consumer-facing guide for the alpha on branch `roadmap/reliability-v2`.
It is an alpha: not a supported release, not published, and expected to change its
API between candidates. What it can and cannot do is stated in
`<collabtext/ALPHA_CAPABILITIES.md>`, which is installed beside the headers and
versioned independently of this document. **Read that before designing against
this** — several things a text editor normally assumes are not implemented, and
one of them (affinity) will silently give you wrong answers rather than an error.

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

`main.cpp`:

```cpp
#include <collabtext/CrdtEngine.h>
#include <collabtext/IdListEngine.h>
#include <collabtext/Operations.h>

int main() {
    collabtext::Identity allocator{"my-app"};   // persisted by you, once
    collabtext::CrdtEngine text{allocator};
    collabtext::IdListEngine list{allocator};

    text.insert(0, u"hello");
    const auto op = text.localOperation();      // observe, then ship it
    list.insert_after(list.end(), "item");

    text.undo();
    text.redo();
    return text.length() == 5 ? 0 : 1;
}
```

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
- **You supply the allocator.** `Identity` is the borrowed allocator; it does not
  persist anything. Persist the 128-bit actor and the 64-bit counter before you
  use an operation, or you will reissue identities after a restart.
- **Do not store `Anchor` or `Locator` in an external log.** Their layout is
  reserved for evolution, and there is no supported token encoding yet. Storing
  them means your stored positions break when the struct grows.
- **Do not call `collect_garbage()` or `compact()` expecting reclamation.** They
  return zero and retain everything.

## What is not implemented

Summarised here; the manifest is authoritative and versioned.

- **Affinity.** An anchor's `Bias` is stored but never read. Left and right of the
  same character resolve identically. Treat every resolved offset as approximate.
- **Typed unresolvable anchors.** On the text facade, an anchor naming an unknown
  item silently resolves to end-of-document. A silent relocation is data
  corruption; do not build repair logic that assumes you will be told.
  `IdListEngine::try_anchor_of` *is* typed and returns
  `ApplyOutcome::unresolvable_anchor`.
- **End-of-document position tokens**, and **position token persistence**.
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
