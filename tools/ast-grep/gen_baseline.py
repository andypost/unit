#!/usr/bin/env python3
"""
Generate tools/ast-grep/baseline.json: the set of current rule violations
(ruleId, file, line, matched text), sorted for a stable diff.

Run from anywhere; paths in the baseline are relative to the repo root.
Re-run and commit whenever a change deliberately adds or removes a
violation the gate should stop treating as new (or old).
"""
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent.parent
SCAN_PATHS = ["src"]


def run_scan():
    cmd = ["ast-grep", "scan", "-c", str(HERE / "sgconfig.yml"), "--json"] + SCAN_PATHS
    result = subprocess.run(
        cmd, cwd=REPO_ROOT, capture_output=True, text=True
    )
    if result.returncode not in (0, 1):
        # ast-grep exits non-zero when it finds matches with error severity;
        # that is expected here, not a tool failure.
        sys.stderr.write(result.stderr)
        sys.exit(f"ast-grep scan failed (exit {result.returncode})")
    return json.loads(result.stdout)


def to_entry(match):
    path = Path(match["file"])
    try:
        path = path.resolve().relative_to(REPO_ROOT)
    except ValueError:
        pass
    return {
        "ruleId": match["ruleId"],
        "file": str(path),
        "line": match["range"]["start"]["line"],
        "text": match["text"].strip(),
    }


def main():
    matches = run_scan()
    entries = sorted(
        (to_entry(m) for m in matches),
        key=lambda e: (e["ruleId"], e["file"], e["line"]),
    )
    out = HERE / "baseline.json"
    with out.open("w") as f:
        json.dump(entries, f, indent=2)
        f.write("\n")
    print(f"wrote {len(entries)} entries to {out}")


if __name__ == "__main__":
    main()
