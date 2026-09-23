# fidx: a function-level index of FreeUnit's C sources

`build/fidx/functions.jsonl` is one JSON record per C function definition
in `src/`, keyed by `fid = "path::name"`. It exists so the code factory
(and anyone else) can look up a function, its callees, its risk signals
and a starting review tier without re-parsing the tree by hand each time.

## Setup

```
pip install tree-sitter tree-sitter-c ast-grep-cli   # or: npm i -g @ast-grep/cli
apt-get install bear
```

- `tree-sitter` / `tree-sitter-c`: the Python bindings and grammar
  `build_index.py` parses with.
- `ast-grep` (the binary is `ast-grep`, **not** `sg` — `sg` on a Debian/
  Ubuntu box is shadow-utils' `sg`, not this tool): structural search and
  the lint rules other streams build on this index.
- `bear`: wraps a build to emit `compile_commands.json`. Used once to
  confirm the build compiles cleanly and to give downstream tools (an
  editor, `clangd`, a future `--compile-check` on `slice.py`) real
  compiler flags:

  ```
  ./configure --tests
  bear -- make -j2 unitd tests
  ```

  This is a one-time setup step, not part of `build_index.py`, which
  parses source text directly and does not need a full build.

## How parsing handles the macros that aren't valid standalone C

`tree-sitter-c` never sees the preprocessor: it parses the raw file text.
A handful of FreeUnit macros are not valid C on their own — a loop macro
that opens a `do { ... for (...) {` and leaves the brace for the caller
to close (`nxt_queue_each`/`nxt_queue_loop` and four more pairs like it),
or a bare identifier used as a storage-class/attribute qualifier
(`nxt_inline`, `nxt_noinline`, `nxt_cdecl`, `nxt_aligned(x)`). Parsed raw,
every call site of these comes out wrapped in `ERROR` nodes.

`tools/fidx/shim.h` documents, and `build_index.py` (`apply_shim()`)
applies, a length-preserving text substitution for exactly these macros
before parsing:

- an `_each(...)` open is rewritten to `for (;;)` plus trailing spaces,
  so the caller's own literal `{` that always follows turns it into an
  ordinary infinite loop;
- the matching bare `_loop` identifier, and the qualifier-style macros,
  are blanked to spaces (which is what most of them expand to anyway —
  `nxt_cdecl` expands to nothing at all).

Every substitution keeps the exact original byte length (embedded
newlines are preserved, only other characters become spaces), so byte
offsets and line numbers in `functions.jsonl` always describe the
**original** file. The shim only ever exists in memory for one parse;
**the raw source on disk is never edited**, and no edit tool in this
directory is meant to operate on shimmed text — only on the real file,
guarded by `body_sha`.

`shim.h`'s `fidx-shim:` comment directives are the single source of
truth `build_index.py` reads at run time; see the comment block at the
top of that file for the exact directive grammar.

## Running it

```
python3 tools/fidx/build_index.py \
    [--root REPO_ROOT] [--out build/fidx/functions.jsonl] \
    [--lizard-csv PATH_TO_LIZARD_CSV]
```

`--lizard-csv` is optional: given a `lizard -l c --csv src/*.c src/*.h`
report, `build_index.py` joins each function's `ccn` (cyclomatic
complexity) and `nloc` onto its record by `(file, function name, line the
name is written on)` — not the function definition's own start line,
which differs whenever the return type sits on its own line (a common
style in this codebase, e.g. `ssize_t\nfoo(...)  {`). lizard does not see
the loops hidden inside `nxt_*_each`/`_loop` macros (to it they are
plain call-shaped text), so treat its `ccn` as a lower bound; `tier_hint`
below only ever raises a tier off of it, never lowers one.

## Record shape

```jsonc
{
  "fid": "src/nxt_conf_validation.c::nxt_conf_vldt_compressors",
  "file": "src/nxt_conf_validation.c",
  "name": "nxt_conf_vldt_compressors",
  "start_line": 2846, "end_line": 2858,
  "start_byte": 98213, "end_byte": 98460,
  "body_sha": "…sha256 of the exact function bytes, for base_body_sha checks…",
  "signature": "static nxt_int_t nxt_conf_vldt_compressors(nxt_conf_validation_t *vldt, nxt_conf_value_t *value, void *data)",
  "storage_class": "static",
  "callees": ["nxt_conf_type", "nxt_conf_vldt_array_iterator", "nxt_conf_vldt_object"],
  "flags": {
    "memcpy": false, "len_math": false, "loops": false,
    "atomics": false, "shm": false, "cond_compile": false
  },
  "ccn": 3, "nloc": 8,
  "tier_hint": "L1",
  "parse_error": false
}
```

`flags` are coarse, regex-level heuristics (see `FLAG_PATTERNS` in
`build_index.py`), not a precise static analysis — they exist to help
pick a tier and a reviewer, not to replace one. `parse_error` is true
when the function's own subtree still contains an `ERROR` or a missing
node even after the shim (rare; see coverage below).

## Coverage (this run)

```
indexed: 2338/2338 functions found across 285 files
ERROR-node functions: 33 (1.4%)
ccn/nloc joined from lizard: 2224/2338 (95.1%)
```

2338 functions comfortably clears the ~2000 estimate in the sprint plan;
1.4% `parse_error` is well under the 3% target. The remaining errors are
concentrated in `src/nxt_php_sapi.c` (10 of its 34 functions) — legacy
PHP Zend SDK macros (`PHP_FUNCTION(...)`, and `TSRMLS_DC`, now shimmed)
in code inherited from older PHP SAPI support, not a FreeUnit macro. A
handful of others are one-offs. A Day-2 follow-up could extend `shim.h`
for `PHP_FUNCTION` if that file's index quality matters for factory work
there.

## Not built yet

- `tools/fidx/slice.py FID`: print a function, the struct definitions it
  uses, and its callees' signatures, from `functions.jsonl` plus a fresh
  read of the relevant files.
- `enrich_clang.py`, `apply_edit.py`, `tools/factory/run.py` (A4–A7 in
  the sprint plan) are out of scope for this pass.
