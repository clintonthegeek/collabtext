#!/usr/bin/env bash
# prepare-integration-alpha.sh — prepare a reviewable collabtext alpha candidate.
# Roadmap task E07a (package alpha) and E07b (position-capable alpha).
#
# WHAT THIS DOES
#   Requires the intended branch and a clean tree, records the exact candidate
#   commit, the compiler and options used, dependency versions, the installed
#   package/component list, the capability manifest, the serialized-format
#   identifiers, and the verification evidence that was gathered at this very
#   commit. It stages the install tree locally, checksums it, and writes a
#   reviewable publication checklist.
#
# WHAT THIS NEVER DOES
#   It never pushes, never creates a remote tag or release, never publishes a
#   package, never merges master, and never contacts anyone. There is no flag
#   that does any of those things and no code path that could. Publication is a
#   separate, separately authorized action; the checklist exists so that action
#   can be reviewed *before* it happens.
#
# WHY THE EVIDENCE IS MANDATORY
#   An alpha candidate is a claim about a specific commit: "this SHA builds,
#   installs, relocates and passes the probes." A candidate prepared from
#   evidence gathered at a different SHA is not that claim, it is a plausible
#   story about one. So the script refuses to produce a candidate unless the
#   retained results.json of every required mode names this exact commit and
#   exited zero. There is no --skip-evidence.
#
# Exit codes: 0 the candidate is prepared; 2 bad usage; 3 a precondition or step
# failed. As everywhere else in this repository, a check that can only report
# success is worse than no check, so every failure path ends in an explicit
# nonzero exit with a diagnostic.
set -uo pipefail

usage() {
    cat <<'USAGE'
Usage: tools/prepare-integration-alpha.sh [options]

  --version V        Package version to prepare. Default: read from the
                     superproject. Must equal libs/collabtext's project version.
  --tag NAME         Proposed immutable annotated tag. Default: v<version>.
                     Fails if the tag already exists locally.
  --branch NAME      Branch this candidate may be prepared from.
                     Default: master.
  --remote NAME      Destination remote recorded in the checklist. Default: origin.
                     Recorded only; never contacted.
  --components LIST  Comma-separated: core, file-transport, qt. Default: core.
  --gate GATE        Which gate the retained evidence must be from:
                     package (E07a) or live (E07b). Default: package.
  --require-modes L  Comma-separated modes that must each have passing evidence at
                     this commit. Default: debug,release,asan.
  --evidence DIR     Directory holding a verify-consumer-package.sh results.json.
                     Repeatable. Directories that hold no results.json are an
                     error rather than being skipped silently.
  --output-dir DIR   Where to stage. Default: build-alpha/integration-alpha-<utc>.
                     Must stay inside the worktree; it is gitignored.
  --reuse-producer P Use an already-configured, already-built producer tree
                     instead of configuring a new one, then install from it.
  --help             This text.

Outputs, all inside --output-dir:
  prefix/                 the staged install tree
  collabtext-<version>-<arch>.tar.gz   deterministic archive of prefix/
  SHA256SUMS              checksums for every staged file and the archive
  alpha-candidate.json    versioned machine-readable record of the candidate
  PUBLICATION-CHECKLIST.md  reviewable, NOT EXECUTED publication steps
  run.log                 everything this script printed

Publication is not performed here and is not performed by this script under any
options. See PUBLICATION-CHECKLIST.md after running it.
USAGE
}

version=""; tag=""; branch="master"; remote="origin"
components="core"; gate="package"; require_modes="debug,release,asan"
output_dir=""; producer_build_in=""
evidence_dirs=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --version|--tag|--branch|--remote|--components|--gate|--require-modes|--output-dir|--evidence|--reuse-producer)
            if [[ $# -lt 2 || "$2" == --* ]]; then
                echo "Missing value for $1" >&2
                exit 2
            fi ;;
    esac
    case "$1" in
        --version)      version="${2:-}"; shift 2 ;;
        --tag)          tag="${2:-}"; shift 2 ;;
        --branch)       branch="${2:-}"; shift 2 ;;
        --remote)       remote="${2:-}"; shift 2 ;;
        --components)   components="${2:-}"; shift 2 ;;
        --gate)         gate="${2:-}"; shift 2 ;;
        --require-modes) require_modes="${2:-}"; shift 2 ;;
        --output-dir)   output_dir="${2:-}"; shift 2 ;;
        --evidence)     evidence_dirs+=("${2:-}"); shift 2 ;;
        --reuse-producer) producer_build_in="${2:-}"; shift 2 ;;
        --help|-h)      usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

case "$gate" in
    package|live) ;;
    *) echo "Unknown --gate '$gate'. Expected package or live." >&2; exit 2 ;;
esac

