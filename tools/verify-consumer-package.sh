#!/usr/bin/env bash
# verify-consumer-package.sh — producer/install/consumer verification for the
# collabtext alpha. Roadmap task A06a owns `--gate package`; W02 and W03 add
# `--gate baseline` and `--gate live`; A06/E05 add `--gate final`.
#
# The check is a relocation. The producer is configured and built in a
# task-specific tree, installed to prefix A, **moved to prefix B**, and only then
# is the consumer configured — against prefix B, with no path back to the source
# tree or to prefix A. A consumer that accidentally resolved against either
# would still build and still pass, which is exactly why the move is the point
# rather than an optimisation.
#
# Exit zero means the gate's assertions passed. Never "the worker ran". Any
# configure, build, install, move or test failure; a zero-test selection; an
# unexpected signal; or an unimplemented gate/component combination is nonzero,
# with a diagnostic naming what failed.
#
# A versioned results.json is retained alongside the logs, recording the producer
# revision, the mode and every option, the compiler, the components, the
# capability manifest's version, and each step's exit status.
set -uo pipefail

usage() {
    cat <<'USAGE'
Usage: tools/verify-consumer-package.sh --mode <debug|release|asan> --gate <package|baseline|live|final> [options]

  --mode MODE         debug, release, or asan. Sanitizers follow the existing
                      runner's supported-compiler contract.
  --gate GATE         package  A06a: install, export and an independent consumer.
                      baseline W02:  the eight consumer probes, installed.
                      live    W03:  the live-position probes, installed, strict.
                      final   A06/E05: the complete promised set.
  --components LIST   Comma-separated: core, file-transport, qt. `core` is always
                      included; each other component is added on top.
  --producer-build PATH
                      Install from an already-configured and built producer tree
                      instead of configuring a new one. This is what makes the gate
                      cheap enough to run on every packaging edit: the inner loop
                      should not pay for a second full build of a tree that is
                      already sitting there built. Omit it for the acceptance-tier
                      run, where a from-scratch producer is the point.
  --output-dir PATH   Retain logs and results here. Default: a unique ignored
                      directory under the worktree. Never overwrites an existing
                      install or build tree.
  --help              This text, plus which gates are implemented.

Exit codes: 0 the gate passed; 2 bad usage; 3 a step failed; 4 a gate or component
combination is not implemented yet.
USAGE
}

mode=""; gate=""; components="core"; output_dir=""; producer_build_in=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --mode)       mode="${2:-}"; shift 2 ;;
        --gate)       gate="${2:-}"; shift 2 ;;
        --components) components="${2:-}"; shift 2 ;;
        --output-dir) output_dir="${2:-}"; shift 2 ;;
        --producer-build) producer_build_in="${2:-}"; shift 2 ;;
        --help|-h)    usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ -z "$mode" || -z "$gate" ]]; then
    echo "Both --mode and --gate are required." >&2
    usage >&2
    exit 2
fi

case "$mode" in
    debug|release|asan) ;;
    *) echo "Unknown --mode '$mode'. Expected debug, release or asan." >&2; exit 2 ;;
esac

# Which gates exist yet. A gate that is not implemented must fail with a
# diagnostic naming its owning task, never with an empty run that looks green.
gate_owner() {
    case "$1" in
        package)  echo "A06a" ;;
        baseline) echo "W02" ;;
        live)     echo "W03" ;;
        final)    echo "A06/E05" ;;
        *)        echo "" ;;
    esac
}
owner=$(gate_owner "$gate")
if [[ -z "$owner" ]]; then
    echo "Unknown --gate '$gate'. Expected package, baseline, live or final." >&2
    exit 2
fi

IFS=',' read -r -a wanted_components <<< "$components"
for comp in "${wanted_components[@]}"; do
    case "$comp" in
        core|file-transport|qt) ;;
        *) echo "Unknown component '$comp'. Expected core, file-transport or qt." >&2; exit 2 ;;
    esac
done

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
producer_revision=$(git -C "$repo_root" rev-parse HEAD 2>/dev/null || echo unknown)

if [[ -z "$output_dir" ]]; then
    output_dir="$repo_root/build-consumer-verify/$mode-$gate-$(date -u +%Y%m%dT%H%M%SZ)-$$"
fi
mkdir -p "$output_dir"
run_log="$output_dir/run.log"
: > "$run_log"

