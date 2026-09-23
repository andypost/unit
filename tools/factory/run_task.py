#!/usr/bin/env python3
"""
tools/factory/run_task.py <card-id> <edit.json> [--model LABEL] [options]

Deterministic, no-LLM runner for one executor edit against one task card
from tools/factory/tasks.jsonl. It:

  1. Loads the card and the edit JSON, and cross-checks fid/base_body_sha
     between them (a wrong pairing is refused before anything else runs).
  2. Creates a disposable git worktree under a scratch directory with
     `git worktree add --detach <path> HEAD`, applies the edit there with
     tools/fidx/apply_edit.py, writes any files from the edit's `tests`
     list, runs the card's selected gates, and always removes the
     worktree afterward (`git worktree remove --force`), even on failure.
  3. Appends one JSON result line to tools/factory/results.jsonl.

It NEVER modifies the invoking (main) worktree: every build, test run,
and file write happens inside the disposable worktree, which is deleted
before this script exits.

Gates: a card's acceptance.gates list (e.g. ["G1", "G2", "G5"]) selects
which of these run:

  G1  hardened build, one compiler (gcc) -- `--fast` (the default) builds
      once with gcc only; `--full` instead shells out to
      tools/gates/run_gates.sh -g 1 for the gcc+clang pair.
  G2  build + the card's pytest slice (only meaningful if the edit wrote
      a test/*.py file; skipped otherwise).
  G3  ASan+UBSan build + the card's C test(s) run under ASan/UBSan.
  G5  ast-grep baseline (tools/ast-grep/check_baseline.py).
  G6  @contract stub check (tools/gates/run_gates.sh -g 6).

Any standalone C test file the edit wrote (a path under src/test/ or
tools/factory/, ending in .c) is compiled directly with gcc against the
worktree's src/ headers and run as its own tiny binary -- it does NOT
need to be registered in src/test/nxt_tests.c, since apply_edit.py can
only touch the one target function and registering a new suite entry
would touch a second one. A .py test file is run with pytest from the
worktree's test/ directory, against a build of the daemon (G2/G3 do
this).

Usage:
    python3 tools/factory/run_task.py T01-l1-memcasecmp edit.json --model sonnet
    python3 tools/factory/run_task.py T05-l2-chunk-range-valid-TRAP bad_edit.json \\
        --model sonnet --full

Exit codes: 0 the run completed and a result line was written (this is
true whether the edit was accepted or rejected -- "the bad edit was
correctly rejected" is a successful run of this script); 2 usage error
(bad card id, malformed edit, mismatched fid/sha) before any worktree
was touched.
"""

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
TASKS_PATH = HERE / "tasks.jsonl"
RESULTS_PATH = HERE / "results.jsonl"

GATE_NUM = {"G1": "1", "G2": "2", "G3": "3", "G4": "4", "G5": "5", "G6": "6"}


def load_card(card_id, tasks_path):
    with open(tasks_path) as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            c = json.loads(line)
            if c["id"] == card_id:
                return c
    return None


def sh(cmd, cwd, log_path=None, timeout=None, env=None):
    """Run cmd (list) in cwd, tee output to log_path if given, return
    (returncode, combined_output)."""

    proc = subprocess.run(
        cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout, env=env,
    )
    out = proc.stdout + proc.stderr
    if log_path:
        Path(log_path).write_text(out)
    return proc.returncode, out


def make_worktree(scratch_root):
    scratch_root.mkdir(parents=True, exist_ok=True)
    wt_path = Path(tempfile.mkdtemp(prefix="factory-wt-", dir=str(scratch_root)))
    # tempfile.mkdtemp already created wt_path; `git worktree add` needs the
    # target directory to not exist yet.
    wt_path.rmdir()
    rc, out = sh(["git", "worktree", "add", "--detach", str(wt_path), "HEAD"],
                 cwd=REPO_ROOT)
    if rc != 0:
        raise RuntimeError(f"git worktree add failed:\n{out}")
    return wt_path


def remove_worktree(wt_path):
    subprocess.run(["git", "worktree", "remove", "--force", str(wt_path)],
                    cwd=REPO_ROOT, capture_output=True, text=True)
    shutil.rmtree(wt_path, ignore_errors=True)


def seed_index(wt_path):
    """Copy the main worktree's fidx index into the fresh worktree. Valid
    because the worktree was just created --detach at HEAD, so its src/
    is byte-identical to what the index was last built from."""

    src_index = REPO_ROOT / "build/fidx/functions.jsonl"
    if not src_index.exists():
        raise RuntimeError(
            f"{src_index} missing -- run tools/fidx/build_index.py in the "
            "main worktree first"
        )
    dst = wt_path / "build/fidx/functions.jsonl"
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src_index, dst)
    return dst


def apply_edit(wt_path, index_path, edit_path, log_dir):
    rc, out = sh(
        [sys.executable, str(wt_path / "tools/fidx/apply_edit.py"),
         "--root", str(wt_path), "--index", str(index_path),
         "--edit-file", str(edit_path)],
        cwd=wt_path,
        log_path=log_dir / "apply_edit.log",
    )
    return rc == 0, out