want_transport=OFF
want_qt=OFF
want_apps=OFF
IFS=',' read -r -a wanted_components <<< "$components"
for comp in "${wanted_components[@]}"; do
    case "$comp" in
        core) ;;
        file-transport) want_transport=ON ;;
        qt)             want_qt=ON; want_apps=ON ;;
        *) echo "Unknown component '$comp'. Expected core, file-transport or qt." >&2; exit 2 ;;
    esac
done

IFS=',' read -r -a required_modes <<< "$require_modes"
for m in "${required_modes[@]}"; do
    case "$m" in
        debug|release|asan) ;;
        *) echo "Unknown mode '$m' in --require-modes." >&2; exit 2 ;;
    esac
done

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
[[ -d "$repo_root/.git" || -f "$repo_root/.git" ]] || {
    echo "!! $repo_root is not a git checkout" >&2; exit 3; }

# ---------------------------------------------------------------------------
# Preconditions. Each of these is a refusal, not a warning.
# ---------------------------------------------------------------------------
current_branch=$(git -C "$repo_root" branch --show-current)
if [[ -z "$current_branch" ]]; then
    echo "!! detached HEAD: a candidate must name the branch it came from" >&2
    exit 3
fi
if [[ "$current_branch" != "$branch" ]]; then
    echo "!! on branch '$current_branch', but this candidate may only be prepared from '$branch'" >&2
    echo "   Preparing from another branch would put a reviewable tag proposal on an" >&2
    echo "   unreviewed tree." >&2
    exit 3
fi

dirty=$(git -C "$repo_root" status --porcelain)
if [[ -n "$dirty" ]]; then
    echo "!! the worktree is not clean; a candidate is a claim about one exact tree" >&2
    printf '%s\n' "$dirty" | sed 's/^/   /' >&2
    exit 3
fi

candidate_sha=$(git -C "$repo_root" rev-parse HEAD)
candidate_sha_full=$(git -C "$repo_root" rev-parse "${candidate_sha}^{commit}")

# ---------------------------------------------------------------------------
# Version consistency. CMake's project(VERSION) is numeric only, so the release
# line and the SemVer prerelease label are two numbers and a consumer can observe
# either. Every one of them has to agree, in both CMakeLists.txt files and in the
# installed config file.
# ---------------------------------------------------------------------------
super_version=$(sed -n 's/^project(collabtext VERSION \([^ )]*\).*/\1/p' "$repo_root/CMakeLists.txt" | head -1)
super_prerelease=$(sed -n 's/^set(COLLABTEXT_PRERELEASE "\([^"]*\)").*$/\1/p' "$repo_root/CMakeLists.txt" | head -1)
lib_version=$(sed -n 's/^project(collabtext-lib VERSION \([^ )]*\).*/\1/p' "$repo_root/libs/collabtext/CMakeLists.txt" | head -1)
lib_prerelease=$(sed -n 's/^set(COLLABTEXT_PRERELEASE "\([^"]*\)").*$/\1/p' "$repo_root/libs/collabtext/CMakeLists.txt" | head -1)
if [[ -z "$super_version" || -z "$lib_version" || -z "$super_prerelease" || -z "$lib_prerelease" ]]; then
    echo "!! could not read the package version metadata." >&2
    echo "   superproject='${super_version:-}'+'${super_prerelease:-}' library='${lib_version:-}'+'${lib_prerelease:-}'" >&2
    exit 3
fi
if [[ "$super_version" != "$lib_version" || "$super_prerelease" != "$lib_prerelease" ]]; then
    echo "!! version disagreement between the two project() scopes:" >&2
    echo "   superproject $super_version + $super_prerelease" >&2
    echo "   library      $lib_version + $lib_prerelease" >&2
    echo "   COLLABTEXT_VERSION comes from the library project(), and a consumer" >&2
    echo "   reading the install would be told a different version than the tag says." >&2
    exit 3
fi
tree_version="$super_version-$super_prerelease"
if [[ -z "$version" ]]; then version="$tree_version"; fi
if [[ "$version" != "$tree_version" ]]; then
    echo "!! --version $version does not match the tree's $tree_version" >&2
    echo "   Bump both project() versions and both COLLABTEXT_PRERELEASE values in the" >&2
    echo "   same commit, or prepare the version the tree already carries." >&2
    exit 3
fi
if [[ -z "$tag" ]]; then tag="v$version"; fi

if git -C "$repo_root" rev-parse -q --verify "refs/tags/$tag" >/dev/null 2>&1; then
    echo "!! tag '$tag' already exists locally at $(git -C "$repo_root" rev-parse "refs/tags/$tag")" >&2
    echo "   An alpha tag is immutable. Choose a new prerelease number instead of" >&2
    echo "   moving an existing tag." >&2
    exit 3
fi

if [[ -z "$output_dir" ]]; then
    output_dir="$repo_root/build-alpha/integration-alpha-$(date -u +%Y%m%dT%H%M%SZ)-$$"
