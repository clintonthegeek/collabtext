#!/usr/bin/env bash
# verify-changed.sh — build and run only the tests a change can plausibly affect.
#
# The full sweep is the thing we trust and the thing we cannot afford to run
# thirty times an hour. This script draws the line in between: it looks at what
# actually changed, routes each changed path to the test areas that can see it,
# builds and runs that subset, and prints its reasoning so a wrong route is
# visible rather than silent.
#
# It reuses the warm build tree. A from-scratch rebuild is verify-roadmap.sh's
# --fresh, and is an acceptance-tier thing: one per arc, where a stale object
# file surviving a flag change would actually be a plausible story.
#
# Tiers, cheapest first:
#   verify-changed.sh                      routed subset, slow tests excluded
#   verify-changed.sh --thorough           routed subset, slow tests included
#   verify-changed.sh --all                everything, warm tree
#   verify-roadmap.sh --fresh <mode> '.'   everything, from scratch  (arc gate)
#   verify-consumer-package.sh             packaging; A06a and after
#
# Exit zero means the routed tests passed. Zero selected tests is a failure, not
# a pass: a routing table that has quietly stopped matching a file is exactly the
# bug this script could otherwise hide.
set -uo pipefail

mode="debug"
baseline=""
scope="changed"
thorough=0
package_gate=0
explain=0
regex_override=""

usage() {
    cat <<'USAGE'
Usage: tools/verify-changed.sh [options]

  --mode MODE       debug, release, asan. Default: debug.
  --since REV       Route on changes since REV (default: uncommitted changes
                    against HEAD, staged and unstaged).
  --all             Route to every test, ignoring the table.
  --target REGEX    Use this CTest regex instead of the routed one.
  --thorough        Include the slow property/fuzz/simulation tests.
  --package-gate    Also run verify-consumer-package.sh in the current mode.
                    Implied when the change touches packaging.
  --explain         Print the routing table and the decision, run nothing.
  --help            This text.

Exit codes: 0 routed tests passed; 2 bad usage; 3 a test or build failed;
4 nothing could be routed and no --all was given.
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --mode)         mode="${2:-}"; shift 2 ;;
        --since)        baseline="${2:-}"; shift 2 ;;
        --all)          scope="all"; shift ;;
        --target)       regex_override="${2:-}"; scope="target"; shift 2 ;;
        --thorough)     thorough=1; shift ;;
        --package-gate) package_gate=1; shift ;;
        --explain)      explain=1; shift ;;
        --help|-h)      usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

case "$mode" in
    debug|release|asan) ;;
    *) echo "Unknown --mode '$mode'. Expected debug, release or asan." >&2; exit 2 ;;
esac

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
build_dir="$repo_root/build-roadmap-$mode"
jobs=${COLLABTEXT_VERIFY_JOBS:-$(nproc)}