def diff_stats(diff_text):
    added = sum(1 for ln in diff_text.splitlines()
                if ln.startswith("+") and not ln.startswith("+++"))
    removed = sum(1 for ln in diff_text.splitlines()
                  if ln.startswith("-") and not ln.startswith("---"))
    return {"lines_added": added, "lines_removed": removed}


def write_test_files(wt_path, edit):
    written = []
    for t in edit.get("tests") or []:
        path = wt_path / t["path"]
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(t["text"])
        written.append(t["path"])
    return written


def run_c_test(wt_path, rel_path, log_dir):
    """Compile a standalone C test file directly with gcc, against the
    worktree's own configured build tree (headers under src/ and
    build/include, and build/lib/libnxt.a for anything the test's own
    file needs to link with -- most of the target functions live in a
    .c file that itself pulls in other nxt_*.o symbols at link time even
    when the test never calls them, e.g. nxt_string.c needs nxt_mp_get()
    from the memory-pool allocator). The test file is expected to be
    self-contained (its own main()) -- it is NOT registered in
    src/test/nxt_tests.c, see the module docstring.

    Requires gate_1_fast (or -g 1 via run_gates.sh) to have built the
    tree first; the include/lib paths below come from build/Makefile's
    own @EXTRA_LIBS@ substitution (-lm -lrt -lpthread -lssl -lcrypto
    -lpcre2-8 -lz), not guessed independently, so a future dependency
    change to that Makefile line should be mirrored here.
    """

    src_path = wt_path / rel_path
    bin_path = wt_path / (rel_path + ".bin")
    build_dir = wt_path / "build"
    libnxt = build_dir / "lib/libnxt.a"

    if not libnxt.exists():
        return False, (
            f"{libnxt} not found -- G1 must build successfully before a "
            "C test can be compiled against it"
        )

    include_dirs = [
        "-I", str(wt_path / "src"),
        "-I", str(build_dir),
        "-I", str(build_dir / "include"),
    ]
    extra_libs = ["-lm", "-lrt", "-lpthread", "-lssl", "-lcrypto",
                  "-lpcre2-8", "-lz"]

    rc, out = sh(
        ["gcc", "-std=gnu11", "-g", "-O0", "-Wall",
         *include_dirs, str(src_path), str(libnxt), *extra_libs,
         "-o", str(bin_path)],
        cwd=wt_path, log_path=log_dir / f"cc-{Path(rel_path).name}.log",
    )
    if rc != 0:
        return False, f"compile failed:\n{out}"

    rc, out = sh([str(bin_path)], cwd=wt_path,
                 log_path=log_dir / f"run-{Path(rel_path).name}.log")
    return rc == 0, out


def gate_1_fast(wt_path, log_dir):
    env = None
    rc, out = sh(
        ["./configure", "--zlib", "--openssl", "--hardening=strict"],
        cwd=wt_path, log_path=log_dir / "g1-configure.log",
    )
    if rc != 0:
        return False, f"configure failed:\n{out[-4000:]}"
    rc, out = sh(["make", "-j2"], cwd=wt_path, log_path=log_dir / "g1-make.log")
    if rc != 0:
        return False, f"make failed:\n{out[-4000:]}"
    return True, "built OK (gcc, --hardening=strict)"


def gate_via_run_gates(wt_path, gate_num, log_dir, extra_args=None):
    cmd = [str(wt_path / "tools/gates/run_gates.sh"), "-g", gate_num]
    if extra_args:
        cmd += extra_args
    rc, out = sh(cmd, cwd=wt_path, log_path=log_dir / f"g{gate_num}-run_gates.log",
                 timeout=1800)
    ok = rc == 0 and f"G{gate_num}: PASS" in out
    return ok, out[-4000:]


def gate_5_ast_grep(wt_path, log_dir):
    rc, out = sh(["ast-grep", "--version"], cwd=wt_path)
    if rc != 0:
        return None, "ast-grep not installed -- SKIP"
    rc, out = sh([sys.executable, "tools/ast-grep/check_baseline.py"],
                 cwd=wt_path, log_path=log_dir / "g5-ast-grep.log")
    return rc == 0, out[-4000:]


