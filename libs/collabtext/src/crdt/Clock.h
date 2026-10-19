#pragma once

/// src/crdt/Clock.h — internal shim after roadmap task A02a.
///
/// `Identity`, `Global` and `IdentityAllocator` were promoted to
/// <collabtext/Engine.h> because six public headers included this one, which is
/// why `src/` had to be on the build interface. This file re-exports them so the
/// ~60 engine translation units and the tests that reach for `crdt/Clock.h`
/// keep compiling unchanged — the same promotion `crdt/Operations.h` already
/// went through.
///
/// New code should include <collabtext/Engine.h>.

#include <collabtext/Engine.h>
