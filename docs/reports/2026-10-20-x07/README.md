# X07 — UTF-16 tree summaries and range seeking

**Disposition: retain the implementation; close X07 with a measured byte-control miss and run X07b before X04.** The public 1-MB replacement target passed by a wide margin. Matched byte controls miss the 10% guard at 100 KB and narrowly at 1 MB, so the next task investigates the added summary cost before mutation/index work changes the tree representation.

## Implementation and correctness

`Buffer::utf16_length()` reads the maintained visible UTF-16 total. `Buffer::utf16_range()` seeks each requested UTF-16 boundary through the fragment SumTree while carrying the byte prefix, then converts within the selected fragment. Negative and out-of-range inputs, exact byte-offset overflow, and a boundary inside a surrogate pair remain refused.

Fragments cache their UTF-16 count. ASCII and short fragments need no checkpoint allocation; long non-ASCII fragments retain sparse scalar-boundary checkpoints at roughly 128 UTF-16 units. A range endpoint scans at most 128 scalars locally. Ordinary fragment copies keep a valid shared index, splits partition checkpoints, and restored fragments rebuild an absent cache. The independent conversion oracle exercises ASCII, non-BMP and combining text, marker adjacency, run boundaries, surrogate splits, long runs, split/delete/undo and restore.

Focused Debug checks passed: `tst_unicode_seek`, `tst_summary_laws`, `tst_sumtree`, `tst_positions`, and `tst_crdtengine_op_api` (5/5). After restoring the exact-width byte aggregate, `tst_summary_laws` and `tst_unicode_seek` passed again (2/2), and offscreen `tst_perf_diagnostics` passed (1/1). Diagnostics observed two endpoint fragment visits for the short query, versus 13 scalar steps before the seek; a long Unicode range decoded at most 258 scalars across both endpoints and visited exactly the two endpoint fragments.

## Paired performance result

The final representation uses a 64-bit visible-byte aggregate and a 32-bit visible-UTF-16 aggregate. The five-pair pinned Release comparison in [`focused-wide-bytes/`](focused-wide-bytes/) used baseline `d04f151b3a70b161b72c33a8de6895efbd1c13e2`, candidate `8d78709`, CPU affinity `[2]`, powersave governor, GCC Release `-O3 -DNDEBUG`, diagnostics disabled. The same exact summary/checkpoint implementation is committed at `eefcf9d`; the only source difference is removal of redundant narrowing casts in SumTree dimensions. All 54 public case results, operation fingerprints and work counts matched.

| Case | Median candidate/baseline mean ratio | Result |
|---|---:|---|
| Public 1-KB replacement | 0.979 | 2.1% faster |
| Public 10-KB replacement | 0.957 | 4.3% faster |
| Public 100-KB replacement | 0.758 | 24.2% faster |
| Public 1-MB replacement | 0.322 | 3.11× faster; target was 2× |
| Matched byte 1-KB replacement | 0.988 | 1.2% faster |
| Matched byte 10-KB replacement | 1.072 | 7.2% slower |
| Matched byte 100-KB replacement | 1.165 | 16.5% slower; guard missed |
| Matched byte 1-MB replacement | 1.101 | 10.1% slower; guard narrowly missed |
| 70-KB UTF-8 Unicode append | 0.373 | 2.68× faster |

Ratios are medians of the five paired mean ratios. The individual 100-KB byte ratios were 1.165, 1.426, 1.139, 1.089 and 1.779; the 1-MB byte ratios were 1.101, 1.190, 1.096, 1.015 and 1.643. Host load changed during the run (load average before commands ranged from 5.23 to 16.26), despite pinning the benchmark to one CPU. Raw per-run environment, samples and aggregates are retained in `focused-wide-bytes/`; interpret small differences cautiously. The larger 100-KB byte-control signal warrants follow-up.

A later 32-bit visible-byte sum plus composable overflow flag was tested against the same 54-case profile in [`focused-overflow-bit/`](focused-overflow-bit/). It preserved results but performed worse: median matched-byte mean ratios were 1.60 at 100 KB and 1.187 at 1 MB. That representation was rejected; the retained 64-bit sum keeps exact overflow checking and has the better measured byte-control profile. [`focused-u32-interim/`](focused-u32-interim/) preserves an earlier 32-bit-summary experiment and is not the final candidate. The root-level `aggregate.json` and run files preserve the initial cache-preserving candidate measurement; it preceded exact wide-byte overflow accounting and is historical only.

## Memory and next step

On this 64-bit build, `sizeof(FragmentSummary)` is 16 bytes versus 8 bytes at baseline; `sizeof(Fragment)` is 200 versus 176 bytes. Each long non-ASCII checkpoint stores an 8-byte `(byte, UTF-16 unit)` pair at roughly 128-unit spacing, plus vector/shared-pointer allocation overhead. ASCII and short fragments allocate no checkpoint vector. This footprint is the principal concrete area for X07b to investigate alongside summary composition cost.

X07b is inserted before X04. It must preserve exact overflow refusal and Unicode semantics, bring both the 100-KB and 1-MB matched byte-control median mean ratios to at most 1.10, and retain at least a 2× public 1-MB speedup. If that tradeoff is not achievable, X07b must record measured evidence and a keep/no-change disposition. No consumer behavior or public API changed.