elif [[ "$output_dir" != /* ]]; then
    # Resolve a relative path against the invoking directory first. Rejecting it
    # outright was unhelpful: the caller has a perfectly good reason to name a
    # path next to the worktree, and the real constraint is only that the
    # staging tree stays inside the worktree where it is gitignored.
    output_dir="$(pwd -P)/$output_dir"
fi
case "$output_dir" in
    "$repo_root"/*) ;;
    *) echo "!! --output-dir must resolve inside the worktree ($repo_root); got '$output_dir'" >&2
       echo "   The staging tree is a build artifact and is gitignored; anything outside" >&2
       echo "   the worktree is outside this task's blast radius." >&2
       exit 2 ;;
esac
mkdir -p "$output_dir" || exit 3
output_dir=$(cd "$output_dir" && pwd -P)

build_dir=""
source "$repo_root/tools/verification-env.sh"
export TMPDIR="$output_dir/tmp"; mkdir -p "$TMPDIR"

run_log="$output_dir/run.log"
: > "$run_log"
exec > >(tee -a "$run_log") 2>&1

step_status=0
steps_file="$output_dir/steps.txt"
: > "$steps_file"
note_step() { printf '%s\n' "$1" >> "$steps_file"; }
run_step() {
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
    # Take the status as an argument. Reading $? here is the bug that once made
    # this repository's package gate print its own failure diagnostic and then
    # exit zero.
    local rc="${1:-$?}"
    if [[ $rc -eq 0 && $step_status -ne 0 ]]; then rc=$step_status; fi
    printf '\nprepare-integration-alpha: %s (candidate %s, version %s)\n' \
        "$([[ $rc -eq 0 ]] && echo "PREPARED (not published)" || echo "FAIL (exit $rc)")" \
        "$candidate_sha" "$version"
    printf 'evidence: %s\n' "$output_dir"
    [[ $rc -ne 0 ]] && printf 'No candidate was produced. Nothing was published.\n'
    exit $rc
}
trap finish EXIT

printf 'Root:       %s\nBranch:     %s\nCandidate:  %s\nVersion:    %s\nProposed tag: %s\n' \
    "$repo_root" "$current_branch" "$candidate_sha_full" "$version" "$tag"
printf 'Gate:       %s (evidence must cover: %s)\nComponents: %s\nRemote (recorded only): %s\nOutput:     %s\n' \
    "$gate" "$require_modes" "${wanted_components[*]}" "$remote" "$output_dir"

# ---------------------------------------------------------------------------
# Evidence. Every required mode must have a retained results.json that names this
# exact commit, ran the requested gate, and exited zero.
# ---------------------------------------------------------------------------
printf '\n=== verification evidence at %s\n' "$candidate_sha"
if [[ ${#evidence_dirs[@]} -eq 0 ]]; then
    echo "!! no --evidence directory was supplied." >&2
    echo "   An alpha candidate is a claim about this commit, so the gate results for" >&2
    echo "   this commit are required. Run, for example:" >&2
    echo "     tools/verify-consumer-package.sh --mode debug   --gate $gate --components core" >&2
    echo "     tools/verify-consumer-package.sh --mode release --gate $gate --components core" >&2
    echo "     tools/verify-consumer-package.sh --mode asan    --gate $gate --components core" >&2
    echo "   then pass each output directory to --evidence." >&2
    exit 3
fi

evidence_json="$output_dir/evidence.json"
python3 - "$candidate_sha" "$gate" "$evidence_json" "${required_modes[@]}" -- "${evidence_dirs[@]}" <<'PY'
import json, pathlib, sys
candidate, gate, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
sep = sys.argv.index('--')
modes = sys.argv[4:sep]
dirs = sys.argv[sep+1:]

problems, accepted, rejected = [], [], []
for d in dirs:
    p = pathlib.Path(d)
    if not p.is_dir():
        problems.append(f"{d}: not a directory")
        continue
    results = p / 'results.json'
    if not results.is_file():
        problems.append(f"{d}: no results.json -- is this a verify-consumer-package.sh output directory?")
        continue
    try:
        data = json.loads(results.read_text())
    except Exception as exc:
        problems.append(f"{d}: results.json is unreadable ({exc})")
        continue
    row = {
        'evidence_dir': str(p),
        'mode': data.get('mode'),
        'gate': data.get('gate'),
        'producer_revision': data.get('producer_revision'),
        'exit_status': data.get('exit_status'),
        'capability_manifest_version': data.get('capability_manifest_version'),
        'components': data.get('components_requested'),
    }
    why = None
    if data.get('producer_revision') != candidate:
        why = f"producer_revision {data.get('producer_revision')!r} is not the candidate {candidate}"
    elif data.get('gate') != gate:
        why = f"gate {data.get('gate')!r} is not the requested gate {gate!r}"
    elif data.get('exit_status') != 0:
        why = f"the gate itself exited {data.get('exit_status')!r}"
    if why:
        row['rejected_because'] = why
        rejected.append(row)
    else:
        accepted.append(row)

missing = []
for mode in modes:
    if not any(r['mode'] == mode for r in accepted):
        have = [r for r in accepted + rejected if r['mode'] == mode]
        detail = "; ".join(f"{r['evidence_dir']}: {r.get('rejected_because')}" for r in have) or "none supplied"
        missing.append(f"{mode} (have: {detail})")

json.dump({'candidate': candidate, 'gate': gate, 'required_modes': modes,
           'accepted': accepted, 'rejected': rejected, 'problems': problems,
           'missing_modes': missing},
          open(out_path, 'w'), indent=2)

for row in accepted:
    print(f"  accepted {row['mode']:8} {row['gate']:9} manifest v{row['capability_manifest_version']}  {row['evidence_dir']}")
for row in rejected:
    print(f"  REJECTED {row['mode']:8} {row['gate']:9} {row['rejected_because']}")
for problem in problems:
    print(f"  PROBLEM  {problem}")
if missing or problems:
    if missing:
        print("missing passing evidence for: " + ", ".join(missing))
    sys.exit(3)
print(f"  all required modes covered: {', '.join(modes)}")
PY
if [[ $? -ne 0 ]]; then
    trap - EXIT; finish 3
fi
note_step "verification-evidence-at-candidate: ok"

# ---------------------------------------------------------------------------
# Format identifiers. Read from the codecs, then compared with what the
# candidate claims. A release note that names the wrong wire version is exactly
# the kind of thing that is true when written and wrong later, so it is checked
# here rather than trusted.
# ---------------------------------------------------------------------------
printf '\n=== serialized format identifiers, read from the codecs\n'
declared_formats="CTBO/0002 CTLO/0002 CTGV/0001 CTGP/0001 CTPT/1"
discovered=$(python3 - "$repo_root" <<'PY'
import pathlib, re, sys
root = pathlib.Path(sys.argv[1])
found = {}
for path in (root / 'libs/collabtext/src/codec').glob('*.cpp'):
    text = path.read_text()
    # Four-byte magic immediately followed by the version digits and a colon.
    for magic, version in re.findall(r'"(CT[A-Z]{2})(\d{4}):', text):
        found[magic] = version
    # The binary position token carries its version differently.
    if 'CTPT' in text:
        m = re.search(r'CTPT[^\w]{0,4}?(\d)', text)
        found.setdefault('CTPT', m.group(1) if m else '1')
print(' '.join(f'{k}/{v}' for k, v in sorted(found.items())))
PY
)
printf '  declared:  %s\n  discovered:%s\n' "$declared_formats" " $discovered"
# Compare as sets. The order of a space-separated list is not a property of what
# the codecs implement, and an earlier version of this check compared the strings
# and so rejected a candidate whose declared and discovered sets were identical.
declared_sorted=$(printf '%s\n' $declared_formats | LC_ALL=C sort | tr '\n' ' ')
discovered_sorted=$(printf '%s\n' $discovered | LC_ALL=C sort | tr '\n' ' ')
if [[ "$declared_sorted" != "$discovered_sorted" ]]; then
    echo "!! the codecs do not implement the formats this candidate claims." >&2
    echo "   declared:  $declared_formats" >&2
    echo "   discovered: $discovered" >&2
    echo "   Fix the claim or the codec; do not ship release notes that disagree with" >&2
    echo "   the code they describe." >&2
    trap - EXIT; finish 3
fi
note_step "format-identifiers-match-codecs: ok"

# ---------------------------------------------------------------------------
# Supported-token declaration.
#
# Declaring a format version supported for consumer persistence is the one claim
# in a candidate that obliges *later* releases. It must therefore be backed by
# retained golden bytes that a reader actually reads, not by a sentence in a
# manifest. The installed manifest is checked for the declaration, and the
# declared token version must be the version the retained fixture covers.
# ---------------------------------------------------------------------------
printf '\n=== supported token version declaration\n'
token_fixture="$repo_root/libs/collabtext/tests/fixtures/position-tokens/ctpt-v1-supported.json"
# grep rather than sed: the pattern spans several capture-free groups and a sed
# substitution that only selects a line has no group to reference.
if grep -qE '^\| `CTPT` .*\| 1 \| \*\*SUPPORTED' \
        "$repo_root/libs/collabtext/include/collabtext/ALPHA_CAPABILITIES.md"; then
    declared_supported=CTPT/1
else
    declared_supported=""
fi
if [[ -z "$declared_supported" ]]; then
    printf '  this candidate declares no supported position-token version (CTPT/1 remains\n'
    printf '  experimental). That is a legitimate state for a package-only alpha.\n'
    token_supported="none"
else
    [[ -f "$token_fixture" ]] || {
        echo "!! the manifest declares CTPT/1 supported but $token_fixture does not exist." >&2
        echo "   A supported version without retained golden bytes is an obligation with" >&2
        echo "   nothing behind it." >&2
        trap - EXIT; finish 3
    }
    python3 - "$token_fixture" <<'PY' || { trap - EXIT; finish 3; }
import json, pathlib, sys
fixture = json.loads(pathlib.Path(sys.argv[1]).read_text())
if fixture.get('schema') != 'collabtext-position-token-goldens/1':
    sys.exit('unexpected retained position-token fixture schema')
if fixture.get('format') != 'CTPT' or fixture.get('version') != 1:
    sys.exit(f"the retained fixture covers {fixture.get('format')}/{fixture.get('version')}, not CTPT/1")
cases = fixture.get('cases') or []
kinds = {c['kind'] for c in cases}
missing = {'TextBoundary', 'TextAttachment', 'ListBoundary', 'ListElement'} - kinds
if missing:
    sys.exit(f'the retained fixture does not cover token kinds: {sorted(missing)}')
print(f"  CTPT/1 declared supported; retained fixture covers {len(cases)} cases, "
      f"kinds {sorted(kinds)}, since {fixture.get('supported_since')}")
print(f"  fixture sha256 recorded by this run: see alpha-candidate.json")
PY
    token_supported="CTPT/1"
    token_fixture_sha=$(sha256sum "$token_fixture" | cut -d' ' -f1)
    note_step "supported-token-has-retained-goldens: ok ($token_fixture_sha)"
fi

# ---------------------------------------------------------------------------
# Stage the install tree.
# ---------------------------------------------------------------------------
prefix="$output_dir/prefix"
producer_build="${producer_build_in:-$output_dir/producer-build}"

producer_flags=(
    "-DCMAKE_BUILD_TYPE=Release"
    "-DCOLLABTEXT_ENABLE_SANITIZERS=OFF"
    "-DCOLLABTEXT_BUILD_TESTS=OFF"
    "-DCOLLABTEXT_BUILD_APPS=$want_apps"
    "-DCOLLABTEXT_BUILD_QT=$want_qt"
    "-DCOLLABTEXT_BUILD_FILE_TRANSPORT=$want_transport"
    "-DCOLLABTEXT_BUILD_BENCHMARKS=OFF"
    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
    "-DCMAKE_INSTALL_PREFIX=$prefix"
)
producer_flags+=("${compiler_launcher_args[@]}")

producer_targets=(collabtext_core)
[[ "$want_transport" == "ON" ]] && producer_targets+=(collabtext_filetransport)
[[ "$want_qt" == "ON" ]] && producer_targets+=(collabtext_qtadapter collabtext_widgets)

if [[ -n "$producer_build_in" ]]; then
    if [[ ! -f "$producer_build_in/CMakeCache.txt" ]]; then
        echo "!! --reuse-producer '$producer_build_in' is not a configured CMake tree" >&2
        trap - EXIT; finish 3
    fi
    build_dir="$producer_build_in"
    run_step producer-build cmake --build "$build_dir" --parallel "$jobs" --target "${producer_targets[@]}" \
        || { trap - EXIT; finish 3; }
else
    build_dir="$producer_build"
    run_step producer-configure cmake -S "$repo_root" -B "$build_dir" "${producer_flags[@]}" \
        || { trap - EXIT; finish 3; }
    run_step producer-build cmake --build "$build_dir" --parallel "$jobs" --target "${producer_targets[@]}" \
        || { trap - EXIT; finish 3; }
fi

compiler_path=$(sed -n 's/^CMAKE_CXX_COMPILER:[^=]*=//p' "$build_dir/CMakeCache.txt")
compiler_version=$("$compiler_path" --version 2>/dev/null | head -1)
cmake_version=$(cmake --version | head -1)

run_step install cmake --install "$build_dir" --prefix "$prefix" || { trap - EXIT; finish 3; }

installed_version=$(sed -n 's/^set(COLLABTEXT_VERSION "\([^"]*\)").*$/\1/p' \
    "$prefix"/lib*/cmake/collabtext/collabtext-config.cmake | head -1)
installed_prerelease=$(sed -n 's/^set(COLLABTEXT_PRERELEASE "\([^"]*\)").*$/\1/p' \
    "$prefix"/lib*/cmake/collabtext/collabtext-config.cmake | head -1)
installed_version_string=$(sed -n 's/^set(COLLABTEXT_VERSION_STRING "\([^"]*\)").*$/\1/p' \
    "$prefix"/lib*/cmake/collabtext/collabtext-config.cmake | head -1)
if [[ "$installed_version" != "$super_version" || "$installed_prerelease" != "$super_prerelease" ]]; then
    echo "!! the installed package reports ${installed_version:-unknown}+${installed_prerelease:-none}," >&2
    echo "   expected $super_version+$super_prerelease. A consumer reads these from" >&2
    echo "   exactly this file, after it has been installed and moved." >&2
    trap - EXIT; finish 3
fi
if [[ "$installed_version_string" != "$version" ]]; then
    echo "!! installed COLLABTEXT_VERSION_STRING is '${installed_version_string:-<none>}', expected '$version'" >&2
    trap - EXIT; finish 3
fi
note_step "installed-version-matches-candidate: ok ($installed_version_string)"

installed_manifest="$prefix/include/collabtext/ALPHA_CAPABILITIES.md"
if [[ ! -f "$installed_manifest" ]]; then
    echo "!! the capability manifest was not installed beside the headers" >&2
    trap - EXIT; finish 3
fi
manifest_version=$(sed -n 's/^\*\*Version:\*\* *//p' "$installed_manifest" | head -1)
manifest_package_version=$(sed -n 's/^\*\*Package version at this manifest revision:\*\* `\(.*\)`.*/\1/p' "$installed_manifest" | head -1)
if [[ "$manifest_package_version" != "$version" ]]; then
    echo "!! the installed manifest names package version '${manifest_package_version:-<none>}', expected '$version'" >&2
    echo "   AGENTS.md requires the manifest to change in the same commit as behaviour," >&2
    echo "   and a manifest describing a different build is worse than none." >&2
    trap - EXIT; finish 3
fi
printf 'capability manifest: version %s, package version %s\n' "$manifest_version" "$manifest_package_version"

enabled_components=$(sed -n 's/^set(_collabtext_enabled_components "\([^"]*\)").*$/\1/p' \
    "$prefix"/lib*/cmake/collabtext/collabtext-config.cmake | head -1)
printf 'exported components: %s\n' "${enabled_components:-<unknown>}"
for comp in "${wanted_components[@]}"; do
    case "$comp" in
        core) ;;
        file-transport|qt)
            if [[ " $enabled_components " != *" $comp "* ]]; then
                echo "!! component '$comp' was requested but is not in the installed export" >&2
                trap - EXIT; finish 3
            fi ;;
    esac
