#pragma once

/// src/crdt/Anchor.h — internal shim after roadmap task A02a.
///
/// `Anchor` and `Bias` moved to <collabtext/Engine.h>: the presence/profile
/// header <collabtext/Identity.h> includes an Anchor, and a public header cannot
/// reach into src/. The SumTree that used to define `Bias` now takes it from
/// here.

#include <collabtext/Engine.h>
