# Play-result baseline

`play_results_baseline.json` was captured from the Release engine after pursuit
optimization #1, before movement precomputation (#2). It freezes the current
behavior for performance refactors; football-quality assertions remain in the
C++ tests.

Run from the repository root:

```sh
ctest --preset release -R '^Baseline.PlayResults$'
# Or, without CTest:
python3 scripts/check_play_baseline.py --exe build/release/cli/cfbsim
```

CTest registers this check when Python 3 is available. The fixture includes 108
plays (all six offensive plays against all six defensive calls, with three
seed/hash/flip/direction configurations), six kick returns, and three complete
game logs. Each case checks the exact CLI result text and SHA-256 of every byte
of its tracking file or game log. Only the output destination in CLI text is
normalized. Temporary tracking files are removed after the run.

The fixture also records a fingerprint of the JSON data. A data change fails
explicitly rather than silently comparing different inputs. Intentional data or
behavior changes require reviewing a separately recorded baseline:

```sh
python3 scripts/check_play_baseline.py --record --baseline out/proposed_baseline.json
```

Recording never overwrites a baseline. Review the proposed results before
replacing the checked-in fixture; do not regenerate it to conceal an unexplained
regression. Performance-only changes must pass the existing fixture unchanged.
