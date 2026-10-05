#!/usr/bin/env bash
# Changed-path selection is tested in test_verify_changed.py. Use one shared
# configuration/build traversal; benchmarks are never part of verification.
set -euo pipefail
repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
build_dir="$repo_root/build-roadmap-debug"
source "$repo_root/tools/verification-env.sh"
exec python3 "$repo_root/tools/verify_changed.py" "$@"
