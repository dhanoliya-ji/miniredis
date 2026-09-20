# Architecture

How MiniRedis is put together, and why each part is the way it is.

---

## 1. The path of a command

Everything starts here. A client sends `SET user:1 alice`. Here is every step
between the bytes arriving and the reply leaving.

```
   client socket
        │
        │  bytes arrive (possibly a partial command, possibly several)
        ▼
┌───────────────────┐
│  poll() wakes up  │   one thread, watching every socket at once
└─────────┬─────────┘
          ▼
┌───────────────────┐
│  read into the    │   appended to the client's input buffer;
│  client's buffer  │   nothing is assumed about message boundaries
└─────────┬─────────┘
          ▼
┌───────────────────┐
│  RESP parser      │   Incomplete? rewind, consume nothing, wait for more.
│  (incremental)    │   Complete? hand over Args = ["SET","user:1","alice"]
└─────────┬─────────┘
          ▼
┌───────────────────┐
│  command lookup   │   hash the name, find the CommandSpec descriptor
└─────────┬─────────┘
          ▼
┌───────────────────────────────────────────────────────┐
│  dispatcher gates, in order:                          │
│    arity        → wrong count?      -ERR              │
│    auth         → not authenticated? -NOAUTH          │
│    subscribe    → in pub/sub mode?  only pub/sub cmds │
│    MULTI        → transaction open? queue it, +QUEUED │
│    cluster      → wrong slot?       -MOVED            │
│    replica      → read-only?        -READONLY         │
│    memory       → over maxmemory?   evict, or -OOM    │
└─────────┬─────────────────────────────────────────────┘
          ▼
┌───────────────────┐
│  handler runs     │   mutates the keyspace, writes its reply,
│                   │   bumps the dirty counter if it changed anything
└─────────┬─────────┘
          ├──────────────► AOF buffer      (only if dirty changed)
          ├──────────────► replica sockets (only if dirty changed)
          ├──────────────► WATCH invalidation for this key
          └──────────────► slow log, if it took too long
          ▼
┌───────────────────┐
│  reply appended   │   written to the socket immediately if it fits;
│  to output buffer │   otherwise poll() for writability
└───────────────────┘
```

Two details in that diagram carry most of the design weight.

**The parser rewinds on incomplete input.** TCP has no message boundaries. A
single `SET` can arrive as three separate reads, and two commands can arrive in
one. The parser handles this by being restartable: given a partial command it
consumes *zero* bytes and reports `Incomplete`, so the caller simply keeps the
buffer and tries again after the next read. Every alternative — a length
prefix, a scan for a terminator, a state machine holding partial state — is
either less general or more code.

**Propagation is gated on the dirty counter, not on the command's flags.**
`SET k v` where `k` already holds `v`, `DEL` of a missing key, and `SADD` of a
member already present are all write commands that changed nothing. Shipping
them to the log and the replicas would inflate both for no benefit. So handlers
increment a counter only when they genuinely mutate, and the dispatcher
compares before and after.

---

## 2. Threading: one thread, on purpose

The original version of this project used a thread per connection with a
`shared_mutex` over the map. MiniRedis uses a single thread and no locks at
all. This is the most important decision in the system.

### Why fewer threads is faster here

The work in a command is a hash lookup and a memory copy: **hundreds of
nanoseconds**. Against that:

| Operation | Rough cost |
|---|---|
| Hash lookup + copy (the actual work) | ~100–500 ns |
| Uncontended mutex lock/unlock | ~20–50 ns |
| **Contended** mutex under load | ~1,000+ ns |
| Thread context switch | ~2,000–10,000 ns |
| Cache line bouncing between cores | ~100+ ns per access |

Synchronisation is not a small tax on the work — under contention it *is* the
work. Threads pay off when each one has substantial independent work to do.
Here, each has almost none.

Meanwhile the thing that actually takes time — waiting on the network — does
not need a thread at all. One `poll()` call waits on ten thousand sockets
simultaneously.

### What it buys

