# Command Reference

148 commands. Syntax and semantics follow Redis, so anything written against
Redis works here unless noted.

Check what a running server implements with `COMMAND COUNT`, and inspect one
command's metadata with `COMMAND INFO <name>`.

**Conventions.** `key` is binary safe. Ranges are inclusive and accept negative
indexes counting back from the end. A container that loses its last element is
deleted, so an empty list and a missing key are indistinguishable. Operating on
a key of the wrong type returns `WRONGTYPE`.

---

## Strings

| Command | Notes |
|---|---|
| `SET key value [EX s\|PX ms\|EXAT ts\|PXAT ts] [NX\|XX] [KEEPTTL] [GET]` | **A plain SET clears any existing TTL** — it replaces the whole key, not just its value. `KEEPTTL` opts out. |
| `SETNX key value` | Set only if absent |
| `SETEX key seconds value` / `PSETEX key ms value` | Set with a TTL |
| `GET key` | |
| `GETSET key value` | Set and return the previous value |
| `GETDEL key` | Get and delete atomically |
| `MGET key [key...]` | A wrong-typed key reports `nil` rather than failing the whole command |
| `MSET key value [key value...]` | |
| `MSETNX key value [key value...]` | All-or-nothing: every key is checked before any is written |
| `INCR key` / `DECR key` | |
| `INCRBY key n` / `DECRBY key n` | **Overflow is an error, not a wrap** |
| `INCRBYFLOAT key n` | Propagates the computed result, not the increment |
| `APPEND key value` | Returns the new length |
| `STRLEN key` | |
| `GETRANGE key start stop` | |
| `SETRANGE key offset value` | Writing past the end zero-pads the gap |

Counters are stored as strings and re-parsed on each operation, which is why
`INCR` on a 20-digit value is still O(1) but `INCR` on `"hello"` is an error
rather than a coercion.

---

## Keys

| Command | Notes |
|---|---|
| `DEL key [key...]` / `UNLINK key [key...]` | `UNLINK` is an honest alias — with one thread there is nothing to defer |
| `EXISTS key [key...]` | Counts repeats: `EXISTS k k` on a present key returns 2 |
| `TYPE key` | |
| `EXPIRE key seconds [NX\|XX\|GT\|LT]` | |
| `PEXPIRE key ms [NX\|XX\|GT\|LT]` | |
| `EXPIREAT key ts` / `PEXPIREAT key ms-ts` | |
| `TTL key` / `PTTL key` | **-1** = no TTL, **-2** = no such key |
| `PERSIST key` | Remove the TTL |
| `KEYS pattern` | Walks the whole keyspace and blocks the server — use `SCAN` |
| `SCAN cursor [MATCH p] [COUNT n] [TYPE t]` | `COUNT` is a hint about work, not result size |
| `RANDOMKEY` | |
| `DBSIZE` | |
| `RENAME key newkey` / `RENAMENX key newkey` | The TTL travels with the value |
| `COPY src dst [REPLACE]` | A genuine deep copy |
| `MOVE key db` | |
| `TOUCH key [key...]` | Refreshes LRU/LFU metadata without returning the value |
| `OBJECT ENCODING\|REFCOUNT\|IDLETIME\|FREQ key` | `ENCODING` reports the real type rather than inventing a physical encoding |

`EXPIRE` with a past deadline deletes the key immediately, which is why
`EXPIRE k -1` is a documented way to delete something.

All expiry commands propagate as absolute `PEXPIREAT`. A relative expiry
replayed from the log would be re-anchored to that moment, extending every
key's life on each restart.

---

## Lists

Backed by a deque, so both ends are O(1) and `LINDEX` is O(1).

| Command | Notes |
|---|---|
| `LPUSH key v [v...]` / `RPUSH key v [v...]` | |
| `LPUSHX key v [v...]` / `RPUSHX key v [v...]` | Only extend an existing list |
| `LPOP key [count]` / `RPOP key [count]` | `count` changes the reply from a bulk string to an array |
| `LLEN key` | |
| `LRANGE key start stop` | |
| `LINDEX key index` | |
| `LSET key index value` | |
| `LREM key count value` | `count` > 0 from the head, < 0 from the tail, 0 removes all |
| `LTRIM key start stop` | An empty range deletes the key |
| `LINSERT key BEFORE\|AFTER pivot value` | Returns -1 when the pivot is absent (0 means no such key) |
| `LPOS key element [RANK r] [COUNT n] [MAXLEN m]` | |
| `LMOVE src dst LEFT\|RIGHT LEFT\|RIGHT` | `src == dst` works, and is the idiomatic list rotation |
| `RPOPLPUSH src dst` | |

---

## Hashes

