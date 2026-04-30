#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import shutil
import statistics
import subprocess
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class TrialResult:
    workload: str
    trial: int
    seconds: float
    exit_code: int
    max_rss_kb: int | None
    user_sec: float | None
    sys_sec: float | None
    db_bytes: int | None
    wal_bytes: int | None


def _parse_time_v(stderr: str) -> tuple[int | None, float | None, float | None]:
    max_rss = None
    user_sec = None
    sys_sec = None
    for line in stderr.splitlines():
        line = line.strip()
        if line.startswith("Maximum resident set size (kbytes):"):
            try:
                max_rss = int(line.split(":")[1].strip())
            except Exception:
                pass
        elif line.startswith("User time (seconds):"):
            try:
                user_sec = float(line.split(":")[1].strip())
            except Exception:
                pass
        elif line.startswith("System time (seconds):"):
            try:
                sys_sec = float(line.split(":")[1].strip())
            except Exception:
                pass
    return max_rss, user_sec, sys_sec


def _db_sizes(root: Path) -> tuple[int | None, int | None]:
    # SQLyt creates: <root>/benchdb/database.db and <root>/benchdb/database.db-wal
    db = root / "benchdb" / "database.db"
    wal = root / "benchdb" / "database.db-wal"
    db_bytes = db.stat().st_size if db.exists() else None
    wal_bytes = wal.stat().st_size if wal.exists() else None
    return db_bytes, wal_bytes


def run_trial(db_path: Path, workload_sql: Path, trial: int) -> TrialResult:
    tmp_root = Path(tempfile.mkdtemp(prefix="sqlyt_bench_"))
    try:
        cmd = ["/usr/bin/time", "-v", str(db_path), "--root", str(tmp_root), "--run", str(workload_sql), "--quiet"]
        t0 = time.perf_counter()
        p = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        t1 = time.perf_counter()
        max_rss, user_sec, sys_sec = _parse_time_v(p.stderr)
        db_bytes, wal_bytes = _db_sizes(tmp_root)
        return TrialResult(
            workload=workload_sql.name,
            trial=trial,
            seconds=t1 - t0,
            exit_code=p.returncode,
            max_rss_kb=max_rss,
            user_sec=user_sec,
            sys_sec=sys_sec,
            db_bytes=db_bytes,
            wal_bytes=wal_bytes,
        )
    finally:
        shutil.rmtree(tmp_root, ignore_errors=True)


def _median(nums: list[float]) -> float:
    return statistics.median(nums) if nums else float("nan")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", required=True, help="Path to SQLyt binary (e.g. ./db)")
    ap.add_argument("--workloads", required=True, help="Folder containing *.sql workloads")
    ap.add_argument("--trials", type=int, default=5)
    ap.add_argument("--out", default="bench/results_sqlyt.jsonl", help="JSONL output path")
    ap.add_argument(
        "--pattern",
        default="W*__*.sql",
        help="Workload filename glob (default: W*__*.sql). Use '*.sql' to include all.",
    )
    args = ap.parse_args()

    db_path = Path(args.db).resolve()
    workloads_dir = Path(args.workloads).resolve()
    out_path = Path(args.out).resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)

    sql_files = sorted(workloads_dir.glob(args.pattern))
    if not sql_files:
        raise SystemExit(f"No workloads matching {args.pattern} in {workloads_dir}")

    all_results: list[TrialResult] = []
    with out_path.open("w", encoding="utf-8") as f:
        for wf in sql_files:
            for t in range(1, args.trials + 1):
                r = run_trial(db_path, wf, t)
                all_results.append(r)
                f.write(json.dumps(r.__dict__) + "\n")
                f.flush()
                if r.exit_code != 0:
                    print(f"FAIL {wf.name} trial={t} exit={r.exit_code}")
                    return 1
            secs = [x.seconds for x in all_results if x.workload == wf.name]
            print(f"{wf.name}: median={_median(secs):.3f}s over {args.trials} trials")

    print(f"Wrote: {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

