#!/usr/bin/env python3
"""Compare an endgame kernel with the original Game.step, without full matches.

All 225 mechanics snapshots exercise every legal action pair. A few real
terminal snapshots and synthetic corner cases exercise full depth <= 3 pure
maximin, with an exhaustive Python oracle that performs no alpha-beta pruning.
The entire process, including compilation and Python search, runs at nice 19.
"""
from __future__ import annotations

import argparse
from collections import Counter
import copy
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from verify_mechanics import find_engine, legal, restore, sha256

HERE = Path(__file__).resolve().parent


def fork(game):
    # Copy only mutable state; Game.step is the sole transition implementation.
    child = copy.copy(game)
    for key in ("owner", "trail", "pos", "dir", "deaths", "kills", "out"):
        setattr(child, key, getattr(game, key).copy())
    child.trails = [line.copy() for line in game.trails]
    return child


def encoded(game):
    return {"owner": "".join("." if x < 0 else str(x) for x in game.owner),
            "trail": "".join("." if x < 0 else str(x) for x in game.trail),
            "pos": [y * game.W + x for x, y in game.pos], "dir": game.dir,
            "outside": [bool(line) for line in game.trails], "area": game.areas()}


def brute(game, mine, depth, oracle, stats):
    if not depth:
        area = game.areas()
        return area[mine] - area[1 - mine], {}
    values = {}
    for d in legal(game, mine, oracle):
        results = []
        for z in legal(game, 1 - mine, oracle):
            moves = [None, None]
            moves[mine], moves[1 - mine] = d, z
            child = fork(game)
            child.step(moves)
            stats["brute_Game.step_calls"] += 1
            results.append(brute(child, mine, depth - 1, oracle, stats)[0])
        values[d] = min(results)
    return max(values.values()), values


