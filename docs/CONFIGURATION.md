# Configuration

Every directive works three ways, and they share one implementation so they
cannot drift into accepting different syntax for the same setting:

```bash
miniredis-server --maxmemory 512mb           # command line
```
```conf
maxmemory 512mb                              # config file
```
```bash
CONFIG SET maxmemory 512mb                   # at runtime
```

```bash
miniredis-server miniredis.conf --port 6390  # a file, with overrides after it
```

Inspect what a running server is using:

```bash
CONFIG GET maxmemory*
CONFIG GET *                # every directive
```

---

## Networking

| Directive | Default | Notes |
|---|---|---|
| `bind` | `127.0.0.1` | `0.0.0.0` listens on every interface. **The loopback default is deliberate** — a database reachable from the network without a password is how data leaks. |
| `port` | `6380` | Not 6379, so MiniRedis can run alongside a real Redis |
| `tcp-backlog` | `511` | Pending-connection queue depth |
| `timeout` | `0` | Seconds before an idle client is dropped; `0` disables. Replicas and subscribers are exempt, since they are legitimately idle. |
| `maxclients` | `10000` | Further connections are refused with an error rather than dropped silently |
| `nodeid` | `miniredis` | Name used in logs, `INFO` and the cluster config |

---

## Storage

| Directive | Default | Notes |
|---|---|---|
| `dir` | `.` | Working directory for the snapshot, log and cluster config |
| `databases` | `16` | Logical databases reachable with `SELECT` |
| `load-on-startup` | `yes` | Load a dataset on boot |

---

## Security

| Directive | Default | Notes |
|---|---|---|
| `requirepass` | *(none)* | Requires `AUTH` before any command except `PING`, `AUTH`, `HELLO`, `QUIT` and `RESET` |
| `masterauth` | *(none)* | Password a replica presents to its master |

`CONFIG GET requirepass` returns `<hidden>` rather than the password. Real
Redis returns the value; printing a secret into a client's terminal and the
slow log is a poor default.

---

## Memory

| Directive | Default | Notes |
|---|---|---|
| `maxmemory` | `0` | `0` means unlimited. Accepts `512mb`, `1gb`, or raw bytes. |
| `maxmemory-policy` | `noeviction` | See below |
| `maxmemory-samples` | `5` | Keys sampled per eviction round |

### Policies

| Policy | Evicts |
|---|---|
| `noeviction` | Nothing — writes are refused with `OOM` |
| `allkeys-lru` | Least recently used, across every key |
| `allkeys-lfu` | Least frequently used, across every key |
| `allkeys-random` | Any key |
| `volatile-lru` | Least recently used, **among keys with a TTL** |
| `volatile-lfu` | Least frequently used, among keys with a TTL |
| `volatile-random` | Any key with a TTL |
| `volatile-ttl` | Whichever key expires soonest |

**Choosing between them is a question about what the data is:**

- A **cache** — everything is reconstructible from the source of truth — wants
  `allkeys-lru` or `allkeys-lfu`. Refusing writes would be worse than dropping
  a value that can be fetched again.
- A **datastore** wants `noeviction`. Silently deleting data is far worse than
  an error the client can see and retry.
- A **mixed** workload puts a TTL on the disposable keys and uses a `volatile-*`
  policy, so the durable keys are never candidates.

`maxmemory-samples` trades accuracy for CPU. Sampling 5 lands within a few
percent of exact LRU; raising it to 10 closes most of the remaining gap.
Exact LRU would need two pointers on every key, which for millions of small
values is a large fraction of total memory spent on bookkeeping.

**With `maxmemory` unset, the server will grow until the machine runs out.**
`MEMORY DOCTOR` says so.

---

## Persistence

### Append-only log

| Directive | Default | Notes |
|---|---|---|
| `appendonly` | `no` | |
| `appendfilename` | `miniredis.aof` | |
| `appendfsync` | `everysec` | `always`, `everysec` or `no` |
| `auto-aof-rewrite-percentage` | `100` | Rewrite when the file has grown this much since the last one |
| `auto-aof-rewrite-min-size` | `64mb` | Never rewrite below this |

`appendfsync` is the durability dial. `write()` only reaches the OS page cache;
a power cut loses everything still sitting in it.