1. **Atomicity for free.** Nothing runs concurrently, so every command is
   atomic by construction. There is no lock to forget and no window between a
   mutation and its log record. Three of the six defects fixed at the start of
   this project's history were exactly those failure modes; in this
   architecture they cannot be written.
2. **No synchronisation cost at all.** No locks, no atomics, no false sharing.
3. **Reasoning is tractable.** "What happens if two clients do X at once?" has
   one answer: one of them goes first, completely.

### What it costs

**One slow command blocks everyone.** This is real and is not hidden:

- `KEYS` walks the whole keyspace, so `SCAN` exists and `KEYS` is discouraged.
- The slow log records anything over a threshold, so you can find the culprit.
- `DEBUG SLEEP` is implemented specifically so the cost can be *demonstrated*.

The event loop also caps per-client output buffers. A subscriber that stops
reading would otherwise make the server buffer without bound until the OS kills
it.

### One subtle but important detail

`poll()` is only asked about writability when a client actually has pending
output. Registering for writability unconditionally makes `poll()` return
immediately on every iteration — the socket is almost always writable — which
spins a CPU core at 100% on a completely idle server.

---

## 3. Modules

```
include/miniredis/          src/
├── common.hpp              ├── common.cpp        parsing, glob, formatting
├── net.hpp                 ├── net.cpp           Winsock2 / POSIX sockets
├── resp.hpp                ├── resp.cpp          RESP2 codec
├── object.hpp              ├── object.cpp        the five value types
├── keyspace.hpp            ├── keyspace.cpp      dict + expires + eviction
├── command.hpp             ├── command.cpp       the command table
├── server.hpp              ├── server.cpp        event loop + dispatcher
│                           ├── config.cpp        directives
│                           ├── info.cpp          INFO
├── aof.hpp                 ├── aof.cpp           append-only log
├── rdb.hpp                 ├── rdb.cpp           binary snapshots
├── pubsub.hpp              ├── pubsub.cpp        channels and patterns
├── replication.hpp         ├── replication.cpp   offsets, backlog, PSYNC
├── cluster.hpp             ├── cluster.cpp       hash slots, MOVED
                            └── cmd_*.cpp         command implementations
```

Nothing outside `net.cpp` includes a platform socket header. That is what keeps
the Windows/POSIX difference — `SOCKET` vs `int`, `closesocket` vs `close`,
`WSAPoll` vs `poll`, `WSAStartup` — in one file instead of smeared everywhere.

### The command table

Commands are a table of descriptors, not a `switch`. The metadata is what the
rest of the server reasons about:

```cpp
table.add({"SET", cmdSet, -3, kWrite | kDenyOom, /*firstKey=*/1, 1, 1});
//          name  handler  arity  flags                 where the keys are
```

- `kWrite` → refuse on a read-only replica, log it, ship it to replicas
- `kDenyOom` → refuse when over `maxmemory`
- `firstKey/lastKey/keyStep` → where cluster mode finds keys to hash
- negative arity → "at least this many", for variadic commands

Each family registers explicitly from `buildCommandTable()` rather than through
a static initialiser. Self-registration looks tidier but breaks in a static
library: the linker discards an object file nothing references, silently taking
its registrations with it.

---

## 4. The keyspace

### Two dictionaries, not one

```
Keyspace
├── m_dict     : key → { value, lruClock, lfuCounter }
└── m_expires  : key → absolute deadline in milliseconds   (only volatile keys)
```

Putting the TTL in a second dictionary rather than as a field on the first
looks redundant. It is the thing that makes background expiry affordable.

The expiry cycle samples keys and deletes the dead ones. If TTLs lived on the
main dict, then in a database with a million keys of which ten have a TTL,
sampling twenty keys would almost always sample twenty non-volatile ones and do
nothing at all. With a separate dictionary, the sample is drawn *only* from
keys that can actually expire.

### Two expiry mechanisms, because one is not enough

**Lazy** — every lookup checks the deadline and deletes if it has passed. Cheap
and exact, but it only fires when something reads the key.

