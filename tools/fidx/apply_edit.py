#!/usr/bin/env python3
"""
Apply one fidx-scoped edit: replace a single function's body (by byte
range, keyed on the function's own body_sha) and verify that nothing else
in the file changed.

Input (stdin, or --edit-file): a JSON object

    {
      "fid": "src/nxt_foo.c::nxt_foo_bar",
      "base_body_sha": "<sha256 the caller last saw functions.jsonl say
                          for this fid>",
      "edits": [
        {"op": "replace_function", "text": "<the whole new function,\n"
                                             "return type through the\n"
                                             "closing brace>"}
      ],
      "new_symbols": ["nxt_foo_helper"]   // optional: names of brand-new
                                          // top-level functions the new
                                          // text is allowed to introduce
    }

Only a single "replace_function" edit is supported; anything else in
"edits" is refused.

IMPORTANT: "text" replaces exactly functions.jsonl's [start_byte,
end_byte) span for the fid, which for a function qualified with one of
shim.h's blanked storage-class macros (nxt_inline, nxt_noinline,
nxt_cdecl, ...) starts AFTER that qualifier -- the qualifier's own text
sits just before start_byte and is left untouched on disk. "text" must
therefore start at the return type, matching the record's "signature"
field (which also omits the qualifier), not at the qualifier itself; a
"text" that repeats "nxt_inline" produces a doubled qualifier rather
than an error, since it is valid to write it twice syntactically.

Steps:
  1. Look the fid up in functions.jsonl (build with build_index.py first).
  2. Refuse if base_body_sha does not match the index's current body_sha
     for that fid -- the caller is looking at a stale function body.
  3. Re-hash the exact byte range straight off the file on disk and
     refuse if that does not match either -- the index itself may be
     stale relative to a file someone edited by hand since it was built.
  4. Splice the new text into that byte range and write it to a temp
     copy of the file (the real file is untouched until step 6).
  5. Re-parse both the original and the edited file with the same
     tree-sitter machinery build_index.py uses, and diff their function
     sets:
       - every pre-existing function OTHER than the target must still be
         present with an identical body_sha;
       - any function that is new after the edit must have a name in
         "new_symbols";
     Anything else -- a changed sibling, an unannounced new function, a
     function that disappeared -- refuses the whole edit and leaves the
     real file untouched.
  6. On success, write the edited content over the real file and print
     a unified diff of the whole file (old vs new).

Exit codes: 0 applied; 1 refused (message on stderr, no file written);
2 usage/lookup error.
"""

import argparse
import difflib
import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import build_index as bi  # noqa: E402


def load_index(index_path):
    by_fid = {}
    with open(index_path) as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            rec = json.loads(line)
            by_fid[rec["fid"]] = rec
    return by_fid


def records_for_file(index_by_fid, rel_file):
    return {
        fid: rec for fid, rec in index_by_fid.items() if rec["file"] == rel_file
    }


def index_single_file(path, repo_root, blanks, loops, parser):
    records, _, _ = bi.index_file(path, parser, blanks, loops, repo_root, {})
    return {r["fid"]: r for r in records}