# ---------------------------------------------------------------------------
# The routing table
#
# Each entry is <path glob>|<CTest regex>. The regexes are alternatives joined
# into one anchored regex per run, so a test named tst_buffer is matched by
# '^tst_buffer$' and never by accident by a substring of another name.
#
# Order matters: the first matching glob for a path wins, so the specific
# entries come before the catch-alls.
# ---------------------------------------------------------------------------
routes=(
    # Nothing to build. Docs, plans, task registries, ignore files.
    '.gitignore|'
    'docs/*|'
    '*.md|'
    'tools/verify-consumer-package.sh|PACKAGING'
    'tools/verify-changed.sh|'
    'tools/verify-roadmap.sh|'

    # Packaging, build system, public surface. A change here can change what a
    # consumer sees without changing a single line of engine behaviour, which is
    # why these get the package gate rather than a pile of unit tests.
    'libs/collabtext/CMakeLists.txt|PACKAGING'
    'cmake/*|PACKAGING'
    'CMakeLists.txt|PACKAGING'
    'tests/consumer-smoke/*|PACKAGING'
    'libs/collabtext/include/*|PACKAGING'

    # Identity.
    'libs/collabtext/src/identity/*|tst_actor_identity|tst_identity_types|tst_identity_json|tst_identity_store|tst_identity_projector|tst_identity_widgets|tst_sha256|tst_presence_manager'

    # IdList. Anchors are shared with Buffer, so tst_anchor comes along.
    'libs/collabtext/src/crdt/IdList*|tst_idlist'
    'libs/collabtext/src/crdt/idlist/*|tst_idlist'
    'libs/collabtext/tests/tst_idlist*|tst_idlist'
    'libs/collabtext/tests/tst_anchor*|tst_anchor|tst_idlist_anchor'

    # Buffer and the rope/sumtree/locator machinery under it.
    'libs/collabtext/src/crdt/Buffer*|tst_buffer|tst_edits_since|tst_gc|tst_undomap|tst_convergence|tst_serialization|tst_decode_validation|tst_operation_validation'
    'libs/collabtext/src/crdt/SumTree*|tst_sumtree|tst_buffer'
    'libs/collabtext/src/crdt/Rope*|tst_rope|tst_rope_integration|tst_buffer'
    'libs/collabtext/src/crdt/Locator*|tst_locator'
    'libs/collabtext/src/crdt/Segment*|tst_segment_format|tst_segment_writer|tst_segment_reader'
    'libs/collabtext/src/crdt/Anchor*|tst_anchor|tst_idlist_anchor'
    'libs/collabtext/src/crdt/OpQueue*|tst_opqueue'
    'libs/collabtext/src/crdt/Utf8*|tst_utf8|tst_decode_validation'
    'libs/collabtext/src/crdt/Undo*|tst_undomap'

    # Transport: OpStream, StreamSync, the file floor, compression.
    'libs/collabtext/src/transport/*|tst_opstream|tst_filesync|tst_stream_sync|tst_durable_file|tst_sidecar_manifest|tst_zstd_util|tst_store_fault_harness'

    # Qt widgets. Expensive to build, so they stay out of an engine-only change.
    'libs/collabtext/src/ui/*|tst_comment|tst_chat_message|tst_multicursor|tst_collab_plain_text_edit|tst_presence_manager|tst_identity_widgets|tst_scroll_stability'

    # Tests and harnesses: the thing that changed is the oracle, so run broadly.
    'libs/collabtext/tests/*|ENGINE_BROAD'
)

# The slow half. Property, fuzz and simulation tests dominate wall-clock and are
# exactly the ones whose failure is least likely to be caught by a fast subset.
slow_tests='tst_fuzz|tst_idlist_fuzz|tst_convergence|tst_idlist_convergence|tst_network_sim|tst_realistic|tst_trace_reduction|tst_benchmark|tst_store_fault_harness|tst_behavior_fixtures|tst_evaluation_regressions|tst_summary_laws'

# Broad but not total: everything that exercises engine behaviour, without the
# Qt widgets and without the file-floor and compression suites.
broad_tests='tst_buffer|tst_sumtree|tst_rope|tst_locator|tst_segment|tst_anchor|tst_idlist|tst_undomap|tst_opqueue|tst_serialization|tst_opstream|tst_utf8|tst_gc|tst_edits_since|tst_clock|tst_seed_op|tst_crdtengine_op_api|tst_operation_validation|tst_decode_validation|tst_rope_integration|tst_serialization_public|collabtext_core_smoke|collabtext_alpha_consumer'

