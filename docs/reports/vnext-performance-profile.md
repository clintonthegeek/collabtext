# X02 initial profile

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
updates compile to no-ops. Two exact QtTest fixtures validate snapshot and
position-projection counts. Five alternating quick-profile pairs compared
Release builds with diagnostics disabled/enabled across all 54 cases. Every
case retained identical result/operation fingerprints and work counts. The
median across case-level paired ratios was 1.060 (diagnostic / disabled);
23 case medians exceeded 1.10 and 12 were below 0.90. That spread, together
with the host's load and governor conditions, makes this a rough estimate of
measurement overhead rather than a stable per-call penalty. Raw JSONL and the
per-case paired calculation are archived in `2026-10-19-x02-profile/raw/diagnostics-*.jsonl` and
`2026-10-19-x02-profile/raw/diagnostics-overhead.json`.

Representative quick-profile counts show the mechanisms the next task should
separate: five aged 5,000-entry list insertions visit and flatten 50,020 tree
items, rebuild 25,015 entries, and perform 25,010 linear origin comparisons;
one 1 MB bulk replacement snapshots 1,000,000 fragment-text bytes; ten codec
encodes at a 512-actor frontier visit 5,120 actor entries. These are exact
counts for the named fixtures, not estimates of total allocator traffic.

The profile used Valgrind 3.25.1 Massif and Linux `perf`; `heaptrack` was not
installed. The X01 host ran with the `powersave` governor and a 1-minute load
average between 1.92 and 6.27 on eight logical CPUs. This profile is diagnostic
evidence, not a controlled before/after timing comparison. The native timed
region is retained for workload context; Massif's process duration is not used
as a performance result.

Raw `perf.data`, the Massif output, formatted summaries and test logs are in
[`2026-10-19-x02-profile/raw/`](2026-10-19-x02-profile/raw/). Reproduction commands and exact tool output are recorded there.
