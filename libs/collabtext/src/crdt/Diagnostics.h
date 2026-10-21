#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace CollabText::Crdt::Diagnostics {

inline constexpr std::string_view schema = "collabtext-perf-diagnostics/10";

enum class Counter : size_t {
    sumtree_items_visited,
    sumtree_items_flattened,
    sumtree_items_pushed,
    fragment_snapshots,
    fragment_snapshot_items,
    fragment_snapshot_text_bytes,
    fragment_snapshot_deletion_ids,
    fragment_tree_rebuild_items,
    origin_index_rows_inserted,
    origin_index_rows_erased,
    deletion_index_rows_inserted,
    deletion_index_rows_erased,
    fragment_index_full_rebuilds,
    fragment_index_publications,
    idlist_snapshots,
    idlist_snapshot_entries,
    idlist_tree_rebuild_entries,
    position_items_materialized,
    position_scalars_materialized,
    position_items_examined,
    utf16_range_calls,
    utf16_range_fragments_visited,
    utf16_decoder_steps,
    utf16_index_builds,
    utf16_index_ascii_probe_bytes,
    utf16_checkpoint_entries_built,
    fragment_summary_compositions,
    fragment_splits,
    fragment_split_bytes_copied,
    visibility_recomputations,
    visibility_deletion_checks,
    origin_lookup_comparisons,
    idlist_engine_identity_comparisons,
    placement_index_builds,
    placement_index_runs,
    placement_identity_queries,
    placement_identity_comparisons,
    undo_map_queries,
    undo_map_entries_examined,
    undo_retirement_groups,
    undo_retirement_operation_ids,
    undo_retirement_inserted_items,
    undo_retirement_deletions,
    undo_retirement_budget_bytes,
    undo_active_groups_observed,
    undo_active_budget_bytes_observed,
    undo_retired_groups_observed,
    undo_retired_budget_bytes_observed,
    codec_frontier_actors_encoded,
    codec_frontier_actors_decoded,
    middle_insert_refused_guard,
    middle_insert_refused_nonterminal_predecessor,
    middle_insert_refused_missing_successor,
    middle_insert_refused_ambiguous_ancestry,
    partial_replace_attempts,
    partial_replace_failures,
    partial_replace_refused_boundary_rank,
    partial_replace_failure_stage_1,
    partial_replace_failure_stage_2,
    partial_replace_failure_stage_3,
    partial_replace_failure_stage_4,
    partial_replace_failure_stage_5,
    undo_budget_entries_examined,
    count,
};

inline constexpr size_t counter_count = static_cast<size_t>(Counter::count);

struct Snapshot {
    std::array<uint64_t, counter_count> values{};

    uint64_t get(Counter counter) const noexcept {
        return values[static_cast<size_t>(counter)];
    }
};

inline constexpr std::array<std::string_view, counter_count> counter_names{
    "sumtree_items_visited",
    "sumtree_items_flattened",
    "sumtree_items_pushed",
    "fragment_snapshots",
    "fragment_snapshot_items",
    "fragment_snapshot_text_bytes",
    "fragment_snapshot_deletion_ids",
    "fragment_tree_rebuild_items",
    "origin_index_rows_inserted",
    "origin_index_rows_erased",
    "deletion_index_rows_inserted",
    "deletion_index_rows_erased",
    "fragment_index_full_rebuilds",
    "fragment_index_publications",
    "idlist_snapshots",
    "idlist_snapshot_entries",
    "idlist_tree_rebuild_entries",
    "position_items_materialized",
    "position_scalars_materialized",
    "position_items_examined",
    "utf16_range_calls",
    "utf16_range_fragments_visited",
    "utf16_decoder_steps",
    "utf16_index_builds",
    "utf16_index_ascii_probe_bytes",
    "utf16_checkpoint_entries_built",
    "fragment_summary_compositions",
    "fragment_splits",
    "fragment_split_bytes_copied",
    "visibility_recomputations",
    "visibility_deletion_checks",
    "origin_lookup_comparisons",
    "idlist_engine_identity_comparisons",
    "placement_index_builds",
    "placement_index_runs",
    "placement_identity_queries",
    "placement_identity_comparisons",
    "undo_map_queries",
    "undo_map_entries_examined",
    "undo_retirement_groups",
    "undo_retirement_operation_ids",
    "undo_retirement_inserted_items",
    "undo_retirement_deletions",
    "undo_retirement_budget_bytes",
    "undo_active_groups_observed",
    "undo_active_budget_bytes_observed",
    "undo_retired_groups_observed",
    "undo_retired_budget_bytes_observed",
    "codec_frontier_actors_encoded",
    "codec_frontier_actors_decoded",
    "middle_insert_refused_guard",
    "middle_insert_refused_nonterminal_predecessor",
    "middle_insert_refused_missing_successor",
    "middle_insert_refused_ambiguous_ancestry",
    "partial_replace_attempts",
    "partial_replace_failures",
    "partial_replace_refused_boundary_rank",
    "partial_replace_failure_stage_1",
    "partial_replace_failure_stage_2",
    "partial_replace_failure_stage_3",
    "partial_replace_failure_stage_4",
    "partial_replace_failure_stage_5",
    "undo_budget_entries_examined",
};

#if defined(COLLABTEXT_ENABLE_PERF_DIAGNOSTICS)
inline thread_local Snapshot current;
inline thread_local bool recording = false;

inline constexpr bool enabled() noexcept { return true; }
inline void record(Counter counter, uint64_t amount = 1) noexcept {
    if (recording) current.values[static_cast<size_t>(counter)] += amount;
}
inline Snapshot snapshot() noexcept { return current; }
inline void reset() noexcept { current = {}; }
inline void begin_sample() noexcept { recording = true; }
inline void end_sample() noexcept { recording = false; }
#else
inline constexpr bool enabled() noexcept { return false; }
inline constexpr void record(Counter, uint64_t = 1) noexcept {}
inline constexpr Snapshot snapshot() noexcept { return {}; }
inline constexpr void reset() noexcept {}
inline constexpr void begin_sample() noexcept {}
inline constexpr void end_sample() noexcept {}
#endif

}  // namespace CollabText::Crdt::Diagnostics
