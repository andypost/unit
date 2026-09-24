# T-ownership executor prompt

You are editing exactly one C function in the FreeUnit server (an NGINX
Unit fork). This is a **T-ownership** task: the function writes into a
buffer it does not allocate and does not size -- some other function
(often its own sibling, e.g. a `*_length()` counterpart) computed how
many bytes the caller allocated, and this function must never write more
than that, or read past the boundary of memory it does not own.

Rules for this task:

- Touch only the target function's body. No new functions unless
  declared in `new_symbols`. In particular: do **not** edit the sizing
  counterpart function even if you can see it is related -- if the card
  names one in `known_issue`, treat it as a fixed, correct read-only
  reference and write your test against the pair of them, not a change
  to both.
- State, in `contract`, which function (or field) is responsible for the
  destination's capacity, and what this function promises never to
  exceed. If this function and its sizing counterpart can drift out of
  sync (one function changes how many bytes some input produces without
  the other agreeing), that drift is the actual bug class this template
  exists to catch -- your test should try to catch it, not just re-check
  today's behavior.
- Do not change how many bytes this function writes for any input class
  it already handles unless `known_issue` asks for that; a write-count
  change with no matching change to the sizing counterpart is exactly
  the bug this template guards against, so making one without the other
  is never correct here, and neither is this card asking you to touch
  both (you can only touch one function per edit).
- Where a null/empty/zero-count input is possible (an empty object, an
  empty string, a zero-length span), make sure your test states what
  this function does with it -- write nothing, write a fixed delimiter,
  etc. -- explicitly, since sizing/writing pairs are exactly where an
  empty case is where the two sides most often silently disagree.
- If, after reading the function and its sizing counterpart, you
  conclude there is nothing to fix, answer with `"edits": []` -- an
  explicit empty list. Repeating the function byte-for-byte in a
  `replace_function` edit is also accepted and means the same thing, but
  `"edits": []` is the simpler, preferred way to say it. Either way,
  still write your test(s) in `tests[]`; the gates run against them
  regardless of whether the function changed.
- **Testing a `static` function.** Most target functions in this batch
  are declared `static` in their .c file (this includes most sizing
  counterparts), so a separate test translation unit cannot link against
  them directly, and it must not be made to: removing `static`, changing
  the function's signature, or copying its body into the test file are
  all out of scope for this card and will be rejected at review even if
  the gates pass. The one sanctioned method is for your test file to
  `#include` the whole target `.c` file directly, e.g.:

  ```c
  #include "nxt_conf.c"   /* the file both functions of the pair live in */
  #include <stdio.h>

  int
  main(void)
  {
      /* call both the now-visible static sizing and writing functions */
      ...
  }
  ```

  `run_task.py` compiles this with `-I src` (so the quoted `#include`
  resolves) and links it against `build/lib/libnxt.a` exactly like any
  other standalone C test; the archive will not pull in a duplicate
  member for that `.c` file, since your test's own object already
  defines every symbol it provides. If the card gave you a
  `test_skeleton`, it already uses this pattern and compiles and passes
  as-is -- start from it. Also do not write a test that re-implements
  either function's sizing or writing logic (a "reference copy" to
  compare against itself); that proves nothing was broken, it only
  proves your copy agrees with itself. Call the two real functions and
  compare their answers.

## Output format (return ONLY this JSON, nothing else)

```json
{
  "fid": "<copied from the card>",
  "base_body_sha": "<copied from the card>",
  "edits": [
    {"op": "replace_function", "text": "<the whole new function, return type through the closing brace>"}
  ],
  "new_symbols": [],
  "contract": "<one or two sentences: who sizes the destination, and what this function promises never to exceed>",
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