wants_package=0
unrouted=()
routes_for_file() {   # sets ROUTE_REGEX, ROUTE_PACKAGING, ROUTE_EXTRA_TARGETS
    # Reset every output on entry. ROUTE_UNROUTED is a global that bash functions
    # share, and without this reset the first file that fails to match marks every
    # later file unrouted -- which reads as "nothing is covered here" and quietly
    # reduces the whole run to whatever matched before it.
    ROUTE_REGEX=""; ROUTE_PACKAGING=0; ROUTE_EXTRA_TARGETS=""; ROUTE_UNROUTED=0
    local rel="$1"
    for entry in "${routes[@]}"; do
        local glob="${entry%%|*}" regex="${entry#*|}"
        # shellcheck disable=SC2053  # intentional glob match
        [[ "$rel" == $glob ]] || continue
        if [[ "$regex" == "PACKAGING" ]]; then
            # The header-independence targets are not CTest tests, they are
            # object libraries whose only evidence is that they compiled. They
            # are named separately for that reason.
            ROUTE_REGEX="collabtext_core_smoke|collabtext_alpha_consumer"
            ROUTE_EXTRA_TARGETS="collabtext_public_headers_core"
            if grep -q '^COLLABTEXT_BUILD_QT:BOOL=ON' "$build_dir/CMakeCache.txt" 2>/dev/null; then
                ROUTE_EXTRA_TARGETS="$ROUTE_EXTRA_TARGETS collabtext_public_headers_qt"
            fi
            ROUTE_PACKAGING=1
        elif [[ "$regex" == "ENGINE_BROAD" ]]; then
            ROUTE_REGEX="$broad_tests"
        elif [[ -n "$regex" ]]; then
            ROUTE_REGEX="$regex"
        fi
        ROUTE_GLOB="$glob"
        return
    done
    ROUTE_GLOB=""
    ROUTE_UNROUTED=1
}

# ---------------------------------------------------------------------------
# Which files changed
# ---------------------------------------------------------------------------
if [[ -n "$baseline" ]]; then
    mapfile -t changed < <(git -C "$repo_root" diff --name-only "$baseline" --)
elif [[ "$scope" == "all" || "$scope" == "target" ]]; then
    changed=()
else
    mapfile -t changed < <( { git -C "$repo_root" diff --name-only HEAD --; \
                              git -C "$repo_root" ls-files --others --exclude-standard; } | sort -u )
fi

if [[ "$scope" == "all" ]]; then
    selected_regex='.'
elif [[ "$scope" == "target" ]]; then
    selected_regex="$regex_override"
