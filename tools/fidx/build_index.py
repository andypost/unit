#!/usr/bin/env python3
"""
Build build/fidx/functions.jsonl: one JSON record per C function definition
in src/, keyed by fid = "path::name".

Parsing never sees raw FreeUnit source directly.  A handful of nxt_*
macros (loop constructs that leave a brace open for the caller, bare
identifiers used as storage-class/attribute qualifiers) are not valid
standalone C, so tree-sitter-c would surround every call site with ERROR
nodes.  apply_shim() rewrites just those spans, using the directives in
shim.h, into text tree-sitter can parse -- always padding with spaces to
the exact original byte length (and keeping embedded newlines), so byte
offsets and line numbers in the output always refer to the ORIGINAL file.
The raw source on disk is never touched; the shim only exists in memory
for the duration of one parse.

Usage:
    python3 tools/fidx/build_index.py [--root REPO_ROOT] [--out OUT_JSONL]

Prints a one-line coverage report to stderr:
    indexed: DONE/TOTAL functions (X.X%), ERROR-node functions: Y (Y.Y%)
"""

import argparse
import csv
import hashlib
import json
import re
import sys
from pathlib import Path

import tree_sitter as ts
import tree_sitter_c as tsc


REPO_ROOT = Path(__file__).resolve().parents[2]
SHIM_H = Path(__file__).resolve().parent / "shim.h"

# Source globs to index. Headers are included because a fair number of
# nxt_inline helpers live entirely in .h files.
SOURCE_GLOBS = ("src/*.c", "src/*.h", "src/test/*.c")

# Heuristic substrings for the per-function flags. These are a coarse,
# no-LLM signal meant to help pick a risk tier / review level, not a
# precise static analysis.
FLAG_PATTERNS = {
    "memcpy": re.compile(
        r"\b(memcpy|memmove|nxt_memcpy|nxt_cpymem|strcpy|strcat|sprintf)\b"
    ),
    "len_math": re.compile(
        r"\b(length|len|size|nbytes|n)\s*[-+*]{1,2}\s*[A-Za-z0-9_]"
    ),
    "loops": re.compile(
        r"\b(for|while)\s*\(|nxt_\w*_each\s*\(|nxt_\w*_loop\b"
    ),
    "atomics": re.compile(
        r"\b(nxt_atomic_\w+|__atomic_\w+|__sync_\w+)\b"
    ),
    "shm": re.compile(
        r"\b(mmap|munmap|shm_open|shmget|shmat|nxt_mem_map|port_mmap)\b"
    ),
    "cond_compile": re.compile(r"^\s*#\s*(if|ifdef|ifndef)\b", re.MULTILINE),
}


def load_shim_directives(shim_path):
    """Parse the "fidx-shim:" comment directives out of shim.h."""

    blanks = []
    loops = []

    text = shim_path.read_text()
    for m in re.finditer(r"fidx-shim:\s*(\w+)\s+([^\*\n]+)", text):
        kind, rest = m.group(1), m.group(2).split()
        if kind == "blank":
            blanks.append(rest[0])
        elif kind == "loop":
            loops.append((rest[0], rest[1]))

    return blanks, loops


def _blank_span(buf, start, end):
    """Overwrite buf[start:end] with spaces, preserving newlines."""

    for i in range(start, end):
        if buf[i] not in (0x0A,):  # '\n'
            buf[i] = 0x20  # ' '