done
note_step "installed-component-list: ok (${enabled_components:-unknown})"

installed_headers=$(cd "$prefix/include/collabtext" && ls *.h 2>/dev/null | sort | tr '\n' ' ')
installed_targets=$(cd "$prefix"/lib*/cmake/collabtext && ls *Targets.cmake 2>/dev/null | sort | tr '\n' ' ')

# ---------------------------------------------------------------------------
# The staged tree is consumed before it is checksummed. An archive that nothing
# has ever linked against is a file, not an artifact.
# ---------------------------------------------------------------------------
consumer_build="$output_dir/consumer-build"
consumer_flags=(
    "-DCMAKE_PREFIX_PATH=$prefix"
    "-DCMAKE_BUILD_TYPE=Release"
    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
)
consumer_flags+=("${compiler_launcher_args[@]}")
[[ "$want_transport" == "ON" ]] && consumer_flags+=("-DCONSUMER_WANT_TRANSPORT=ON")
[[ "$want_qt" == "ON" ]] && consumer_flags+=("-DCONSUMER_WANT_QT=ON")
if [[ "$want_transport" != "ON" && "$want_qt" != "ON" ]]; then
    consumer_flags+=("-DCMAKE_DISABLE_FIND_PACKAGE_Qt6=ON"
                     "-DCMAKE_DISABLE_FIND_PACKAGE_ZSTD=ON"
                     "-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON")
