#!/usr/bin/env python3
"""Reproducible, strict territory-bot evaluation using the unchanged official referee.

Examples (from the repository root)::

    python land/analysis/domination/benchmark.py --list-opponents
    python land/analysis/domination/benchmark.py --bot my_bot.cpp \
        --opponents sonnet sparring --maps 6 --split dev --out /tmp/land-dev
    python land/analysis/domination/benchmark.py --bot my_bot.cpp \
        --maps 100 --split acceptance --out /tmp/land-acceptance --jobs 2

Every map is played from both seats (all four rotations in melee). Draws count
as zero wins. Forfeits, malformed replies and timeouts remain in every record.
Development and acceptance map seeds occupy disjoint numerical ranges. Keep
acceptance results unseen until the candidate source is frozen.
"""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from concurrent.futures import ProcessPoolExecutor, as_completed
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import random
import shutil
import subprocess
import sys
import time
from typing import Any


HERE = Path(__file__).resolve().parent
LAND = HERE.parents[1]
ROOT = LAND.parent
ENGINE = LAND / "workspace" / "engine"
sys.path.insert(0, str(ENGINE))
import build  # noqa: E402
import referee  # noqa: E402

SCHEMA = 1
DEFAULT_OPPONENTS = ["community", "sonnet", "sparring", "opus", "astra"]
OPPONENTS = {
    "community": ROOT / "community/land/mohen2006__land-gpt-61-sol/bot.cpp",
    "sonnet": LAND / "bots/sonnet-5@claude-code#1/bot.cpp",
    "sparring": LAND / "tournament/sparring.cpp",
    "opus": LAND / "bots/opus-5.5@claude-code#1/bot.cpp",
    "astra": LAND / "bots/gpt-6-astra@codex#1/bot.cpp",
    "sol": LAND / "bots/gpt-6-sol@codex#1/bot.cpp",
    "mimo": LAND / "bots/mimo-2.6-pro@zcode#1/bot.cpp",
    "greedy": LAND / "workspace/anchors/greedy.cpp",
    "random": LAND / "workspace/anchors/random.cpp",
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def canonical_digest(value: Any) -> str:
    data = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(data).hexdigest()


def write_json(path: Path, value: Any) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def wilson(wins: int, games: int, z: float = 1.959963984540054) -> list[float]:
    """Two-sided Wilson 95% interval for strict wins, not win-plus-half-draws."""
    if games == 0:
        return [0.0, 1.0]
    p = wins / games
    z2 = z * z
    center = (p + z2 / (2 * games)) / (1 + z2 / games)
    radius = z * math.sqrt(p * (1 - p) / games + z2 / (4 * games * games)) / (1 + z2 / games)
    return [max(0.0, center - radius), min(1.0, center + radius)]


def source_snapshot(source: Path, name: str, out: Path) -> dict[str, Any]:
    """Compile immutable copies so editing a candidate cannot change an ongoing run."""
    source = source.resolve()
    if not source.is_file():
        raise ValueError(f"Bot source/executable does not exist: {source}")
    digest = sha256(source)
    suffix = ".cpp" if source.suffix.lower() == ".cpp" else ".bin"
    snapshot = out / "sources" / name / f"bot-{digest[:16]}{suffix}"
    snapshot.parent.mkdir(parents=True, exist_ok=True)
    if not snapshot.exists():
        shutil.copy2(source, snapshot)
    if sha256(snapshot) != digest:
        raise ValueError(f"Source snapshot was modified: {snapshot}")
    if suffix != ".cpp":
        snapshot.chmod(snapshot.stat().st_mode | 0o100)
    executable = Path(build.resolve(str(snapshot), str(out / "build")))
    return {
        "source": str(source), "source_sha256": digest,
        "snapshot": str(snapshot), "executable": str(executable),
        "executable_sha256": sha256(executable),
    }


def seed_suite(split: str, salt: int, maps: int) -> list[int]:
    # Disjoint even if the same salt is intentionally or accidentally reused.
    start, stop = ((1_000_000, 1_000_000_000) if split == "dev"
                   else (1_000_000_000, 2_000_000_000))
    rng = random.Random(f"arena-land-domination-v{SCHEMA}:{split}:{salt}")
    return rng.sample(range(start, stop), maps)


def play(job: dict[str, Any]) -> dict[str, Any]:
    started = time.perf_counter()
    names = job["names"]
    stderr_dir = Path(job["stderr_dir"])
    stderr_dir.mkdir(parents=True, exist_ok=True)
    result = referee.run_match(
        job["executables"], names, job["seed"],
        replay_path=job["replay_path"],
        stderr_paths=[str(stderr_dir / f"seat-{i}-{name}.log") for i, name in enumerate(names)],
    )
    seat = names.index(job["candidate"])
    if result["winner"] == seat:
        outcome = "win"
    elif result["winner"] == -1 and result["ranks"][seat] == 1:
        outcome = "draw"
    else:
        outcome = "loss"
    result.update({
        "schema": SCHEMA, "run_fingerprint": job["run_fingerprint"],
        "job_key": job["job_key"], "matchup": job["matchup"],
        "candidate": job["candidate"], "candidate_seat": seat,
        "rotation": job["rotation"], "split": job["split"],
        "outcome": outcome, "elapsed_seconds": time.perf_counter() - started,
        "replay_path": job["replay_path"], "stderr_dir": str(stderr_dir),
    })
    return result


def statistics(records: list[dict[str, Any]], expected: int, rotations: int) -> dict[str, Any]:
    counts = Counter(r["outcome"] for r in records)
    games = len(records)
    by_seed: dict[int, list[dict[str, Any]]] = defaultdict(list)
    candidate_faults = Counter()
    opponent_faults = Counter()
    candidate_max_ms = opponent_max_ms = 0.0
    clean_games = clean_wins = candidate_area = opposing_area = 0
    opponent_fault_games = opponent_fault_wins = 0
    for r in records:
        by_seed[r["seed"]].append(r)
        candidate = r["candidate_seat"]
        candidate_area += r["areas"][candidate]
        opposing_area += max(a for i, a in enumerate(r["areas"]) if i != candidate)
        for i in range(len(r["names"])):
            faults = candidate_faults if i == candidate else opponent_faults
            faults["timeouts"] += r["timeouts"][i]
            faults["errors"] += r["errors"][i]
            faults["forfeits"] += bool(r["forfeit"][i])
            if i == candidate:
                candidate_max_ms = max(candidate_max_ms, r["max_ms"][i])
            else:
                opponent_max_ms = max(opponent_max_ms, r["max_ms"][i])
        if any(r["timeouts"][i] or r["errors"][i] or r["forfeit"][i]
               for i in range(len(r["names"])) if i != candidate):
            opponent_fault_games += 1
            opponent_fault_wins += r["outcome"] == "win"
        if not any(r["timeouts"]) and not any(r["errors"]) and not any(r["forfeit"]):
            clean_games += 1
            clean_wins += r["outcome"] == "win"
    complete_groups = [group for group in by_seed.values()
                       if len(group) == rotations
                       and len({(r["matchup"], r["rotation"]) for r in group}) == rotations]
    sweeps = sum(all(r["outcome"] == "win" for r in group) for group in complete_groups)
    # Same-map swapped seats are correlated. A map-level sweep is a Bernoulli
    # event and its probability lower-bounds the mean game win probability.
    # Report its Wilson interval separately, without claiming games are independent.
    rate = counts["win"] / games if games else 0.0
    fault_defaults = {"timeouts": 0, "errors": 0, "forfeits": 0}
    return {
        "games": games, "expected_games": expected, "complete": games == expected,
        "wins": counts["win"], "draws": counts["draw"], "losses": counts["loss"],
        "strict_win_rate": rate,
        "strict_win_wilson_ci95": wilson(counts["win"], games),
        "score_rate_including_half_draws": (counts["win"] + 0.5 * counts["draw"]) / games if games else 0.0,
        "complete_maps": len(complete_groups), "games_per_complete_map": rotations, "map_sweeps": sweeps,
        "map_sweep_rate": sweeps / len(complete_groups) if complete_groups else 0.0,
        "map_sweep_wilson_ci95": wilson(sweeps, len(complete_groups)),
        "candidate_faults": dict(fault_defaults | candidate_faults),
        "opponent_faults": dict(fault_defaults | opponent_faults),
        "opponent_fault_games": opponent_fault_games,
        "opponent_fault_wins": opponent_fault_wins,
        # Sensitivity analysis, not a confidence interval or a rerun estimate:
        # keep every game in the denominator and give no win credit to games
        # in which an opponent had any recorded fault, regardless of its cause.
        "opponent_fault_win_rate_floor":
            (counts["win"] - opponent_fault_wins) / games if games else 0.0,
        "candidate_max_ms": candidate_max_ms, "opponent_max_ms": opponent_max_ms,
        "clean_games": clean_games, "clean_wins": clean_wins,
        "clean_strict_win_rate": clean_wins / clean_games if clean_games else None,
        "mean_candidate_area": candidate_area / games if games else None,
        "mean_best_opponent_area": opposing_area / games if games else None,
    }


def summarize(records: list[dict[str, Any]], spec: dict[str, Any], expected: int) -> dict[str, Any]:
    rotations = 2 if spec["mode"] == "duel" else 4
    matchup_names = spec["opponents"] if rotations == 2 else ["+".join(spec["opponents"])]
    grouped = {name: [r for r in records if r["matchup"] == name] for name in matchup_names}
    matchups = {name: statistics(group, len(spec["seeds"]) * rotations, rotations)
                for name, group in grouped.items()}
    # Opponents share map seeds. Count an overall map only once, requiring every
    # matchup and rotation, instead of assuming opponent-map pairs independent.
    total = statistics(records, expected, rotations * len(matchups))
    no_faults = total["clean_games"] == total["games"]
    threshold = 0.8
    complete = len(records) == expected and all(s["complete"] for s in matchups.values())
    empirical_pass = complete and all(s["strict_win_rate"] > threshold for s in matchups.values())
    fault_sensitive_pass = complete and all(s["opponent_fault_win_rate_floor"] > threshold
                                           for s in matchups.values())
    individual_interval_pass = complete and all(s["strict_win_wilson_ci95"][0] > threshold
                                               for s in matchups.values())
    sweep_interval_pass = complete and all(s["map_sweep_wilson_ci95"][0] > threshold
                                          for s in matchups.values())
    return {
        "schema": SCHEMA, "updated_at": datetime.now(timezone.utc).isoformat(),
        "candidate": spec["candidate"], "split": spec["split"], "mode": spec["mode"],
        "run_fingerprint": canonical_digest(spec), "threshold": threshold,
        "total": total, "matchups": matchups,
        "criterion": {
            "complete": complete, "all_runs_clean": no_faults,
            # Observed performance is separate from the unchanged strict
            # acceptance gates below; dev results do not become acceptance.
            "target_met": empirical_pass,
            "target_met_if_opponent_fault_wins_count_as_nonwins": fault_sensitive_pass,
            "each_matchup_strict_win_rate_above_80_percent": empirical_pass,
            "each_matchup_game_wilson_lower_above_80_percent": individual_interval_pass,
            "each_matchup_map_sweep_wilson_lower_above_80_percent": sweep_interval_pass,
            "acceptance_empirical_pass": spec["split"] == "acceptance" and empirical_pass and no_faults,
            "acceptance_conservative_evidence": spec["split"] == "acceptance" and sweep_interval_pass and no_faults,
        },
        "methodology": [
            "Official referee and rules engine imported without modifications; default map size and 600 turns.",
            "Official 50 ms move budget, 2000 ms initialization budget, 15 ms referee grace, 512 MiB address-space limit.",
            "Each map has all seat rotations. Strict wins exclude every draw. Forfeit results remain in all-game statistics.",
            "Per-game Wilson intervals are descriptive: swapped games on a shared map may be correlated.",
            "Map-sweep intervals use each map once. Sweep probability lower-bounds average strict game win probability.",
            "An overall map sweep requires wins in every matchup and seat on that seed; shared seeds are not counted as independent samples.",
            "Intervals are per matchup, not a simultaneous family-wise guarantee across all opponents.",
            "Clean-game statistics are diagnostic only; no faulty game is silently dropped or rerun.",
            "target_met reports complete, all-game observed win rates above 80% in every matchup; the split is reported separately.",
            "The opponent-fault floor counts every win with an opponent fault as a nonwin, keeping the full denominator; it is sensitivity analysis, not a confidence interval or a claim about fault causes.",
            "The existing acceptance gates additionally require all runs clean; target_met does not override those gates.",
            "Source, executable and engine hashes plus exact map seeds are preserved in manifest.json.",
            "Acceptance seeds must not be used for tuning; this script cannot enforce the developer's information barrier.",
        ],
    }


def summarize_existing(out: Path) -> None:
    """Recompute derived statistics without recompiling or playing any game."""
    manifest = json.loads((out / "manifest.json").read_text(encoding="utf-8"))
    spec = manifest["spec"]
    fingerprint = canonical_digest(spec)
    if manifest["run_fingerprint"] != fingerprint:
        raise ValueError("Manifest fingerprint does not match its specification")
    rotations = 2 if spec["mode"] == "duel" else 4
    matchups = spec["opponents"] if rotations == 2 else ["+".join(spec["opponents"])]
    expected_keys = {f"{opponent}__seed-{seed}__rot-{rotation}"
                     for opponent in matchups for seed in spec["seeds"] for rotation in range(rotations)}
    records = [json.loads(line) for line in (out / "games.jsonl").read_text(encoding="utf-8").splitlines()]
    seen = set()
    for record in records:
        key = record.get("job_key")
        if record.get("run_fingerprint") != fingerprint or key not in expected_keys or key in seen:
            raise ValueError("Results contain a foreign or duplicate game")
        seen.add(key)
    summary = summarize(records, spec, len(expected_keys))
    write_json(out / "summary.json", summary)
    print(f"Refreshed {out / 'summary.json'} from {len(records)} existing games; no matches played.")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bot", type=Path, help="candidate standalone .cpp source or executable")
    ap.add_argument("--name", default="candidate")
    ap.add_argument("--opponents", nargs="*", choices=sorted(OPPONENTS), default=None)
    ap.add_argument("--opponent", action="append", default=[], metavar="NAME=PATH",
                    help="additional opponent source/executable; repeatable")
    ap.add_argument("--list-opponents", action="store_true")
    ap.add_argument("--summarize", type=Path, metavar="RUN_DIR",
                    help="refresh derived summary from an existing run; no compilation or games")
    ap.add_argument("--mode", choices=["duel", "melee"], default="duel")
    ap.add_argument("--maps", type=int, default=6, help="independent maps per matchup (2 games each in duel)")
    ap.add_argument("--split", choices=["dev", "acceptance"], default="dev")
    ap.add_argument("--seed-base", type=int, default=0,
                    help="reproducible seed-suite salt; split seed ranges never overlap")
    ap.add_argument("--seeds", help="comma-separated explicit DEV map seeds; prohibited for acceptance")
    ap.add_argument("--jobs", type=int, default=1, help="parallel matches; use modest concurrency to avoid wall-time artifacts")
    ap.add_argument("--out", type=Path, help="dedicated run directory; matching interrupted runs resume automatically")
    ap.add_argument("--replay-maps", type=int, default=0, help="save all rotations of the first K maps per matchup")
    args = ap.parse_args()
    if args.list_opponents:
        for name, source in OPPONENTS.items():
            print(f"{name:12s} {source.relative_to(ROOT)}")
        return
    if args.summarize:
        summarize_existing(args.summarize.resolve())
        return
    if args.bot is None or args.out is None:
        ap.error("--bot and --out are required")
    if args.maps < 1 or args.jobs < 1 or args.replay_maps < 0:
        ap.error("--maps / --jobs must be positive; --replay-maps must be nonnegative")
    if not args.name or any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-" for c in args.name):
        ap.error("--name must contain only letters, numbers, underscores or hyphens")
    selected = DEFAULT_OPPONENTS if args.opponents is None and not args.opponent else (args.opponents or [])
    opponents = {name: OPPONENTS[name] for name in selected}
    for entry in args.opponent:
        if "=" not in entry:
            ap.error("--opponent requires NAME=PATH")
        name, path = entry.split("=", 1)
        if not name or any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-" for c in name):
            ap.error("custom opponent names must contain only letters, numbers, underscores or hyphens")
        if name in opponents:
            ap.error(f"duplicate opponent name: {name}")
        opponents[name] = Path(path)
    if not opponents or args.name in opponents:
        ap.error("select at least one opponent; candidate name must be distinct")
    if args.mode == "melee" and len(opponents) != 3:
        ap.error("melee requires exactly three opponents; all four seat rotations will be played")
    seeds = seed_suite(args.split, args.seed_base, args.maps)
    if args.seeds:
        if args.split != "dev":
            ap.error("explicit seeds are allowed only for development")
        try:
            seeds = [int(s) for s in args.seeds.split(",")]
        except ValueError:
            ap.error("--seeds must be comma-separated integers")
        if not seeds or len(set(seeds)) != len(seeds):
            ap.error("explicit seeds must be nonempty and unique")
        if any(seed >= 1_000_000_000 for seed in seeds):
            ap.error("explicit development seeds must be below the reserved acceptance range")
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    sources = {args.name: args.bot, **opponents}
    try:
        snapshots = {name: source_snapshot(path, name, out) for name, path in sources.items()}
    except (RuntimeError, ValueError) as error:
        raise SystemExit(str(error)) from error
    spec = {
        "schema": SCHEMA, "candidate": args.name, "opponents": list(opponents),
        "sources": {name: {key: row[key] for key in ("source_sha256", "executable_sha256")}
                    for name, row in snapshots.items()},
        "engine_sha256": {name: sha256(ENGINE / name) for name in ("game.py", "referee.py", "build.py")},
        "official_flags": list(build.FLAGS),
        "limits": {"move_ms": referee.MOVE_MS, "init_ms": referee.INIT_MS,
                   "grace_ms": referee.GRACE_MS, "max_errors": referee.MAX_ERRORS, "memory_mb": referee.MEM_MB},
        "mode": args.mode, "config": dict(referee.config_for(2 if args.mode == "duel" else 4)),
        "split": args.split, "seed_base": args.seed_base, "seeds": seeds,
        "jobs": args.jobs, "replay_maps": args.replay_maps,
    }
    fingerprint = canonical_digest(spec)
    manifest_path = out / "manifest.json"
    if manifest_path.exists():
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        if manifest.get("run_fingerprint") != fingerprint:
            raise SystemExit("Existing output directory has a different frozen specification. Use a new --out directory.")
    else:
        compiler = subprocess.run([os.environ.get("CXX", "g++"), "--version"], capture_output=True, text=True)
        manifest = {
            "created_at": datetime.now(timezone.utc).isoformat(), "run_fingerprint": fingerprint,
            "spec": spec, "artifacts": snapshots,
            "environment": {"python": sys.version, "platform": platform.platform(),
                            "cpu_count": os.cpu_count(),
                            "cpu_affinity": sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None,
                            "compiler": compiler.stdout.splitlines()[0] if compiler.stdout else None},
        }
        write_json(manifest_path, manifest)
    rotations = 2 if args.mode == "duel" else 4
    tables = [[args.name, name] for name in opponents] if rotations == 2 else [[args.name] + list(opponents)]
    jobs = []
    for map_index, seed in enumerate(seeds):
        for table in tables:
            matchup = "+".join(table[1:])
            for rotation in range(rotations):
                names = table[rotation:] + table[:rotation]
                key = f"{matchup}__seed-{seed}__rot-{rotation}"
                replay = out / "replays" / f"{key}.json" if map_index < args.replay_maps else None
                jobs.append({
                    "job_key": key, "matchup": matchup, "names": names, "seed": seed,
                    "rotation": rotation, "candidate": args.name, "split": args.split,
                    "executables": [snapshots[name]["executable"] for name in names],
                    "replay_path": str(replay) if replay else None,
                    "stderr_dir": str(out / "stderr" / key), "run_fingerprint": fingerprint,
                })
    expected_keys = {job["job_key"] for job in jobs}
    results_path = out / "games.jsonl"
    records = []
    done = set()
    if results_path.exists():
        for line in results_path.read_text(encoding="utf-8").splitlines():
            row = json.loads(line)
            key = row.get("job_key")
            if row.get("run_fingerprint") != fingerprint or key not in expected_keys or key in done:
                raise SystemExit("Existing results contain a foreign or duplicate game. Preserve them and use a new --out.")
            records.append(row)
            done.add(key)
    pending = [job for job in jobs if job["job_key"] not in done]
    started = time.perf_counter()
    print(f"{args.split}: {len(jobs)} official {args.mode} games; {len(done)} already complete; jobs={args.jobs}", flush=True)
    print(f"Candidate SHA256: {snapshots[args.name]['source_sha256']}", flush=True)
    print(f"Output: {out}", flush=True)
    with results_path.open("a", encoding="utf-8") as stream:
        def accept(row: dict[str, Any]) -> None:
            records.append(row)
            stream.write(json.dumps(row, ensure_ascii=False, separators=(",", ":")) + "\n")
            stream.flush()
            summary = summarize(records, spec, len(jobs))
            write_json(out / "summary.json", summary)
            completed_now = len(records) - len(done)
            elapsed = time.perf_counter() - started
            print(f"[{len(records)}/{len(jobs)}] {row['matchup']} seed={row['seed']} rot={row['rotation']} "
                  f"{row['outcome'].upper()} areas={row['areas']} errors={row['errors']} "
                  f"timeouts={row['timeouts']} forfeit={row['forfeit']} "
                  f"({elapsed / max(1, completed_now):.2f}s/game throughput)", flush=True)
        if args.jobs == 1:
            for job in pending:
                accept(play(job))
        else:
            with ProcessPoolExecutor(max_workers=args.jobs) as pool:
                futures = [pool.submit(play, job) for job in pending]
                for future in as_completed(futures):
                    # Infrastructure errors propagate: never fabricate a win or drop a game.
                    accept(future.result())
    summary = summarize(records, spec, len(jobs))
    write_json(out / "summary.json", summary)
    for matchup, row in summary["matchups"].items():
        low, high = row["strict_win_wilson_ci95"]
        print(f"{matchup}: W/D/L={row['wins']}/{row['draws']}/{row['losses']}; "
              f"strict win={row['strict_win_rate']:.2%}; descriptive Wilson95=[{low:.2%},{high:.2%}]; "
              f"clean={row['clean_games']}/{row['games']}")
    print(json.dumps(summary["criterion"], ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