**Active** — a cycle in `serverCron` samples volatile keys and reclaims the
dead ones. Without this, a key that expires and is *never read again* holds its
memory forever. That is the whole reason both exist.

The cycle reports what fraction of its sample was already dead and goes round
again while that stays high, so it works hard when there is a lot to reclaim
and stops when there is not.

**Replicas do not expire keys on their own.** The master sends an explicit
`DEL` when a key dies. If both expired independently, a read on the replica
could differ from the same read on the master purely because their clocks
differ by a few milliseconds.

### Eviction: sampled, not exact

Exact LRU needs a doubly-linked list — two pointers on every key, updated on
every access. For a database holding millions of small values that is a large
fraction of total memory spent on bookkeeping.

So instead each key carries:
- a **32-bit coarse clock** (seconds, not milliseconds) for LRU
- an **8-bit logarithmic counter** for LFU, which decays over time so a key
  that was hot an hour ago does not stay immortal

and eviction **samples** N random keys and evicts the best candidate among
them. Sampling five already lands within a few percent of exact LRU, at a
fraction of the memory.

All eight Redis policies are implemented: `noeviction`, `allkeys-{lru,lfu,random}`,
`volatile-{lru,lfu,random,ttl}`.

### Why LFU's counter is logarithmic

An 8-bit counter saturates after 255 hits, which is nothing. Redis's trick is
to make the *probability* of incrementing fall as the counter rises, so 8 bits
can represent frequencies from "once" to "millions of times". MiniRedis uses
the same encoding.

---

## 5. Persistence

Two mechanisms that solve the same problem from opposite ends. A real
deployment runs both.

|  | AOF (command log) | RDB (snapshot) |
|---|---|---|
| What it is | every write, appended | a point-in-time image |
| Data at risk | one fsync interval | one save interval |
| File size | grows with writes | proportional to the dataset |
| Restart | replay every command | one sequential read |
| Ongoing cost | a little on every write | a burst when it runs |

So the AOF is the **durability** story and the RDB is the **restart-time and
backup** story. A dataset built from a billion commands reloads from a snapshot
in seconds and from a log in a very long time.

### The AOF

Records are **RESP-encoded commands**, so replaying the file is literally
feeding it back through the command dispatcher. There is no second parser to
keep in sync with the first and no escaping scheme to get wrong — the original
project's space-escaping log was a whole class of bugs that simply does not
exist in this design.

**fsync is made explicit**, because "written" and "durable" are different
things. `write()` only reaches the OS page cache; a power cut loses everything
still sitting in it.

| `appendfsync` | Data at risk | Cost |
|---|---|---|
| `always` | nothing acknowledged | a disk round trip per loop iteration |
| `everysec` | up to 1 second | negligible — the default, and the knee of the curve |
| `no` | whatever the OS hasn't flushed | none |

Commands are buffered and written **once per event loop iteration**, so a
pipeline of a thousand commands costs one `write()` rather than a thousand.

**Rewriting.** A log of every command ever run grows without bound. Periodically
it is replaced by the shortest command sequence that reproduces the current
dataset — one `SET` per key instead of the thousand `INCR`s that produced it.
The new file is renamed into place only after it is complete and fsynced, so a
crash mid-rewrite leaves the old one intact.

**A truncated tail is recovered from, not treated as corruption.** A process
killed mid-write leaves a partial final record; that is the *expected* shape of
a crash. Everything before it is valid, so it is kept and the partial tail is
dropped with a warning.

### The RDB

Length-prefixed, versioned, and explicitly little-endian byte-assembled so a
snapshot moves between machines of opposite byte order. Doubles are bit-cast
rather than printed, so a sorted-set score survives the round trip exactly.

**A CRC64 trailer covers the whole file.** Without it, a truncated snapshot
loads as a plausible-looking half dataset — which is far worse than failing
outright. Saves go through a temporary file and an atomic rename, so an
interrupted save never destroys the last good snapshot.

### Why TTLs are always written as absolute deadlines