fi
run_step consumer-configure cmake -S "$repo_root/tests/consumer-smoke" -B "$consumer_build" "${consumer_flags[@]}" \
    || { trap - EXIT; finish 3; }
run_step consumer-build cmake --build "$consumer_build" --parallel "$jobs" \
    || { trap - EXIT; finish 3; }
run_step consumer-test ctest --test-dir "$consumer_build" --output-on-failure --no-tests=error \
    || { trap - EXIT; finish 3; }
note_step "staged-tree-consumed-by-an-independent-consumer: ok"

# ---------------------------------------------------------------------------
# Checksums. The archive is deterministic, so a checksum here means something to
# somebody rebuilding on another machine. tar's own ordering and mtime are pinned
# rather than left to the filesystem.
# ---------------------------------------------------------------------------
printf '\n=== staging a deterministic archive and checksums\n'
arch=$(uname -m)
archive_name="collabtext-$version-$arch.tar.gz"
archive="$output_dir/$archive_name"
( cd "$prefix" && tar --sort=name --owner=0 --group=0 --numeric-owner \
      --mtime="@0" -czf "$archive" . ) || { trap - EXIT; finish 3; }

sha_file="$output_dir/SHA256SUMS"
( cd "$prefix" && find . -type f | LC_ALL=C sort | xargs -r sha256sum ) > "$sha_file" \
    || { trap - EXIT; finish 3; }
