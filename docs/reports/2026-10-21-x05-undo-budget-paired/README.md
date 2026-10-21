# X05 incremental undo-budget accounting evaluation

This five-pair Release public matrix compares the current working-tree change
with its immediate parent, `26de7bb1a4d87b8b015a0eeee8e0cb3064df4941`, using
trace v2 (`7cd89dec0cac1b6f768471c25e91141855f62e260c0aa174ed8c0e4209cae939`).
All 56 public state and operation fingerprints matched. This is a public-only
matrix; native and recovery workloads were not run. The candidate source tree
was dirty at measurement time: its HEAD was the parent revision, and the exact
`git diff HEAD` SHA-256 was
`3b23d0582ee63dc93f0ab974fcabd5d91087a268e5409fd360e5043d3943b440`. The
aggregate's `candidate_sha` therefore names HEAD; this paragraph and the
recorded diff hash identify the actual candidate. Raw samples, aggregate,
environment, and build logs are in [`raw/`](raw/).

The change keeps `m_undo_bytes` incrementally, replaces the front-trimmed undo
vector with a deque, and subtracts bytes as redo entries are discarded or undo
groups are retired. In the diagnostics-enabled 10 KB/3,000-history workload,
200 sampled edits examined 400 undo-budget entries in total, while each edit
retired one oldest group. This is structural evidence that the hot budget path
now examines entries to retire rather than rescanning the active history.

The aged public 10 KB/3,000-history replacement mean improved from a median
2.057 ms to 1.856 ms across pairs (median paired ratio 0.899). Median per-run
p99 improved from 12.51 ms to 11.39 ms (paired ratio 0.910); it remains far
above X05's 2 ms target. The byte-control aged mean was effectively unchanged
(paired ratio 0.998). Bulk-undo mean ratios were 0.892 at 1 KB and 0.922 at
100 KB, with noisy p50 results. These are modest and mixed latency changes,
not a general performance win. The host governor was `powersave` and load was
elevated during several samples, so treat small differences as directional.

Correctness verification passed the affected Debug route (104/104), focused
Release model/recovery/undo route (12/12), and serial leak-enabled ASan/UBSan
route (11/11). `tst_perf_diagnostics` verifies that appending to a 1,000-entry
history examines exactly the two expected entries and retains the configured
depth. The full X05 acceptance is still unmet: aged p99 exceeds 2 ms, aged mean
has not reached the 5x target, and the mixed 5 KB/50%-tombstone target was not
measured in this public-only matrix. The full routed Release sweep and later
relocated package gate also remain outstanding.