def run_gates(wt_path, gates, edit, test_paths, mode, log_dir):
    results = {}

    py_tests = [p for p in test_paths if p.endswith(".py")]
    c_tests = [p for p in test_paths if p.endswith(".c")]

    if "G1" in gates:
        if mode == "full":
            ok, detail = gate_via_run_gates(wt_path, "1", log_dir)
        else:
            ok, detail = gate_1_fast(wt_path, log_dir)
        results["G1"] = {"status": "pass" if ok else "fail", "detail": detail}

    # C tests run right after G1 -- they need the headers a configured
    # tree provides for anything beyond the most trivial include set, and
    # G1 already built (or attempted to build) that tree.
    if c_tests:
        c_results = {}
        for p in c_tests:
            ok, detail = run_c_test(wt_path, p, log_dir)
            c_results[p] = {"status": "pass" if ok else "fail",
                             "detail": detail[-2000:]}
        results["C_TESTS"] = c_results

    if "G2" in gates:
        if py_tests:
            slice_ = " ".join(Path(p).name for p in py_tests)
            ok, detail = gate_via_run_gates(
                wt_path, "2", log_dir, extra_args=["-t", slice_]
            )
        else:
            ok, detail = None, "no .py test in this edit -- SKIP"
        results["G2"] = {
            "status": "pass" if ok else ("skip" if ok is None else "fail"),
            "detail": detail,
        }

    if "G3" in gates:
        ok, detail = gate_via_run_gates(wt_path, "3", log_dir)
        results["G3"] = {"status": "pass" if ok else "fail", "detail": detail}

    if "G5" in gates:
        ok, detail = gate_5_ast_grep(wt_path, log_dir)
        results["G5"] = {
            "status": "pass" if ok else ("skip" if ok is None else "fail"),
            "detail": detail,
        }

    if "G6" in gates:
        ok, detail = gate_via_run_gates(wt_path, "6", log_dir)
        results["G6"] = {"status": "pass" if ok else "fail", "detail": detail}

    return results


def overall_pass(gate_results):
    for name, r in gate_results.items():
        if name == "C_TESTS":
            if any(v["status"] != "pass" for v in r.values()):
                return False
            continue
        if r["status"] == "fail":
            return False
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("card_id")
    ap.add_argument("edit_json", help="path to the executor's edit JSON")
    ap.add_argument("--model", default="unknown",
                     help="label recorded in results.jsonl, e.g. haiku/sonnet/fable")
    ap.add_argument("--tasks", default=str(TASKS_PATH))
    ap.add_argument("--results", default=str(RESULTS_PATH))
    ap.add_argument("--scratch", default="/tmp/factory-worktrees",
                     help="parent dir for disposable worktrees")
    ap.add_argument("--full", dest="mode", action="store_const",
                     const="full", default="fast",
                     help="run full tools/gates/run_gates.sh gates instead "
                          "of the fast single-compiler subset")
    ap.add_argument("--keep-worktree", action="store_true",
                     help="do not delete the disposable worktree (debugging)")
    args = ap.parse_args()

    tasks_path = Path(args.tasks)
    card = load_card(args.card_id, tasks_path)
    if card is None:
        print(f"error: unknown card id {args.card_id!r} in {tasks_path}",
              file=sys.stderr)
        return 2

    edit_path = Path(args.edit_json).resolve()
    if not edit_path.exists():
        print(f"error: edit file not found: {edit_path}", file=sys.stderr)
        return 2
    edit = json.loads(edit_path.read_text())

    if edit.get("fid") != card["fid"]:
        print(
            f"error: edit fid {edit.get('fid')!r} does not match card fid "
            f"{card['fid']!r}", file=sys.stderr,
        )
        return 2
    if edit.get("base_body_sha") != card["base_body_sha"]:
        print(
            "error: edit base_body_sha does not match the card's -- "
            "the edit was made against a different (or stale) view of "
            "this function", file=sys.stderr,
        )
        return 2

    t0 = time.time()
    wt_path = make_worktree(Path(args.scratch))
    log_dir = wt_path / ".factory-logs"
    log_dir.mkdir(exist_ok=True)

    result = {
        "card": args.card_id,
        "fid": card["fid"],
        "model": args.model,
        "mode": args.mode,
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }

    try:
        index_path = seed_index(wt_path)
        apply_ok, apply_out = apply_edit(wt_path, index_path, edit_path, log_dir)
        result["apply_ok"] = apply_ok
        result["scope_ok"] = apply_ok  # apply_edit.py refuses any
        # out-of-scope edit before writing, so a successful apply already
        # proves the diff touched only the target function.
        result["diff_stats"] = diff_stats(apply_out)

        if not apply_ok:
            result["apply_refusal"] = apply_out[-2000:]
            result["gates"] = {}
            result["gates_pass"] = False
        else:
            written = write_test_files(wt_path, edit)
            result["test_files_written"] = written
            gates = card.get("acceptance", {}).get("gates", [])
            gate_results = run_gates(
                wt_path, gates, edit, written, args.mode, log_dir
            )
            result["gates"] = gate_results
            result["gates_pass"] = overall_pass(gate_results)

        result["wall_time_sec"] = round(time.time() - t0, 2)

    finally:
        if args.keep_worktree:
            result["worktree_kept_at"] = str(wt_path)
        else:
            remove_worktree(wt_path)

    with open(args.results, "a") as fh:
        fh.write(json.dumps(result, sort_keys=False))
        fh.write("\n")

    verdict = "PASS" if result.get("gates_pass") and result.get("apply_ok") else "FAIL"
    print(f"{args.card_id} [{args.model}]: apply_ok={result.get('apply_ok')} "
          f"gates_pass={result.get('gates_pass')} -> {verdict}  "
          f"({result['wall_time_sec']}s)")
    print(f"result appended to {args.results}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