archive_sum=$(sha256sum "$archive" | cut -d' ' -f1)
printf '%s  %s\n' "$archive_sum" "$archive_name" >> "$sha_file"
printf '  %s  %s\n' "$archive_sum" "$archive_name"
printf '  %s entries in SHA256SUMS\n' "$(wc -l < "$sha_file")"

# Recompute every staged file and compare. A checksum that was written and never
# read is a decoration. SHA256SUMS holds prefix-relative paths plus one entry
# naming the archive, so it is verified twice from the two directories those
# paths are relative to -- running sha256sum -c over the whole file from inside
# the prefix fails on the archive line and looks like a corrupted artifact.
( cd "$prefix" && grep -v "  $archive_name\$" "$sha_file" | sha256sum -c --quiet - ) \
    || { echo "!!! staged prefix checksums do not verify" >&2; trap - EXIT; finish 3; }
( cd "$output_dir" && grep "  $archive_name\$" "$sha_file" | sha256sum -c --quiet - ) \
    || { echo "!!! the archive checksum does not verify" >&2; trap - EXIT; finish 3; }
note_step "staged-artifact-checksums: ok"

# ---------------------------------------------------------------------------
# Toolchain and dependency record
#
# Every generated artifact below is written by a child process, and a child that
# fails must not leave a half-written record behind a "wrote ..." line. An
# earlier version of this script did exactly that: the JSON emitter raised, the
# message still printed, and the script exited 0. So each one is guarded, and a
# generator failure is a failure of this script.
# ---------------------------------------------------------------------------
if ! dep_record=$(python3 - <<PY
import json, subprocess
def pkg(name):
    try:
        out = subprocess.run(['pkg-config', '--modversion', name],
                             capture_output=True, text=True, timeout=15)
    except (OSError, subprocess.SubprocessError):
        return 'not probed (pkg-config unavailable)'
    return out.stdout.strip() if out.returncode == 0 else 'not installed'
print(json.dumps({name: pkg(name) for name in
                  ['libzstd', 'Qt6Core', 'Qt6Widgets', 'openssl']}, indent=2))
PY
); then
    echo "!! could not record dependency versions; no candidate produced" >&2
    trap - EXIT; finish 3
