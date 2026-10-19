#!/usr/bin/env python3
"""Inventory an existing benchmark directory without playing or changing games.

The manifest supplies the already-used seeds; no seed suite is generated. Only
the requested inventory JSON is written. Existing benchmark summary functions
are reused unchanged, so this is an integrity check, not a new strength gate.
"""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import sys

sys.dont_write_bytecode = True
import benchmark  # noqa: E402; import only, never main/seed_suite/play/build.


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def differences(actual, expected, path="") -> list[dict]:
    if isinstance(actual, dict) and isinstance(expected, dict):
        result = []
        for key in sorted(actual.keys() | expected.keys()):
            where = f"{path}.{key}" if path else key
            if key not in actual or key not in expected:
                result.append({"field": where, "stored": actual.get(key), "recomputed": expected.get(key)})
            else:
                result.extend(differences(actual[key], expected[key], where))
        return result
    return [] if actual == expected else [{"field": path, "stored": actual, "recomputed": expected}]


def inventory(run: Path, args) -> dict:
    issues = []

    def check(condition, message):
        if not condition:
            issues.append(message)

    inputs = [run / name for name in ("manifest.json", "games.jsonl", "summary.json")]
    before = {p.name: (p.stat().st_size, p.stat().st_mtime_ns) for p in inputs if p.is_file()}
    manifest = json.loads(inputs[0].read_text(encoding="utf-8"))
    spec = manifest["spec"]
    fingerprint = benchmark.canonical_digest(spec)
    check(manifest.get("run_fingerprint") == fingerprint, "Manifest fingerprint mismatch")
    mode, candidate = spec["mode"], spec["candidate"]
    rotations = 2 if mode == "duel" else 4
    check(mode in ("duel", "melee"), "Unknown mode")
    opponents, seeds = spec["opponents"], spec["seeds"]
    check(len(seeds) == len(set(seeds)), "Manifest contains duplicate maps")
    check(len(opponents) == len(set(opponents)), "Manifest contains duplicate opponents")
    check(candidate not in opponents and bool(opponents), "Invalid candidate/opponent roster")
    check(mode != "melee" or len(opponents) == 3, "Melee requires three opponents")
    tables = [[candidate, name] for name in opponents] if rotations == 2 else [[candidate] + opponents]
    expected = {}
    for map_index, seed in enumerate(seeds):
        for table in tables:
            matchup = "+".join(table[1:])
            for rotation in range(rotations):
                names = table[rotation:] + table[:rotation]
                key = f"{matchup}__seed-{seed}__rot-{rotation}"
                expected[key] = {
                    "matchup": matchup, "seed": seed, "rotation": rotation,
                    "names": names, "candidate_seat": names.index(candidate),
                    "replay": map_index < spec["replay_maps"],
                }
    expected_games = len(seeds) * rotations * len(tables)
    check(len(expected) == expected_games, "Expected job keys are not unique")
    if args.expected_games is not None:
        check(expected_games == args.expected_games, "Manifest game count differs from launch record")

    sources = {}
    check(set(spec["sources"]) == {candidate, *opponents}, "Source roster differs from participants")
    check(set(manifest["artifacts"]) == set(spec["sources"]), "Artifact roster differs from source roster")
    for name, recorded in spec["sources"].items():
        artifact = manifest["artifacts"].get(name, {})
        entry = {"original_source": artifact.get("source"), **recorded}
        for field, directory in (("source_sha256", "sources"), ("executable_sha256", "build")):
            item = "snapshot" if field == "source_sha256" else "executable"
            recorded_path = artifact.get(item)
            path = run / directory
            if directory == "sources":
                path /= name
            path /= Path(recorded_path).name if recorded_path else "MISSING"
            actual = digest(path) if path.is_file() else None
            entry[item] = str(path)
            entry["actual_" + field] = actual
            check(actual == recorded.get(field) == artifact.get(field), f"{name}: {item} SHA256 mismatch or missing")
        sources[name] = entry
    if args.source_sha256:
        check(spec["sources"].get(candidate, {}).get("source_sha256") == args.source_sha256,
              "Candidate SHA256 differs from launch record")
    engines = {}
    for name, recorded in spec["engine_sha256"].items():
        path = benchmark.ENGINE / name
        actual = digest(path) if path.is_file() else None
        engines[name] = {"recorded_sha256": recorded, "current_sha256": actual, "matches": actual == recorded}
        check(actual == recorded, f"Current engine {name} differs from the recorded engine")

    records, invalid_lines = [], []
    lines = inputs[1].read_text(encoding="utf-8").splitlines()
    for number, line in enumerate(lines, 1):
        try:
            row = json.loads(line)
            if not isinstance(row, dict):
                raise ValueError("Record must be an object")
            records.append(row)
        except (ValueError, TypeError) as error:
            invalid_lines.append({"line": number, "error": str(error)})
    check(not invalid_lines, "Unparseable game lines; no lines were silently discarded")
    key_counts = Counter(str(row.get("job_key")) for row in records)
    duplicates = {key: count for key, count in key_counts.items() if count > 1}
    missing = sorted(expected.keys() - key_counts.keys())
    unexpected = sorted(key_counts.keys() - expected.keys())
    check(not duplicates, "Duplicate job keys")
    check(not unexpected, "Foreign or invalid job keys")
    check(not missing and len(records) == expected_games, "Run is incomplete or has extra records")
    record_errors, fault_games, missing_stderr = [], [], []
    covered = defaultdict(lambda: defaultdict(list))
    result_fields = ("mode", "seed", "names", "winner", "ranks", "areas", "turns", "forfeit",
                     "deaths", "kills", "timeouts", "errors", "max_ms")
    valid_rows = {}
    for row in records:
        key = str(row.get("job_key"))
        errors = []
        planned = expected.get(key)
        if not planned:
            continue
        for field in ("matchup", "seed", "rotation", "names", "candidate_seat"):
            if row.get(field) != planned[field]:
                errors.append(f"{field} differs from expected rotation")
        for field, value in (("candidate", candidate), ("mode", mode), ("split", spec["split"]),
                             ("schema", spec["schema"]), ("run_fingerprint", fingerprint)):
            if row.get(field) != value:
                errors.append(f"{field} mismatch")
        try:
            for field in ("ranks", "areas", "forfeit", "deaths", "kills", "timeouts", "errors", "max_ms"):
                if len(row[field]) != rotations:
                    raise ValueError(f"{field} seat count mismatch")
            ranks = benchmark.referee.ranks_of(row["areas"], row["forfeit"])
            leaders = [seat for seat, rank in enumerate(ranks) if rank == 1]
            winner = leaders[0] if len(leaders) == 1 else -1
            seat = planned["candidate_seat"]
            outcome = "win" if winner == seat else "draw" if winner == -1 and ranks[seat] == 1 else "loss"
            if (row["ranks"], row["winner"], row["outcome"]) != (ranks, winner, outcome):
                errors.append("Ranks, winner or strict outcome differs from official result rule")
            if not 0 <= row["turns"] <= spec["config"]["max_turns"]:
                errors.append("Invalid final turn")
            if not any(row["forfeit"]) and row["turns"] != spec["config"]["max_turns"]:
                errors.append("Early termination without a recorded forfeit")
            faults = []
            for seat, name in enumerate(planned["names"]):
                log = run / "stderr" / key / f"seat-{seat}-{name}.log"
                if not log.is_file():
                    missing_stderr.append(str(log.relative_to(run)))
                if row["timeouts"][seat] or row["errors"][seat] or row["forfeit"][seat]:
                    faults.append({"seat": seat, "name": name, "candidate": name == candidate,
                                   "timeouts": row["timeouts"][seat], "errors": row["errors"][seat],
                                   "forfeit": row["forfeit"][seat], "stderr": str(log)})
            if faults:
                fault_games.append({"job_key": key, "outcome": row["outcome"], "faults": faults})
            covered[planned["matchup"]][planned["seed"]].append(planned["candidate_seat"])
            valid_rows[key] = row
        except (KeyError, TypeError, ValueError, IndexError) as error:
            errors.append(str(error))
        if errors:
            record_errors.append({"job_key": key, "issues": errors})
    check(not record_errors, "One or more game records are inconsistent")
    check(not missing_stderr, "Missing per-seat stderr logs")
    coverage = {}
    for table in tables:
        matchup = "+".join(table[1:])
        groups = covered[matchup]
        coverage[matchup] = {
            "expected_games": len(seeds) * rotations,
            "actual_games": sum(len(seats) for seats in groups.values()),
            "expected_maps": len(seeds), "actual_maps": len(groups),
            "maps_with_every_seat_once": sum(sorted(seats) == list(range(rotations)) for seats in groups.values()),
            "candidate_seat_counts": dict(sorted(Counter(seat for seats in groups.values() for seat in seats).items())),
        }

    replay_issues, complete_replays, full_length, early_forfeit = [], 0, 0, 0
    expected_replays = {key for key, job in expected.items() if job["replay"]}
    found_replays = {path.stem for path in (run / "replays").glob("*.json")}
    for key in sorted(expected_replays):
        path = run / "replays" / f"{key}.json"
        row = valid_rows.get(key)
        try:
            replay = json.loads(path.read_text(encoding="utf-8"))
            if row is None:
                raise ValueError("Replay has no structurally valid game record")
            if not row.get("replay_path") or Path(row["replay_path"]).name != path.name:
                raise ValueError("Game replay reference mismatch")
            if replay.get("version") != 2 or replay.get("config") != spec["config"]:
                raise ValueError("Replay version/config mismatch")
            if replay.get("seed") != row["seed"] or replay.get("names") != row["names"]:
                raise ValueError("Replay identity mismatch")
            if replay.get("result") != {field: row[field] for field in result_fields}:
                raise ValueError("Replay final result differs from games.jsonl")
            if not isinstance(replay.get("frames"), list) or len(replay["frames"]) != row["turns"]:
                raise ValueError("Replay frame count differs from recorded final turn")
            cells = spec["config"]["width"] * spec["config"]["height"]
            if len(replay.get("initial_owner", "")) != cells:
                raise ValueError("Replay initial board incomplete")
            if len(replay.get("spawns", [])) != rotations or len(replay.get("init_dirs", [])) != rotations:
                raise ValueError("Replay initial player metadata incomplete")
            complete_replays += 1
            full_length += row["turns"] == spec["config"]["max_turns"]
            early_forfeit += row["turns"] < spec["config"]["max_turns"] and any(row["forfeit"])
        except (OSError, ValueError, KeyError, TypeError) as error:
            replay_issues.append({"job_key": key, "error": str(error)})
    extra_replays = sorted(found_replays - expected_replays)
    check(not replay_issues and not extra_replays, "Missing, extra or inconsistent requested replays")
    if args.require_all_replays:
        check(len(expected_replays) == expected_games and complete_replays == expected_games,
              "Full-replay launch requirement not met")

    summary = json.loads(inputs[2].read_text(encoding="utf-8"))
    summary_differences = []
    try:
        recomputed = benchmark.summarize(records, spec, expected_games)
        for field in ("schema", "candidate", "split", "mode", "run_fingerprint", "threshold", "total", "matchups", "criterion"):
            summary_differences.extend(differences(summary.get(field), recomputed[field], field))
    except (KeyError, TypeError, ValueError, IndexError) as error:
        summary_differences.append({"field": "recomputation", "error": str(error)})
    check(not summary_differences, "Stored summary differs from unchanged benchmark statistics")
    after = {p.name: (p.stat().st_size, p.stat().st_mtime_ns) for p in inputs if p.is_file()}
    check(before == after, "Input files changed during inventory; run again after the benchmark stops")
    return {
        "schema": 1, "checked_at": datetime.now(timezone.utc).isoformat(), "run_directory": str(run),
        "inventory_ok": not issues,
        "scope": "Artifact integrity only. Faults are retained and do not by themselves fail inventory. No strength criterion is changed.",
        "run_fingerprint": fingerprint, "benchmark_sha256_current": digest(Path(benchmark.__file__)),
        "candidate_source_sha256": spec["sources"][candidate]["source_sha256"],
        "parameters": {key: spec[key] for key in ("candidate", "opponents", "mode", "split", "seed_base", "jobs", "replay_maps", "official_flags", "limits", "config")},
        "sources": sources, "engines": engines,
        "games": {"expected": expected_games, "actual": len(records), "input_lines": len(lines),
                  "unique_job_keys": len(key_counts), "expected_maps": len(seeds),
                  "duplicate_job_keys": duplicates, "missing_job_keys": missing, "unexpected_job_keys": unexpected,
                  "invalid_lines": invalid_lines, "record_errors": record_errors},
        "coverage": coverage,
        "faults": {"games_with_faults": len(fault_games), "all_fault_records": fault_games,
                   "note": "Timeouts are included in official errors; do not add these counters as disjoint events."},
        "stderr": {"expected_files": len(records) * rotations, "missing_files": missing_stderr},
        "replays": {"requested_by_manifest": len(expected_replays), "files_found": len(found_replays),
                    "complete_final_result_replays": complete_replays, "full_length_replays": full_length,
                    "complete_early_forfeit_replays": early_forfeit,
                    "all_games_have_complete_final_replays": complete_replays == expected_games,
                    "issues": replay_issues, "extra_replays": extra_replays,
                    "scope": "Checks final result, metadata and frame count; does not replay Game.step."},
        "summary_matches_records": not summary_differences, "summary_differences": summary_differences,
        "existing_summary": {key: summary.get(key) for key in ("total", "matchups", "criterion")},
        "input_sha256": {p.name: digest(p) for p in inputs}, "input_files_unchanged": before == after,
        "issues": issues,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path, help="completed benchmark output directory")
    parser.add_argument("--out", type=Path, help="inventory JSON; default RUN/inventory.json")
    parser.add_argument("--source-sha256", help="expected frozen candidate SHA256 from launch record")
    parser.add_argument("--expected-games", type=int, help="expected total from launch record")
    parser.add_argument("--require-all-replays", action="store_true", help="verify every scheduled game has a complete final replay")
    args = parser.parse_args()
    run = args.run.resolve()
    out = (args.out or run / "inventory.json").resolve()
    if out in {run / name for name in ("manifest.json", "games.jsonl", "summary.json")}:
        parser.error("Inventory must not overwrite benchmark inputs")
    if any(out.is_relative_to(run / name) for name in ("sources", "build", "replays", "stderr")):
        parser.error("Inventory must not overwrite benchmark artifacts")
    if hasattr(os, "nice"):
        try:
            os.nice(19)
        except OSError:
            pass
    try:
        report = inventory(run, args)
    except (OSError, ValueError, KeyError, TypeError) as error:
        report = {"schema": 1, "run_directory": str(run), "inventory_ok": False,
                  "issues": [f"Cannot finish inventory: {type(error).__name__}: {error}"]}
    out.parent.mkdir(parents=True, exist_ok=True)
    benchmark.write_json(out, report)
    print(json.dumps({key: report.get(key) for key in ("inventory_ok", "candidate_source_sha256", "games", "coverage", "replays", "issues")},
                     ensure_ascii=False, indent=2))
    print(f"Inventory: {out}")
    raise SystemExit(0 if report["inventory_ok"] else 1)


if __name__ == "__main__":
    main()