# Keep compiler temporaries inside the tree we own. /tmp may be full or shared,
# and a "No space left on device" half way through a verification is not evidence
# about the thing under test.
export TMPDIR="$output_dir/tmp"
mkdir -p "$TMPDIR"

exec > >(tee -a "$run_log") 2>&1

step_status=0
steps_file="$output_dir/steps.txt"
: > "$steps_file"

note_step() { printf '%s\n' "$1" >> "$steps_file"; }

run_step() {   # run_step <name> <command...>
    local name="$1"; shift
    printf '\n=== %s: %s\n' "$name" "$*"
    if "$@"; then
        note_step "$name: ok"
    else
        local rc=$?
        note_step "$name: FAILED ($rc)"
        step_status=$rc
        printf '!!! %s failed with %d\n' "$name" "$rc"
        return $rc
    fi
}

finish() {
    # Take the status as an argument. Reading $? here looked equivalent and was
    # not: on every explicit `finish 3` failure path $? was the status of the
    # preceding echo, so a failed check printed its own diagnostic and then
    # exited zero with PASS. A verification script that can report success after
    # a failure it just described is worse than no script.
    local rc="${1:-$?}"
    [[ $rc -eq 0 && $step_status -eq 0 ]] || rc=${rc:-1}
    if [[ $rc -eq 0 && $step_status -eq 0 ]]; then
        printf '\nverify-consumer-package: PASS (%s/%s)\n' "$mode" "$gate"
    else
        printf '\nverify-consumer-package: FAIL (%s/%s, last status %s)\n' "$mode" "$gate" "$step_status"
    fi

    python3 - "$output_dir/results.json" "$steps_file" <<PY
import json, pathlib, sys
steps = [l for l in pathlib.Path(sys.argv[2]).read_text().splitlines() if l.strip()]
json.dump({
    "schema": "collabtext-consumer-package-results/1",
    "tool": "tools/verify-consumer-package.sh",
    "mode": "$mode",
    "gate": "$gate",
    "gate_owner": "$owner",
    "components_requested": "${wanted_components[*]}",
    "producer_revision": "$producer_revision",
    "repo_root": "$repo_root",
    "output_dir": "$output_dir",
    "capability_manifest_version": "${manifest_version:-}",
    "steps": steps,
    # rc, not step_status: they disagree on the paths that fail before any
    # run_step, and the recorded value has to be the one the caller saw.
    "exit_status": $rc,
}, open(sys.argv[1], "w"), indent=2)
PY
    printf 'evidence: %s\n' "$output_dir"
    exit $rc
}
trap finish EXIT

printf 'Root:      %s\nMode:      %s\nGate:      %s (%s)\nComponents: %s\nRevision:  %s\nOutput:    %s\n' \
    "$repo_root" "$mode" "$gate" "$owner" "${wanted_components[*]}" "$producer_revision" "$output_dir"

# ---------------------------------------------------------------------------
# Gate dispatch
# ---------------------------------------------------------------------------
case "$gate" in
    package)
        ;;
    baseline)
        step_status=4
        echo "!! --gate baseline is owned by W02 (installed consumer probe matrix) and is not implemented yet." >&2
        exit 4
        ;;
    live)
        step_status=4
        echo "!! --gate live is owned by W03 (strict installed live-position probes) and is not implemented yet." >&2
        exit 4
        ;;
    final)
        step_status=4
        echo "!! --gate final is owned by A06/E05 (complete promised probe set) and is not implemented yet." >&2
        exit 4
        ;;
esac

# ---------------------------------------------------------------------------
# Producer configuration
# ---------------------------------------------------------------------------
producer_build="$output_dir/producer-build"
prefix_a="$output_dir/prefix-a"
prefix_b="$output_dir/prefix-b"
consumer_build="$output_dir/consumer-build"

build_type=Debug
sanitizers=OFF
case "$mode" in
    release) build_type=Release ;;
    asan)    build_type=Debug; sanitizers=ON ;;
esac

want_transport=OFF
want_qt=OFF
want_apps=OFF
for comp in "${wanted_components[@]}"; do
    case "$comp" in
        file-transport) want_transport=ON ;;
        qt)             want_qt=ON; want_apps=ON ;;
    esac
done

