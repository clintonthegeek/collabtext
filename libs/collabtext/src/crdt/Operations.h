/// src/crdt/Operations.h — internal shim after Task 1.1 promotion
///
/// The canonical definitions now live in <collabtext/Operations.h>.
/// This file re-exports them so that internal TUs using the old path
/// ("crdt/Operations.h") continue to compile without modification.
///
/// NOTE: UndoMap.h was previously included here but is not used by any
/// Operation struct definition.  It is NOT re-exported here; internal
/// files that need UndoMap.h must include it directly.
#pragma once

#include <collabtext/Operations.h>
