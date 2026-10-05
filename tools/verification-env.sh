#!/usr/bin/env bash
# Sourced by verification scripts after repo_root is known. Cache only affects
# compilation; test execution and sanitizer instrumentation are never cached.
jobs=${COLLABTEXT_VERIFY_JOBS:-$(getconf _NPROCESSORS_ONLN)}
if [[ ! "$jobs" =~ ^[1-9][0-9]*$ ]]; then
    echo 'COLLABTEXT_VERIFY_JOBS must be a positive integer' >&2
    exit 2
fi
export TMPDIR=${TMPDIR:-"$repo_root/build-compiler-temp"}
mkdir -p "$TMPDIR"
compiler_launcher_args=()
if [[ ${COLLABTEXT_USE_CCACHE:-1} == 1 ]] && command -v ccache >/dev/null; then
    export CCACHE_DIR=${CCACHE_DIR:-"$repo_root/.cache/ccache"}
    export CCACHE_MAXSIZE=${CCACHE_MAXSIZE:-512M}
    export CCACHE_TEMPDIR=${CCACHE_TEMPDIR:-"$CCACHE_DIR/tmp"}
    mkdir -p "$CCACHE_TEMPDIR"
    # Preserve a deliberately configured different launcher (e.g. sccache).
    launcher=$(sed -n 's/^CMAKE_CXX_COMPILER_LAUNCHER:[^=]*=//p' "${build_dir:-}/CMakeCache.txt" 2>/dev/null || true)
    if [[ -z "$launcher" || "$launcher" == *ccache* ]]; then
        compiler_launcher_args=("-DCMAKE_CXX_COMPILER_LAUNCHER=$(command -v ccache)")
    fi
fi
