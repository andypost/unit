/*
 * fidx parse-time shim.
 *
 * tree-sitter-c parses raw source with no preprocessor, so any macro that
 * is not itself valid standalone C syntax -- a loop macro that leaves a
 * brace open for the caller to close, or a bare identifier used as a
 * storage-class/attribute qualifier -- produces ERROR nodes around every
 * call site.  build_index.py never feeds the raw file to the parser
 * directly: it first runs a length-preserving text substitution (see
 * apply_shim() in build_index.py) driven by the "fidx-shim:" directives
 * below, then parses the result.
 *
 * Every substitution keeps the exact byte length of what it replaces
 * (padding with spaces, keeping embedded newlines) so byte offsets and
 * line numbers in functions.jsonl always refer to the ORIGINAL file.
 * That makes the "offset map" from shim text to source text the identity
 * map; build_index.py never has to translate a position back.  This file
 * is read only for indexing. The raw source is never edited, and no edit
 * from tools/fidx is ever applied to shim-substituted text.
 *
 * Two directive kinds:
 *
 *   fidx-shim: blank NAME
 *     NAME is a bare identifier, optionally followed by a parenthesized,
 *     balanced argument list (e.g. an attribute-style macro).  Every
 *     occurrence -- the name and, if present, its whole "(...)" -- is
 *     replaced with spaces.  This is exactly what these macros expand to
 *     when they carry no parse-relevant meaning (nxt_cdecl expands to
 *     nothing at all; the others expand to storage-class or attribute
 *     specifiers that are irrelevant to control flow and safe to drop
 *     for indexing purposes).
 *
 *   fidx-shim: loop OPEN CLOSE
 *     OPEN is an "each"-style macro invocation, e.g. "foo_each(a, b)";
 *     the whole call (name + balanced parens) is replaced with
 *     "for (;;)" plus trailing spaces, so the caller's own literal "{"
 *     that always follows turns it into an ordinary infinite for loop.
 *     CLOSE is the matching bare "_loop" identifier; each occurrence is
 *     blanked like a "blank" directive.  This mirrors nxt_queue_each()/
 *     nxt_queue_loop and friends: real preprocessing turns the pair into
 *     "do { ... for (...) { <caller body> } } while (0)"; the shim only
 *     needs the result to parse, not to execute, so a plain for-loop
 *     stands in for the do/for nesting.
 */

/* fidx-shim: blank nxt_inline */
/* fidx-shim: blank nxt_noinline */
/* fidx-shim: blank nxt_cdecl */
/* fidx-shim: blank nxt_aligned */

/*
 * Not FreeUnit's own macro: PHP's Zend SDK headers still define
 * TSRMLS_DC as an (empty, on modern non-ZTS PHP) trailing parameter-list
 * macro in a handful of nxt_php_sapi.c signatures inherited from older
 * PHP SAPI code. Blanking it here is the same trick as nxt_cdecl, for
 * the same reason: it is a bare identifier that is not a valid parameter
 * declaration on its own.
 */
/* fidx-shim: blank TSRMLS_DC */

/* fidx-shim: loop nxt_queue_each nxt_queue_loop */
/* fidx-shim: loop nxt_list_each nxt_list_loop */
/* fidx-shim: loop nxt_http_fields_each nxt_http_fields_loop */
/* fidx-shim: loop nxt_process_port_each nxt_process_port_loop */
/* fidx-shim: loop nxt_runtime_process_each nxt_runtime_process_loop */
