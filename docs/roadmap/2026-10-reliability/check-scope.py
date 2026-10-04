"""Check the roadmap registry against the constraints D01 sets.

D01's acceptance requires that no task *requires* a Rust rewrite, Yrs adoption,
a backend substitution experiment, or legacy-data migration. This script makes
that claim checkable instead of asserted: it scans every task's text for the
sensitive vocabulary and classifies each hit by the sentence it appears in.

A hit is only a violation when the surrounding text asks for the thing. "no
porting shim or data migration is required" and "do not launch a Rust/backend
experiment" are prohibitions, which is exactly what the constraints want. So the
script looks at each hit in context and reports it for review; it does not try to
grade English.

Usage:
    python3 docs/roadmap/2026-10-reliability/check-scope.py            # report
    python3 docs/roadmap/2026-10-reliability/check-scope.py --strict   # fail if
                                                                     # any hit is
                                                                     # unreviewed

Exit status: 0 clean or all hits classified as prohibitions, 1 otherwise.
"""

import argparse
import json
import re
import sys
from pathlib import Path

REGISTRY = Path(__file__).with_name("TASKS.json")

# Vocabulary that would indicate a forbidden direction if it appeared as a
# requirement rather than as a prohibition.
SENSITIVE = {
    "rust": "a Rust rewrite",
    "yrs": "Yrs adoption",
    "yjs": "Yjs adoption",
    "rewrite": "a rewrite",
    "backend": "a backend experiment or substitution",
    "migration": "legacy-data migration",
    "migrate": "legacy-data migration",
    "importer": "a legacy importer",
    "shim": "a compatibility shim",
}

# Words that make the surrounding sentence a prohibition or a negation rather
# than a requirement.
PROHIBITION_MARKERS = (
    "no ", "not ", "never", "without", "forbid", "prohibit", "do not", "does not",
    "is not", "are not", "rather than", "instead of", "outside", "out of scope",
    "no additional", "nothing",
)

# Legitimate uses of the vocabulary, each with the reason it is legitimate. These
# are visible on purpose: an exception nobody can see is an exception nobody can
# review. Adding one is a decision, not a convenience.
ALLOWED_PHRASES = (
    # E04 rewrites the architecture *documentation* around what is implemented.
    # Rewriting prose is not rewriting the engine.
    ("rewrite current architecture", "documentation rewrite, not an engine rewrite"),
)


def classify(context: str) -> str:
    lowered = context.lower()
    for phrase, _reason in ALLOWED_PHRASES:
        if phrase in lowered:
            return "prohibition"
    if any(marker in lowered for marker in PROHIBITION_MARKERS):
        return "prohibition"
    return "REVIEW"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--strict", action="store_true",
                        help="exit nonzero if any hit needs review")
    parser.add_argument("--context", type=int, default=110,
                        help="characters of context to show per hit")
    args = parser.parse_args()

    registry = json.loads(REGISTRY.read_text())
    tasks = registry["tasks"]

    counts = {"prohibition": 0, "REVIEW": 0}
    unreviewed = []
    for task in tasks:
        blob = json.dumps(task)
        lowered = blob.lower()
        for term, description in SENSITIVE.items():
            # Word boundaries: "trusted" is not a Rust rewrite, and "migrated
            # fuzz suite" is not legacy-data migration.
            for match in re.finditer(r"\b" + re.escape(term) + r"\b", lowered):
                start = max(0, match.start() - args.context)
                end = min(len(blob), match.end() + args.context)
                verdict = classify(blob[start:end])
                counts[verdict] += 1
                if verdict == "REVIEW":
                    unreviewed.append((task["id"], description,
                                       blob[start:end].replace("\\n", " ")))

    print(f"scanned {len(tasks)} tasks for forbidden directions")
    print(f"  {counts['prohibition']} hit(s) in prohibitive or explicitly "
          f"allowed context")
    if ALLOWED_PHRASES:
        print("  allowed phrases:")
        for phrase, reason in ALLOWED_PHRASES:
            print(f"    {phrase!r}: {reason}")
    print(f"  {counts['REVIEW']} hit(s) needing review")
    if unreviewed:
        print("\nhits needing review:")
        for task_id, description, context in unreviewed:
            print(f"\n[{task_id}] {description}\n  ...{context}...")
        print("\nEach of these must state a requirement, not a prohibition. "
              "Fix the task text or extend the classification below.")
    else:
        print("\nNo task requires a Rust rewrite, Yrs adoption, a backend "
              "experiment, or legacy-data migration.")

    return 1 if (args.strict and unreviewed) else 0


if __name__ == "__main__":
    sys.exit(main())
