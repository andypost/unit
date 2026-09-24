# tools/factory: the code-factory model evaluation harness

This is the D3 "measure the models" harness described in
`freeunit-factory-plan.md` section 4: 12 task cards over real FreeUnit
functions (a mix of L1/L2/L3, four of them substituted away from
`src/nxt_unit.c` because the CODE stream is editing that file today --
see each card's `substitution_reason`), a deterministic no-LLM apply +
gate runner, and per-template executor/reviewer prompts. Nothing in this
directory calls a model. The orchestrator pastes `make_context.py`'s
output into an executor agent (Haiku / Sonnet / Fable), gets back one
JSON edit, and hands that JSON to `run_task.py`.

## Workflow

```
tools/factory/tasks.jsonl (one card)
        |
        v
make_context.py <card-id>          -- template + card + fidx slice(s)
        |                              (+ @contract snippet if listed)
        v
[ executor agent: Haiku / Sonnet / Fable ]
        |  returns ONE JSON edit:
        |  {fid, base_body_sha, edits:[{op:"replace_function", text}],
        |   new_symbols, contract, tests:[{path, text}], rationale, risk}
        v
run_task.py <card-id> edit.json --model <label>
        |  in a disposable `git worktree`, never the main tree:
        |    1. tools/fidx/apply_edit.py       (scope enforced here)
        |    2. write edit["tests"] files
        |    3. the card's acceptance.gates (G1 fast/full, G2, G3, G5, G6)
        v
tools/factory/results.jsonl        -- one line per run: apply_ok,
                                       scope_ok, gate verdicts, diff
                                       stats, wall time
        |
        v
[ reviewer agent, T-review.md prompt: diff + contract + card,
  NOT the executor's rationale/risk ]
        |
        v
accept / reject  -->  experiment.jsonl (not built in this pass; append
                       the reviewer's verdict + results.jsonl's line for
                       that (card, model) alongside expected_outcome_notes)
```

`make_context.py` also records a `chars/4` token-size estimate per card
back into `tasks.jsonl` (`context_chars_est` / `context_tokens_est`), run
once for all 12 cards already (see the table below) -- re-run it with
`--update-card` if a card's `known_issue` or template text changes.

## Files

- `tasks.jsonl` -- the 12 cards.
- `prompt_templates/T-scope.md`, `T-bounds.md`, `T-ownership.md` -- the
  three executor templates a card's `template` field selects.
- `prompt_templates/T-review.md` -- the reviewer template: diff +
  contract + card, never the executor's `rationale`/`risk`.
- `make_context.py` -- builds one card's full executor prompt.
- `run_task.py` -- applies one edit and runs its gates, deterministically,
  in a disposable worktree it always removes.
- `selftest/` -- fixtures proving `run_task.py` actually distinguishes a
  correct edit from two different kinds of wrong one (see below).
- `results.jsonl` -- appended to by every `run_task.py` run, including
  the self-test's four fixture runs already in it from this pass.

## The 12 cards

| id | fid | tier | template | trap | context (tokens est.) |
|---|---|---|---|---|---|
| T01-l1-memcasecmp | `nxt_string.c::nxt_memcasecmp` | L1 | T-bounds | no | ~1702 |
| T02-l1-fields-hash | `nxt_http_parse.c::nxt_http_fields_hash` | L1 | T-scope | no | ~1897 |
| T03-l1-json-position-TRAP | `nxt_conf.c::nxt_conf_json_position` | L1 | T-bounds | **yes** | ~1711 |
| T04-l1-mmap-get-method | `nxt_port_memory.c::nxt_port_mmap_get_method` | L1 | T-scope | no | ~3239 |
| T05-l2-chunk-range-valid-TRAP | `nxt_port_memory_int.h::nxt_port_mmap_chunk_range_valid` | L2 | T-bounds | **yes** | ~1668 |
| T06-l2-parse-field-name | `nxt_http_parse.c::nxt_http_parse_field_name` | L2 | T-bounds | no | ~3077 |
| T07-l2-parse-field-end | `nxt_http_parse.c::nxt_http_parse_field_end` | L2 | T-scope | no | ~2552 |
| T08-l2-json-escape-pair | `nxt_conf.c::nxt_conf_json_escape_length` (+ read-only companion `nxt_conf_json_escape`) | L2 | T-ownership | no | ~2276 |
| T09-l2-json-skip-space | `nxt_conf.c::nxt_conf_json_skip_space` | L2 | T-scope | no | ~1954 |
| T10-l2-lookup-field-end | `nxt_http_parse.c::nxt_http_lookup_field_end` | L2 | T-bounds | no | ~1987 |
| T11-l2-json-print-object | `nxt_conf.c::nxt_conf_json_print_object` | L2 | T-ownership | no | ~2433 |
| T12-l3-parse-field-value | `nxt_http_parse.c::nxt_http_parse_field_value` | L3 | T-bounds | no | ~2751 |

Four cards (T01, T02, T03, T04's sibling T06/T07/T11, see each card's
`substitution_reason`) stand in for plan-sample functions that live in
`src/nxt_unit.c`, which the CODE stream is editing today
(`nxt_unit_mmap_read` is in that file); the substitutes are same-shape,
similar-ccn functions in `src/nxt_string.c`, `src/nxt_conf.c` or
`src/nxt_http_parse.c`, none of which any other stream is touching
today. The plan's other two L3 IPC functions
(`nxt_port_mmap_read`, `nxt_unit_mmap_read`), `nxt_router_prepare_msg`,
the `nxt_app_queue_*` functions, and `nxt_port_read_msg_process` are
excluded entirely per this pass's brief, as are the L4
`nxt_nncq_*`/`nxt_app_queue_*` litmus functions (analysis-only, later).

Ten of the twelve cards are **not** traps, but ten of the ten still turn
out, on close reading, to already be correct for the specific edge case
named in `known_issue` -- the task is coverage (write the test that
would have caught a regression, and fix only if it actually finds one),
not "the code is definitely broken, go fix it." This mirrors the actual
plan-sample TRAPs (T03, T05) rather than diluting them: a model that
"fixes" a working function on every one of these cards, not just the two
formal traps, is a model that cannot be trusted with independent scope
on the real IPC files this batch deliberately avoided.

## Self-test

`tools/factory/selftest/` holds four fixtures, two per card, for two
cards (T01, a non-trap coverage task, and T05, the L2 trap):

| fixture | card | shape | expected `run_task.py` result |
|---|---|---|---|
| `T01-good.json` | T01 | unchanged function + the missing test | `apply_ok=true`, `gates_pass=true` |
| `T01-bad.json` | T01 | case-folding silently dropped (a real semantic bug) + the same test | `apply_ok=true`, `gates_pass=false` (the C test fails) |
| `T05-good.json` | T05 | unchanged (trap) function + boundary tests | `apply_ok=true`, `gates_pass=true` |
| `T05-bad.json` | T05 | a second, undeclared function smuggled into `edits[0].text` | `apply_ok=false` (refused before any gate runs) |

All four were run for real (`--model selftest`) and are already the
first four lines of `results.jsonl`:

```
T01-l1-memcasecmp        | apply_ok=True  | gates_pass=True   (good edit)
T01-l1-memcasecmp        | apply_ok=True  | gates_pass=False  (bad edit: broke a test)
T05-l2-chunk-range-valid-TRAP | apply_ok=True  | gates_pass=True   (good edit)
T05-l2-chunk-range-valid-TRAP | apply_ok=False | gates_pass=False (bad edit: out of scope)
```

This exercises both documented failure modes from the brief -- "breaks a
test" and "goes out of scope" -- through the real `run_task.py` path
(disposable `git worktree`, a real `./configure && make -j2`, a real
compiled-and-run C test), not just through `tools/fidx/test_apply_edit.py`'s
own narrower `--dry-run` unit tests.

## Metrics (per the plan's section 4)

Read off `results.jsonl` once a batch of (card × model) runs exists:

- gates passed on the first attempt (no re-submission needed);
- number of iterations to an accepted edit;
- accepted-edit rate;
- out-of-scope edits (`apply_ok=false` from a scope violation) -- target 0;
- false "fixes" on trap cards (T03, T05: any `apply_ok=true` diff that is
  not byte-identical to the original body is a miss, regardless of
  whether gates passed);
- known issues missed (a non-trap card where the submitted diff and test
  do not address `known_issue`, caught at review, not at the gate);
- tokens per accepted function (`context_tokens_est` from `tasks.jsonl`
  plus whatever the orchestrator logs for the executor's own output,
  divided by accepted edits).

## Model-tier hypothesis

Per the plan's pipeline (`fidx -> executor: Haiku (L1) / Sonnet (L2, L3)
/ Fable or Opus (L4) -> apply + gate -> review`):

| Tier | Expected executor | Hypothesis this batch tests |
|---|---|---|
| L1 (T01-T05) | Haiku | Low-ccn, single-file, coverage-shaped tasks; expect a high first-attempt gate-pass rate and, critically, a low false-fix rate on the two traps (T03, T05) -- a model that reflexively "improves" a correct 4-20-line helper is a signal to keep it off L2+ scope, not just a wasted turn. |
| L2 (T06-T11) | Sonnet | Higher ccn (7-19), multi-branch state machines and a sizing/writing pair (T08, T11); expect more iterations before a clean gate pass than L1, and this is where `contract` quality (does the model correctly state the sizing/ownership invariant, not just pass the test) should start to separate models. |
| L3 (T12) | Sonnet, cross-checked against Fable/Opus | The one card that chains a resumable, attacker-controlled multi-read parse (`nxt_http_parse_field_value`) through its own callee (`nxt_http_lookup_field_end`) and its continuation (`nxt_http_parse_field_end`); the plan's budget-cut order treats L3 cross-model comparison as the second thing to drop under a token crunch, so this card is the one to run across models first if only a partial batch fits. |
| L4 (not in this batch) | Fable / Opus, analysis-only | `nxt_nncq_*` / `nxt_app_queue_*`: no cards here, per the brief -- litmus-test-only later, never an unattended `replace_function` edit. |

The two traps sit at L1 and L2, not L3+, on purpose: if a model already
"fixes" a correct 3-4-ccn helper, it has failed the cheapest, least
ambiguous check this batch has, and there is no reason to spend L3/L4
budget finding that out a second time.

## Harness v2

Run 1 (`REPORT.md`, 28 executor runs across Haiku/Sonnet/Fable) found
that more than half of Haiku's failures, and some of Sonnet's, were the
harness's own bugs, not the model's: an empty-`edits` no-op answer being
rejected, no sanctioned way to test a `static` function, and no test
skeleton to anchor the expected file shape. Three fixes, all in this
pass:

1. **`"edits": []` is now an accepted no-op.** `tools/fidx/apply_edit.py`
   used to require exactly one entry in `edits`; it now also accepts an
   empty list as an explicit "no change" answer (the templates always
   allowed this in words, the tool just refused to take yes for an
   answer). `fid` and `base_body_sha` are still required and still
   checked against the index and the on-disk bytes exactly as for a real
   edit -- a no-op against a stale or wrong fid/sha is refused just like
   a real one would be. On success it writes nothing and prints "no-op"
   to stderr. `run_task.py` treats this apply exactly like a real one
   once it succeeds: it still writes the edit's `tests[]` files and
   still runs the card's gates, so a "no change + test" answer gets a
   full, real gate run, not a free pass. `tools/fidx/test_apply_edit.py`
   has two new cases covering this (accepted no-op, and a no-op refused
   for a stale `base_body_sha`). Repeating the function byte-for-byte in
   a `replace_function` edit is still accepted too and means the same
   thing; `"edits": []` is just the simpler way to say it, and all three
   executor templates now say so.

2. **A sanctioned way to test a `static` function.** Most target
   functions in this batch are `static`, so a separate C test
   translation unit cannot link against them -- and, in run 1, models
   worked around that by removing `static` (out of scope), copying the
   function into the test (T01: an empty test that "passes" against a
   fork of itself, not the real code), or re-implementing its logic as a
   "reference" to test against (T09: same problem, dressed up). The
   sanctioned method, now documented with a short example in every
   executor template (`T-bounds.md`, `T-scope.md`, `T-ownership.md`) and
   in `run_task.py`'s `run_c_test()` docstring: the test file
   `#include`s the whole target `.c` file directly (e.g.
   `#include "nxt_conf.c"`), compiled with `-I src` and linked against
   `build/lib/libnxt.a` exactly like any other standalone C test. This
   does not produce a duplicate-symbol error: the test's own object file
   already defines every symbol that `.c` file provides, so the linker
   never needs to pull the matching `.o` out of the archive (archive
   members are only extracted for symbols still undefined after the
   objects named directly on the command line). Verified empirically for
   a `static` function in both `src/nxt_conf.c`
   (`nxt_conf_json_skip_space`) and `src/nxt_http_parse.c`
   (`nxt_http_lookup_field_end`) while building this fix -- no
   duplicate-symbol problem appeared for either file, so no fallback was
   needed for this batch. `run_c_test()` still carries a fallback path
   (retry with `-Wl,--allow-multiple-definition`) for a future TU this
   hasn't been exercised against. Removing `static`, changing a target's
   signature, or copying its body into the test file are explicitly
   called out in every template as out of scope and rejected, as is a
   test that re-implements the function under test instead of calling
   it.

3. **Each card now carries a `test_skeleton`.** Run 1's other failure
   mode was format drift -- Haiku wrote tests in `src/test`'s harness
   shape with no `main()`. Every card in `tasks.jsonl` now has a
   `test_skeleton` field: a minimal, already-compiling standalone C test
   file using the exact include style that works for that card's target
   (plain `nxt_main.h` for a non-`static` target, the TU-include pattern
   above for a `static` one), with one trivial placeholder check and a
   `main()` returning 0/non-zero. `make_context.py` now prints it in the
   executor prompt as the stated starting point for `tests[]`. All 12
   skeletons were compiled and run through `run_task.py`'s C-test
   machinery (via a no-op edit per card) and pass as-is; all 12 cards'
   `base_body_sha` were also re-checked against a freshly built index --
   none had changed since the cards were written.