else
    declare -A seen_glob=()
    parts=()
    extra_targets=()
    ROUTE_UNROUTED=0
    unrouted=()
    for f in "${changed[@]}"; do
        routes_for_file "$f"
        if [[ "${ROUTE_UNROUTED:-0}" == "1" ]]; then
            unrouted+=("$f")
            continue
        fi
        [[ -n "$ROUTE_REGEX" ]] || continue
        wants_package=$((wants_package + ROUTE_PACKAGING))
        for t in $ROUTE_EXTRA_TARGETS; do extra_targets+=("$t"); done
        seen_glob["$ROUTE_GLOB"]=1
        IFS='|' read -r -a alts <<< "$ROUTE_REGEX"
        for a in "${alts[@]}"; do parts+=("$a"); done
    done

    if [[ ${#parts[@]} -eq 0 ]]; then
        if [[ ${#changed[@]} -eq 0 ]]; then
            echo "No changes to route. Pass --all to run everything anyway." >&2
            exit 4
        fi
        if [[ ${#unrouted[@]} -eq ${#changed[@]} ]]; then
            echo "Nothing routable among:" >&2
            printf '  %s\n' "${unrouted[@]}" >&2
            echo >&2
            echo "No route matched. Either add a route, or pass --all to run the" >&2
            echo "full sweep. Guessing here would make a green run meaningless." >&2
            exit 4
        fi
    fi
    selected_regex=$(IFS='|'; echo "${parts[*]}")
fi

# Docs-only changes are legitimately nothing to build.
if [[ "$scope" == "changed" && ${#changed[@]} -gt 0 && -z "${selected_regex:-}" ]]; then
    printf 'Changed files:\n'
    printf '  %s\n' "${changed[@]}"
    echo
    echo "All changes are documentation. Nothing to build."
    exit 0
fi

if [[ $thorough -eq 0 && "$scope" == "changed" ]]; then
    selected_regex="$selected_regex|$slow_tests"
    # --thorough means "and also run the slow ones", so they must not still be
    # excluded. Splitting on '|' and dropping the slow alternatives is clearer
    # than trying to subtract with sed.
    IFS='|' read -r -a all_alts <<< "$selected_regex"
    keep=()
    for a in "${all_alts[@]}"; do
        skip=0
        IFS='|' read -r -a slow_alts <<< "$slow_tests"
        for s in "${slow_alts[@]}"; do
            [[ "$a" == "$s" ]] && skip=1 && break
        done
        [[ $skip -eq 0 ]] && keep+=("$a")
    done
    selected_regex=$(IFS='|'; echo "${keep[*]}")
    echo "Slow tests excluded (--thorough to include): $slow_tests"
fi

# A path can route into the same test from several globs. Duplicates are
# harmless to CTest but make the printed regex unreadable, and an unreadable
# regex is hard to check against the routing table when a run looks wrong.
selected_regex=$(printf '%s' "$selected_regex" | tr '|' '\n' | awk 'NF && !seen[$0]++' | paste -sd'|' -)

# ---------------------------------------------------------------------------
# Announce the route, then run it
# ---------------------------------------------------------------------------
echo "Root:        $repo_root"
echo "Mode:        $mode  (tree: $build_dir, -j $jobs)"
echo "Baseline:    ${baseline:-uncommitted vs HEAD}"
if [[ "$scope" == "changed" ]]; then
    echo "Routed by:"
    for g in "${!seen_glob[@]}"; do printf '  %-52s\n' "$g"; done | sort
fi
echo "Regex:       ^($selected_regex)\$"
echo

if [[ "$explain" -eq 1 ]]; then
    echo "Routing table:"
    for entry in "${routes[@]}"; do printf '  %s\n' "$entry"; done
    exit 0
fi

if [[ ! -f "$build_dir/CMakeCache.txt" ]]; then
    echo "No warm $mode tree at $build_dir." >&2
    echo "Configure it once, or use 'verify-roadmap.sh --fresh $mode .' for the" >&2
    echo "acceptance-tier run that also creates it." >&2
    exit 2
fi

# Ask CTest to apply its own regex dialect, and refuse to proceed if the pattern
# matches nothing. A zero-test run exits 0 and would read as a pass.
mapfile -t selected_tests < <(ctest --test-dir "$build_dir" -N -R "^($selected_regex)\$" \
    | sed -n 's/^ *Test *#[0-9]*: //p')
if [[ ${#selected_tests[@]} -eq 0 ]]; then
    echo "CTest selection matched zero tests for ^($selected_regex)\$" >&2
    exit 4
fi
echo "Selected ${#selected_tests[@]} tests:"
printf '  %s\n' "${selected_tests[@]}"
echo

if [[ ${#unrouted[@]} -gt 0 ]]; then
    echo "WARNING: no route matched these changed files:" >&2
    printf '  %s\n' "${unrouted[@]}" >&2
    echo "         They are not covered by this run." >&2
fi

status=0
# Repository convention: each CTest test and its build target share a name.
# Repository convention: each CTest test and its build target share a name. The
# extra targets are the ones whose evidence is compilation, not a test result.
mapfile -t build_targets < <(printf '%s\n' "${selected_tests[@]}" "${extra_targets[@]:-}" | sed '/^$/d' | sort -u)
cmake --build "$build_dir" --parallel "$jobs" --target "${build_targets[@]}" || status=$?
if [[ $status -eq 0 ]]; then
    QT_QPA_PLATFORM=offscreen ctest --test-dir "$build_dir" \
        -R "^($selected_regex)\$" --parallel "$jobs" --output-on-failure \
        --no-tests=error || status=$?
fi

if [[ $status -ne 0 ]]; then
    echo "FAILED ($mode): ${#selected_tests[@]} routed tests, status $status" >&2
    exit 3
fi

echo "PASS ($mode): ${#selected_tests[@]} routed tests"

if [[ $wants_package -gt 0 || $package_gate -eq 1 ]]; then
    echo
    echo "Packaging changed: running the consumer package gate."
    "$repo_root/tools/verify-consumer-package.sh" --mode "$mode" --gate package \
        --components core --producer-build "$build_dir" || status=$?
    [[ $status -ne 0 ]] && exit 3
fi

if [[ ${#unrouted[@]} -gt 0 ]]; then
    echo
    echo "PASS, with unrouted files listed above. Review them before committing."
fi
exit $status
