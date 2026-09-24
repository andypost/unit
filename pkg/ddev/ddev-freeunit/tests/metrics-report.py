#!/usr/bin/env python3
"""Turn metrics.sh's raw `hey` outputs into JSON and a markdown table.

    metrics-report.py <raw dir> <out.json>

The table goes to stdout and, in GitHub Actions, to $GITHUB_STEP_SUMMARY.
Each figure is the median of the repeated runs.
"""
import glob
import json
import os
import re
import statistics
import sys

SERVERS = ["nginx-fpm", "freeunit"]
CASES = ["anon_front", "anon_node", "uncached_front", "uncached_node", "static"]


def parse_hey(text):
    rps = float(re.search(r"Requests/sec:\s+([\d.]+)", text).group(1))
    lat = dict(re.findall(r"(\d+)% in ([\d.]+) secs", text))
    codes = [(int(c), int(n)) for c, n in re.findall(r"\[(\d+)\]\s+(\d+) responses", text)]
    errs = text.split("Error distribution:")[1] if "Error distribution:" in text else ""
    errors = sum(n for c, n in codes if c >= 400)
    errors += sum(int(n) for n in re.findall(r"^\s+\[(\d+)\]", errs, re.M))
    return {
        "rps": rps,
        "p50_ms": float(lat["50"]) * 1000,
        "p99_ms": float(lat["99"]) * 1000,
        "errors": errors,
        "requests": sum(n for _, n in codes),
    }


def kv(path):
    with open(path) as f:
        return dict(line.strip().split("=", 1) for line in f if "=" in line)


def mem_mib(s):
    m = re.match(r"([\d.]+)\s*([KMG]i?B)", s)
    if not m:
        return None
    return float(m.group(1)) * {"KiB": 1 / 1024, "MiB": 1, "GiB": 1024}.get(m.group(2), 1)


def main(raw, out):
    result = {"env": {k: os.environ.get(k) for k in
                      ("PROJECT_TYPE", "DRUPAL_CONSTRAINT", "PHP_VERSION", "DURATION",
                       "CONCURRENCY", "REPEAT", "GITHUB_SHA", "RUNNER_NAME")},
              "servers": {}}
    vfile = os.path.join(raw, "versions.txt")
    if os.path.exists(vfile):
        result["env"]["drush_status"] = open(vfile).read().strip()
    for server in SERVERS:
        s = {"cases": {}}
        for case in CASES:
            runs = [parse_hey(open(p).read())
                    for p in sorted(glob.glob(os.path.join(raw, f"{server}.{case}.*.txt")))]
            if not runs:
                continue
            s["cases"][case] = {
                k: statistics.median(r[k] for r in runs)
                for k in ("rps", "p50_ms", "p99_ms", "errors")
            }
            s["cases"][case]["runs"] = runs
        cold = kv(os.path.join(raw, f"{server}.cold.txt"))
        code, first = cold["first_request"].split()
        s["restart_s"] = float(cold["restart_s"])
        s["first_request_ms"] = float(first) * 1000
        s["first_request_status"] = int(code)
        mem = open(os.path.join(raw, f"{server}.mem.txt")).read().strip()
        s["memory"] = mem
        s["memory_mib"] = mem_mib(mem)
        result["servers"][server] = s

    with open(out, "w") as f:
        json.dump(result, f, indent=2)

    env = result["env"]
    md = [f"### ddev-freeunit metrics: {env['PROJECT_TYPE']} ({env['DRUPAL_CONSTRAINT']}), "
          f"PHP {env['PHP_VERSION']}",
          "",
          f"`hey -z {env['DURATION']} -c {env['CONCURRENCY']}` through ddev-router (HTTP), "
          f"median of {env['REPEAT']} runs.",
          "",
          "| case | server | req/s | p50 ms | p99 ms | errors |",
          "|---|---|--:|--:|--:|--:|"]
    for case in CASES:
        for server in SERVERS:
            c = result["servers"][server]["cases"].get(case)
            if c:
                md.append(f"| {case} | {server} | {c['rps']:.0f} | {c['p50_ms']:.1f} | "
                          f"{c['p99_ms']:.1f} | {c['errors']:.0f} |")
    md += ["", "| server | web container memory after load | `ddev restart` s | "
               "first request ms (cold) |", "|---|--:|--:|--:|"]
    for server in SERVERS:
        s = result["servers"][server]
        md.append(f"| {server} | {s['memory']} | {s['restart_s']:.1f} | "
                  f"{s['first_request_ms']:.0f} ({s['first_request_status']}) |")
    text = "\n".join(md) + "\n"
    print(text)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a") as f:
            f.write(text)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