producer_flags=(
    "-DCMAKE_BUILD_TYPE=$build_type"
    "-DCOLLABTEXT_ENABLE_SANITIZERS=$sanitizers"
    "-DCOLLABTEXT_BUILD_TESTS=OFF"
    "-DCOLLABTEXT_BUILD_APPS=$want_apps"
    "-DCOLLABTEXT_BUILD_QT=$want_qt"
    "-DCOLLABTEXT_BUILD_FILE_TRANSPORT=$want_transport"
    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
)
# A sanitizer run has to instrument *both* sides of the check. The producer's
# flags are set with add_compile_options/add_link_options inside its own tree and
# do not reach a separate consumer project, so the consumer is configured with
# them explicitly. Passing them only to the producer produces an ASan build whose
# consumer fails to link with undefined __asan_* symbols -- a failure that looks
# like a packaging bug and is really a harness bug.
if [[ "$mode" == "asan" ]]; then
    sanitizer_flags="-fsanitize=address,undefined -fno-omit-frame-pointer"
    producer_flags+=("-DCMAKE_CXX_FLAGS=$sanitizer_flags"
                     "-DCMAKE_EXE_LINKER_FLAGS=$sanitizer_flags")
fi

if [[ -n "$producer_build_in" ]]; then
    if [[ ! -f "$producer_build_in/CMakeCache.txt" ]]; then
        echo "!! --producer-build '$producer_build_in' is not a configured CMake tree" >&2
        trap - EXIT; finish 3
    fi
    # Build it anyway: it may be stale, and a stale install is the one failure
    # mode this script must not have.
    producer_build="$producer_build_in"
    run_step producer-build cmake --build "$producer_build" --parallel "$(nproc)" \
        || { trap - EXIT; finish 3; }
else
    run_step producer-configure cmake -S "$repo_root" -B "$producer_build" "${producer_flags[@]}" \
        || { trap - EXIT; finish 3; }
    run_step producer-build cmake --build "$producer_build" --parallel "$(nproc)" \
        || { trap - EXIT; finish 3; }
fi

if [[ "$mode" == "asan" ]]; then
    # The sanitizer runner's rule, applied here: prove instrumentation is actually
    # on rather than trusting the flag. A silent -fsanitize=OFF would make an
    # ASan run indistinguishable from a Debug one.
    if ! grep -q 'fsanitize=address' "$producer_build/compile_commands.json"; then
        echo "!!! asan mode requested but no -fsanitize=address in the producer compile commands" >&2
        trap - EXIT; finish 3
    fi
fi

# ---------------------------------------------------------------------------
# Install to A, then MOVE to B
# ---------------------------------------------------------------------------
run_step install-prefix-a cmake --install "$producer_build" --prefix "$prefix_a" \
    || { trap - EXIT; finish 3; }

run_step relocate-prefix run_step relocate mv "$prefix_a" "$prefix_b" \
    || { trap - EXIT; finish 3; }

if [[ -e "$prefix_a" ]]; then
    echo "!!! prefix A still exists after the move; the consumer could resolve back to it" >&2
    trap - EXIT; finish 3
fi

manifest="$prefix_b/include/collabtext/ALPHA_CAPABILITIES.md"
if [[ ! -f "$manifest" ]]; then
    echo "!!! the capability manifest was not installed beside the headers" >&2
    trap - EXIT; finish 3
fi
manifest_version=$(sed -n 's/^\*\*Version:\*\* *//p' "$manifest" | head -1)
printf 'capability manifest: version %s at %s\n' "${manifest_version:-unknown}" "$manifest"

# ---------------------------------------------------------------------------
# Consumer, against prefix B only
# ---------------------------------------------------------------------------
consumer_flags=(
    "-DCMAKE_PREFIX_PATH=$prefix_b"
    "-DCMAKE_BUILD_TYPE=$build_type"
    # Needed so the leakage check below has something to inspect.
    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
)
if [[ "$mode" == "asan" ]]; then
    consumer_flags+=("-DCMAKE_CXX_FLAGS=$sanitizer_flags"
                     "-DCMAKE_EXE_LINKER_FLAGS=$sanitizer_flags")
fi
# Ask for exactly the components the producer was configured with, so a missing
# component is a configure error here rather than a silent link surprise later.
for comp in "${wanted_components[@]}"; do
    consumer_flags+=("-DCONSUMER_WANT_TRANSPORT=OFF")
done
[[ "$want_transport" == "ON" ]] && consumer_flags[-1]="-DCONSUMER_WANT_TRANSPORT=ON"
[[ "$want_qt" == "ON" ]] && consumer_flags+=("-DCONSUMER_WANT_QT=ON")

