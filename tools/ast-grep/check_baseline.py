#!/usr/bin/env python3
"""Fail on ast-grep violations in src/ not in baseline.json, keyed by rule,
file and text rather than line.  --update rewrites the baseline.

Exit codes: 0 no new violations, 1 new violations, 2 the scan failed.
"""
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
BASELINE = HERE / 'baseline.json'


def scan():
    cmd = ['ast-grep', 'scan', '-c', str(HERE / 'sgconfig.yml'), '--json']
    res = subprocess.run(
        cmd + ['src'], cwd=ROOT, capture_output=True, text=True
    )

    try:
        if res.returncode not in (0, 1):
            raise ValueError(f'exit {res.returncode}')
        matches = json.loads(res.stdout)
    except ValueError as e:
        sys.stderr.write(res.stdout + res.stderr)
        print(f'ast-grep scan failed: {e}')
        sys.exit(2)

    entries = []

    for m in matches:
        path = Path(m['file'])
        try:
            path = path.resolve().relative_to(ROOT)
        except ValueError:
            pass

        entries.append({
            'ruleId': m['ruleId'],
            'file': str(path),
            'line': m['range']['start']['line'],
            'text': m['text'].strip(),
        })

    return sorted(entries, key=lambda e: (e['ruleId'], e['file'], e['line']))


def key(e):
    return (e['ruleId'], e['file'], e['text'])


def main():
    matches = scan()

    if sys.argv[1:] == ['--update']:
        BASELINE.write_text(json.dumps(matches, indent=2) + '\n')
        print(f'wrote {len(matches)} entries to {BASELINE}')
        return 0

    known = {key(e) for e in json.loads(BASELINE.read_text())}
    new = [m for m in matches if key(m) not in known]

    if not new:
        print(f'ast-grep: {len(matches)} violation(s), all in baseline. OK.')
        return 0

    print(f'ast-grep: {len(new)} NEW violation(s) not in baseline.json:')
    for m in new:
        print(f"  {m['file']}:{m['line']}: [{m['ruleId']}] {m['text']}")
    print('\nIf reviewed and intended, run '
          "'python3 tools/ast-grep/check_baseline.py --update' and commit "
          'baseline.json with the change.')
    return 1


if __name__ == '__main__':
    sys.exit(main())
