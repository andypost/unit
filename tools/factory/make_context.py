#!/usr/bin/env python3
"""
tools/factory/make_context.py <card-id> [--tasks tools/factory/tasks.jsonl]
                                          [--index build/fidx/functions.jsonl]

Prints the full executor prompt for one task card: the card's template
(tools/factory/prompt_templates/<template>.md), the card itself, the
tools/fidx/slice.py context slice for its fid (and, for T08/T11-style
paired cards, its companion_fid too), and any ADR/contract snippet the
card's file is covered by (tools/gates/contract_functions.txt, if the
target function is on it).

This is what the orchestrator pastes into an executor agent (Haiku /
Sonnet / Fable) verbatim -- make_context.py does not call a model itself.

Also prints, to stderr, a token-size estimate for the prompt it just
built (chars/4, the same rough heuristic used elsewhere in this repo's
planning), so the orchestrator can budget a batch of executor calls.
Pass --update-card to also write that estimate back into the card's
"context_chars_est"/"context_tokens_est" fields in tasks.jsonl in place.

Usage:
    python3 tools/factory/make_context.py T01-l1-memcasecmp
    python3 tools/factory/make_context.py T08-l2-json-escape-pair --update-card
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
SLICE_PY = REPO_ROOT / "tools/fidx/slice.py"


def load_cards(tasks_path):
    cards = {}
    with open(tasks_path) as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            c = json.loads(line)
            cards[c["id"]] = c
    return cards


def run_slice(fid, index_path):
    proc = subprocess.run(
        [sys.executable, str(SLICE_PY), fid, "--index", str(index_path)],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        return f"(slice.py failed for {fid}: {proc.stderr.strip()})"
    return proc.stdout


def contract_snippet(fn_name):
    """
    tools/gates/contract_functions.txt is a plain list of function names
    G6 requires an "@contract" comment for. If `fn_name` is on the list,
    say so and quote the @contract comment lines actually found above its
    definition under src/ (a small grep+sed, mirroring run_gates.sh's own
    gate_6()); otherwise return None.
    """

    list_path = REPO_ROOT / "tools/gates/contract_functions.txt"
    if not list_path.exists():
        return None

    names = {
        ln.strip()
        for ln in list_path.read_text().splitlines()
        if ln.strip() and not ln.strip().startswith("#")
    }
    if fn_name not in names:
        return None

    grep = subprocess.run(
        ["grep", "-rn", "-E",
         rf"^[A-Za-z_][A-Za-z0-9_ \*]*\b{fn_name}\(",
         "src", "--include=*.c"],
        cwd=REPO_ROOT, capture_output=True, text=True,
    )
    for line in grep.stdout.splitlines():
        file_, lineno, _ = line.split(":", 2)
        lineno = int(lineno)
        start = max(1, lineno - 15)
        sed = subprocess.run(
            ["sed", "-n", f"{start},{lineno}p", file_],
            cwd=REPO_ROOT, capture_output=True, text=True,
        )
        if "@contract" in sed.stdout:
            return f"{file_}:{start}-{lineno}\n{sed.stdout}"
    return f"(listed in tools/gates/contract_functions.txt but no @contract comment found yet for {fn_name} -- G6 will fail until one is added)"


def build_prompt(card, index_path):
    template_path = HERE / "prompt_templates" / f"{card['template']}.md"
    template_text = template_path.read_text()

    parts = [template_text, "\n---\n", "## Task card\n",
             "```json\n" + json.dumps(card, indent=2, sort_keys=False) + "\n```\n"]

    parts.append("\n---\n## Context slice: target function\n")
    parts.append("```\n" + run_slice(card["fid"], index_path) + "\n```\n")

    companion_fid = card.get("companion_fid")
    if companion_fid:
        parts.append(
            "\n---\n## Context slice: companion function "
            "(read-only reference -- do not edit)\n"
        )
        parts.append("```\n" + run_slice(companion_fid, index_path) + "\n```\n")

    fn_name = card["fid"].split("::", 1)[1]
    snippet = contract_snippet(fn_name)
    if snippet:
        parts.append("\n---\n## @contract (tools/gates/contract_functions.txt, G6)\n")
        parts.append("```\n" + snippet + "\n```\n")

    return "".join(parts)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("card_id")
    ap.add_argument("--tasks", default=str(HERE / "tasks.jsonl"))
    ap.add_argument(
        "--index", default=str(REPO_ROOT / "build/fidx/functions.jsonl")
    )
    ap.add_argument(
        "--update-card",
        action="store_true",
        help="write the chars/4 token estimate back into the card in "
             "--tasks (context_chars_est, context_tokens_est fields)",
    )
    args = ap.parse_args()

    tasks_path = Path(args.tasks)
    cards = load_cards(tasks_path)
    card = cards.get(args.card_id)
    if card is None:
        print(f"error: unknown card id {args.card_id!r} in {tasks_path}",
              file=sys.stderr)
        print(f"known ids: {', '.join(sorted(cards))}", file=sys.stderr)
        return 1

    prompt = build_prompt(card, Path(args.index))
    print(prompt)

    chars = len(prompt)
    tokens_est = chars // 4
    print(f"\n[make_context.py: {chars} chars, ~{tokens_est} tokens (chars/4)]",
          file=sys.stderr)

    if args.update_card:
        card["context_chars_est"] = chars
        card["context_tokens_est"] = tokens_est
        cards[args.card_id] = card
        with tasks_path.open("w") as fh:
            for cid in cards:
                fh.write(json.dumps(cards[cid], sort_keys=False))
                fh.write("\n")
        print(f"[make_context.py: wrote context_chars_est/context_tokens_est "
              f"into {tasks_path} for {args.card_id}]", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
