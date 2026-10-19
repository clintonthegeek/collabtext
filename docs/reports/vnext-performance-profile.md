# X02 profile and work-selection record

This is the first X02 profile of the X01 baseline, measured from Release build
`1701d35fe9f88a4071d4fb9e71224cd5d8a429bd` before diagnostics were added. It
profiles `tst_benchmark single_replica_large_doc`: a 1,000,000-character ASCII
document seeded in 1,000-character chunks, followed by 100 deterministic
random local edits. The workload ends with 1,244 fragments, 82 tombstones and
999,979 visible bytes; undo depth is 1,000 and the default undo byte budget is
16 MiB.

The native benchmark measured 427.344 ms for the 100 edits (4.273 ms/edit) and
reported 79,724 KiB process RSS after the method. The RSS figure is a point
measurement, not a peak. A separate Massif run, using byte-time accounting,
recorded a peak of 28,044,176 heap bytes (27,979,474 useful bytes); it excludes
stack use and is not process RSS. At that peak, Massif attributed 16,491,024 B
(58.80%) to `Buffer::try_apply_validated_local_edit` and 7,896,000 B (28.16%)
to undo-entry snapshots created while trimming/retiring history. Those figures
describe allocation sites contributing to the live peak, not cumulative
allocation totals.

A separate `perf` CPU-clock sample captured 43 samples with no lost samples.
The call tree attributes 37.21% child time to `Buffer::set_fragments`, 32.56%
to `SumTree::push_item`, 11.63% to `Buffer::get_fragments`, and 9.30% to
`SequenceOrder::mint_link` including construction of its per-call identity
index. These are inclusive child percentages in one profile; they overlap and
must not be added. The profile indicates that vector extraction and full tree
rebuild are substantial, while undo retirement is a separate memory cost.

These findings select the X02 counters to add: fragment snapshots by item/text/
deletion bytes, scalar position expansion, tree visits and rebuild inputs,
fragment splits, visibility/deletion checks, placement index construction and
lookups, and active-to-retired undo work. They do not authorize an algorithm
change by themselves. X04a owns the mutation design/model gate after X02
measures copy and allocation counts on all relevant workloads.

The opt-in counter build is enabled with
`-DCOLLABTEXT_ENABLE_PERF_DIAGNOSTICS=ON`; the default is OFF and counter
updates compile to no-ops. Exact QtTest fixtures cover fragment snapshots,
scalar projection, UTF-16 conversion, boundary mint/resolve, list identity
lookup, and active/retired undo accounting. Five alternating quick-profile
pairs compared Release builds with diagnostics disabled/enabled across all 54
cases. Every case retained identical result/operation fingerprints and work
counts. The complete counter set is versioned as
`collabtext-perf-diagnostics/3`. Its median case-level paired timing ratio was
0.887 (diagnostic / disabled), with 8 case medians above 1.10 and 28 below
0.90. The host was heavily loaded after the run (load averages 20.02, 16.93,
13.52 on eight logical CPUs, powersave governor), and earlier counter versions
gave different medians (1.060 and 1.023). Treat overhead as unquantified: these
quick runs establish output parity, but their timing spread does not support a
stable overhead estimate. Raw JSONL and per-case calculations are archived in
`2026-10-19-x02-profile/raw/diagnostics-{off,on}-pair-*.jsonl`,
`diagnostics-v2-{off,on}-pair-*.jsonl`, `diagnostics-v3-{off,on}-pair-*.jsonl`,
and their corresponding overhead JSON files.

Representative quick-profile counts show the measured mechanisms. Five
1,000,000-byte public UTF-16 replacements perform 9,707,606 UTF-16 decoder
steps and snapshot 5,000,010 fragment-text bytes. Five boundary mint calls and
five resolves at 100 KB each materialize and examine 505,000 scalar items.
Five aged 5,000-entry list insertions visit/flatten 50,020 tree items, rebuild
25,015 entries, and perform 25,010 linear origin comparisons. The list facade's
identity and anchor lookups each flatten 25,000 entries over five samples and
perform 12,501 identity comparisons. Ten codec encodes at a 512-actor frontier
visit 5,120 actor entries.

Undo budget counters separate active and retired logical retention from newly
retired work. Across five 10,000-byte edits after 3,000 prior edits, the timed
samples observe 5,000 active groups / 250,000 active budget bytes in aggregate
and 10,020 retired groups / 2,963,280 retired budget bytes; five new retirement
groups contribute 880 budget bytes. These are sums of post-edit budget
observations across the five calls, not unique bytes or allocator usage.
Massif remains the evidence for heap attribution. All counter values above are
exact counts for their named fixtures, not estimates of total allocator
traffic.

The measured source paths explain these counts. `Buffer::try_apply_validated_local_edit`
uses `get_fragments()` (`SumTree::items()`, a full vector materialization), then
`set_fragments()` recomputes visibility and pushes the full resulting vector
back into the tree. `Buffer::utf16_range()` traverses retained fragments and
uses `utf16_length()`, `utf16_to_byte_offset()` and `byte_to_utf16_offset()`;
the million-byte fixture confirms repeated document-scale scans. Boundary
`CrdtEngine` methods call `Buffer::position_items()`, which expands text runs
to one `PositionItem` per scalar; `PositionTraversal::mint_boundary()` and
`resolve_boundary()` then inspect that projection. The IdList facade's
`element_of()` and `try_anchor_of()` each obtain a full `position_items()`
projection and linearly compare identities; `IdList::get_entries()` and
`set_entries()` separately flatten and rebuild the retained tree, while
`find_origin()` performs a linear origin scan. `SequenceOrder::mint_link()`
builds a per-call placement index. `OperationCodec` visits each causal-frontier
actor, which is expected linear work in the encoded frontier size. Undo
retirement snapshots old groups, while the new counters distinguish active and
retired logical budget charges from the one operation's retirement work.

The evidence justifies measuring summary-guided UTF-16 rank/seek, run-aware
boundary traversal, indexed list identity lookup, and transactional affected-
range mutation. X07a/X07/X08 own the read-path strategies. For mutation,
X04a's independent model/design gate must define stable handles, split/splice,
visibility maintenance, and rebuild behavior before X05 changes the tree path.
Codec actor traversal is expected to scale with frontier size; optimize it only
if X11 finds time or allocation beyond that linear payload requirement. X03
layout changes remain conditional on a real live-memory comparison. This profile
does not authorize API, ordering, undo-policy, collection, or archive-reader
changes.

The profile used Valgrind 3.25.1 Massif and Linux `perf`; `heaptrack` was not
installed. The X01 host ran with the `powersave` governor and a 1-minute load
average between 1.92 and 6.27 on eight logical CPUs. This profile is diagnostic
evidence, not a controlled before/after timing comparison. The native timed
region is retained for workload context; Massif's process duration is not used
as a performance result.

Raw `perf.data`, the Massif output, formatted summaries and test logs are in
[`2026-10-19-x02-profile/raw/`](2026-10-19-x02-profile/raw/). Reproduction commands and exact tool output are recorded there.
