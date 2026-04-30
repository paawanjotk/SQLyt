# SQLyt

## Build

```bash
make build
```

Or directly with gcc:

```bash
gcc -std=c11 -pthread -Iinclude src/app.c src/readline_runtime.c src/btree.c src/pager.c src/parser.c src/cli.c -ldl -o db
```

## Test

```bash
make test
```

Interactive line-editing enhancements (history, arrow-key navigation, and tab
completion) are enabled automatically when `libreadline` is available at
runtime.

## Project Structure

```text
SQLyt/
├── include/
│   └── sqlyt.h        # shared types, constants, and cross-module interfaces
├── src/
│   ├── app.c          # program entrypoint (`main`) and startup loop
│   ├── readline_runtime.c
│   │                   # readline runtime integration and completion wiring
│   ├── btree.c        # B+ tree layout, cursor operations, insert/delete/rebalance
│   ├── pager.c        # pager, WAL, checkpointing, open/close lifecycle
│   ├── parser.c       # SQL parsing, schema encoding, row pack/unpack helpers
│   └── cli.c          # REPL helpers, meta-commands, and SQL execution routing
├── tests/
│   └── test_main.py   # end-to-end SQL and storage behavior tests
├── data/              # default runtime root path for databases
├── Makefile           # build/test/clean targets
└── README.md
```

## Architecture

At a high level, SQLyt is organized into six layers:

1. Application entry layer (`src/app.c`)
- Defines `main()`.
- Initializes session/root path and drives the input/execute loop.

2. CLI layer (`src/cli.c`)
- Reads input, normalizes commands, handles meta-commands.
- Dispatches parsed SQL statements to execution paths.

3. Readline runtime layer (`src/readline_runtime.c`)
- Dynamically loads readline when available.
- Provides history and tab-completion integration.

4. Parsing and row-shaping layer (`src/parser.c`)
- Parses SQL text into `SqlStatement` structures.
- Encodes schema metadata and row payloads.

5. B+ tree layer (`src/btree.c`)
- Navigates and mutates table/index nodes.
- Handles node split, merge, and rebalancing.

6. Pager and durability layer (`src/pager.c`)
- Manages page cache and file I/O.
- Implements WAL frame writes and checkpointing.

Shared contract layer (`include/sqlyt.h`)
- Defines shared structs, enums, constants, and function contracts across modules.

Execution flow:

```text
CLI input -> parser -> statement executor -> btree operations -> pager/WAL -> disk
```

## Run

Default root folder is `./data`:

```bash
./db
```

Optional custom root folder:

```bash
./db /path/to/root
```

Additional flags (benchmarking-friendly):

```bash
./db --root /path/to/root --run workload.sql --quiet --timer
```

- `--run <file>`: execute a script non-interactively and exit.
- `--quiet`: suppress prompts and normal output (errors may still print).
- `--timer`: print per-statement elapsed time (disabled in `--quiet`).
- `--root <path>`: explicit root folder (equivalent to the positional root arg).

## Command Cheat Sheet

### Meta commands

1. Exit CLI

```text
.exit
```

2. Use an existing database (must already be created)

```text
.usedatabase <db_name>
```

3. List all database folders under root

```text
.showdatabases
```

4. List tables in active database with root pages

```text
.showtables
```

5. Print B+ tree structure for a table

```text
.btree <table_name>
```

6. Print storage constants

```text
.constants
```

7. Benchmarking helpers

```text
.quiet on|off
.timer on|off
.begin
.commit
```

### SQL commands

1. Create a database folder

```sql
create database app
```

2. Create a table
- first column must be `id int` (optionally `primary key`)
- remaining columns can be `int` or `text`
- `text` values are capped at **64 characters**; longer strings will be rejected

```sql
create table user (
  id int primary key,
  user_id int,
  user_name text,
  email text,
  city text
)
```

3. Insert a row
- first value is row key (`id`)
- remaining values must match schema order
- quoted strings are supported

```sql
insert into user values (1, 101, "Alice Doe", "alice@example.com", "New York")
```

4. Select all rows

```sql
select * from user
```

5. Delete a row

```sql
delete from user where id = 1
```

```sql
select * from user
```

## Quick Session Example

```text
create database app
.usedatabase app

create table user (id int primary key, user_id int, user_name text, email text)
insert into user values (1, 101, "Alice Doe", "alice@example.com")
insert into user values (2, 102, "Bob", "bob@example.com")

select * from user

delete from user where id = 1

select * from user

.showtables
.btree user
.exit
```

## Benchmarks

This repo includes a small, deterministic benchmark harness under `bench/` that runs the same generated SQL workloads against **SQLyt** and **SQLite**.

### Reproduce

```bash
make build
python3 bench/gen_workloads.py --out bench/workloads --rows 10000 --seed 1

# SQLyt (defaults to 5 trials/workload)
python3 bench/run_sqlyt.py --db ./db --workloads bench/workloads --trials 5

# SQLite baseline (preferred: sqlite3 CLI; fallback: Python sqlite3 module)
python3 bench/run_sqlite.py --engine cli --workloads bench/workloads --trials 5
python3 bench/run_sqlite.py --engine python --workloads bench/workloads --trials 5
```

### Results (5-trial mean; lower is better)

Workloads are generated in two variants:
- **`__autocommit`**: each statement commits independently (worst-case commit overhead)
- **`__grouped`**: mutating statements are wrapped in `.begin`/`.commit`

Headline: **SQLyt is ~7×–113× faster than SQLite on autocommit-heavy write workloads** in this harness, while **SQLite is currently faster on grouped-transaction workloads**.

| Workload | SQLyt mean (s) | SQLite mean (s) | Speedup (SQLite/SQLyt) |
| --- | ---: | ---: | ---: |
| W1 insert-seq (autocommit) | 0.131 | 6.894 | 52.8× |
| W2 insert-rand (autocommit) | 0.159 | 7.958 | 50.1× |
| W3 update-rand (autocommit) | 0.104 | 4.385 | 42.1× |
| W4 delete-rand (autocommit) | 0.112 | 4.410 | 39.5× |
| W5 scan (autocommit) | 0.069 | 0.485 | 7.0× |
| W6 churn (autocommit) | 0.970 | 109.423 | 112.8× |
| W1 insert-seq (grouped) | 0.073 | 0.063 | 0.9× |
| W2 insert-rand (grouped) | 0.072 | 0.039 | 0.5× |
| W3 update-rand (grouped) | 0.071 | 0.028 | 0.4× |
| W4 delete-rand (grouped) | 0.071 | 0.025 | 0.4× |
| W5 scan (grouped) | 0.077 | 0.020 | 0.3× |
| W6 churn (grouped) | 0.255 | 0.203 | 0.8× |

Memory note: in these runs, **SQLyt used ~5–6 MB peak RSS** vs **~19–40 MB** for SQLite, depending on workload.

## Acknowledgements
Built on top of cstack's implementation guide of mini SQLite