| Value | Data at risk | Cost |
|---|---|---|
| `always` | nothing acknowledged | a disk round trip per event loop iteration |
| `everysec` | up to 1 second | negligible — the knee of the curve |
| `no` | whatever the OS hasn't flushed | none |

Enabling `appendonly` at runtime writes the existing dataset out first, so the
log is complete rather than recording only future writes.

### Snapshots

| Directive | Default | Notes |
|---|---|---|
| `dbfilename` | `miniredis.rdb` | |
| `save` | `900 1`, `300 100`, `60 10000` | `<seconds> <changes>` pairs |

`save 900 1` means: snapshot if at least 1 key changed in the last 900 seconds.
Repeated `save` lines accumulate; a bare `save` with no arguments disables
snapshotting entirely, which is what a deployment relying solely on the log
wants.

### Which to use

|  | Log | Snapshot |
|---|---|---|
| Data at risk | one fsync interval | one save interval |
| File size | grows with writes | proportional to the dataset |
| Restart | replay every command | one sequential read |

A real deployment runs both: the log for durability, the snapshot for
restart time and backups. When both exist, the log wins on load, because it is
the more recent record.

---

## Replication

| Directive | Default | Notes |
|---|---|---|
| `replicaof` | *(none)* | `<host> <port>`, or `NO ONE` |
| `replica-read-only` | `yes` | |
| `repl-timeout` | `60` | Seconds without data before the link is dropped |
| `repl-backlog-size` | `1mb` | Bytes of recent stream kept for partial resync |

**`repl-backlog-size` directly buys reconnect resilience.** A replica that
reconnects with its offset still inside the backlog receives only the bytes it
missed; otherwise it needs a full dataset transfer. At 100 KB/s of writes, 1 MB
covers a ten-second outage.

Leaving `replica-read-only` on is strongly advised. A write accepted on a
replica diverges it from its master, and the next resynchronisation silently
discards it.

---

## Cluster

| Directive | Default | Notes |
|---|---|---|
| `cluster-enabled` | `no` | |
| `cluster-config-file` | `nodes.conf` | Written automatically if absent |
| `cluster-announce-ip` | *(none)* | Address advertised to clients, for NAT |
| `cluster-announce-port` | `0` | |

With cluster mode on and no config file, the node claims all 16384 slots and
persists that, so its identity survives a restart. `SELECT` is refused, because
multiple databases have no meaning once keys are distributed by hash slot.

---

## Observability

| Directive | Default | Notes |
|---|---|---|
| `loglevel` | `notice` | `debug`, `verbose`, `notice`, `warning` |
| `logfile` | *(stdout)* | |
| `slowlog-log-slower-than` | `10000` | Microseconds; `-1` disables |
| `slowlog-max-len` | `128` | Entries retained |

The slow log matters more here than on a threaded server: one slow command
blocks every client, so finding it is the difference between a diagnosis and a
guess. Logged arguments are truncated, so one `MSET` with a hundred thousand
arguments cannot pin that much memory indefinitely.

---

## Example configurations

### Cache

Disposable data, so evict rather than refuse, and skip persistence entirely.

```conf
port 6380
maxmemory 4gb
maxmemory-policy allkeys-lru
maxmemory-samples 10

save
appendonly no
```

### Datastore

Durable data, so refuse rather than evict, and fsync every write.

```conf
port 6380
dir /var/lib/miniredis

maxmemory 8gb
maxmemory-policy noeviction

appendonly yes
appendfsync always
auto-aof-rewrite-percentage 100
auto-aof-rewrite-min-size 64mb

save 900 1
save 300 100

requirepass change-this
bind 127.0.0.1
```

### Replica

```conf
port 6381
replicaof 127.0.0.1 6380
replica-read-only yes
masterauth change-this

repl-backlog-size 8mb
repl-timeout 60
```

### Cluster node

```conf
port 6380
cluster-enabled yes
cluster-config-file nodes-6380.conf
nodeid node-a
```

---

## Runtime changes

Most directives take effect immediately via `CONFIG SET`. Two do more than
assign a field:

- `appendonly yes` opens the log **and rewrites the current dataset into it**,
  so the file is complete rather than recording only writes from that moment on.
- `appendonly no` flushes and closes it cleanly.

`port`, `bind` and `databases` are read at startup and require a restart.

`CONFIG REWRITE` is not supported — MiniRedis does not rewrite its own config
file, and says so rather than silently doing nothing.
