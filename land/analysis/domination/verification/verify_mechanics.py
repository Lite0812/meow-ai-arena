#!/usr/bin/env python3
"""Portable mechanics regression checks using the original Game.step as oracle.

Run from any directory:
  python verify_mechanics.py --bot /path/to/bot.cpp

Without --bot, the default is submission/bot.cpp next to the discovered engine/
directory. --engine may select an explicit original game.py or engine directory.
The three required files are this script, mechanics_probe.cpp, and fixtures.json;
historical runs/replays and earlier audit scripts are not needed.

This compiles one O2 C++20 probe at low priority and checks frozen states. It does
not run full matches, search for strategies, or claim anything about win rate.
"""
from __future__ import annotations

import argparse
from collections import Counter
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def find_engine(explicit: Path | None) -> Path:
    if explicit:
        path = explicit.resolve()
        return path / "game.py" if path.is_dir() else path
    for root in (HERE, *HERE.parents, Path.cwd()):
        for relative in ("engine/game.py", "workspace/engine/game.py", "land/workspace/engine/game.py"):
            candidate = root / relative
            if candidate.is_file():
                return candidate.resolve()
    raise FileNotFoundError("Original engine/game.py not found; pass --engine /path/to/game.py")


def decode_rle(pairs: list[int], cells: int) -> list[int]:
    if len(pairs) % 2:
        raise ValueError("RLE must contain value/count pairs")
    values = []
    for owner, count in zip(pairs[::2], pairs[1::2]):
        if owner not in (-1, 0, 1) or count <= 0:
            raise ValueError("Invalid owner or RLE count")
        values.extend([owner] * count)
    if len(values) != cells:
        raise ValueError(f"Grid has {len(values)} cells; expected {cells}")
    return values


def restore(state: dict, boards: list[dict], oracle):
    board = boards[state["board"]]
    game = oracle.Game(board["config"], board["spawns"], board["init_dirs"])
    if game.N != 2:
        raise ValueError("This mechanics suite requires two active players")
    game.owner = decode_rle(state["owner_rle"], game.W * game.H)
    game.trail = decode_rle(state["trail_rle"], game.W * game.H)
    # These mechanics use trail membership, not historical drawing order.
    game.trails = [[k for k, player in enumerate(game.trail) if player == i] for i in range(game.N)]
    for i, (x, y, direction, deaths, out) in enumerate(state["players"]):
        game.pos[i], game.dir[i], game.deaths[i], game.out[i] = (x, y), direction, deaths, out
    if any(game.out):
        raise ValueError("Duel kernel snapshots must have both players active")
    game.turn = state["turn"]
    return game


def owner_text(game) -> str:
    return "".join("." if owner < 0 else str(owner) for owner in game.owner)


def legal(game, player: int, oracle) -> list[str]:
    return [d for d in oracle.DIRS if d != oracle.OPPOSITE[game.dir[player]]]


