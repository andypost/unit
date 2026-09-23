#!/usr/bin/env python3
"""
Gate G5's ast-grep half: fail only on violations not already in
baseline.json. A violation is identified by (ruleId, file, matched text) --
not line number, so an unrelated diff earlier in the file does not turn an
old violation into a "new" one.

Exit codes: 0 clean or only-baseline violations; 1 new violations found;
2 the scan itself failed to run.
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
    result = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True, text=True)
    if result.returncode not in (0, 1):
        sys.stderr.write(result.stderr)
        print(f"ast-grep scan failed to run (exit {result.returncode})")
        sys.exit(2)
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as e:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        print(f"could not parse ast-grep output: {e}")
        sys.exit(2)


def key(entry):
    return (entry["ruleId"], entry["file"], entry["text"])


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
    baseline_path = HERE / "baseline.json"
    baseline = json.loads(baseline_path.read_text()) if baseline_path.exists() else []
    known = {key(e) for e in baseline}

    matches = [to_entry(m) for m in run_scan()]
    new = [m for m in matches if key(m) not in known]

    if not new:
        print(f"ast-grep: {len(matches)} violation(s), all in baseline. OK.")
        return 0

    print(f"ast-grep: {len(new)} NEW violation(s) not in baseline.json:")
    for m in new:
        print(f"  {m['file']}:{m['line']}: [{m['ruleId']}] {m['text']}")
    print()
    print(
        "If these are intentional and reviewed, run "
        "'python3 tools/ast-grep/gen_baseline.py' and commit the updated "
        "baseline.json alongside this change."
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