# Core must be findable with no Qt, no zstd and no crypto provider discoverable.
# This is the negative-discovery check the gate requires, and it is only
# meaningful for a Core-only consumer.
if [[ "$want_transport" != "ON" && "$want_qt" != "ON" ]]; then
    consumer_flags+=(
        "-DCMAKE_DISABLE_FIND_PACKAGE_Qt6=ON"
        "-DCMAKE_DISABLE_FIND_PACKAGE_ZSTD=ON"
        "-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON"
    )
    printf 'negative discovery: Qt6, zstd and OpenSSL disabled for the consumer\n'
fi

run_step consumer-configure cmake -S "$repo_root/tests/consumer-smoke" -B "$consumer_build" "${consumer_flags[@]}" \
    || { trap - EXIT; finish 3; }
run_step consumer-build cmake --build "$consumer_build" --parallel "$(nproc)" \
    || { trap - EXIT; finish 3; }

# ---------------------------------------------------------------------------
# The consumer must be using the installation and only the installation
#
# This is the check that would otherwise be missing. A consumer inside the
# source tree compiles happily against whatever it can reach: an inherited
# include path from a producer target, a source-tree -I, a link to the build
# directory. Every one of those produces a green build that proves nothing
# about the install tree, so the acceptance criterion is checked directly
# rather than inferred from the tests passing.
# ---------------------------------------------------------------------------
printf '\n=== no source-tree or build-tree leakage into the consumer\n'
consumer_cc="$consumer_build/compile_commands.json"
if [[ ! -f "$consumer_cc" ]]; then
    echo "!!! the consumer produced no compile_commands.json; nothing to inspect" >&2
    trap - EXIT; finish 3
fi
leaks=$(grep -oE '(-I|-isystem)[^ ]*' "$consumer_cc" | tr ' ' '\n' \
        | sed 's/^-I//; s/^-isystem//' | sort -u \
        | grep -E "^$repo_root/(libs/collabtext/(src|include)|cmake)|^$prefix_a" || true)
if [[ -n "$leaks" ]]; then
    echo "!!! the consumer compiles against paths it should not have:" >&2
    printf '  %s\n' "$leaks" >&2
    trap - EXIT; finish 3
fi
link_line=$(cat "$consumer_build"/CMakeFiles/*/link.txt 2>/dev/null || true)
leak_links=$(printf '%s' "$link_line" | grep -oE "$repo_root/(libs/collabtext|build-roadmap[^ /]*)[^ ]*" | sort -u || true)
if [[ -n "$leak_links" ]]; then
    echo "!!! the consumer links against source or build tree artifacts:" >&2
    printf '  %s\n' "$leak_links" >&2
    trap - EXIT; finish 3
fi
note_step "consumer-uses-install-tree-only: ok"
echo "include paths used by the consumer:"
python3 -c "
import json,collections
paths=collections.Counter()
for e in json.load(open('$consumer_cc')):
    parts=e.get('arguments') or e['command'].split()
    for i,a in enumerate(parts):
        if a in ('-I','-isystem'):
            paths[parts[i+1]]+=1
for p,n in sorted(paths.items()): print(f'  {n:3d} x {p}')
"
if ! printf '%s' "$link_line" | grep -q "$prefix_b"; then
    echo "!!! the consumer links nothing from prefix B; it did not use the install" >&2
    trap - EXIT; finish 3
fi
note_step "consumer-links-prefix-b: ok"

run_step consumer-test ctest --test-dir "$consumer_build" --output-on-failure --no-tests=error \
    || { trap - EXIT; finish 3; }

# ---------------------------------------------------------------------------
# Negative check: an unknown component must be refused
# ---------------------------------------------------------------------------
neg_build="$output_dir/consumer-negative"
printf '\n=== negative: requesting a component that does not exist must fail\n'
if cmake -S "$repo_root/tests/consumer-smoke" -B "$neg_build" \
        "-DCMAKE_PREFIX_PATH=$prefix_b" -DCONSUMER_WANT_MISSING=ON \
        > "$output_dir/negative-component.log" 2>&1; then
    echo "!!! configuring with an unknown component succeeded; it must fail loudly" >&2
    trap - EXIT; finish 3
fi
note_step "negative-unknown-component: ok (refused as expected)"
printf 'refused as expected\n'

printf '\nAll steps passed.\n'
