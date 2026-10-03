# Rust and the collabtext backend decision

**Date:** 2026-10-03. This is a researched design assessment, not a completed
backend benchmark or an authorization to replace the core. Sources are official
language documentation and the candidate project's own documentation. No Rust
toolchain or new backend has been installed for this assessment.

## Recommendation for this project

Rust is a credible production language. Language instability is not a good
reason to reject it. The meaningful tradeoff is whether its ownership safety
and an established CRDT implementation outweigh an additional toolchain,
language boundary, and dependence on another engine's semantics.

For collabtext, evaluate an existing Rust text engine behind a narrow C interface
before committing to either a custom Rust rewrite or the full native C++
ordering/GC redesign. Yrs is a practical first candidate. Treat persistent undo
and redo, offline retirement, retained anchors, and recovery as pass/fail gates,
then measure performance. A candidate failing those gates is not rescued by a
fast throughput number. This recommendation is an engineering inference from
the evaluation and sources, not a claim that a candidate already satisfies them.

The fresh-start decision makes this experiment substantially easier: no existing
operation history, store format, or binary API must be preserved. The Qt/C++
editor and file-sync direction can remain regardless of the engine language.

## What “stable Rust” means

Rust 1.0 was released on May 15, 2015, with a commitment to source compatibility.
The official announcement explicitly allows limited exceptions, including some
compiler bug fixes; compatibility is a strong policy rather than a promise that
every imaginable program will compile forever. Rust compiles native code without
requiring a tracing garbage collector or managed virtual machine.
[Rust 1.0 announcement](https://blog.rust-lang.org/2015/05/15/Rust-1.0/).

There are stable, beta, and nightly channels. Nightly is where experimental
features are enabled. A normal library does not need nightly just because it is
written in Rust. For this project, require stable compiler support and reject a
nightly-only dependency unless there is a concrete reason to reconsider that
requirement. Frequent releases do not mean every release changes the language
incompatibly. [Release channels](https://doc.rust-lang.org/book/appendix-07-nightly-rust.html).

Editions allow selected language changes while old crates keep their chosen
edition. Opting into a new edition is separate from upgrading the compiler;
crates using different editions can coexist. We can pin an edition and adopt a
new one deliberately. [Edition compatibility](https://doc.rust-lang.org/edition-guide/editions/).

Three other kinds of stability are separate:

| Concern | Actual implication here |
|---|---|
| Native Rust binary ABI | It has no stability guarantee. Do not expose Rust layouts or native Rust symbols as the consumer ABI. |
| Third-party crate APIs | These evolve under their own versioning policies. A language compatibility commitment does not freeze a CRDT library's API. |
| Persistent/wire formats | These need their own explicit contract and upgrade tests, independent of the compiler, edition, or ABI. |

The language reference distinguishes native Rust ABI from the platform C ABI.
Cargo's compatibility convention treats a change from `0.x` to `0.(x+1)` as
potentially incompatible. A 0.x crate can be useful and well tested, but we must
pin and review upgrades. [ABI reference](https://doc.rust-lang.org/reference/items/external-blocks.html),
[Cargo version compatibility](https://doc.rust-lang.org/cargo/reference/semver.html).

## Benefits and costs relative to the current C++ core

| Area | Rust benefit | Cost or limit for collabtext |
|---|---|---|
| Memory and ownership | Safe Rust checks reference lifetimes and exclusive mutation, reducing use-after-free, invalidation, and aliasing mistakes. | Raw pointers, FFI, and unsafe internals still require careful review. The remaining C++ UI is outside those checks. |
| Concurrency | Ownership traits make many invalid transfers and shared accesses compile-time errors. | They do not prevent deadlock or prove distributed correctness. One engine owner remains the simplest baseline. |
| Error modeling | Enums and Result support explicit applied/deferred/rejected and storage outcomes. | Equivalent contracts can be implemented in C++20; the language does not choose good semantics for us. |
| Performance | Native compilation and no required tracing collector fit latency-sensitive editing. | Rust is not inherently faster. Representation, indexing, batching, allocation, and history size dominate this workload. |
| Dependencies | Cargo provides a coherent build/dependency workflow. | We must integrate Cargo with CMake, pin dependencies, audit licenses, and maintain two build paths. |
| Learning and maintenance | Ownership becomes explicit in the implementation. | Borrowing/lifetimes take practice; mutable trees and cross-linked indexes can require redesign rather than mechanical translation. |
| Existing CRDT ecosystem | Reusing an engine may remove much custom integration/codec/ordering maintenance. | Its semantics, retained history, IDs, and extension APIs may not match our requirements. |

The memory-safety benefit applies when safe abstractions and their unsafe
implementations uphold their contracts. Rust explicitly permits unsafe code for
low-level operations and FFI; the compiler cannot verify everything across that
boundary. Learning ownership is useful even when integration stays single
threaded. [Safe and unsafe Rust](https://doc.rust-lang.org/book/ch20-01-unsafe-rust.html).

Rust's Send/Sync traits constrain what can cross threads and what can be shared.
That is helpful for ownership discipline, but collabtext would still need
application-level scheduling and callback rules. [Send and Sync](https://doc.rust-lang.org/book/ch16-04-extensible-concurrency-sync-and-send.html).

The performance, learning, and maintenance judgments in the table are project
inferences. This assessment contains no measurements of a Rust implementation.
Compilation time, binary size, and adapter allocations belong in the experiment,
not in an unsupported promise of improvement.

## What Rust does and does not address in the evaluation

Rust would help reduce implementation errors around manual allocation, fragment
lifetimes, ownership, and callback state. It would not independently fix any of
F1–F10's distributed/lifecycle rules: premature GC, incorrect coverage meet,
reused stream identity, absent replay, missing-segment gaps, wrong anchor
affinity, character interleaving, insufficient durable sync, or incomplete
semantic validation can all be written in safe Rust.

There are three different projects to choose among:

1. **Repair the native C++ engine.** Retain expertise and direct Qt debugging;
   own the ordering, undo, checkpoints, GC, and their ongoing verification.
2. **Write a new custom Rust engine.** Gain ownership checks, but still own those
   algorithms while also rewriting and integrating the core. This is the least
   convincing default without a stronger reason than language preference.
3. **Adopt an existing Rust engine.** Gain an established implementation and
   reduce bespoke algorithm work where its contract matches ours. Concentrate
   on integration, durability, application history, retirement, and verification.

The third route is the reason Rust deserves a trial here. Installing a compiler
alone contributes much less than selecting a suitable implementation.

## Keeping Qt and consumers in C++

A reasonable boundary is:

```text
Qt widgets / QTextDocument (C++)
             |
C++ document-session facade: batches, errors, offsets, subscriptions
             |
Versioned C ABI: opaque handles, checked slices, explicit results
             |
Rust engine and its internal data structures
```

Keep QObject ownership, signals, Qt event-loop work, and display conversion in
C++. Export coarse edit batches, incremental deltas, stable-position queries,
and update/checkpoint bytes. Avoid one FFI call per character or copying the
whole document for every edit. Those are proposed integration choices; actual
crossing and copying costs must be measured.

Define who allocates/frees each returned buffer, handle destruction, callback
lifetime, thread ownership, and cancellation. Do not pass QString, std::string,
or Rust String directly through a plain C ABI. Stop exceptions and panics at the
boundary and return defined errors. With `panic=abort`, a panic ends the process;
catching an unwinding panic also does not establish that partially mutated
engine state is safe to keep using. Specify failure atomicity and discard or
quarantine a failed handle when necessary. [FFI contracts and unwinding](https://doc.rust-lang.org/nomicon/ffi.html).

CXX is another option for a custom Rust/C++ bridge: it generates glue and checks
a restricted shared type boundary. It is not required to use an existing C
interface, and it does not decide the CRDT's ownership or durability semantics.
[CXX documentation](https://cxx.rs/).

Source builders need Cargo/rustc alongside the existing C++ tools. A consumer
linking a prebuilt library need not install Rust when native link/runtime
dependencies and supported targets are packaged correctly. Platform/toolchain
coverage still requires verification. Rust publishes explicit support tiers;
collabtext's usable tier is the intersection with Qt, the selected engine, and
our filesystem durability adapters. [Platform support](https://doc.rust-lang.org/rustc/platform-support.html).

For reproducible builds, pin a stable compiler, selected dependency revisions,
and Cargo.lock for our wrapper/workspace; use `cargo build --locked` in CI.
Vendored dependencies can support offline builds after acquisition. Toolchain
pinning is accompanied by reviewed upgrades, not indefinite avoidance of fixes.
[Locked builds](https://doc.rust-lang.org/cargo/commands/cargo-build.html),
[source vendoring](https://doc.rust-lang.org/cargo/reference/source-replacement.html).

## Why Yrs is a candidate, and what remains unproven

Yrs implements the Yjs algorithm/protocol in Rust; its project supplies a C FFI
through yffi. That provides a concrete path to native C++ integration instead
of a hypothetical full Qt rewrite. Its project also lists text/list features and
undo support. A feature table is a starting point for verification, not an
acceptance test. [Yrs project](https://github.com/y-crdt/y-crdt).

Yrs's documented StickyIndex supports relative positions and association, useful
for cursor/annotation behavior. Its API includes encoding and decoding. We must
verify our exact left/right boundary fixtures and deleted-neighbor behavior
through the chosen C interface. [StickyIndex](https://docs.rs/yrs/latest/yrs/struct.StickyIndex.html).

The UndoManager API documents grouped undo/redo, origins, stack observation, and
stack access. I did not find a documented complete persistence/restore API in
that public surface. **Inference:** reopening with both stacks, grouping, redo
tails, and remote restoration safety cannot be assumed from “has undo.” A trial
must demonstrate a supported serialization or deterministic private-history
reconstruction path. This is not a claim that persistence is impossible.
[UndoManager API](https://docs.rs/yrs/latest/yrs/undo/struct.UndoManager.html).

Yrs document options include client identity, offset counting, and `skip_gc`;
GC is enabled by default in the documented options. Identity collisions are
explicitly warned about. Begin the retention trial with GC disabled and choose
offset semantics deliberately. A local GC option is not evidence of a protocol
that honors another offline device's persistent undo. Conversely, our native
certificate design must not be imposed on a backend with different undo
semantics without first establishing what must be retained.
[Document options](https://docs.rs/yrs/latest/yrs/doc/struct.Options.html).

The public API pages inspected identify Yrs 0.28.0; floating repository files
and feature tables can lag or differ. Pin one matching engine/header/wrapper
revision and its compiler requirements for a trial. Do not mix versions or infer
C-FFI coverage from the Rust API alone. No candidate was built or timed here.

## Proposed bounded feasibility experiment

These are proposed tasks for a separately selected trial, not additional
authorized production implementation. Add them to TASKS.json with concrete
paths and dependencies when that route is chosen.

| Order | Task | Acceptance before proceeding |
|---|---|---|
| 1 | Pin stable toolchain, candidate revision, licenses, and C headers; build an independent C++ smoke consumer. | Repeatable native build, explicit minimum compiler, clean destruction/ownership, no nightly requirement. |
| 2 | Test required text/list and anchor behavior through the facade. | Exact concurrent-passage, nested-edit, duplicate/reordered-delivery, Unicode/UTF-16, and boundary-affinity fixtures pass. |
| 3 | Prove durable private undo and redo. | Restart with both stacks and grouped edits; peer changes survive local undo; trimming/crash recovery works without patching private layout. |
| 4 | Prove retention and retired-device handling, then file-store recovery. | Still-enrolled offline actor can restore supported history; retired actor follows explicit rescue/rejoin; missing segments and process crashes do not lose committed edits. |
| 5 | Measure matched workloads and issue an adoption decision. | Fixed-trace p50/p95/p99, memory/history, reopen time, binary/build cost, and adapter allocations; list remaining custom code and maintenance burden. |

Stop the experiment early if durable history needs a large permanent private
fork or the candidate cannot meet the selected semantics. If it succeeds,
rewrite affected roadmap tasks around the proven facade and backend guarantees
before implementation. If it fails, retain the native C++ route with a clear
record of why. No benchmark result alone authorizes adoption.