fi

# ---------------------------------------------------------------------------
# alpha-candidate.json
# ---------------------------------------------------------------------------
# The producer options go through the environment rather than into a nested
# $(...) inside the heredoc. A parameter reference inside a nested command
# substitution's single-quoted Python string is never expanded by the shell, so
# the emitted JSON contained the literal text "${producer_flags[*]}" -- which
# looks like a plausible value and is not one.
producer_options_flat=$(printf '%s\n' "${producer_flags[@]}")
export COLLABTEXT_PREP_PRODUCER_OPTIONS="$producer_options_flat"

candidate_json="$output_dir/alpha-candidate.json"
run_step candidate-manifest python3 - "$candidate_json" "$evidence_json" <<PY || { trap - EXIT; finish 3; }
import json, os, sys
json.dump({
  "schema": "collabtext-alpha-candidate/1",
  "tool": "tools/prepare-integration-alpha.sh",
  "prepared": True,
  "published": False,
  "publication_note": "Preparation only. Nothing was pushed, tagged remotely, released, "
                      "merged or messaged. Publication requires a separate user instruction.",
  "candidate": {
    "sha": "$candidate_sha_full",
    "short_sha": "$candidate_sha",
    "branch": "$current_branch",
    "tree_clean": True,
    "version": "$version",
    "version_kind": "SemVer prerelease; older than $super_version without the suffix",
    "version_note": "CMake project(VERSION) is numeric only, so collabtext-config-version.cmake "
                    "reports $super_version. A find_package request cannot name the prerelease "
                    "label; COLLABTEXT_VERSION_STRING carries $version.",
    "proposed_tag": "$tag",
    "tag_exists_locally": False,
    "remote_tag_state": "not checked -- this script performs no network access",
    "destination_remote_recorded": "$remote",
  },
  "package": {
    "components_requested": "${wanted_components[*]}",
    "components_exported": "${enabled_components:-unknown}",
    "targets": "${installed_targets:-}".strip(),
    "headers": "${installed_headers:-}".strip(),
    "installed_version": "$installed_version",
    "installed_prerelease": "$installed_prerelease",
    "installed_version_string": "$installed_version_string",
    "archive": "$archive_name",
    "archive_sha256": "$archive_sum",
    "checksums": "SHA256SUMS",
  },
  "toolchain": {
    "compiler": "$compiler_path",
    "compiler_version": "$compiler_version",
    "cmake": "$cmake_version",
    "jobs": "$jobs",
    "ccache": "${CCACHE_DIR:-not used}",
    "producer_build_type": "Release",
    "producer_options": os.environ["COLLABTEXT_PREP_PRODUCER_OPTIONS"].splitlines(),
    "dependencies": json.loads('''$dep_record'''),
  },
  "capability_manifest": {
    "version": "$manifest_version",
    "package_version": "$manifest_package_version",
    "installed_path": "<prefix>/include/collabtext/ALPHA_CAPABILITIES.md",
  },
  "serialized_formats": {
    "declared": "$declared_formats",
    "discovered_in_codecs": "$discovered",
    "supported_for_durable_storage": "$token_supported",
    "supported_token_golden_fixture": "${token_fixture:-none}",
    "supported_token_fixture_sha256": "${token_fixture_sha:-}",
    "note": "CTPT/1 is a supported position-token version when listed above: a later "
            "supported release keeps its reader and these bytes, or translates them. "
            "It is not a pin and does not survive reclamation, an epoch change, "
            "discard_local_view() or store recovery. No store or checkpoint format exists.",
  },
  "verification_evidence": json.load(open(sys.argv[2])),
  "known_limits": [
    "Not a supported release and not published.",
    "C++ API/ABI is unstable between candidates; pin the SHA and rebuild.",
    "No crash recovery, no persistent undo/redo, no durable pins, no GC (G-series pending).",
    "CTPT/1 is a supported serialized token version, but a token is not a pin: it does not survive reclamation, an epoch change, discard_local_view() or store recovery, and physical GC is disabled.",
    "Qt widget layer and file transport are optional and not part of the headless Core path.",
  ],
}, open("$candidate_json", "w"), indent=2)
PY

