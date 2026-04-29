#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
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


def _db_sizes(db_file: Path) -> tuple[int | None, int | None]:
    db_bytes = db_file.stat().st_size if db_file.exists() else None
    wal = db_file.with_suffix(db_file.suffix + "-wal")  # <db>-wal
    wal_bytes = wal.stat().st_size if wal.exists() else None
    return db_bytes, wal_bytes


def _translate_sqlyt_workload_to_sqlite(src_sql: Path, dst_sql: Path) -> None:
    """
    SQLyt benchmark workloads contain a few SQLyt-specific directives.
    This translator keeps the SQL semantics but makes the script runnable by sqlite3.
    """
    out_lines: list[str] = []
    for raw in src_sql.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line:
            continue

        # SQLyt directives that sqlite3 doesn't understand.
        if line.lower().startswith("create database "):
            continue
        if line.lower().startswith(".usedatabase"):
            continue
        if line == ".begin":
            out_lines.append("BEGIN;")
            continue
        if line == ".commit":
            out_lines.append("COMMIT;")
            continue
        if line == ".exit":
            # sqlite3 CLI supports .quit / .exit, but we can just stop the file here.
            break

        # The generated workloads use double-quotes for string literals. SQLite may interpret
        # those as identifiers depending on build/config, so normalize to single quotes.
        sql = raw.rstrip().replace('"', "'")

        # SQLyt treats each line as a complete statement; sqlite3 expects semicolon delimiters.
        if not sql.endswith(";"):
            sql += ";"
        out_lines.append(sql)

    dst_sql.write_text("\n".join(out_lines) + "\n", encoding="utf-8")


def _run_trial_cli(sqlite3_path: str, workload_sql: Path, trial: int) -> TrialResult:
    tmp_root = Path(tempfile.mkdtemp(prefix="sqlite_bench_"))
    try:
        db_file = tmp_root / "bench.db"
        translated = tmp_root / "workload.sql"
        _translate_sqlyt_workload_to_sqlite(workload_sql, translated)

        cmd = ["/usr/bin/time", "-v", sqlite3_path, str(db_file)]
        t0 = time.perf_counter()
        try:
            p = subprocess.run(
                cmd,
                stdin=translated.open("r", encoding="utf-8"),
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                text=True,
            )
        except FileNotFoundError as e:
            raise SystemExit(
                f"sqlite3 CLI not found ({e}). Install it (e.g. `sudo apt-get install sqlite3`) "
                "or run with `--engine python`."
            )
        t1 = time.perf_counter()

        max_rss, user_sec, sys_sec = _parse_time_v(p.stderr)
        db_bytes, wal_bytes = _db_sizes(db_file)
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


def _run_trial_python(workload_sql: Path, trial: int) -> TrialResult:
    # Avoid depending on the sqlite3 CLI being installed.
    import resource
    import sqlite3
    import sys

    tmp_root = Path(tempfile.mkdtemp(prefix="sqlite_bench_"))
    try:
        db_file = tmp_root / "bench.db"
        translated = tmp_root / "workload.sql"
        _translate_sqlyt_workload_to_sqlite(workload_sql, translated)

        sql_text = translated.read_text(encoding="utf-8")
        t0 = time.perf_counter()
        exit_code = 0
        try:
            con = sqlite3.connect(str(db_file))
            try:
                con.executescript(sql_text)
                con.commit()
            finally:
                con.close()
        except Exception as e:
            print(f"[sqlite python] workload={workload_sql.name} trial={trial} error={e}", file=sys.stderr)
            exit_code = 1
        t1 = time.perf_counter()

        ru = resource.getrusage(resource.RUSAGE_SELF)
        # On Linux, ru_maxrss is in kilobytes.
        max_rss_kb = int(ru.ru_maxrss) if ru.ru_maxrss is not None else None
        user_sec = float(ru.ru_utime) if ru.ru_utime is not None else None
        sys_sec = float(ru.ru_stime) if ru.ru_stime is not None else None

        db_bytes, wal_bytes = _db_sizes(db_file)
        return TrialResult(
            workload=workload_sql.name,
            trial=trial,
            seconds=t1 - t0,
            exit_code=exit_code,
            max_rss_kb=max_rss_kb,
            user_sec=user_sec,
            sys_sec=sys_sec,
            db_bytes=db_bytes,
            wal_bytes=wal_bytes,
        )
    finally:
        shutil.rmtree(tmp_root, ignore_errors=True)


def run_trial(engine: str, sqlite3_path: str, workload_sql: Path, trial: int) -> TrialResult:
    if engine == "cli":
        return _run_trial_cli(sqlite3_path, workload_sql, trial)
    if engine == "python":
        return _run_trial_python(workload_sql, trial)
    raise ValueError(f"Unknown engine: {engine}")


def _median(nums: list[float]) -> float:
    return statistics.median(nums) if nums else float("nan")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--workloads", required=True, help="Folder containing *.sql workloads")
    ap.add_argument("--trials", type=int, default=5)
    ap.add_argument("--engine", choices=["cli", "python"], default="cli")
    ap.add_argument("--sqlite3", default="sqlite3", help="sqlite3 CLI binary (default: sqlite3)")
    ap.add_argument("--out", default="bench/results_sqlite.jsonl", help="JSONL output path")
    args = ap.parse_args()

    workloads_dir = Path(args.workloads).resolve()
    out_path = Path(args.out).resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)

    sql_files = sorted([p for p in workloads_dir.iterdir() if p.suffix == ".sql"])
    if not sql_files:
        raise SystemExit(f"No .sql workloads found in {workloads_dir}")

    all_results: list[TrialResult] = []
    with out_path.open("w", encoding="utf-8") as f:
        for wf in sql_files:
            for t in range(1, args.trials + 1):
                r = run_trial(args.engine, args.sqlite3, wf, t)
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

