#!/usr/bin/env python3
"""
tools/fidx/slice.py FID

Print a context slice for one function out of build/fidx/functions.jsonl:
the function's own source, the definitions of the struct/union/enum types
its signature and body mention, and the one-line signature of each of its
callees (from their own fidx record, when they have one).

This is read-only context assembly for a human or an executor deciding
how to change FID -- it never edits anything (see apply_edit.py, not
built yet, for that).

Usage:
    python3 tools/fidx/slice.py 'src/nxt_conf_validation.c::nxt_conf_vldt_compressors'
    python3 tools/fidx/slice.py FID --index build/fidx/functions.jsonl
"""

import argparse
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# Struct/union/enum tag references we can find by grepping types out of a
# signature or body: "nxt_conf_value_t *", "struct nxt_foo_s", etc.
TYPE_REF_RE = re.compile(r"\b(nxt_[a-zA-Z0-9_]*_t)\b")
TAG_REF_RE = re.compile(r"\b(?:struct|union|enum)\s+(nxt_[a-zA-Z0-9_]*)\b")


def load_index(index_path):
    by_fid = {}
    with open(index_path) as fh:
        for line in fh:
            rec = json.loads(line)
            by_fid[rec["fid"]] = rec
    return by_fid


def read_span(repo_root, rec):
    path = repo_root / rec["file"]
    data = path.read_bytes()
    return data[rec["start_byte"]:rec["end_byte"]].decode("utf-8", "replace")


def find_type_names(text):
    names = set(TYPE_REF_RE.findall(text))
    names |= set(TAG_REF_RE.findall(text))
    return names


def find_type_definitions(repo_root, names, limit_bytes=4000):
    """
    Best-effort: grep src/*.h and src/*.c for a typedef/struct/union/enum
    definition whose tag or typedef name matches one of `names`, and
    return {name: definition_text}. This is a text search, not a parse --
    good enough for showing "here is roughly the shape of this type"
    alongside a function slice.
    """

    found = {}
    remaining = set(names)
    if not remaining:
        return found

    pattern = re.compile(
        r"(?:typedef\s+)?(?:struct|union|enum)\s*(?:[A-Za-z0-9_]*\s*)?\{",
    )

    for path in sorted(repo_root.glob("src/*.h")) + sorted(repo_root.glob("src/*.c")):
        if not remaining:
            break

        text = path.read_text(errors="replace")

        for name in list(remaining):
            # A tagged "struct name { ... }" / "struct name_s { ... }",
            # matched by name or by name with "_t" swapped for "_s"
            # (this codebase's usual tag/typedef convention). Anonymous
            # "typedef struct { ... } name_t;" typedefs and non-struct
            # typedefs (e.g. "typedef intptr_t nxt_int_t;") are not
            # matched here -- disambiguating which anonymous struct goes
            # with which typedef name would need real parsing, not grep.
            tag_guess = name[:-2] + "_s" if name.endswith("_t") else name
            m = re.search(
                r"(typedef\s+)?(struct|union|enum)\s+(?:"
                + re.escape(name) + "|" + re.escape(tag_guess)
                + r")\b[^;{]*\{",
                text,
            )

            if m is None:
                continue

            start = m.start()
            # Find the matching closing brace by simple depth counting.
            depth = 0
            i = text.index("{", start)
            j = i
            while j < len(text):
                if text[j] == "{":
                    depth += 1
                elif text[j] == "}":
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            end = text.index(";", j) + 1 if j < len(text) else j

            snippet = text[start:end]
            if len(snippet) > limit_bytes:
                snippet = snippet[:limit_bytes] + "\n    /* ... truncated ... */"

            found[name] = f"{path.relative_to(repo_root)}:\n{snippet}"
            remaining.discard(name)

    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("fid")
    ap.add_argument(
        "--index", default=str(REPO_ROOT / "build/fidx/functions.jsonl")
    )
    ap.add_argument("--root", default=str(REPO_ROOT))
    args = ap.parse_args()

    repo_root = Path(args.root).resolve()
    by_fid = load_index(args.index)

    rec = by_fid.get(args.fid)
    if rec is None:
        print(f"error: {args.fid!r} not found in {args.index}", file=sys.stderr)
        print(
            "(run tools/fidx/build_index.py first, or check the fid spelling "
            "-- it is \"path::name\", exactly as build_index.py wrote it)",
            file=sys.stderr,
        )
        return 1

    body = read_span(repo_root, rec)

    print(f"=== {rec['fid']} ({rec['file']}:{rec['start_line']}-{rec['end_line']}) ===")
    print(f"signature: {rec['signature']}")
    print(f"tier_hint: {rec['tier_hint']}  ccn: {rec['ccn']}  flags: "
          f"{[k for k, v in rec['flags'].items() if v]}")
    print(f"body_sha: {rec['body_sha']}")
    print()
    print(body)
    print()

    type_names = find_type_names(rec["signature"] + "\n" + body)
    type_defs = find_type_definitions(repo_root, type_names)

    if type_defs:
        print("=== types referenced ===")
        for name in sorted(type_defs):
            print(f"--- {name} ---")
            print(type_defs[name])
            print()

    if rec["callees"]:
        print("=== callee signatures ===")
        for callee in rec["callees"]:
            matches = [r for fid, r in by_fid.items() if r["name"] == callee]
            if not matches:
                print(f"{callee}: (not in index -- external, or not a "
                      f"function_definition fidx indexes)")
                continue
            for m in matches:
                print(f"{callee} [{m['fid']}]: {m['signature']}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
