# Bot mechanics regression checks

This directory checks frozen game states against the repository's unchanged Python rules engine. It uses `Game.step` as the independent oracle. It does not start full matches or measure bot strength.

## Run

Place the final single-file bot at `submission/bot.cpp` beside the original `engine/` directory. From a directory containing this `verification/` folder, run:

```bash
python3 verification/verify_mechanics.py
python3 verification/verify_endgame.py
```

The directory containing these scripts may be moved. Both scripts discover the original engine by walking their directory's ancestors and the current working directory. In this repository, their default bot resolves to `land/workspace/submission/bot.cpp`.

For an explicit candidate or a different layout:

```bash
python3 verification/verify_mechanics.py --bot /path/to/bot.cpp --engine /path/to/engine/game.py
python3 verification/verify_endgame.py --bot /path/to/bot.cpp --engine /path/to/engine/game.py
```

Paths in the example are placeholders. Both scripts accept `--out /path/to/report.json` and `--cxx g++`. They lower their own process priority and compile the probe with `-O2 -std=c++20` at low priority where the operating system supports it.

## Required files and tools

The complete package consists of these seven files:

| File | Purpose |
| --- | --- |
| `verify_mechanics.py` | Mechanics runner and shared fixture/oracle helpers |
| `mechanics_probe.cpp` | Adapter to the bot's home-rescue and self-reset functions |
| `fixtures.json` | 225 compact mechanics snapshots |
| `verify_endgame.py` | Endgame state and exhaustive small-depth search runner |
| `endgame_probe.cpp` | Adapter to the bot's endgame functions |
| `terminal_fixtures.json` | 12 compact real terminal snapshots |
| `README.md` | This usage and scope description |

Dependencies are Python 3.10 or newer, its standard library, a C++20 compiler with `std::popcount`, the original `engine/game.py`, and the selected bot source. No third-party Python packages, network, opponent programs, historical benchmark directories, or large replay files are required. Generated JSON reports are evidence outputs; they are not input dependencies. Temporary build directories and compiled binaries do not belong in the deliverable.

The probes include the source with `LAND_TEST` defined. The mechanics adapter expects the bot's existing `Bot` interfaces `duelCaptureMaps`, `resolveDuel`, `rescueHome`, `finalizeMove`, `resetOptions`, and `simulateReset`. The endgame adapter additionally expects `endState`, `endStep`, `endSolve`, `exactEndgame`, and `endInitialDirections`. Use the endgame runner when the selected source contains that module.

## Coverage

`verify_mechanics.py` checks 171 duel states and all 1,539 legal action pairs, plus 472 self-reset paths from 60 states. The combined fixture count is 225 because some snapshots participate in both checks. Expected outcomes include the full territory owner grid, areas, deaths, captures, protected cells, simultaneous overlapping claims, trail priority, respawn position and initial direction, retained territory, intermediate captures before death, and the scoped home-rescue action/cache rule.

`verify_endgame.py` checks the full owner and trail grids, position, direction, outside-line flags, and areas for all 2,025 legal action pairs across the 225 snapshots. It also performs exhaustive pure maximin at depths one through three on 12 real terminal states and four corner cases. The Python oracle enumerates every legal branch without alpha-beta pruning. The final coverage is 4,599 oracle steps, 48 root choices, already-expired deadline fallback at depths 1, 3, and 8, and searches that expire after beginning. A missed required timer case is reported as a failed coverage check, rather than silently skipped.

The reports include the tested source SHA256, original engine SHA256, fixture and probe SHA256 values, detailed failures, and tested counts. Each compilation uses a temporary source snapshot, so a later concurrent source edit cannot change the program after its hash is recorded. The exit status is zero only when the checks complete successfully.

## Scope

These tests validate the modeled mechanics and the endgame search's small-depth values, tie behavior, and timeout fallback. They do not validate all strategic forecasts, all possible boards, full-match runtime behavior, or an 80% win-rate target. Those claims require separate benchmark results for the exact same source hash.

## TP approach regression

The optional TP regression adds `verify_tp_approach.py`, `tp_approach_probe.cpp`, and `tp_approach_fixtures.json` to the seven files above. It imports the existing mechanics runner's fixture helpers. These three files contain the full test inputs; neither historical replays nor the separate audit report is a runtime dependency.

Run the following from the repository root when the full-match benchmark is idle:

```bash
python3 land/analysis/domination/verification/verify_tp_approach.py --expect legacy --out land/analysis/domination/verification/tp_legacy_report.json
python3 land/analysis/domination/verification/verify_tp_approach.py --bot land/analysis/domination/candidate_tp_own_approach.cpp --out land/analysis/domination/verification/tp_patched_report.json
```

The first command uses the default `submission/bot.cpp` and expects the frozen 389 behavior: both real states begin with U and miss the immediate two-step approach. Use it before replacing that legacy submission, or pass `--bot` with its frozen source. The default expectation for the second command is `patched`, which requires the complete routes DR and RD. A later final submission containing the patch can be checked without `--bot`.

The adapter calls only `fields`, spawn preparation, and `forcedCutPath`; it does not call `decide` or run games. It checks two real snapshots and nine disabled-condition controls per snapshot: phase 0, phase 1, four players on a 48×48 board, an existing own trail, a non-owned first cell, enemy home distance 1, one remaining turn, an illegal reversing first move, and a cleared victim trail. The controls mutate only the relevant model inputs; they do not claim to simulate all future enemy choices. Invalid victim IDs are not sent to the legacy implementation, which lacks that public-API guard.

The Python runner independently recomputes the two real-state guarantees with the original `Game.step`: 18 steps across 14 early-terminated branches, all of which must kill the target within two moves. It compares the full recorded first-step and terminal branch results. Source, engine, fixture, and probe hashes are written to the report. This regression checks the narrow TP admission rule; it makes no win-rate claim.
