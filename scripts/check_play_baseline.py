#!/usr/bin/env python3
"""Exact seeded regression checks. Recording requires an explicit new output file."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DEFAULT = ROOT / 'tests/fixtures/play_results_baseline.json'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def data_hash():
    h = hashlib.sha256()
    for p in sorted((ROOT / 'data').rglob('*.json')):
        h.update(str(p.relative_to(ROOT)).encode())
        h.update(b'\0')
        h.update(p.read_bytes())
    return h.hexdigest()


def cases():
    offense = json.loads((ROOT / 'data/playbooks/spread_base.json').read_text())['plays']
    defense = json.loads((ROOT / 'data/playbooks/43_base.json').read_text())['plays']
    for o in offense:
        for d in defense:
            for seed, extra in [(1, []), (3, ['--hash', 'left', '--flip']),
                                (17, ['--hash', 'right', '--direction', 'left'])]:
                args = ['run-play', '--team', 'clemson', '--play', o['id'],
                        '--defense', 'georgia', '--def-play', d['id'], '--no-huddle',
                        '--seed', str(seed), *extra]
                yield dict(name=f"{o['id']}/{d['id']}/{seed}", args=args, output='--out')
    for kick in ['kickoff_deep', 'punt_normal']:
        for seed in [1, 8, 17]:
            yield dict(name=f'{kick}/{seed}', args=['run-kick', '--team', 'clemson',
                       '--vs', 'georgia', '--kick', kick, '--seed', str(seed)], output='--out')
    for seed in [1, 2, 3]:
        yield dict(name=f'game/{seed}', args=['play-game', '--home', 'georgia',
                   '--away', 'clemson', '--seed', str(seed)], output='--log')


def run(exe, case, directory):
    output = directory / 'output.jsonl'
    p = subprocess.run([str(exe), *case['args'], case['output'], str(output)],
                       cwd=ROOT, capture_output=True, check=True, timeout=180)
    # Only the destination varies between runs; retain every other stdout byte.
    summary = p.stdout.replace(str(output).encode(), b'<output>')
    return dict(summary=summary.decode(), tracking_sha256=digest(output.read_bytes()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, default=ROOT / 'build/release/cli/cfbsim')
    parser.add_argument('--baseline', type=Path, default=DEFAULT)
    parser.add_argument('--record', action='store_true', help='create a NEW baseline; never overwrite')
    args = parser.parse_args()
    stamp = data_hash()
    if args.record:
        if args.baseline.exists():
            parser.error('baseline exists; use a new --baseline path and review differences')
        baseline = dict(data_sha256=stamp, cases=list(cases()))
    else:
        baseline = json.loads(args.baseline.read_text())
        if baseline['data_sha256'] != stamp:
            parser.error('data changed since baseline capture; review intentional changes before recording a new baseline')
    failures = []
    with tempfile.TemporaryDirectory(prefix='cfbsim-baseline-') as tmp:
        for case in baseline['cases']:
            actual = run(args.exe.resolve(), case, Path(tmp))
            if args.record:
                case['expected'] = actual
            elif actual != case['expected']:
                failures.append(case['name'])
                print(f"FAIL {case['name']}: results and/or tracking differ")
    if data_hash() != stamp:
        raise SystemExit('data changed during the run')
    if args.record:
        with args.baseline.open('x') as f:
            json.dump(baseline, f, indent=2)
            f.write('\n')
    print(f"{len(baseline['cases'])} baseline cases; {len(failures)} differences")
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())
