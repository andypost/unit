# T-scope executor prompt

You are editing exactly one C function in the FreeUnit server (an NGINX
Unit fork). This is a **T-scope** task: the risk is in the *scope* of the
change, not in a single length calculation. FreeUnit's state machines
(parsers, dispatch loops) hand off to sibling handlers and callers by
contract -- return codes, `rp->handler` chaining, which struct field means
what -- and a fix that quietly widens or narrows that contract breaks
callers you were not shown.

Rules for this task:

- Touch only the target function's body. You may not edit any other
  function, add a second function, or change a struct/macro definition.
  A brand-new small static helper is allowed only if you list its name in
  `new_symbols`; prefer not to add one.
- Keep every return code, every `rp->handler = &...` assignment, and every
  externally-visible field write exactly as meaningful as before, unless
  the card's `known_issue` specifically asks you to change one. If you are
  not sure a caller depends on some detail, assume it does.
- Do not reorder side effects that other code depends on seeing in a
  particular order (e.g. a state update before a callee is invoked, or a
  break out of a loop before a log line).
- If, after reading the function and its context, you conclude there is
  nothing to fix (the trap case), say so in `rationale` and answer with
  `"edits": []` -- an explicit empty list, not one entry. This is the
  no-change answer this harness expects; it is still checked against
  `fid`/`base_body_sha` exactly like a real edit, it just writes nothing.
  Repeating the function byte-for-byte in a `replace_function` edit
  (`edits[0].text` identical to what you were shown) is also accepted and
  means the same thing, but `"edits": []` is the simpler, preferred way to
  say "no change". Either way, still write your test(s) in `tests[]` --
  the gates run against them regardless of whether the function changed.
- Add or extend a test per the card's `acceptance` section. Put the whole
  new test file's content in `tests[]`; do not try to append to an
  existing test file (you cannot touch other functions, and existing test
  suites are registered by function, not by file).
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

## Output format (return ONLY this JSON, nothing else)

```json
{
  "fid": "<copied from the card>",
  "base_body_sha": "<copied from the card>",
  "edits": [
    {"op": "replace_function", "text": "<the whole new function, return type through the closing brace>"}
  ],
  "new_symbols": [],
  "contract": "<one or two sentences: what callers may still assume is true about this function after your change>",
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
