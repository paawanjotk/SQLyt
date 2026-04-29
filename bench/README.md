## SQLyt Benchmarks

This folder contains a small, deterministic benchmark harness for **SQLyt**.

### What it measures
- **W1 Insert-seq**: sequential primary-key inserts (batched up to 32 rows/statement)
- **W2 Insert-rand**: randomized primary-key inserts (batched)
- **W3 Update-rand**: point updates by id
- **W4 Delete-rand**: point deletes by id
- **W5 Scan**: `select *` scan (SQLyt uses `--quiet`, so it scans without printing)

### Requirements
- Build SQLyt first:

```bash
make build
```

### Quick start
Generate workloads:

```bash
python3 bench/gen_workloads.py --out bench/workloads --rows 10000 --seed 1
```

Run SQLyt benchmarks (5 trials per workload by default):

```bash
python3 bench/run_sqlyt.py --db ./db --workloads bench/workloads --trials 5
```

Run SQLite baseline using the same workloads:

```bash
# Preferred (requires sqlite3 CLI):
python3 bench/run_sqlite.py --engine cli --workloads bench/workloads --trials 5

# Fallback (no sqlite3 install needed; uses Python's sqlite3):
python3 bench/run_sqlite.py --engine python --workloads bench/workloads --trials 5
```

### Notes
- The runner uses `./db --run ... --quiet` and creates a **fresh root directory per trial**.
- Insert scripts use multi-row inserts (up to 32 rows) to match SQLyt’s `MAX_INSERT_ROWS`.
- If you want to benchmark autocommit vs grouped commits, generate both variants:
  - Autocommit: no `.begin/.commit`
  - Grouped: wrap mutating statements with `.begin`/`.commit`