def sample_terminal(pack, oracle):
    result = []
    for depth in (1, 2, 3):
        eligible = [s for s in pack["states"] if pack["boards"][s["board"]]["config"]["max_turns"] - s["turn"] == depth]
        indices = sorted({round(j * (len(eligible) - 1) / 3) for j in range(4)}) if eligible else []
        for j in indices:
            state = eligible[j]
            game = restore(state, pack["boards"], oracle)
            if "ordered_trails" in state:
                game.trails = copy.deepcopy(state["ordered_trails"])
            result.append({"id": "terminal/" + state["id"], "state": state, "game": game, "flags": 2 | 4, "depth": depth})
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bot", type=Path, help="Default: submission/bot.cpp beside engine/.")
    parser.add_argument("--engine", type=Path)
    parser.add_argument("--fixtures", type=Path, default=HERE / "fixtures.json")
    parser.add_argument("--terminal-fixtures", type=Path, default=HERE / "terminal_fixtures.json")
    parser.add_argument("--out", type=Path, default=HERE / "endgame_report.json")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    args = parser.parse_args()
    if hasattr(os, "nice"):
        try:
            os.nice(19)
        except OSError:
            pass
    started = time.monotonic()
    engine = find_engine(args.engine)
    bot = args.bot.resolve() if args.bot else engine.parent.parent / "submission" / "bot.cpp"
    spec = importlib.util.spec_from_file_location("arena_original_endgame_oracle", engine)
    oracle = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(oracle)
    fixture_bytes = args.fixtures.read_bytes()
    pack = json.loads(fixture_bytes)
    cases = [{"id": "mechanics/" + s["id"], "state": s, "game": restore(s, pack["boards"], oracle),
              "flags": 1, "depth": 0} for s in pack["states"]]
    mechanics_count = len(cases)
    # Add small tactical states, with only the remaining horizon changed.
    for index, depth in ((6, 2), (7, 3), (18, 1), (0, 1)):
        base = cases[index]
        cases.append({**base, "id": "corner/" + base["state"]["id"], "flags": 2 | 4, "depth": depth})
    terminal_bytes = args.terminal_fixtures.read_bytes()
    cases.extend(sample_terminal(json.loads(terminal_bytes), oracle))
    request = [str(len(cases))]
    for case in cases:
        state, game = case["state"], case["game"]
        request.append(f"CASE {case['id']} {case['flags']} {case['depth']}")
        request.append(f"INIT {game.W} {game.H} {game.cfg['max_turns']} 2 {state['mine']} 50 2000")
        request.extend(f"SPAWN {i} {x} {y}" for i, (x, y) in enumerate(game.spawns))
        request.append("INITIAL " + " ".join(game.init_dirs))
        request.append(game.state_text(game.turn))
    source_bytes = bot.read_bytes()
    probe = HERE / "endgame_probe.cpp"
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="endgame-build-", dir=args.out.parent) as temporary:
        build = Path(temporary).resolve()
        source, binary = build / "tested_bot.cpp", build / "probe"
        source.write_bytes(source_bytes)
        command = [args.cxx, "-O2", "-std=c++20", "-DBOT_SOURCE=" + json.dumps(str(source)), str(probe), "-o", str(binary)]
        if shutil.which("nice"):
            command = ["nice", "-n", "19", *command]
        compiled = subprocess.run(command, text=True, capture_output=True, timeout=120)
        if compiled.returncode:
            raise RuntimeError("Probe compilation failed:\n" + compiled.stderr[-8000:])
        run = subprocess.run([str(binary)], input="\n".join(request) + "\n", text=True,
                             capture_output=True, timeout=120, check=True)
        answers = [json.loads(line) for line in run.stdout.splitlines()]
    if len(cases) != len(answers) or any(c["id"] != a["id"] for c, a in zip(cases, answers)):
        raise ValueError("Probe returned a different fixture sequence")
    stats, failures, search = Counter(), [], []
    for case, answer in zip(cases, answers):
        state, game = case["state"], case["game"]
        mine = state["mine"]
        if case["flags"] & 1:
            seen = set()
            for branch in answer["matrix"]:
                d, z = branch["mine"], branch["enemy"]
                seen.add((d, z))
                moves = [None, None]
                moves[mine], moves[1 - mine] = d, z
                child = fork(game)
                frame = child.step(moves)
                expected = encoded(child)
                checks = {key: value == branch["after"][key] for key, value in expected.items()}
                if not all(checks.values()):
                    detail = {key: {"expected": expected[key], "actual": branch["after"][key]}
                              for key, ok in checks.items() if not ok and key not in ("owner", "trail")}
                    for key in ("owner", "trail"):
                        if not checks[key]:
                            detail[key] = [k for k, (a, b) in enumerate(zip(expected[key], branch["after"][key])) if a != b][:12]
                    failures.append({"id": case["id"], "check": "endStep", "moves": moves, "fields": detail})
                stats["action_pairs"] += 1
                for event in frame["events"]:
                    stats[event["type"] + ("_" + event["reason"] if event["type"] == "death" else "")] += 1
                capturers = {e["player"] for e in frame["events"] if e["type"] == "capture"}
                if len(capturers) == 2:
                    stats["simultaneous_capture_branches"] += 1
                    claims = [set(game._enclosed(i)) for i in range(2)]
                    stats["competing_enclosure_claims"] += bool(claims[0] & claims[1])
            expected_pairs = {(d, z) for d in legal(game, mine, oracle) for z in legal(game, 1 - mine, oracle)}
            if seen != expected_pairs:
                failures.append({"id": case["id"], "check": "all_nine_actions"})
        if case["flags"] & 2:
            value, worst = brute(game, mine, case["depth"], oracle, stats)
            if answer["aborted"] or value != answer["value"]:
                failures.append({"id": case["id"], "check": "exact_value", "expected": value,
                                 "actual": answer["value"], "aborted": answer["aborted"]})
            for original, observed in answer["choices"].items():
                expected = original
                for d in legal(game, mine, oracle):
                    if worst[d] > worst[expected]:
                        expected = d
                if observed["aborted"] or observed["chosen"] != expected:
                    failures.append({"id": case["id"], "check": "exact_action", "original": original,
                                     "expected": expected, "observed": observed, "worst": worst})
                stats["root_choices"] += 1
                stats["ties_preserving_original"] += worst[original] == value and expected == original
                stats["strict_improvement_choices"] += expected != original
            search.append({"id": case["id"], "depth": case["depth"], "value": value, "worst": worst,
                           "predicted_value": answer["value"], "choices": answer["choices"], "nodes": answer["nodes"]})
            stats["search_states"] += 1
        if case["flags"] & 4:
            if not answer["recursive_abort"]:
                failures.append({"id": case["id"], "check": "recursive_deadline"})
            for observed in answer["expired_choices"]:
                if not observed["aborted"] or observed["chosen"] != observed["original"]:
                    failures.append({"id": case["id"], "check": "expired_root_fallback", "observed": observed})
                stats["expired_root_choices"] += 1
            for observed in answer["midsearch_choices"]:
                if observed["aborted"]:
                    if observed["chosen"] != observed["original"]:
                        failures.append({"id": case["id"], "check": "midsearch_fallback", "observed": observed})
                    stats["midsearch_aborts" if observed["nodes"] > 0 else "midsearch_preentry_aborts"] += 1
                else:
                    stats["midsearch_completed_before_deadline"] += 1
    if not stats["midsearch_aborts"]:
        failures.append({"check": "coverage", "missing": "root_abort_after_search_began"})
    report = {"passed": not failures, "mechanics_states": mechanics_count, **stats,
              "failures": len(failures), "source": str(bot), "engine": str(engine),
              "sha256": {"source": sha256(source_bytes), "engine": sha256(engine.read_bytes()),
                         "fixtures": sha256(fixture_bytes), "terminal_fixtures": sha256(terminal_bytes),
                         "probe": sha256(probe.read_bytes())},
              "elapsed_seconds": round(time.monotonic() - started, 3), "search_cases": search, "failure_details": failures}
    args.out.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({k: v for k, v in report.items() if k not in ("search_cases", "failure_details")}, ensure_ascii=False, indent=2), flush=True)
    if failures:
        print(json.dumps(failures[:10], ensure_ascii=False, indent=2), flush=True)
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