def make_parser():
    import tree_sitter as ts
    import tree_sitter_c as tsc

    lang = ts.Language(tsc.language())
    return ts.Parser(lang)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=str(bi.REPO_ROOT))
    ap.add_argument(
        "--index", default=str(bi.REPO_ROOT / "build/fidx/functions.jsonl")
    )
    ap.add_argument(
        "--edit-file",
        default=None,
        help="path to the JSON edit; default is to read it from stdin",
    )
    ap.add_argument(
        "--dry-run",
        action="store_true",
        help="validate and print the diff, but never write the real file",
    )
    args = ap.parse_args()

    repo_root = Path(args.root).resolve()
    index_path = Path(args.index)

    if args.edit_file:
        edit = json.loads(Path(args.edit_file).read_text())
    else:
        edit = json.loads(sys.stdin.read())

    fid = edit.get("fid")
    base_body_sha = edit.get("base_body_sha")
    edits = edit.get("edits") or []
    new_symbols = set(edit.get("new_symbols") or [])

    if not fid or not base_body_sha or len(edits) != 1:
        print(
            "refused: edit must have 'fid', 'base_body_sha' and exactly "
            "one entry in 'edits'",
            file=sys.stderr,
        )
        return 2

    op = edits[0]
    if op.get("op") != "replace_function" or "text" not in op:
        print(
            "refused: the only supported edit is "
            '{"op": "replace_function", "text": "..."}',
            file=sys.stderr,
        )
        return 2

    if not index_path.exists():
        print(
            f"refused: index not found at {index_path}; run "
            "build_index.py first",
            file=sys.stderr,
        )
        return 2

    index_by_fid = load_index(index_path)
    target = index_by_fid.get(fid)
    if target is None:
        print(f"refused: fid not found in index: {fid}", file=sys.stderr)
        return 2

    if target["body_sha"] != base_body_sha:
        print(
            f"refused: base_body_sha does not match the index for {fid}\n"
            f"  index has:  {target['body_sha']}\n"
            f"  edit gave:  {base_body_sha}\n"
            "The index (or your view of this function) is stale. Re-read "
            "the function and retry.",
            file=sys.stderr,
        )
        return 1

    rel_file = target["file"]
    file_path = repo_root / rel_file
    if not file_path.exists():
        print(f"refused: {file_path} does not exist", file=sys.stderr)
        return 2

    original_bytes = file_path.read_bytes()
    start, end = target["start_byte"], target["end_byte"]
    on_disk_sha = hashlib.sha256(original_bytes[start:end]).hexdigest()

    if on_disk_sha != base_body_sha:
        print(
            f"refused: the function's bytes on disk no longer match the "
            f"index for {fid}\n"
            f"  on disk:  {on_disk_sha}\n"
            f"  expected: {base_body_sha}\n"
            "Someone edited this file since the index was built. Rebuild "
            "the index and retry.",
            file=sys.stderr,
        )
        return 1

    new_text = op["text"]
    if not new_text.endswith("\n") and original_bytes[end - 1:end] == b"\n":
        # Keep the file's line ending convention around the splice point
        # rather than silently joining two lines.
        new_text += "\n"

    new_bytes = (
        original_bytes[:start] + new_text.encode("utf-8") + original_bytes[end:]
    )

    blanks, loops = bi.load_shim_directives(bi.SHIM_H)
    parser = make_parser()

    before = index_single_file(file_path, repo_root, blanks, loops, parser)

    tmp_path = file_path.with_name(file_path.name + ".fidx-tmp")
    tmp_path.write_bytes(new_bytes)
    try:
        raw_after = index_single_file(tmp_path, repo_root, blanks, loops, parser)
        # index_single_file computed "file"/"fid" off the temp path's name
        # (rel_file + ".fidx-tmp"); rewrite both back to the real path so
        # fids compare directly against "before".
        tmp_rel = str(tmp_path.relative_to(repo_root))
        after = {}
        for rec in raw_after.values():
            fixed_fid = rec["fid"].replace(tmp_rel, rel_file, 1)
            fixed_rec = {**rec, "file": rel_file, "fid": fixed_fid}
            after[fixed_fid] = fixed_rec
    finally:
        tmp_path.unlink(missing_ok=True)

    problems = []

    for old_fid, old_rec in before.items():
        if old_fid == fid:
            continue
        new_rec = after.get(old_fid)
        if new_rec is None:
            problems.append(f"function disappeared: {old_fid}")
        elif new_rec["body_sha"] != old_rec["body_sha"]:
            problems.append(f"function changed but was not the edit target: {old_fid}")

    for new_fid_, new_rec in after.items():
        if new_fid_ == fid or new_fid_ in before:
            continue
        name = new_rec["name"]
        if name not in new_symbols:
            problems.append(
                f"new function {new_fid_} was not declared in new_symbols"
            )

    if fid not in after:
        problems.append(
            f"edit target {fid} is not a function definition in the new text"
        )

    if problems:
        print(f"refused: edit to {fid} touches more than the target function:", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        return 1

    old_text = original_bytes.decode("utf-8", "replace")
    new_text_full = new_bytes.decode("utf-8", "replace")
    diff = difflib.unified_diff(
        old_text.splitlines(keepends=True),
        new_text_full.splitlines(keepends=True),
        fromfile=f"a/{rel_file}",
        tofile=f"b/{rel_file}",
    )
    sys.stdout.writelines(diff)

    if not args.dry_run:
        file_path.write_bytes(new_bytes)
        print(f"\napplied: {fid}", file=sys.stderr)
    else:
        print(f"\ndry-run: {fid} validated, not written", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