| Command | Notes |
|---|---|
| `HSET key field value [field value...]` | Returns fields *created*, not written |
| `HMSET key field value [...]` | Deprecated twin of `HSET`; replies `+OK` |
| `HSETNX key field value` | |
| `HGET key field` / `HMGET key field [field...]` | |
| `HDEL key field [field...]` | |
| `HEXISTS key field` | |
| `HLEN key` / `HSTRLEN key field` | |
| `HKEYS key` / `HVALS key` / `HGETALL key` | |
| `HINCRBY key field n` / `HINCRBYFLOAT key field n` | |
| `HRANDFIELD key [count [WITHVALUES]]` | A negative count allows repeats |

A hash is what you want when a value has fields read and written
independently. Serialising a struct into a string forces a read-modify-write of
the whole value for every field update, and makes concurrent updates to
different fields clobber each other.

---

## Sets

| Command | Notes |
|---|---|
| `SADD key m [m...]` / `SREM key m [m...]` | |
| `SMEMBERS key` | |
| `SISMEMBER key m` / `SMISMEMBER key m [m...]` | |
| `SCARD key` | |
| `SPOP key [count]` | Propagates as `SREM` naming the members actually removed |
| `SRANDMEMBER key [count]` | A negative count allows repeats |
| `SMOVE src dst member` | |
| `SINTER` / `SUNION` / `SDIFF` `key [key...]` | |
| `SINTERSTORE` / `SUNIONSTORE` / `SDIFFSTORE` `dst key [key...]` | Storing an empty result deletes the destination |

`SINTER` starts from the smallest operand, so the work is bounded by the
smallest set rather than by whichever the client named first.

---

## Sorted sets

| Command | Notes |
|---|---|
| `ZADD key [NX\|XX] [GT\|LT] [CH] [INCR] score member [...]` | Every score is validated before anything is written |
| `ZREM key member [member...]` | |
| `ZSCORE key member` / `ZMSCORE key member [member...]` | |
| `ZINCRBY key increment member` | |
| `ZCARD key` / `ZCOUNT key min max` | |
| `ZRANGE key start stop [WITHSCORES]` | |
| `ZREVRANGE key start stop [WITHSCORES]` | |
| `ZRANGEBYSCORE key min max [WITHSCORES] [LIMIT off cnt]` | `(5` is exclusive; `-inf`/`+inf` are accepted |
| `ZREVRANGEBYSCORE key max min [WITHSCORES] [LIMIT off cnt]` | Bounds are in the opposite order |
| `ZRANK key member` / `ZREVRANK key member` | **O(n)** here, not O(log n) — see below |
| `ZREMRANGEBYSCORE key min max` | |
| `ZREMRANGEBYRANK key start stop` | |
| `ZPOPMIN key [count]` / `ZPOPMAX key [count]` | Propagates as `ZREM` |

Ties are broken lexicographically by member, which is what makes ordering
deterministic.

**`ZRANK` is the one place MiniRedis is asymptotically worse than Redis.**
Redis's skip list carries span counters that make rank a O(log n) lookup;
a balanced tree has none, so the rank scan is linear. Documented rather than
hidden.

---

## Expiry, transactions and pub/sub

### Transactions

| Command | Notes |
|---|---|
| `MULTI` | Cannot nest |
| `EXEC` | Returns `nil` if a watched key changed |
| `DISCARD` | Also releases every `WATCH` |
| `WATCH key [key...]` | Not allowed inside `MULTI` |
| `UNWATCH` | |

A Redis transaction gives **isolation** and **all-or-nothing dispatch**. It does
**not** give rollback: if the third of five commands fails at runtime, the first
two stay applied and the last two still run.

`WATCH` is a flag, not a value comparison — a key written and written back to
its original value still aborts, which is what catches the ABA problem.

A command that fails to queue poisons the batch, and `EXEC` then replies
`EXECABORT` rather than running a sequence with one operation missing.

### Pub/Sub

| Command | Notes |
|---|---|
| `SUBSCRIBE ch [ch...]` / `UNSUBSCRIBE [ch...]` | |
| `PSUBSCRIBE pat [pat...]` / `PUNSUBSCRIBE [pat...]` | Every pattern is tested against every publish |
| `PUBLISH channel message` | Forwarded to replicas, but never written to the AOF |
| `PUBSUB CHANNELS\|NUMSUB\|NUMPAT` | |

While subscribed, only the pub/sub commands, `PING`, `QUIT` and `RESET` are
allowed — the connection is in push mode and an ordinary reply would be
indistinguishable from a delivered message.

Delivery is fire-and-forget. A message published while a subscriber is away is
gone; there is no queue and no replay.

---

## Connection

| Command | Notes |
|---|---|
| `PING [message]` | Replies in push shape while subscribed |
| `ECHO message` | |
| `AUTH [username] password` | Only the `default` username is accepted |
| `SELECT index` | Not allowed in cluster mode |
| `SWAPDB a b` | Atomically promotes a dataset rebuilt in another database |
| `QUIT` | |
| `HELLO [protover]` | **RESP3 is explicitly rejected**, rather than accepted and then unsupported |
| `CLIENT ID\|GETNAME\|SETNAME\|LIST\|KILL\|HELP` | |
| `RESET` | Returns the connection to its just-opened state |