def check_duel(state: dict, game, answer: dict, oracle, coverage: Counter, failures: list) -> dict:
    mine, enemy = state["mine"], 1 - state["mine"]
    outcomes = {}
    claims = [set(game._enclosed(i)) for i in range(2)]
    for branch in answer["matrix"]:
        mine_dir, enemy_dir = branch["mine"], branch["enemy"]
        moves = [None, None]
        moves[mine], moves[enemy] = mine_dir, enemy_dir
        actual = copy.deepcopy(game)
        frame = actual.step(moves)
        deaths = [actual.deaths[i] > game.deaths[i] for i in range(2)]
        captures = {e["player"] for e in frame["events"] if e["type"] == "capture"}
        checks = {"owner": branch["owner"] == owner_text(actual), "areas": branch["areas"] == actual.areas(),
                  "dead": branch["dead"] == deaths, "captured": branch["captured"] == [i in captures for i in range(2)]}
        if not all(checks.values()):
            differing = [k for k, (a, b) in enumerate(zip(branch["owner"], owner_text(actual))) if a != b]
            failures.append({"state": state["id"], "labels": state["labels"], "check": "duel",
                             "moves": moves, "checks": checks, "first_different_cells": differing[:8],
                             "actual_areas": actual.areas(), "predicted_areas": branch["areas"]})
        reasons = {e["player"]: e["reason"] for e in frame["events"] if e["type"] == "death"}
        for reason in reasons.values():
            coverage["death_" + reason] += 1
        if captures:
            coverage["capture_branches"] += 1
            for player in captures:
                coverage["protected_capture_attempts"] += any(game.protected[k] not in (-1, player)
                    for k in claims[player] | set(game.trails[player]))
        if len(captures) == 2:
            coverage["simultaneous_captures"] += 1
            coverage["competing_enclosure_claims"] += bool(claims[0] & claims[1])
            coverage["trail_over_enclosure_priority"] += bool((set(game.trails[0]) & claims[1])
                                                               | (set(game.trails[1]) & claims[0]))
        outcomes[mine_dir, enemy_dir] = {"areas": actual.areas(), "dead": deaths, "reasons": reasons}
    directions = legal(game, mine, oracle)
    responses = legal(game, enemy, oracle)
    if set(outcomes) != {(d, z) for d in directions for z in responses}:
        failures.append({"state": state["id"], "check": "all_nine_legal_action_pairs"})
        return {"pairs": len(outcomes), "triggered": False, "overridden": False}

    # Derive collision rejection from the oracle's actual head-death events,
    # rather than reimplementing the C++ collision detector.
    candidates = []
    if game.trails[mine]:
        for d in directions:
            x, y = game.pos[mine]
            dx, dy = oracle.DIRS[d]
            x, y = x + dx, y + dy
            if not (0 <= x < game.W and 0 <= y < game.H):
                continue
            k = y * game.W + x
            if game.owner[k] == mine and game.trail[k] != mine and any(
                    outcomes[d, z]["reasons"].get(mine) == "head" for z in responses):
                candidates.append(d)
    worst = {d: min(outcomes[d, z]["areas"][mine] - outcomes[d, z]["areas"][enemy]
                    for z in responses) for d in directions}
    admissible = {d: all(not outcomes[d, z]["dead"][mine] or outcomes[d, z]["dead"][enemy]
                         for z in responses) for d in candidates}
    for initial in directions:
        expected = initial
        for d in candidates:
            if admissible[d] and worst[d] > worst[expected]:
                expected = d
        if answer["rescues"].get(initial) != expected:
            failures.append({"state": state["id"], "check": "home_rescue", "original": initial,
                             "expected": expected, "actual": answer["rescues"].get(initial),
                             "worst": worst, "admissible": admissible})
    if not answer["rescue_cache_correct"]:
        failures.append({"state": state["id"], "check": "rescue_clears_obsolete_plans"})
    selected = answer["rescues"][state["original"]]
    return {"pairs": len(outcomes), "triggered": bool(candidates), "overridden": selected != state["original"],
            "selected": selected, "original": state["original"], "worst": worst, "admissible": admissible}


