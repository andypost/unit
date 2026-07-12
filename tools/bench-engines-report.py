#!/usr/bin/env python3
# bench-engines-report.py — summarize bench-engines.sh results into a markdown table.
#
# Usage: bench-engines-report.py <results-file> <labelA> <labelB>
#
# Reads lines of the form:
#   <label> <scenario> <tool> rps=.. p50ms=.. p99ms=.. failed=.. cpu_us_per_req=.. engine=..
# and, per scenario, prints mean +/- stdev across rounds for each label plus the
# B-vs-A delta % (oha rows only; ab rows are a secondary continuity metric and
# summarized separately). SKIP lines are reported as skipped scenarios.

import sys
import math
from collections import defaultdict


def stats(xs):
    xs = [x for x in xs if x is not None]
    if not xs:
        return (float("nan"), float("nan"), 0)
    n = len(xs)
    mean = sum(xs) / n
    if n < 2:
        return (mean, 0.0, n)
    var = sum((x - mean) ** 2 for x in xs) / (n - 1)
    return (mean, math.sqrt(var), n)


def fmt(mean, sd):
    if math.isnan(mean):
        return "-"
    return f"{mean:.0f} ± {sd:.0f}" if mean >= 100 else f"{mean:.2f} ± {sd:.2f}"


def delta(a, b):
    if a is None or b is None or math.isnan(a) or math.isnan(b) or a == 0:
        return "-"
    return f"{(b - a) / a * 100:+.1f}%"


def parse(path):
    # rows[(tool, scenario, label)][metric] = list of values
    rows = defaultdict(lambda: defaultdict(list))
    skips = []
    engines = {}
    for line in open(path):
        f = line.split()
        if len(f) >= 4 and f[2] == "SKIP":
            skips.append((f[0], " ".join(f[3:])))
            continue
        if len(f) < 4 or "=" not in f[3]:
            continue
        label, scenario, tool = f[0], f[1], f[2]
        kv = {}
        for tok in f[3:]:
            if "=" in tok:
                k, v = tok.split("=", 1)
                kv[k] = v
        for m in ("rps", "p50ms", "p99ms", "failed", "cpu_us_per_req"):
            v = kv.get(m)
            try:
                rows[(tool, scenario, label)][m].append(float(v))
            except (TypeError, ValueError):
                rows[(tool, scenario, label)][m].append(None)
        if kv.get("engine"):
            engines[label] = kv["engine"]
    return rows, skips, engines


def table(rows, tool, la, lb, metric, unit):
    scenarios = sorted({s for (t, s, _l) in rows if t == tool})
    if not scenarios:
        return
    print(f"\n#### {tool}: {metric} ({unit}) — mean ± stdev over rounds")
    print(f"| scenario | {la} | {lb} | Δ ({lb} vs {la}) |")
    print("|---|---|---|---|")
    for s in scenarios:
        am, asd, _ = stats(rows.get((tool, s, la), {}).get(metric, []))
        bm, bsd, _ = stats(rows.get((tool, s, lb), {}).get(metric, []))
        print(f"| {s} | {fmt(am, asd)} | {fmt(bm, bsd)} | {delta(am, bm)} |")


def main():
    if len(sys.argv) < 4:
        print("usage: bench-engines-report.py <results-file> <labelA> <labelB>",
              file=sys.stderr)
        return 2
    path, la, lb = sys.argv[1], sys.argv[2], sys.argv[3]
    rows, skips, engines = parse(path)

    print("## bench-engines summary")
    if engines:
        print("engines: " + "  ".join(f"{k}={v}" for k, v in engines.items()))

    for tool in ("oha", "ab"):
        table(rows, tool, la, lb, "rps", "req/s, higher better")
        table(rows, tool, la, lb, "p99ms", "ms, lower better")
        # CPU scope differs per scenario: http rows measure the router process
        # only, php_ka measures the whole unitd tree incl. PHP workers.
        table(rows, tool, la, lb, "cpu_us_per_req",
              "µs/req — router only for http scenarios, "
              "whole unitd tree for php_ka; lower better")
    # p50 only meaningful for oha (ab p50 is NA)
    table(rows, "oha", la, lb, "p50ms", "ms, lower better")

    if skips:
        print("\n#### skipped")
        for label, reason in skips:
            print(f"- {label}: {reason}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