Both formats record expiry as `PEXPIREAT <absolute-ms>`, never as a relative
`EXPIRE`. A relative expiry would be re-anchored to whenever the file happened
to be replayed, quietly extending every key's life by the length of the outage.

---

## 6. Replication

### The problem with the naive version

The original implementation re-sent each write as a text line. That works until
the link drops — and then the follower has no idea what it missed, so it throws
its entire dataset away and resynchronises from scratch. A two-second network
blip costs a full transfer of everything.

### The fix: give the stream a byte offset

```
  MASTER                                          REPLICA
  ──────                                          ───────
  offset: 48,291 ──────────────────────────────►  applied: 48,291
                                                       │
  ┌────────────────────────────┐                       │
  │ backlog (ring, e.g. 1 MB)  │                   ╳ link drops
  │ …keeps the most recent     │                       │
  │  bytes of the stream…      │                  reconnects
  └────────────────────────────┘                       │
                                  ◄── PSYNC <replid> 48,291
       offset still in backlog?
            yes → +CONTINUE, send only the missed bytes   (partial resync)
            no  → +FULLRESYNC, send a whole snapshot      (full resync)
```

So **the backlog size directly buys reconnect resilience**: 1 MB on a stream
doing 100 KB/s covers a ten-second outage without a full transfer.

### Details that matter

**The replication id identifies a timeline.** A promoted replica generates a
new one, because its old offsets belonged to the *previous* master's stream.
Letting a replica resume against them would resync it to a history that never
existed.

**A replica flushes its dataset only after the snapshot has fully arrived.**
Flushing first would leave it empty if the link died mid-transfer.

**`REPLCONF GETACK` is itself part of the stream**, so it goes into the backlog
too — otherwise replica offsets could never match the master's.

**Reconnects back off exponentially**, so a master that is down is not dialled
once per event loop iteration.

### The guarantee, stated plainly

Replication is **asynchronous**, as in Redis. A master acknowledges a write to
its client *before* any replica has it. A master that dies immediately after
replying can lose that write. `WAIT` lets a client ask how many replicas have
caught up, but it cannot retroactively make the write safe.

That is a deliberate trade — synchronous replication would put a network round
trip in the path of every write — and it is stated here rather than buried.

---

## 7. Cluster: sharding

Replication makes *copies* of a dataset. It does not make the dataset bigger.
Once the data stops fitting on one machine the keyspace has to be split, and
the split must be something every client and every node can compute
independently, or every request needs a lookup service.

Redis's answer is a fixed grid of **16384 hash slots**:

```
slot = CRC16(key) mod 16384
```

Slots, not keys, are assigned to nodes:

```
  node A: slots     0 – 5460
  node B: slots  5461 – 10922
  node C: slots 10923 – 16383
```

**That indirection is the whole trick.** Moving data means reassigning a slot
range, and the slot count never changes, so no key is ever rehashed. Compare
consistent hashing on a ring, where adding a node moves an arbitrary fraction
of keys.

A node asked for a key it does not own **does not proxy**. It replies:

```
-MOVED 3999 127.0.0.1:6381
```

The client retries there and caches the mapping, so the redirect costs one
extra round trip only while a client's routing table is stale.

The CRC16 here is the exact CCITT variant Redis uses — `CLUSTER KEYSLOT foo`
returns 12182 on both — so real cluster-aware clients route correctly against
MiniRedis.

### Hash tags

Multi-key commands are the catch: `MGET a b c` can only be served locally if
every key is in the same slot, otherwise it is refused with `CROSSSLOT`. Hash
tags exist to force related keys together on purpose:

```
{user:42}:profile  ┐
{user:42}:sessions ├─ only "user:42" is hashed, so all three share a slot
{user:42}:cart     ┘
```

---

## 8. Transactions

A Redis transaction is **not** a database transaction, and being precise about
this matters more than implementing it.

It provides exactly two guarantees:

1. **Isolation.** Queued commands run back to back with nothing interleaved.
   On a single-threaded server this is free — nothing else *can* run.
2. **All-or-nothing dispatch.** Either every queued command is attempted, or
   none is.