def check_resets(state: dict, game, answer: dict, coverage: Counter, failures: list) -> int:
    mine = state["mine"]
    weights = answer["weights"]
    for option in answer["resets"]:
        actual = copy.deepcopy(game)
        actual.out[1 - mine] = True  # Isolate the claimed physical self-reset path.
        death_step, reason, captured_before_death = None, None, False
        kept_territory = False
        for step, d in enumerate(option["route"], 1):
            before_owner = actual.owner.copy()
            moves = [None, None]
            moves[mine] = d
            frame = actual.step(moves)
            captured_before_death |= any(e["type"] == "capture" and e["player"] == mine for e in frame["events"])
            if actual.deaths[mine] > game.deaths[mine]:
                death_step = step
                reason = next(e["reason"] for e in frame["events"] if e["type"] == "death" and e["player"] == mine)
                kept_territory = actual.owner == before_owner
                break
        value = sum(weights[old + 1] for old, new in zip(game.owner, actual.owner) if new == mine and old != mine)
        checks = {"first_death_step": death_step == len(option["route"]) == option["simulated_step"],
                  "secured_value": abs(value - option["secured"]) < 1e-7,
                  "simulate_reset_value": abs(value - option["simulated_value"]) < 1e-7,
                  "territory_survives_death": kept_territory,
                  "spawn_position": tuple(actual.pos[mine]) == tuple(game.spawns[mine]),
                  "spawn_direction": actual.dir[mine] == game.init_dirs[mine],
                  "old_trail_cleared": not actual.trails[mine] and mine not in actual.trail}
        if not all(checks.values()):
            failures.append({"state": state["id"], "labels": state["labels"], "check": "reset", "checks": checks,
                             "route": option["route"], "actual_death_step": death_step,
                             "actual_value": value, "prediction": option})
        coverage["reset_" + str(reason)] += 1
        coverage["reset_capture_before_death"] += captured_before_death
        coverage["reset_positive_secured_value"] += value > 0
    return len(answer["resets"])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bot", type=Path, help="Default: submission/bot.cpp beside engine/.")
    parser.add_argument("--engine", type=Path, help="Original game.py, or the directory containing it.")
    parser.add_argument("--fixtures", type=Path, default=HERE / "fixtures.json")
    parser.add_argument("--out", type=Path, default=HERE / "mechanics_report.json")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"), help="C++ compiler executable.")
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
    if fixtures["version"] != 1:
        raise ValueError("Unsupported fixture format")
    spec = importlib.util.spec_from_file_location("arena_original_mechanics_oracle", engine)
    oracle = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(oracle)
    states = fixtures["states"]
    games = [restore(s, fixtures["boards"], oracle) for s in states]
    request = [str(len(states))]
    for state, game in zip(states, games):
        mine = state["mine"]
        request.append(f"CASE {state['id']} {int('duel' in state['checks'])} {int('reset' in state['checks'])}")
        request.append(f"INIT {game.W} {game.H} {game.cfg['max_turns']} 2 {mine} 50 2000")
        request.extend(f"SPAWN {i} {x} {y}" for i, (x, y) in enumerate(game.spawns))
        request.append("INITIAL " + " ".join(game.init_dirs))
        request.append(game.state_text(game.turn))
    probe_source = HERE / "mechanics_probe.cpp"
    args.out.parent.mkdir(parents=True, exist_ok=True)
    # Snapshot the submitted source so a concurrent edit cannot change the code
    # being checked after its hash is recorded.
    with tempfile.TemporaryDirectory(prefix="mechanics-build-", dir=args.out.parent) as temporary:
        build = Path(temporary).resolve()
        tested_source, binary = build / "tested_bot.cpp", build / "mechanics_probe"
        tested_source.write_bytes(source_bytes)
        command = [args.cxx, "-O2", "-std=c++20", "-DBOT_SOURCE=" + json.dumps(str(tested_source)),
                   str(probe_source), "-o", str(binary)]
        if shutil.which("nice"):
            command = ["nice", "-n", "19", *command]
        compiled = subprocess.run(command, text=True, capture_output=True, timeout=120)
        if compiled.returncode:
            raise RuntimeError("Probe compilation failed:\n" + compiled.stderr[-8000:])
        run = subprocess.run([str(binary)], input="\n".join(request) + "\n", text=True,
                             capture_output=True, timeout=120, check=True)
        answers = [json.loads(line) for line in run.stdout.splitlines()]
    if len(answers) != len(states) or any(a["id"] != s["id"] for a, s in zip(answers, states)):
        raise ValueError("Probe output does not match the frozen snapshot sequence")
    coverage: Counter = Counter()
    failures, rescues = [], []
    duel_states = reset_states = pairs = reset_paths = 0
    for state, game, answer in zip(states, games, answers):
        if "duel" in state["checks"]:
            duel_states += 1
            result = check_duel(state, game, answer, oracle, coverage, failures)
            pairs += result["pairs"]
            if result["triggered"]:
                rescues.append({"state": state["id"], "labels": state["labels"], **result})
        if "reset" in state["checks"]:
            reset_states += 1
            reset_paths += check_resets(state, game, answer, coverage, failures)
    for name, actual in (("duel_states", duel_states), ("reset_states", reset_states)):
        if actual != fixtures["expected"][name]:
            failures.append({"check": "fixture_coverage", "name": name, "actual": actual,
                             "expected": fixtures["expected"][name]})
    for name in ("competing_enclosure_claims", "trail_over_enclosure_priority", "protected_capture_attempts",
                 "death_cut", "death_head", "reset_self", "reset_wall", "reset_capture_before_death"):
        if not coverage[name]:
            failures.append({"check": "required_rule_coverage", "missing": name})
    summary = {"passed": not failures, "states": len(states), "duel_states": duel_states,
               "action_pairs": pairs, "reset_states": reset_states, "reset_paths": reset_paths,
               "failures": len(failures), "home_triggered_states": len(rescues),
               "home_overrides": sum(r["overridden"] for r in rescues), "coverage": dict(coverage)}
    report = {**summary, "source": str(bot), "engine": str(engine),
              "sha256": {"source": sha256(source_bytes), "engine": sha256(engine_bytes),
                         "fixtures": sha256(fixture_bytes), "probe": sha256(probe_source.read_bytes())},
              "engine_sha256_when_fixtures_packed": fixtures["packed_engine_sha256"],
              "compiler": args.cxx, "flags": ["-O2", "-std=c++20"],
              "elapsed_seconds": round(time.monotonic() - started, 3),
              "scope": "Frozen state mechanics checked with original Game.step; no full games or strength claims.",
              "home_rescues": rescues, "failure_details": failures}
    args.out.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({**summary, "source_sha256": report["sha256"]["source"], "report": str(args.out.resolve())},
                     ensure_ascii=False, indent=2))
    return 0 if not failures else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"Mechanics verification could not complete: {error}", file=sys.stderr)
        raise SystemExit(2)
