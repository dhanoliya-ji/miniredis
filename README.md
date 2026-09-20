# MiniRedis

**A Redis-style in-memory database built from scratch in C++20.**

Every layer is hand-written — the event loop, the RESP protocol codec, the data
structures, key expiry, memory eviction, the append-only log, the snapshot
format, replication and the cluster slot map. No database libraries, no
networking frameworks, no serialisation libraries.

It speaks genuine RESP2, so **the stock `redis-cli` connects to it and works
unmodified**.

```
$ redis-cli -p 6380
127.0.0.1:6380> SET user:1 alice
OK
127.0.0.1:6380> ZADD leaderboard 100 alice 90 bob
(integer) 2
127.0.0.1:6380> ZREVRANGE leaderboard 0 -1 WITHSCORES
1) "alice"
2) "100"
3) "bob"
4) "90"
```

---

## Contents

- [Why this exists](#why-this-exists)
- [What it does](#what-it-does)
- [Quick start](#quick-start)
- [Architecture at a glance](#architecture-at-a-glance)
- [Feature tour](#feature-tour)
- [Performance](#performance)
- [Configuration](#configuration)
- [Testing](#testing)
- [Project layout](#project-layout)
- [Documentation](#documentation)
- [Honest limitations](#honest-limitations)

---

## Why this exists

An engineer can operate Redis for years without being able to answer:

- Redis is single-threaded. Why is that *fast* rather than slow?
- If the process is killed mid-write, exactly how much data is lost — and what
  determines that number?
- A replica reconnects after a three-second blip. Does it re-transfer the whole
  dataset? What decides?
- The server hits its memory limit. Which key is deleted, and how was it chosen
  without scanning everything?
- `MULTI`/`EXEC` is called a transaction. Is it? What happens if the third of
  five commands fails?

Each is a real engineering trade-off, and each determines how the system behaves
on the day something goes wrong. Documentation gives you the *what*. Building it
gives you the *why*.

This project began as a ~1,600-line distributed key-value store with six real
defects, including a write-ahead log appended outside the lock (so recovery
could restore the *losing* value of two concurrent writes) and a thread handle
leaked per connection. The first six commits fix them one at a time; the rest
rebuild the system so half of them become unrepresentable.

The full reasoning is in **[docs/PROBLEM_STATEMENT.md](docs/PROBLEM_STATEMENT.md)**.

---

## What it does

| | |
|---|---|
| **Protocol** | RESP2, incremental parser correct under TCP fragmentation; inline commands so it works over plain telnet |
| **Concurrency** | Single-threaded event loop, `poll()`/`WSAPoll`, thousands of connections, zero locks |
| **Data types** | Strings, lists, hashes, sets, sorted sets — 148 commands |
| **Expiry** | TTLs with both lazy and active sampling reclamation |
| **Memory** | `maxmemory` with all 8 eviction policies, sampled approximated LRU/LFU |
| **Durability** | Append-only log (3 fsync policies, self-rewriting) + CRC64-checked binary snapshots |
| **Replication** | Leader/follower with byte offsets, ring backlog and partial resynchronisation |
| **Sharding** | 16384 hash slots, Redis-compatible CRC16, `MOVED` redirection, hash tags |
| **Transactions** | `MULTI`/`EXEC`/`DISCARD`/`WATCH` optimistic concurrency |
| **Messaging** | Pub/Sub with channel and glob-pattern subscriptions |
| **Observability** | `INFO`, `SLOWLOG`, `MONITOR`, `MEMORY DOCTOR`, `CLIENT LIST` |
| **Bonus** | A small SQL query surface over the keyspace |
| **Platforms** | Windows (Winsock2) and Linux/macOS (POSIX) |

---

## Quick start

### Build

**Windows**

```powershell
.\scripts\build.ps1
```

**Linux / macOS**

```bash
./scripts/build.sh
```

Both prefer CMake when it is installed and fall back to invoking the compiler
directly, so the project builds on a bare toolchain with no extra tooling.
Requires a C++20 compiler (GCC 11+, Clang 14+, MSVC 2022+).

### Run

```bash
# Terminal 1 - the server
./bin/miniredis-server --port 6380

# Terminal 2 - the client
./bin/miniredis-cli -p 6380
```

```
mini> SET greeting "hello world"
OK
mini> GET greeting
"hello world"
mini> INCR counter
(integer) 1
mini> EXPIRE greeting 60
(integer) 1
mini> TTL greeting
(integer) 60
```

Type `help` at the prompt for a guided tour.

### Or use the real redis-cli

```bash
redis-cli -p 6380 PING
```

---

## Architecture at a glance

```
   client socket
        │  bytes arrive — possibly a partial command, possibly several
        ▼
   poll()  ──────────  one thread watching every socket at once
        │
        ▼
   RESP parser  ─────  incomplete? consume nothing, wait for more
        │
        ▼
   command table  ───  metadata drives routing: write? denyoom? which keys?
        │
        ▼
   dispatcher gates ─  arity → auth → pubsub → MULTI → cluster slot
        │              → read-only replica → memory limit
        ▼
   handler  ─────────  mutates the keyspace, bumps the dirty counter
        │
        ├──► append-only log      ┐
        ├──► replica sockets      ├─ only when something actually changed
        ├──► WATCH invalidation   │
        └──► slow log             ┘
        │
        ▼
   reply written back
```

**One thread, on purpose.** The work in a command is a hash lookup and a memory
copy — hundreds of nanoseconds. A contended mutex costs comparable time and a
context switch costs ten times more. Synchronisation isn't a small tax on that
work; under load it *is* the work. Removing threads makes every command atomic
by construction, which is why three of the six original defects cannot be
written in this architecture.

The cost is real and documented: one slow command blocks everyone. That is
exactly why `SCAN` exists, why there is a slow log, and why `DEBUG SLEEP` is
implemented — so the cost can be demonstrated rather than described.

Full detail: **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)**.

---

## Feature tour

### Data types

```bash
# Strings — and the counters built on them
SET user:1 alice
APPEND user:1 " smith"
INCR pageviews
INCRBYFLOAT price 19.99
SETRANGE user:1 0 A
MGET user:1 user:2 user:3

# Lists — O(1) at both ends
RPUSH queue job1 job2 job3
LPOP queue
LRANGE queue 0 -1
LMOVE queue processing LEFT RIGHT

# Hashes — fields updated independently, no read-modify-write of the whole value
HSET user:1 name alice email a@example.com
HINCRBY user:1 login_count 1
HGETALL user:1

# Sets
SADD tags redis database cpp
SINTER tags:a tags:b
SPOP tags 2

# Sorted sets — leaderboards, rate limiters, priority queues
ZADD leaderboard 100 alice 90 bob 110 carol
ZREVRANGE leaderboard 0 2 WITHSCORES
ZRANGEBYSCORE leaderboard 90 100
ZINCRBY leaderboard 15 bob
```

### Expiry

```bash
SET session:abc "data" EX 3600      # expire in an hour
TTL session:abc                     # → 3600
PERSIST session:abc                 # remove the TTL
TTL session:abc                     # → -1  (exists, no TTL)
TTL nonexistent                     # → -2  (no such key)
```

Keys expire two ways: **lazily** when something reads them, and **actively**
from a background sampling cycle. The second exists because a key that expires
and is never read again would otherwise hold its memory forever.

### Memory limits and eviction

```bash
miniredis-server --maxmemory 512mb --maxmemory-policy allkeys-lru
```

All eight Redis policies are implemented. Eviction **samples** candidate keys
rather than maintaining an exact LRU list — exact LRU costs two pointers per
key, and sampling five lands within a few percent of it.

```bash
MEMORY DOCTOR      # plain-language advice about the current configuration
INFO memory        # used_memory, the limit, and the percentage between them
```

### Persistence

```bash
# Append-only log: durability
miniredis-server --appendonly yes --appendfsync everysec

# Snapshots: fast restarts and backups
miniredis-server --save 900 1 --save 300 100
```

|  | AOF | RDB |
|---|---|---|
| Data at risk | one fsync interval | one save interval |
| File size | grows with writes | proportional to the dataset |
| Restart | replay every command | one sequential read |

A real deployment runs both. `appendfsync everysec` is the default because it is
the knee of the durability/throughput curve: at most one second at risk, for
negligible cost.

TTLs are always persisted as **absolute** deadlines. A relative expiry would be
re-anchored to whenever the file was replayed, quietly extending every key's
life by the length of the outage.

### Replication

```bash
# Terminal 1 — the master
miniredis-server --port 6380

# Terminal 2 — a replica
miniredis-server --port 6381 --replicaof 127.0.0.1 6380
```

```bash
miniredis-cli -p 6380 SET k v
miniredis-cli -p 6381 GET k        # → "v"
miniredis-cli -p 6381 SET k other  # → READONLY ...
```

The write stream carries a **byte offset**, and the master keeps a ring backlog
of recent bytes. A replica that reconnects within that window sends
`PSYNC <replid> <offset>` and receives only what it missed — a partial
resynchronisation instead of a full dataset transfer.

**The backlog size directly buys reconnect resilience:** 1 MB on a stream doing
100 KB/s covers a ten-second outage.

Replication is asynchronous, as in Redis — a master acknowledges a write before
any replica has it. `WAIT` reports how many replicas have caught up.

### Cluster sharding

```bash
miniredis-server --port 6380 --cluster-enabled yes
```

```bash
CLUSTER KEYSLOT foo     # → 12182, exactly matching real Redis
CLUSTER INFO
CLUSTER SLOTS
```

Slots — not keys — are assigned to nodes, so moving data means reassigning a
range and **no key is ever rehashed**. A node asked for a key it does not own
replies `-MOVED <slot> <host:port>` rather than proxying.

Hash tags force related keys onto one node on purpose:

```bash
{user:42}:profile    # only "user:42" is hashed,
{user:42}:sessions   # so these share a slot and
{user:42}:cart       # multi-key commands work
```

### Transactions

```bash
WATCH balance
GET balance          # → 100
MULTI
SET balance 90
EXEC                 # → nil if anyone wrote `balance` in the meantime
```

A Redis transaction gives you **isolation** and **all-or-nothing dispatch** — it
does *not* give you rollback. If the third of five commands fails at runtime,
the first two stay applied and the last two still run. `WATCH` supplies the
missing piece: optimistic compare-and-swap.

`WATCH` is a *flag*, not a value comparison — a key written and written back to
its original value still aborts, which is what catches the ABA problem.

### Pub/Sub

```bash
# Terminal 1
miniredis-cli -p 6380
mini> SUBSCRIBE news

# Terminal 2
miniredis-cli -p 6380 PUBLISH news "hello"
```

Pattern subscriptions work too (`PSUBSCRIBE news.*`), with the cost made
explicit: every registered pattern is tested against every publish.

Delivery is fire-and-forget — a message published while a subscriber is away is
gone. That is the reason pub/sub is not a message queue.

### The SQL layer

```
mini> SQL SELECT * FROM kv
+---------+-------+
| key     | value |
+---------+-------+
| counter | 42    |
| user:1  | alice |
+---------+-------+
2 row(s) in set.

mini> SQL INSERT INTO kv VALUES ('user:2', 'bob')
Query OK, 1 row affected (INSERT).
```

Carried forward from the project this grew out of. It makes a point about
interface design: the storage underneath is a hash map with no schema, no joins
and no secondary index, so `WHERE key = 'x'` is a hash lookup and
`WHERE value = 'x'` is refused rather than silently becoming a full scan. **A
translation layer cannot invent capabilities the engine does not have.**

---

## Performance

Measured with the included `miniredis-benchmark`, which reports full latency
distributions rather than averages — an average hides the tail, and the tail is
what users notice.

```bash
./bin/miniredis-benchmark -t set,get -n 100000 -c 32
./bin/miniredis-benchmark -t get -P 16 --csv bench/results/pipeline.csv
```

Measured on Windows 11 over loopback (see [docs/BENCHMARKS.md](docs/BENCHMARKS.md)
for the full method and hardware):

| Concurrency | Throughput | p50 latency | p99 latency |
|---:|---:|---:|---:|
| 1 | 27,102 ops/s | **29 µs** | 107 µs |
| 4 | 54,249 ops/s | 57 µs | 152 µs |
| 16 | 57,173 ops/s | 232 µs | 678 µs |
| 64 | 63,482 ops/s | 941 µs | 1,765 µs |
| 128 | 61,014 ops/s | 1,956 µs | 3,159 µs |

The headline finding is a textbook queueing result, visible directly in the
data: **throughput saturates at about 4 concurrent clients, and every client
added after that converts almost entirely into latency.** From 4 to 128 clients
throughput moves 12% while p50 latency grows 34×. Past saturation, concurrency
does not buy work — it buys queue.

The single-client p50 of **29 µs** is the real figure for what the server does:
parse, hash, look up, reply.

Results, charts and analysis: **[notebooks/benchmarks.ipynb](notebooks/benchmarks.ipynb)**
and **[docs/BENCHMARKS.md](docs/BENCHMARKS.md)**.

---

## Configuration

Directives work identically on the command line, in a config file and via
`CONFIG SET` — they share one implementation, so they cannot drift.

```bash
miniredis-server miniredis.conf --port 6390
```

```conf
# miniredis.conf
port 6380
bind 127.0.0.1
dir ./data

maxmemory 512mb
maxmemory-policy allkeys-lru

appendonly yes
appendfsync everysec

save 900 1
save 300 100

loglevel notice
slowlog-log-slower-than 10000
```

```bash
CONFIG GET maxmemory*
CONFIG SET maxmemory-policy allkeys-lfu
```

Full list: `miniredis-server --help`, or **[docs/CONFIGURATION.md](docs/CONFIGURATION.md)**.

---

## Testing

```bash
# Unit tests — protocol, data structures, expiry, eviction, snapshots, backlog
./bin/miniredis-tests

# Integration tests — real processes over real sockets
.\scripts\integration_test.ps1        # Windows
./scripts/integration_test.sh         # Linux / macOS
```

The unit suite covers components in isolation. The integration suite covers what
only appears across process boundaries: **crash recovery** (the server is
`kill`ed, not shut down cleanly), replica synchronisation, read-only
enforcement, eviction under a real memory limit, and transaction aborts.

---

## Project layout

```
miniredis/
├── include/miniredis/     public headers
├── src/
│   ├── net.cpp            the only file that includes a platform socket header
│   ├── resp.cpp           RESP2 codec
│   ├── object.cpp         the five value types
│   ├── keyspace.cpp       dictionary + expiry + eviction metadata
│   ├── server.cpp         event loop and dispatcher
│   ├── aof.cpp            append-only log
│   ├── rdb.cpp            binary snapshots
│   ├── replication.cpp    offsets, backlog, PSYNC
│   ├── cluster.cpp        hash slots and MOVED
│   └── cmd_*.cpp          command implementations, one file per family
├── tools/benchmark.cpp    load generator
├── tests/                 unit suite
├── scripts/               build and integration test scripts
├── docs/                  problem statement, architecture, commands, benchmarks
├── notebooks/             benchmark analysis
└── bench/results/         measured data (CSV)
```

---

## Documentation

| Document | What it covers |
|---|---|
| **[PROBLEM_STATEMENT.md](docs/PROBLEM_STATEMENT.md)** | The problem, objectives, scope, and the six original defects |
| **[ARCHITECTURE.md](docs/ARCHITECTURE.md)** | How every subsystem works and why it is built that way |
| **[COMMANDS.md](docs/COMMANDS.md)** | Every implemented command |
| **[CONFIGURATION.md](docs/CONFIGURATION.md)** | Every directive |
| **[BENCHMARKS.md](docs/BENCHMARKS.md)** | Measurements and what they mean |
| **[notebooks/benchmarks.ipynb](notebooks/benchmarks.ipynb)** | Charts and analysis, re-runnable |

---

## Honest limitations

Stating the gaps is part of the point.

| Not implemented | Why |
|---|---|
| Lua scripting, Streams, HyperLogLog, geospatial, bitfields, modules | Out of scope |
| Automatic failover | Requires consensus. Implemented badly it produces split brain, so `FAILOVER` explains this rather than doing something unsafe. |
| Forked background saves | `fork()` does not exist on Windows. `BGSAVE` saves synchronously and **says so in its reply** rather than pretending to be asynchronous. |
| RESP3 | The handshake is explicitly rejected. Accepting it and then not sending push frames would break clients silently. |
| Multiple memory encodings per type | One encoding per type keeps the data structures readable. The memory difference matters at a scale this project does not target. |
| Live cluster resharding | Needs a slot→key index, which only matters during resharding. |

Two places where MiniRedis is asymptotically worse than Redis, both documented
at the call site rather than glossed over:

- `ZRANK` is O(n) instead of O(log n) — a balanced tree has no span counters.
- `SCAN`'s bucket-index cursor can skip a key under concurrent rehash, which
  Redis's reverse-binary cursor avoids.

---

## License

MIT — see [LICENSE](LICENSE).