---

## Server

| Command | Notes |
|---|---|
| `INFO [section]` | `server`, `clients`, `memory`, `persistence`, `stats`, `replication`, `cluster`, `keyspace`, `all` |
| `CONFIG GET pattern [pattern...]` | Glob matched against directive names |
| `CONFIG SET name value [value...]` | Shares one implementation with the config file loader |
| `CONFIG RESETSTAT` | |
| `DBSIZE` / `FLUSHDB` / `FLUSHALL` | |
| `COMMAND [COUNT\|INFO\|DOCS]` | |
| `SLOWLOG GET [n]\|LEN\|RESET` | |
| `MONITOR` | The connection then only receives the command feed |
| `TIME` | |
| `SHUTDOWN [SAVE\|NOSAVE]` | Refuses to exit if a requested save fails |
| `DEBUG SLEEP s\|OBJECT key\|HELP` | `SLEEP` blocks the whole server, deliberately |
| `MEMORY USAGE key\|DOCTOR\|STATS` | `DOCTOR` gives plain-language advice |
| `LOLWUT` | |

`INFO memory` reports `used_memory_percent_of_max`, which is the number that
actually predicts whether the next write gets an `OOM`.

---

## Persistence

| Command | Notes |
|---|---|
| `SAVE` | Blocks the server for the duration |
| `BGSAVE` | **Synchronous, and the reply says so** — `fork()` does not exist on Windows |
| `BGREWRITEAOF` | Rewrites the log as the shortest sequence reproducing the dataset |
| `LASTSAVE` | |

---

## Replication

| Command | Notes |
|---|---|
| `REPLICAOF host port` / `REPLICAOF NO ONE` | `SLAVEOF` is accepted as an alias |
| `PSYNC replid offset` | `? -1` requests a full transfer |
| `REPLCONF listening-port\|ACK\|GETACK` | |
| `WAIT numreplicas timeout` | **Answers immediately** rather than blocking the server thread |
| `FAILOVER` | Not implemented; the error explains why |

A read-only replica refuses writes with `READONLY`. A promoted replica
generates a new replication id, because its old offsets belonged to the
previous master's timeline.

`FAILOVER` is deliberately absent: coordinated failover needs consensus, and
doing it without agreement produces two masters and a split brain. Promote
explicitly with `REPLICAOF NO ONE` after confirming the old master is down.

---

## Cluster

| Command | Notes |
|---|---|
| `CLUSTER INFO` | State and slot coverage |
| `CLUSTER MYID` / `CLUSTER NODES` | |
| `CLUSTER SLOTS` / `CLUSTER SHARDS` | What a cluster-aware client reads once to build its routing table |
| `CLUSTER KEYSLOT key` | Works even with cluster mode off |
| `CLUSTER COUNTKEYSINSLOT slot` | |
| `CLUSTER GETKEYSINSLOT slot count` | |
| `CLUSTER MEET id host port` | |
| `CLUSTER ADDSLOTSRANGE id start end` | |
| `CLUSTER FORGET id` / `CLUSTER RESET` | |

`CLUSTER KEYSLOT foo` returns **12182**, matching real Redis exactly, so
cluster-aware clients route correctly against MiniRedis.

A key outside this node's slots returns `-MOVED <slot> <host:port>`; keys
spanning slots return `-CROSSSLOT`. Hash tags — the `{user:42}` in
`{user:42}:profile` — force related keys into one slot on purpose.

---

## SQL

| Command |
|---|
| `SQL <statement>` |

```sql
SELECT * FROM kv [LIMIT n]
SELECT value FROM kv WHERE key = 'k'
INSERT INTO kv [(key, value)] VALUES ('k', 'v')
UPDATE kv SET value = 'v' WHERE key = 'k'
DELETE FROM kv WHERE key = 'k'
SHOW TABLES
DESCRIBE kv
```

The keyspace underneath is a hash map: no schema, no joins, no secondary
index. So `WHERE key = 'x'` is a hash lookup, and `WHERE value = 'x'` is
**refused with an explanation** rather than quietly becoming a full scan.

`DELETE` without a `WHERE` clause is also refused — `FLUSHDB` exists and says
what it does.

Statements propagate as their equivalent `SET` or `DEL`, so a replica needs no
SQL parser and the log stays one uniform format.

---

## Not implemented

Lua scripting (`EVAL`), Streams (`XADD`), HyperLogLog (`PFADD`), geospatial
(`GEOADD`), bitfields (`SETBIT`, `BITCOUNT`), modules, ACL users, `OBJECT
HELP` physical encodings, blocking list operations (`BLPOP`), and `CLUSTER`
live resharding.
