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

## Acknowledgements
Built on top of cstack's implementation guide of mini SQLite
