#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import random
from dataclasses import dataclass


MAX_INSERT_ROWS = 32  # keep in sync with SQLyt MAX_INSERT_ROWS


@dataclass(frozen=True)
class WorkloadSpec:
    rows: int
    seed: int
    updates: int
    deletes: int    


SCHEMA_SQLYT = """create database benchdb
.usedatabase benchdb
create table user (id int primary key, user_id int, user_name text, email text, city text)
"""


def _safe_text(s: str) -> str:
    # SQLyt text max is 64; keep plenty of margin.
    return s[:32]


def _batched_inserts(ids: list[int]) -> list[str]:
    stmts: list[str] = []
    for i in range(0, len(ids), MAX_INSERT_ROWS):
        batch = ids[i : i + MAX_INSERT_ROWS]
        values = []
        for k in batch:
            user_id = 100000 + k
            user_name = _safe_text(f"User{k}")
            email = _safe_text(f"user{k}@example.com")
            city = _safe_text(f"City{k % 100}")
            values.append(
                f'({k}, {user_id}, "{user_name}", "{email}", "{city}")'
            )
        stmts.append("insert into user values " + ", ".join(values))
    return stmts


def _write_sql(path: str, lines: list[str]) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        for line in lines:
            f.write(line.rstrip() + "\n")


def _workload_common(spec: WorkloadSpec, ids_for_load: list[int], *, grouped: bool) -> list[str]:
    r = random.Random(spec.seed)
    lines: list[str] = []
    lines.extend(SCHEMA_SQLYT.strip().splitlines())

    if grouped:
        lines.append(".begin")

    lines.extend(_batched_inserts(ids_for_load))

    # Updates: update city for random existing ids
    for _ in range(spec.updates):
        k = r.choice(ids_for_load)
        new_city = _safe_text(f"UCity{k % 100}")
        lines.append(f'update user set city = "{new_city}" where id = {k}')

    # Deletes: delete random ids (may include repeats; that’s fine)
    for _ in range(spec.deletes):
        k = r.choice(ids_for_load)
        lines.append(f"delete from user where id = {k}")

    if grouped:
        lines.append(".commit")

    # Scan
    lines.append("select * from user")
    lines.append(".exit")
    return lines


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, help="Output folder, e.g. bench/workloads")
    ap.add_argument("--rows", type=int, default=10_000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--updates", type=int, default=2_000)
    ap.add_argument("--deletes", type=int, default=2_000)
    args = ap.parse_args()

    spec = WorkloadSpec(rows=args.rows, seed=args.seed, updates=args.updates, deletes=args.deletes)
    out_dir = os.path.abspath(args.out)

    ids_seq = list(range(1, spec.rows + 1))
    ids_rand = ids_seq[:]
    random.Random(spec.seed).shuffle(ids_rand)

    workloads: list[tuple[str, list[int]]] = [
        ("W1_insert_seq", ids_seq),
        ("W2_insert_rand", ids_rand),
    ]

    for name, ids in workloads:
        for grouped in (False, True):
            variant = "grouped" if grouped else "autocommit"
            lines = _workload_common(spec, ids, grouped=grouped)
            _write_sql(os.path.join(out_dir, f"{name}__{variant}.sql"), lines)

    # Focused workloads (no updates/deletes, or only update/delete), useful for isolating hotspots.
    # W3 Update-only
    for grouped in (False, True):
        variant = "grouped" if grouped else "autocommit"
        spec_u = WorkloadSpec(rows=spec.rows, seed=spec.seed, updates=spec.updates, deletes=0)
        lines = _workload_common(spec_u, ids_seq, grouped=grouped)
        _write_sql(os.path.join(out_dir, f"W3_update_rand__{variant}.sql"), lines)

    # W4 Delete-only
    for grouped in (False, True):
        variant = "grouped" if grouped else "autocommit"
        spec_d = WorkloadSpec(rows=spec.rows, seed=spec.seed, updates=0, deletes=spec.deletes)
        lines = _workload_common(spec_d, ids_seq, grouped=grouped)
        _write_sql(os.path.join(out_dir, f"W4_delete_rand__{variant}.sql"), lines)

    # W5 Scan-only (load then scan)
    for grouped in (False, True):
        variant = "grouped" if grouped else "autocommit"
        spec_s = WorkloadSpec(rows=spec.rows, seed=spec.seed, updates=0, deletes=0)
        lines = _workload_common(spec_s, ids_seq, grouped=grouped)
        _write_sql(os.path.join(out_dir, f"W5_scan__{variant}.sql"), lines)

    print(f"Wrote workloads to: {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

