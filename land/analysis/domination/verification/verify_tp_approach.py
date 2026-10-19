#!/usr/bin/env python3
"""Check the two frozen TP collision states and their narrow admission gates.

Run only when the full-match benchmark is idle. This compiles a small C++20
adapter at low priority; it calls fields/forcedCutPath, never decide or a match.

Current 389 behavior (expected to miss both approaches):
  python verify_tp_approach.py --expect legacy --out tp_legacy_report.json
Patched candidate (must select D then R in the two respective snapshots):
  python verify_tp_approach.py --bot ../candidate_tp_own_approach.cpp

Default bot: submission/bot.cpp beside the original engine directory.
The default fixtures are tp_approach_fixtures.json. That file contains
complete snapshots, so no historical replay directory is needed.
"""
from __future__ import annotations

import argparse
import copy
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

from verify_mechanics import find_engine, legal, restore, sha256

HERE = Path(__file__).resolve().parent
CORE_CONTROLS = ["phase-0", "phase-1", "n4", "own-line", "first-nonown", "home-1",
                 "last-1", "first-reverse", "victim-cleared"]


def oracle_matrix(game, mine: int, route: str, oracle) -> tuple[list, list, int]:
    """Enumerate actual Game.step transitions; stop as soon as the victim dies."""
    if len(route) != 2 or any(d not in oracle.DIRS for d in route):
        raise ValueError("Each frozen guarantee must contain exactly two legal directions")
    enemy = 1 - mine
    if route[0] not in legal(game, mine, oracle) or route[1] == oracle.OPPOSITE[route[0]]:
        raise ValueError("Frozen route reverses direction")
    first, terminal, calls = [], [], 0
    for response in legal(game, enemy, oracle):
        after = copy.deepcopy(game)
        moves = [None, None]
        moves[mine], moves[enemy] = route[0], response
        frame = after.step(moves)
        calls += 1
        dead = [after.deaths[i] > game.deaths[i] for i in range(2)]
        first.append({"moves": moves, "events": frame.get("events", []), "dead": dead,
                      "areas": after.areas(), "pos": [list(p) for p in after.pos]})
        if dead[enemy]:
            terminal.append({"moves": [moves], "victim_dead": True, "mine_dead": dead[mine]})
            continue
        if dead[mine]:
            terminal.append({"moves": [moves], "victim_dead": False, "mine_dead": True})
            continue
        for second in legal(after, enemy, oracle):
            end = copy.deepcopy(after)
            next_moves = [None, None]
            next_moves[mine], next_moves[enemy] = route[1], second
            frame2 = end.step(next_moves)
            calls += 1
            terminal.append({"moves": [moves, next_moves],
                             "victim_dead": end.deaths[enemy] > game.deaths[enemy],
                             "mine_dead": end.deaths[mine] > game.deaths[mine],
                             "events": frame2.get("events", [])})
    return first, terminal, calls