def _find_balanced_call_end(text, open_paren_idx):
    """Given the index of a '(' , return the index just past its match."""

    depth = 0
    i = open_paren_idx
    n = len(text)
    while i < n:
        c = text[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return n


def apply_shim(src_bytes, blanks, loops):
    """
    Return a byte-for-byte-same-length copy of src_bytes with the
    directives from shim.h applied, so tree-sitter-c can parse it. The
    identity offset map holds by construction: every replacement is the
    exact length of what it replaces.
    """

    text = src_bytes.decode("latin-1")
    buf = bytearray(src_bytes)

    # "each"-loop opens: replace "NAME(...)" with "for (;;)" + spaces.
    for open_name, _close_name in loops:
        for m in re.finditer(r"\b" + re.escape(open_name) + r"\s*\(", text):
            start = m.start()
            paren_idx = text.index("(", m.end() - 1)
            end = _find_balanced_call_end(text, paren_idx)
            stub = "for (;;)"
            if end - start < len(stub):
                # Not expected for the known macros, but never write past
                # the span we are allowed to touch.
                stub = stub[: end - start]
            for i, ch in enumerate(stub):
                buf[start + i] = ord(ch)
            _blank_span(buf, start + len(stub), end)

    # bare "_loop" identifiers and simple qualifier macros: blank them,
    # with an optional balanced "(...)" for attribute-style ones like
    # nxt_aligned(8).
    bare_names = blanks + [close_name for _open, close_name in loops]
    for name in bare_names:
        for m in re.finditer(r"\b" + re.escape(name) + r"\b", text):
            start, end = m.start(), m.end()
            if end < len(text) and text[end] == "(":
                end = _find_balanced_call_end(text, end)
            _blank_span(buf, start, end)

    return bytes(buf)


def node_has_error(node):
    if node.type == "ERROR" or node.is_missing:
        return True
    for child in node.children:
        if node_has_error(child):
            return True
    return False


def collect_callees(node, out):
    if node.type == "call_expression":
        fn = node.child_by_field_name("function")
        if fn is not None and fn.type == "identifier":
            out.add(fn.text.decode("utf-8", "replace"))
    for child in node.children:
        collect_callees(child, out)


def storage_class_of(func_node, src_bytes):
    """Best-effort storage class: look at the raw text before the
    declarator for a leading "static" (the only storage class FreeUnit
    functions use)."""

    start = func_node.start_byte
    line_start = src_bytes.rfind(b"\n", 0, start) + 1
    prefix = src_bytes[line_start:start]
    if b"static" in prefix.split(b"(")[0]:
        return "static"
    return "extern"


def signature_of(func_node, src_bytes):
    declarator = func_node.child_by_field_name("declarator")
    ret_type = func_node.child_by_field_name("type")
    start = ret_type.start_byte if ret_type is not None else func_node.start_byte
    end = declarator.end_byte if declarator is not None else func_node.start_byte
    sig = src_bytes[start:end].decode("utf-8", "replace")
    return re.sub(r"\s+", " ", sig).strip()


def function_name_node(func_node):
    declarator = func_node.child_by_field_name("declarator")
    node = declarator
    # Walk through pointer/parenthesized declarators down to the
    # function_declarator, then to its identifier.
    while node is not None and node.type != "function_declarator":
        if node.type == "pointer_declarator":
            node = node.child_by_field_name("declarator")
        elif node.type == "parenthesized_declarator":
            node = node.named_children[0] if node.named_children else None
        else:
            break

    if node is None or node.type != "function_declarator":
        return None

    ident = node.child_by_field_name("declarator")
    while ident is not None and ident.type != "identifier":
        if ident.type == "pointer_declarator":
            ident = ident.child_by_field_name("declarator")
        else:
            break

    return ident


def function_name_of(func_node):
    ident = function_name_node(func_node)
    if ident is None:
        return None
    return ident.text.decode("utf-8", "replace")


def flags_for(body_text):
    return {name: bool(pattern.search(body_text)) for name, pattern in
            FLAG_PATTERNS.items()}


def load_lizard_ccn(csv_path):
    """
    Load per-function cyclomatic complexity from a lizard CSV
    (columns: nloc,ccn,tokens,params,length,location,file,function,
    long_name,start,end -- see `lizard -l c --csv src/*.c src/*.h`),
    keyed by (file, function name, start line) to join against fidx
    records.

    lizard does not see the loops hidden inside nxt_*_each/_loop macros
    (they are plain function-call-shaped text to it), so a function
    built entirely around one of those can show a lower ccn than it
    actually has; treat every ccn from this file as a lower bound, which
    is why tier_hint() below only ever raises a tier off of it, never
    lowers one.
    """

    by_key = {}

    if not csv_path.exists():
        return by_key

    with csv_path.open(newline="") as fh:
        reader = csv.reader(fh)
        for row in reader:
            if len(row) < 11:
                continue
            nloc, ccn, _tokens, _params, _length, _loc, file_, func, \
                _long_name, start, _end = row[:11]
            try:
                key = (file_.strip(), func.strip(), int(start))
            except ValueError:
                continue
            try:
                by_key[key] = (int(ccn), int(nloc))
            except ValueError:
                continue

    return by_key


def tier_hint(flags, ccn):
    """
    A cheap, no-LLM starting tier (L1..L4, see freeunit-factory-plan.md
    section 4) for triage: how much review a change to this function
    should get. It is a floor, not a verdict -- risk indicators only
    ever raise the tier, and a human or a later, deeper pass can always
    raise it further.
    """

    tier = 1

    if flags.get("loops") or flags.get("cond_compile"):
        tier = max(tier, 2)

    if flags.get("memcpy") or flags.get("len_math"):
        tier = max(tier, 3)

    if flags.get("shm") or flags.get("atomics"):
        tier = max(tier, 4)

    # lizard's ccn is a lower bound (see load_lizard_ccn), so treat it
    # the same way: it can only push the tier up.
    if ccn is not None:
        if ccn > 40:
            tier = max(tier, 3)
        elif ccn > 20:
            tier = max(tier, 2)

    return f"L{tier}"


def index_file(path, parser, blanks, loops, repo_root, lizard_ccn):
    rel = str(path.relative_to(repo_root))
    src_bytes = path.read_bytes()
    shim_bytes = apply_shim(src_bytes, blanks, loops)

    assert len(shim_bytes) == len(src_bytes), (
        f"shim changed byte length in {rel}"
    )

    tree = parser.parse(shim_bytes)

    records = []
    total_funcs = 0
    error_funcs = 0

    def walk(node):
        nonlocal total_funcs, error_funcs

        if node.type == "function_definition":
            total_funcs += 1

            name = function_name_of(node)
            has_err = node_has_error(node)
            if has_err:
                error_funcs += 1

            body = node.child_by_field_name("body")
            body_text = src_bytes[
                (body.start_byte if body else node.start_byte):node.end_byte
            ].decode("utf-8", "replace")

            callees = set()
            if body is not None:
                collect_callees(body, callees)

            fn_bytes = src_bytes[node.start_byte:node.end_byte]
            start_line = node.start_point[0] + 1

            # lizard keys a function by the line its *name* is written
            # on, which is not always the same line as the function
            # definition's own start (a return type on its own line is
            # common in this codebase's style, e.g. "ssize_t\nfoo(...)").
            name_node = function_name_node(node)
            name_line = (name_node.start_point[0] + 1) if name_node \
                else start_line

            flags = flags_for(body_text)
            ccn, nloc = lizard_ccn.get((rel, name, name_line), (None, None))

            record = {
                "fid": f"{rel}::{name if name else '<anon@%d>' % node.start_point[0]}",
                "file": rel,
                "name": name,
                "start_line": start_line,
                "end_line": node.end_point[0] + 1,
                "start_byte": node.start_byte,
                "end_byte": node.end_byte,
                "body_sha": hashlib.sha256(fn_bytes).hexdigest(),
                "signature": signature_of(node, src_bytes),
                "storage_class": storage_class_of(node, src_bytes),
                "callees": sorted(callees),
                "flags": flags,
                "ccn": ccn,
                "nloc": nloc,
                "tier_hint": tier_hint(flags, ccn),
                "parse_error": has_err,
            }
            records.append(record)
            # Do not recurse into a function's own body for nested
            # function_definitions -- C has none, but this keeps the
            # walk cheap and avoids double-counting.
            return

        for child in node.children:
            walk(child)

    walk(tree.root_node)

    return records, total_funcs, error_funcs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=str(REPO_ROOT))
    ap.add_argument("--out", default=str(REPO_ROOT / "build/fidx/functions.jsonl"))
    ap.add_argument(
        "--lizard-csv",
        default=None,
        help="lizard -l c --csv src/*.c src/*.h output to join ccn/nloc "
             "from (see load_lizard_ccn's docstring); skipped if not given "
             "or the file does not exist.",
    )
    args = ap.parse_args()

    repo_root = Path(args.root).resolve()
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    blanks, loops = load_shim_directives(SHIM_H)
    lizard_ccn = load_lizard_ccn(Path(args.lizard_csv)) if args.lizard_csv \
        else {}

    lang = ts.Language(tsc.language())
    parser = ts.Parser(lang)

    files = []
    for pattern in SOURCE_GLOBS:
        files.extend(sorted(repo_root.glob(pattern)))

    all_records = []
    total_funcs = 0
    error_funcs = 0

    for path in files:
        records, tf, ef = index_file(
            path, parser, blanks, loops, repo_root, lizard_ccn
        )
        all_records.extend(records)
        total_funcs += tf
        error_funcs += ef

    with out_path.open("w") as fh:
        for record in all_records:
            fh.write(json.dumps(record, sort_keys=True))
            fh.write("\n")

    pct_indexed = 100.0 if total_funcs == 0 else \
        100.0 * (total_funcs - 0) / total_funcs
    pct_error = 0.0 if total_funcs == 0 else 100.0 * error_funcs / total_funcs

    print(
        f"indexed: {total_funcs}/{total_funcs} functions found across "
        f"{len(files)} files, wrote {len(all_records)} records to {out_path}",
        file=sys.stderr,
    )
    print(
        f"ERROR-node functions: {error_funcs} ({pct_error:.1f}%)",
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()