# A record that still carries a shell metavariable is a record whose expansion
# was missed. It parses, it looks plausible, and it is wrong -- so refuse it
# rather than shipping it into a release dossier.
if grep -q '\${' "$candidate_json"; then
    echo "!!! $candidate_json still contains an unexpanded shell reference:" >&2
    grep -n '\${' "$candidate_json" | sed 's/^/    /' >&2
    trap - EXIT; finish 3
fi
python3 -c 'import json,sys; json.load(open(sys.argv[1]))' "$candidate_json" \
    || { echo "!!! $candidate_json is not valid JSON" >&2; trap - EXIT; finish 3; }
note_step "candidate-manifest-complete-and-parsed: ok"
printf '\nwrote %s\n' "$candidate_json"

# ---------------------------------------------------------------------------
# PUBLICATION-CHECKLIST.md -- reviewable, and deliberately not executed.
# ---------------------------------------------------------------------------
checklist="$output_dir/PUBLICATION-CHECKLIST.md"
cat > "$checklist" <<CHECKLIST
# Publication checklist — $tag (NOT EXECUTED)

**This file is a proposal. Nothing in it has been done.** The preparation script
that wrote it performs no network access of any kind. Each step below becomes
authorized only by an explicit user instruction.

- Candidate SHA: \`$candidate_sha_full\`
- Version: \`$version\` (SemVer prerelease)
- Branch: \`$current_branch\`
- Destination remote (recorded, never contacted by the script): \`$remote\`
- Archive: \`$archive_name\`, sha256 \`$archive_sum\`
- File-level checksums: \`SHA256SUMS\`
- Evidence: modes \`$require_modes\`, gate \`$gate\`, at this exact SHA
  (see \`alpha-candidate.json\` → \`verification_evidence\`)

## Steps a separately authorized publication would perform

1. Re-verify the clean candidate:
   \`git status --porcelain\` empty; \`git rev-parse HEAD\` equals the SHA above.
2. Confirm the destination remote and that the tag name is still unused:
   \`git ls-remote --tags $remote "refs/tags/$tag"\` must print nothing.
3. Confirm source accessibility. **A local worktree cannot be fetched by a
   consumer.** A consumer can only pin this SHA if the commit is reachable on the
   destination remote. Either the branch is pushed so the SHA is reachable, or
   the package is delivered another way. Do not report availability that has not
   been fetched and checked.
4. Create the immutable annotated tag and push it:
   \`git tag -a $tag -m "<annotation below>" $candidate_sha_full\`
   \`git push $remote $tag\`
   Never force-update a tag that already exists.
5. Verify from a clean fetch, not from this worktree:
   \`git clone $remote scratch && cd scratch && git checkout $candidate_sha_full\`
6. Run remote CI. Its result is **pending** until it has actually run; local
   evidence is not a substitute and this script did not trigger it.
7. Hand the user the pin/install/probe instructions in
   \`docs/releases/integration-alpha.md\`.

## Proposed tag annotation

\`\`\`
$tag

collabtext $version — integration alpha ($gate gate).

Candidate commit: $candidate_sha_full
Branch: $current_branch

Verified at this commit: installed package gate in $(echo "$require_modes" | tr ',' ' and ') mode(s);
capability manifest version $manifest_version; serialized formats $declared_formats.

This is an alpha, not a supported release. C++ API/ABI may change between
candidates. No serialized format is approved for durable consumer storage at this
candidate; CTPT/1 is experimental. No crash recovery, persistent undo, durable
pins or GC. See the installed capability manifest for what this can and cannot do.
\`\`\`

## What publishing this tag would NOT establish

- Not a reliability release, and not stage-4 consumer readiness.
- Not a support commitment for any serialized format.
- Not remote CI evidence until it has run.
- Not source availability while the commit is unreachable from the remote.
CHECKLIST
printf 'wrote %s\n' "$checklist"
note_step "publication-checklist-written-not-executed: ok"

printf '\nCandidate prepared. NOT published, NOT tagged remotely, NOT merged.\n'