def check_controls(state: dict, answer: dict, expectation: str, failures: list) -> dict:
    wanted, mine = state["own_route"][0], state["mine"]
    names = ["real", *CORE_CONTROLS]
    trials = answer["trials"]
    if [t["name"] for t in trials] != names:
        failures.append({"state": state["id"], "check": "control_coverage",
                         "expected": names, "actual": [t["name"] for t in trials]})
        return {"controls": 0, "observed_path": ""}
    real = trials[0]
    required = {"players": 2, "phase": 2, "own_length": 0, "first_legal": 1,
                "first_collision": 1, "first_owner": mine}
    if any(real[k] != value for k, value in required.items()) or real["enemy_home"] < 2:
        failures.append({"state": state["id"], "check": "real_fixture_preconditions", "actual": real})
    if expectation == "patched":
        if real["path"] != state["own_route"]:
            failures.append({"state": state["id"], "check": "patched_approach",
                             "expected": state["own_route"], "actual": real["path"]})
    elif not real["path"] or real["path"][0] != "U":
        failures.append({"state": state["id"], "check": "legacy_missed_approach",
                         "expected_first": "U", "actual": real["path"]})

    settings = {"phase-0": ("phase", 0), "phase-1": ("phase", 1), "n4": ("players", 4),
                "own-line": ("own_length", 1), "first-nonown": ("first_owner", -1),
                "home-1": ("enemy_home", 1), "last-1": ("remaining", 1),
                "first-reverse": ("first_legal", 0), "victim-cleared": ("victim_length", 0)}
    for result in trials[1:]:
        name, path = result["name"], result["path"]
        if name in settings:
            key, expected = settings[name]
            if result[key] != expected:
                failures.append({"state": state["id"], "check": "control_setup", "control": name,
                                 "key": key, "expected": expected, "actual": result[key]})
        if path and path[0] == wanted:
            failures.append({"state": state["id"], "check": "forbidden_approach",
                             "control": name, "forbidden_first": wanted, "actual": path})
        if name == "victim-cleared" and path:
            failures.append({"state": state["id"], "check": "invalid_target_not_empty",
                             "control": name, "actual": path})
    return {"controls": len(trials) - 1, "observed_path": real["path"]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bot", type=Path)
    parser.add_argument("--engine", type=Path)
    parser.add_argument("--fixtures", type=Path, default=HERE / "tp_approach_fixtures.json")
    parser.add_argument("--out", type=Path, default=HERE / "tp_approach_report.json")
    parser.add_argument("--expect", choices=("patched", "legacy"), default="patched",
                        help="patched requires DR/RD; legacy requires the original U/U avoidance.")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    args = parser.parse_args()
    started = time.monotonic()
    if hasattr(os, "nice"):
        try:
            os.nice(19)
        except OSError:
            pass
    engine = find_engine(args.engine)
    bot = args.bot.resolve() if args.bot else engine.parent.parent / "submission" / "bot.cpp"
    source_bytes, engine_bytes = bot.read_bytes(), engine.read_bytes()
    fixture_bytes = args.fixtures.read_bytes()
    fixtures = json.loads(fixture_bytes)
    states = fixtures["cases"]
    if len(states) != 2 or [(s["turn"], s["own_route"]) for s in states] != [(236, "DR"), (237, "RD")]:
        raise ValueError("Expected the two frozen T236/T237 collision fixtures")
    if any(not re.fullmatch(r"[A-Za-z0-9_-]+", s["id"]) for s in states):
        raise ValueError("Fixture ID must be safe for the probe's text protocol")
    spec = importlib.util.spec_from_file_location("arena_tp_original_oracle", engine)
    oracle = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(oracle)
    games = [restore(state, fixtures["boards"], oracle) for state in states]
    failures, matrices = [], []
    oracle_calls = terminal_branches = 0
    for state, game in zip(states, games):
        first, terminal, calls = oracle_matrix(game, state["mine"], state["own_route"], oracle)
        oracle_calls += calls
        terminal_branches += len(terminal)
        if first != state["first_steps"] or terminal != state["terminal_branches"]:
            failures.append({"state": state["id"], "check": "recorded_oracle_matrix",
                             "first_steps_equal": first == state["first_steps"],
                             "terminal_branches_equal": terminal == state["terminal_branches"]})
        if not terminal or not all(branch["victim_dead"] for branch in terminal):
            failures.append({"state": state["id"], "check": "guaranteed_kill", "branches": terminal})
        matrices.append({"id": state["id"], "route": state["own_route"],
                         "first_steps": first, "terminal_branches": terminal})
    if oracle_calls != 18 or terminal_branches != 14:
        failures.append({"check": "oracle_coverage", "step_calls": oracle_calls,
                         "terminal_branches": terminal_branches})
    request = [str(len(states))]
    for state, game in zip(states, games):
        request.append(f"CASE {state['id']} {state['own_route'][0]}")
        request.append(f"INIT {game.W} {game.H} {game.cfg['max_turns']} 2 {state['mine']} 50 2000")
        request.extend(f"SPAWN {i} {x} {y}" for i, (x, y) in enumerate(game.spawns))
        request.append("INITIAL " + " ".join(game.init_dirs))
        request.append(game.state_text(game.turn))
    probe = HERE / "tp_approach_probe.cpp"
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="tp-approach-build-", dir=args.out.parent) as temporary:
        build = Path(temporary).resolve()
        snapshot, binary = build / "tested_bot.cpp", build / "tp_approach_probe"
        snapshot.write_bytes(source_bytes)
        command = [args.cxx, "-O2", "-std=c++20", "-DBOT_SOURCE=" + json.dumps(str(snapshot)),
                   str(probe), "-o", str(binary)]
        if shutil.which("nice"):
            command = ["nice", "-n", "19", *command]
        compiled = subprocess.run(command, capture_output=True, text=True, timeout=120)
        if compiled.returncode:
            raise RuntimeError("TP probe compilation failed:\n" + compiled.stderr[-8000:])
        run = subprocess.run([str(binary)], input="\n".join(request) + "\n", capture_output=True,
                             text=True, timeout=30, check=True)
        answers = [json.loads(line) for line in run.stdout.splitlines()]
    if len(answers) != len(states) or any(a["id"] != s["id"] for a, s in zip(answers, states)):
        raise ValueError("TP probe output does not match the requested fixture sequence")
    observed, controls = [], 0
    for state, answer in zip(states, answers):
        result = check_controls(state, answer, args.expect, failures)
        controls += result["controls"]
        observed.append({"id": state["id"], **result})
    summary = {"passed": not failures, "expectation": args.expect, "real_states": len(states),
               "control_cases": controls, "oracle_step_calls": oracle_calls,
               "oracle_terminal_branches": terminal_branches, "failures": len(failures)}
    report = {**summary, "source": str(bot), "engine": str(engine),
              "sha256": {"source": sha256(source_bytes), "engine": sha256(engine_bytes),
                         "fixtures": sha256(fixture_bytes), "probe": sha256(probe.read_bytes())},
              "engine_sha256_when_fixtures_packed": fixtures["engine_sha256"],
              "scope": "Original Game.step checks two real states. Model-field controls check admission boundaries; no full games or win-rate claims.",
              "observed": observed, "trials": answers, "oracle_matrices": matrices,
              "failure_details": failures, "elapsed_seconds": round(time.monotonic() - started, 3)}
    args.out.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({**summary, "observed": observed, "source_sha256": report["sha256"]["source"],
                      "report": str(args.out.resolve())}, ensure_ascii=False, indent=2))
    return 0 if not failures else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"TP approach verification could not complete: {error}", file=sys.stderr)
        raise SystemExit(2)
