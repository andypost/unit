#!/usr/bin/env python3
"""
Smoke test for apply_edit.py: one edit that should be accepted, one that
should be refused. Runs against the real functions.jsonl and source tree,
but always with --dry-run, so it never writes to a real file.

Usage: python3 tools/fidx/test_apply_edit.py
  (build the index first: python3 tools/fidx/build_index.py)

Exits 0 if both cases behaved as expected, 1 otherwise.
"""

import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
INDEX = REPO_ROOT / "build/fidx/functions.jsonl"
APPLY_EDIT = HERE / "apply_edit.py"

FID = "src/nxt_checked.h::nxt_size_add"


def load_record(fid):
    with INDEX.open() as fh:
        for line in fh:
            rec = json.loads(line)
            if rec["fid"] == fid:
                return rec
    raise SystemExit(f"fid not found in index: {fid}")


def run(edit):
    proc = subprocess.run(
        [sys.executable, str(APPLY_EDIT), "--dry-run"],
        input=json.dumps(edit),
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
    )
    return proc


def check(condition, message):
    status = "ok" if condition else "FAIL"
    print(f"[{status}] {message}")
    return condition


def main():
    if not INDEX.exists():
        print(f"missing {INDEX}; run build_index.py first", file=sys.stderr)
        return 1

    record = load_record(FID)
    ok = True

    # --- Case 1: accepted -- a benign body edit, same signature -----------
    accepted_edit = {
        "fid": FID,
        "base_body_sha": record["body_sha"],
        "edits": [
            {
                "op": "replace_function",
                "text": (
                    "int\n"
                    "nxt_size_add(size_t a, size_t b, size_t *out)\n"
                    "{\n"
                    "    /* apply_edit.py smoke test: benign comment. */\n"
                    "    return __builtin_add_overflow(a, b, out);\n"
                    "}\n"
                ),
            }
        ],
    }
    proc = run(accepted_edit)
    ok &= check(
        proc.returncode == 0,
        f"accepted edit exits 0 (got {proc.returncode}); stderr: {proc.stderr.strip()}",
    )
    ok &= check(
        "-int nxt_size_add" in proc.stdout or "smoke test" in proc.stdout,
        "accepted edit prints a diff containing the new text",
    )

    # --- Case 2: rejected -- a stale base_body_sha -------------------------
    rejected_edit = {
        "fid": FID,
        "base_body_sha": "0" * 64,
        "edits": [
            {
                "op": "replace_function",
                "text": "nxt_inline int\nnxt_size_add(size_t a, size_t b, size_t *out)\n{\n    return __builtin_add_overflow(a, b, out);\n}\n",
            }
        ],
    }
    proc = run(rejected_edit)
    ok &= check(
        proc.returncode == 1,
        f"stale-sha edit exits 1 (got {proc.returncode})",
    )
    ok &= check(
        "base_body_sha does not match" in proc.stderr,
        "stale-sha edit explains why it was refused",
    )

    # --- Case 3: rejected -- edit smuggles in an undeclared new function ---
    smuggle_edit = {
        "fid": FID,
        "base_body_sha": record["body_sha"],
        "edits": [
            {
                "op": "replace_function",
                "text": (
                    "int\n"
                    "nxt_size_add(size_t a, size_t b, size_t *out)\n"
                    "{\n"
                    "    return __builtin_add_overflow(a, b, out);\n"
                    "}\n"
                    "\n"
                    "int\n"
                    "nxt_size_add_smuggled(size_t a, size_t b, size_t *out)\n"
                    "{\n"
                    "    return __builtin_add_overflow(a, b, out);\n"
                    "}\n"
                ),
            }
        ],
        # deliberately NOT declaring nxt_size_add_smuggled
    }
    proc = run(smuggle_edit)
    ok &= check(
        proc.returncode == 1,
        f"undeclared-new-function edit exits 1 (got {proc.returncode})",
    )
    ok &= check(
        "was not declared in new_symbols" in proc.stderr,
        "undeclared-new-function edit names the offending function",
    )

    # --- Case 4: accepted -- an explicit no-op ("edits": []) --------------
    noop_edit = {
        "fid": FID,
        "base_body_sha": record["body_sha"],
        "edits": [],
    }
    proc = run(noop_edit)
    ok &= check(
        proc.returncode == 0,
        f"no-op edit exits 0 (got {proc.returncode}); stderr: {proc.stderr.strip()}",
    )
    ok &= check(
        "no-op" in proc.stderr,
        "no-op edit says so on stderr",
    )
    ok &= check(
        proc.stdout == "",
        "no-op edit prints no diff",
    )

    # --- Case 5: rejected -- a no-op against a stale base_body_sha --------
    noop_stale_edit = {
        "fid": FID,
        "base_body_sha": "0" * 64,
        "edits": [],
    }
    proc = run(noop_stale_edit)
    ok &= check(
        proc.returncode == 1,
        f"stale no-op edit exits 1 (got {proc.returncode})",
    )
    ok &= check(
        "base_body_sha does not match" in proc.stderr,
        "stale no-op edit is still checked against the index",
    )

    if ok:
        print("\nall cases behaved as expected")
        return 0
    print("\nsome cases did not behave as expected", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