It does **not** provide atomicity on failure. If the third of five commands
fails at runtime — say `INCR` against a key holding a list — the first two stay
applied and the last two still run. **There is no rollback.**

Redis made that choice deliberately: a runtime type error is a bug in the
client, and paying for undo logs on every command to clean up after client bugs
is a bad trade.

### WATCH: the missing piece

`WATCH` supplies optimistic concurrency. Watch a key; if anybody touches it
before your `EXEC`, the `EXEC` aborts and you retry:

```
WATCH balance
GET balance          → 100
MULTI
SET balance 90
EXEC                 → nil if anyone wrote `balance` in the meantime
```

That is compare-and-swap, and it is how you build a correct read-modify-write
without a lock.

**It is a flag, not a value comparison.** A key written and then written back
to its original value still aborts the transaction. That is intentional: it
catches the ABA problem, where a value looks unchanged but the world moved
underneath.

---

## 9. Pub/Sub

Two dispatch paths, because their costs differ sharply:

| | Mechanism | Cost per publish |
|---|---|---|
| `SUBSCRIBE news` | exact channel → hash lookup | O(1) |
| `PSUBSCRIBE news.*` | glob pattern → tested against every publish | O(patterns) |

Keeping them in separate structures makes that asymmetry visible. A server with
a thousand patterns pays for all of them on every single publish, which is
worth knowing before adding the thousandth.

Subscribers are tracked by **client id, not pointer**, so a client that
disconnects mid-publish cannot leave a dangling entry behind.

**Delivery is fire-and-forget.** A message published while a subscriber is away
is gone — there is no queue and no replay. That is Redis's semantic too, and it
is the reason pub/sub is not a message queue.

---

## 10. The SQL layer

This is the one part with no Redis equivalent, carried forward from the project
this grew out of. It is kept for two reasons.

It is genuinely convenient at a terminal:

```
mini> SQL SELECT * FROM kv
+---------+-------+
| key     | value |
+---------+-------+
| user:1  | alice |
| counter | 42    |
+---------+-------+
```

And it makes a real point about interface design. The storage underneath is a
hash map: no schema, no joins, no secondary indexes, no query planner. **SQL
over a key-value store can only express what the storage engine can already
do.** `WHERE key = 'x'` is a hash lookup. `WHERE value = 'x'` would be a full
scan — which is exactly why it is refused rather than quietly executed.

A translation layer cannot invent capabilities the engine does not have, and
pretending otherwise is how a fast system becomes a slow one.

Unqualified `DELETE FROM kv` is also refused: `FLUSHDB` exists and says what it
does, whereas a bare `DELETE` silently destroying a database is the classic
production accident.

---

## 11. Where MiniRedis differs from Redis, and why

Honest accounting of the gaps:

| Area | Redis | MiniRedis | Why |
|---|---|---|---|
| Encodings | several per type (listpack, intset, skiplist…) purely to save memory | one per type | Readability. The memory difference matters at a scale this project does not target. |
| Sorted set | skip list with span counters | balanced tree | Identical except `ZRANK`, which is O(n) instead of O(log n) without the span counters. Documented at the call site. |
| `BGSAVE` | forks; child writes via copy-on-write | synchronous, and the reply says so | `fork()` does not exist on Windows. Pretending to be asynchronous would be worse than being honest. |
| `SCAN` cursor | reverse-binary increments, safe under rehash | bucket index | Simpler; the guarantee it loses under concurrent rehash is documented. |
| Failover | Sentinel / cluster-bus voting | not implemented; `FAILOVER` explains why | Failover without consensus produces split brain. Better absent than wrong. |
| Protocol | RESP2 and RESP3 | RESP2; RESP3 handshake explicitly rejected | Accepting RESP3 and then not sending push frames would break clients silently. |
| Cluster resharding | live slot migration with a slot→key index | static slot assignment | The index only matters during resharding. |

The pattern throughout: where MiniRedis does less, it says so at the point
where someone would look for the missing thing, rather than failing quietly.
