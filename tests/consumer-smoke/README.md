# Consumer smoke test (V08 placeholder)

The behaviour fixtures in `libs/collabtext/tests/fixtures/behavior/` are the
consumer-facing contract at this stage: they state, executably, what the engine
is expected to do. `tst_behavior_fixtures` replays them.

The *installed* consumer check — a separate project that finds the package and
builds against `CollabText::CollabText` — is A01/A06's deliverable and cannot
exist yet: the library still links Qt6 publicly, exposes `src/` on its include
path, and has no install rules or exported CMake config. Building that project
now would encode today's broken packaging as the contract.

What lives here in the meantime is this note, so the directory the plan names
exists with a reason attached rather than as an empty promise. When A01 splits the
Qt-free core and adds install/export rules, this directory gets the real
`CMakeLists.txt`, a `main.cpp` that links only the public headers, and a CTest
entry that fails if the public headers stop being self-sufficient.
