#!/usr/bin/env python3
"""Replay a --debug unit.log against an FSM contract and flag illegal
transitions.

    tools/fsm/trace_check.py docs/fsm/conn.yaml /path/to/unit.log [more logs]
    tools/fsm/trace_check.py docs/fsm/conn.yaml unit.log --examples 5 --json out.json

How a log maps onto the contract is declared in the YAML `trace` section:
the line format, the (pid, ident) key, ordered regex patterns that turn a
message into an event (with named groups such as fd/bl/er), events that are
implied because the code logs them under another task, and lines that are
legal after free.

One (pid, ident) can hold several connection structs at once: an upstream
peer inherits the client's log ident (nxt_conn_create() keeps a non-zero
inherited ident, nxt_conn.c:95-97).  Instances are told apart by fd.  Lines
without an fd are routed to the one live instance that can take the event;
when several can, the most recently touched one is chosen and the choice is
counted as ambiguous.

Exit status is 1 when at least one illegal transition was found.
"""

import argparse
import collections
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from render import load as load_machine  # noqa: E402


class Instance:
    __slots__ = ("id", "state", "fd", "fd_closed", "role", "touched",
                 "created_at", "events")

    def __init__(self, id, role, line_no):
        self.id = id
        self.state = None
        self.fd = None
        self.fd_closed = False
        self.role = role
        self.touched = line_no
        self.created_at = line_no
        self.events = 0

    def __repr__(self):
        return f"#{self.id}({self.role},{self.state},fd={self.fd})"


