# T-bounds executor prompt

You are editing exactly one C function in the FreeUnit server (an NGINX
Unit fork). This is a **T-bounds** task: the risk is in length/offset
arithmetic and pointer bounds, often over bytes a network peer or a
language-module message controls. Get the boundary condition wrong here
and it is an overread, an overwrite, or an infinite loop, not a compile
error.

Rules for this task:

- Touch only the target function's body, exactly as in the T-scope
  template's scope rule. No new functions unless declared in
  `new_symbols`.
- State, in `contract`, exactly what the function assumes about its
  pointer arguments on entry (how many bytes are valid to read/write
  starting from each pointer, and who guarantees it) and what it
  guarantees on return. If the card's `known_issue` is null, this is a
  **trap**: the function is already correct for every case in scope, and
  the only right answer may be "no change, plus a test that pins the
  boundary". Do not add a defensive check that changes behavior on inputs
  that were already handled correctly just because it feels safer --
  that is an out-of-scope change and will be rejected. The no-change
  answer is `"edits": []` -- an explicit empty list. Repeating the
  function byte-for-byte in a `replace_function` edit is also accepted
  and means the same thing, but `"edits": []` is the simpler, preferred
  way to say it. Either way, still write your test(s) in `tests[]`; the
  gates run against them regardless of whether the function changed.
- **Testing a `static` function.** Most target functions in this batch
  are declared `static` in their .c file, so a separate test translation
  unit cannot link against them directly, and it must not be made to:
  removing `static`, changing the function's signature, or copying its
  body into the test file are all out of scope for this card and will be
  rejected at review even if the gates pass. The one sanctioned method is
  for your test file to `#include` the whole target `.c` file directly,
  e.g.:

  ```c
  #include "nxt_conf.c"   /* the file the target function lives in */
  #include <stdio.h>

  int
  main(void)
  {
      /* call the now-visible static function directly */
      ...
  }
  ```

  `run_task.py` compiles this with `-I src` (so the quoted `#include`
  resolves) and links it against `build/lib/libnxt.a` exactly like any
  other standalone C test; the archive will not pull in a duplicate
  member for that `.c` file, since your test's own object already
  defines every symbol it provides. If the card gave you a
  `test_skeleton`, it already uses this pattern (or plain `nxt_main.h`,
  for a target that is not `static`) and compiles and passes as-is --
  start from it. Also do not write a test that re-implements the target
  function's logic (a "reference copy" to compare against itself); that
  proves nothing was broken, it only proves your copy agrees with itself.
  Call the real function.
- Prefer the codebase's existing checked-arithmetic helpers
  (`nxt_checked.h`'s `nxt_size_add`/`nxt_size_sub`, `nxt_span.h`) over a
  hand-rolled `a + b` or `a - b` when you do add or change arithmetic on a
  length that can come from a peer -- but only where the card's
  `known_issue` calls for a change; do not refactor working arithmetic
  into a checked form as a side quest.
- Never widen what counts as "in bounds" (a longer accepted length, an
  off-by-one relaxed) without the card asking for exactly that; a
  *narrower* accepted-input criterion is also a behavior change and needs
  the same justification.
- Add a test per the card's `acceptance` section that specifically
  exercises the boundary named in `known_issue` (or, for a trap, the
  boundary that makes it look risky) -- an exact-buffer-length case, an
  off-by-one short/long input, and (if the function reads a fixed-width
  chunk, e.g. 4/8/16 bytes at a time) at least one length that lands on
  each side of that chunk boundary.

## Output format (return ONLY this JSON, nothing else)

```json
{
  "fid": "<copied from the card>",
  "base_body_sha": "<copied from the card>",
  "edits": [
    {"op": "replace_function", "text": "<the whole new function, return type through the closing brace>"}
  ],
  "new_symbols": [],
  "contract": "<one or two sentences: the precondition on every pointer argument's valid length, and the postcondition on what was read/written>",
  "tests": [
    {"path": "<new file path, e.g. src/test/nxt_foo_case_test.c or test/test_foo_case.py>", "text": "<whole file content>"}
  ],
  "rationale": "<why this change (or non-change) is correct, in your own words>",
  "risk": "<one line: what could still go wrong>"
}
```

**Gotcha on `edits[0].text`:** it replaces exactly the function's
`[start_byte, end_byte)` span from the index, which for a function
qualified with a blanked storage-class macro (`nxt_inline`,
`nxt_noinline`, `nxt_cdecl`, ...) starts **after** that qualifier. Write
`text` to match the card's `signature` field -- start at the return type,
not at the qualifier -- exactly as `apply_edit.py`'s docstring says;
repeating the qualifier in `text` produces a doubled qualifier, not an
error, so this is easy to get subtly wrong without anyone telling you.

Return only the JSON object above. No prose before or after it, no
markdown fence around it.