class Checker:
    def __init__(self, machine, examples=3):
        self.m = machine
        tr = machine.doc["trace"]
        self.line_re = re.compile(tr["line"])
        self.patterns = [(p["event"], re.compile(p["regex"]), p.get("role"))
                         for p in tr["patterns"]]
        self.implied = tr.get("implied", [])
        self.after_free = [re.compile(r) for r in tr.get("after_free_allowed", [])]
        self.examples = examples

        # (pid, ident) -> [Instance]
        self.conns = collections.defaultdict(list)
        self.next_id = 0

        self.violations = collections.OrderedDict()   # key -> {count, examples}
        self.stats = collections.Counter()
        self.state_hist = collections.Counter()

    # ------------------------------------------------------------ report

    def flag(self, kind, inst, event, line_no, line, detail=""):
        key = (kind, inst.state, event)
        v = self.violations.setdefault(key, {"count": 0, "examples": []})
        v["count"] += 1

        if len(v["examples"]) < self.examples:
            v["examples"].append({
                "line": line_no,
                "instance": repr(inst),
                "detail": detail,
                "text": line.rstrip("\n")[:200],
            })

    # ----------------------------------------------------------- routing

    def route(self, key, event, groups, line_no):
        """Pick the instance that a (event, groups) line belongs to."""
        insts = self.conns.get(key)
        if not insts:
            return None

        fd = groups.get("fd")
        fd = int(fd) if fd is not None else None

        live = [i for i in insts if i.state != "RELEASED"]

        if fd is not None and fd >= 0:
            same = [i for i in live if i.fd == fd and not i.fd_closed]
            if same:
                return same[-1]

            # First fd of a fresh instance: accept/connect assign it, and the
            # implied `accept` covers the first I/O line of an accepted conn.
            fresh = [i for i in live if i.fd is None]
            if fresh:
                return fresh[-1]

            # A closed fd number can still be logged by the close timer.
            same = [i for i in live if i.fd == fd]
            if same:
                return same[-1]

            if not live:
                # Everything under this ident is released: use after free.
                return insts[-1]

            self.stats["routed_by_recency"] += 1
            return max(live, key=lambda i: i.touched)

        # No fd in the line.
        if not live:
            return insts[-1]

        if len(live) == 1:
            return live[0]

        cands = live
        role = self.pattern_role
        if role:
            byrole = [i for i in cands if i.role == role]
            if byrole:
                cands = byrole

        ok = [i for i in cands
              if self.m.cell(i.state, event, groups)["verdict"] in ("transition", "ignored")]
        if len(ok) == 1:
            return ok[0]

        if ok:
            cands = ok

        self.stats["ambiguous_routing"] += 1
        return max(cands, key=lambda i: i.touched)

    # ------------------------------------------------------------ replay

    def apply(self, inst, event, groups, line_no, line):
        # Implied events first (accept, immediate connect).
        for imp in self.implied:
            if inst.state == imp["when_in"] and event in imp["before"]:
                self.stats[f"implied:{imp['event']}"] += 1
                self.step(inst, imp["event"], groups, line_no, line)

        self.step(inst, event, groups, line_no, line)

    def step(self, inst, event, groups, line_no, line):
        inst.touched = line_no
        inst.events += 1
        self.stats[f"event:{event}"] += 1

        # fd bookkeeping and cross-checks.
        fd = groups.get("fd")
        if fd is not None:
            fd = int(fd)
            if fd >= 0:
                if inst.fd is None:
                    inst.fd = fd

                elif inst.fd != fd and not inst.fd_closed:
                    self.flag("fd_mismatch", inst, event, line_no, line,
                              f"instance fd {inst.fd}, line fd {fd}")

        if event == "free":
            if inst.fd is not None and not inst.fd_closed:
                self.flag("fd_leak", inst, event, line_no, line,
                          f"fd {inst.fd} never closed")

        cell = self.m.cell(inst.state, event, groups)
        verdict = cell["verdict"]

        if verdict == "transition":
            inst.state = cell["to"]

        elif verdict == "ignored":
            pass

        else:
            self.flag(verdict, inst, event, line_no, line, cell["note"][:160])

        if event == "fd_close":
            inst.fd_closed = True

    def feed(self, line, line_no):
        mo = self.line_re.match(line)
        if mo is None:
            self.stats["unparsed_lines"] += 1
            return

        ident = mo.group("ident")
        if ident is None:
            self.stats["engine_lines"] += 1
            return

        key = (mo.group("pid"), ident)
        msg = mo.group("msg")

        event = None
        groups = None
        self.pattern_role = None

        for ev, rx, role in self.patterns:
            g = rx.match(msg)
            if g:
                event = ev
                groups = g.groupdict()
                self.pattern_role = role
                break

        insts = self.conns.get(key)

        if event == "create":
            role = "peer" if insts else "client"
            inst = Instance(self.next_id, role, line_no)
            self.next_id += 1
            inst.state = self.m.initial
            self.conns[key].append(inst)
            self.stats[f"conn:{role}"] += 1
            self.step(inst, "create", groups, line_no, line)
            return

        if not insts:
            # Listener tasks, engine tasks and other non-conn idents.
            self.stats["untracked_ident_lines"] += 1
            return

        if event is None:
            # An unmapped line: only illegal after every instance is freed.
            if all(i.state == "RELEASED" for i in insts):
                if not any(r.match(msg) for r in self.after_free):
                    self.flag("after_free", insts[-1], "(unmapped)", line_no, line)
            return

        inst = self.route(key, event, groups, line_no)
        self.apply(inst, event, groups, line_no, line)

    def finish(self):
        for insts in self.conns.values():
            for i in insts:
                self.state_hist[i.state] += 1

    # ------------------------------------------------------------ output

    def report(self, out=sys.stdout):
        c = self.m.counts()
        print(f"contract: {self.m.doc.get('machine')} @ {self.m.doc.get('source_commit')}"
              f" ({c['states']} states, {c['events']} events, {c['cells']} cells)", file=out)

        total = sum(v for k, v in self.stats.items() if k.startswith("conn:"))
        print(f"connections replayed: {total} "
              f"(client {self.stats['conn:client']}, peer {self.stats['conn:peer']})", file=out)
        print("final states: " + ", ".join(f"{s}={n}" for s, n in sorted(self.state_hist.items())),
              file=out)

        ev = sorted((k[6:], v) for k, v in self.stats.items() if k.startswith("event:"))
        print("events: " + ", ".join(f"{k}={v}" for k, v in ev), file=out)

        imp = sorted((k[8:], v) for k, v in self.stats.items() if k.startswith("implied:"))
        if imp:
            print("implied events: " + ", ".join(f"{k}={v}" for k, v in imp), file=out)

        print(f"routing: ambiguous={self.stats['ambiguous_routing']} "
              f"by_recency={self.stats['routed_by_recency']}; "
              f"non-conn ident lines={self.stats['untracked_ident_lines']}, "
              f"engine lines={self.stats['engine_lines']}, "
              f"unparsed={self.stats['unparsed_lines']}", file=out)

        n = sum(v["count"] for v in self.violations.values())
        print(f"illegal transitions: {n}", file=out)

        for (kind, state, event), v in self.violations.items():
            print(f"  [{kind}] state={state} event={event}: {v['count']}", file=out)
            for ex in v["examples"]:
                print(f"      line {ex['line']} {ex['instance']}: {ex['text']}", file=out)
                if ex["detail"]:
                    print(f"          {ex['detail']}", file=out)

        return n

    def to_json(self):
        return {
            "stats": dict(self.stats),
            "final_states": dict(self.state_hist),
            "violations": [
                {"kind": k[0], "state": k[1], "event": k[2], **v}
                for k, v in self.violations.items()
            ],
        }


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("yaml", help="contract, e.g. docs/fsm/conn.yaml")
    ap.add_argument("logs", nargs="+", help="unit.log files from a --debug build")
    ap.add_argument("--examples", type=int, default=3,
                    help="example lines kept per violation kind (default 3)")
    ap.add_argument("--json", help="also write a JSON report to this file")
    args = ap.parse_args()

    m = load_machine(args.yaml)
    if m.errors:
        for e in m.errors:
            print(f"error: {e}", file=sys.stderr)
        sys.exit(2)

    total = 0
    reports = {}

    for path in args.logs:
        ck = Checker(m, args.examples)

        with open(path, encoding="utf-8", errors="replace") as f:
            for n, line in enumerate(f, 1):
                ck.feed(line, n)

        ck.finish()
        print(f"== {path}")
        total += ck.report()
        reports[path] = ck.to_json()

    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(reports, f, indent=1)

    sys.exit(1 if total else 0)


if __name__ == "__main__":
    main()
