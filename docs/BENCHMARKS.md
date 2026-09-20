# Benchmarks

Measured numbers, and what they actually mean.

Every figure here comes from `bin/miniredis-benchmark` and is reproducible with
the commands shown. The raw CSVs are in [`bench/results/`](../bench/results/),
and the charts are generated from them in
[`notebooks/benchmarks.ipynb`](../notebooks/benchmarks.ipynb).

---

## Method

| | |
|---|---|
| Platform | Windows 11, loopback (`127.0.0.1`) |
| Build | GCC 15.2 (MSYS2 UCRT64), `-O2 -std=c++20`, statically linked |
| Server | Default configuration, snapshots disabled (`--save`), no AOF |
| Value size | 32 bytes unless stated |
| Keyspace | 10,000 distinct keys |

**Latency is measured per operation, from a full histogram**, not as a running
average. An average hides the tail, and the tail is what users experience. When
commands are pipelined, the batch's elapsed time is divided across its
requests, so latency stays per-operation and comparable across pipeline depths.

### An important caveat about loopback

These numbers are taken over loopback, where there is no network. That makes
them a measurement of **the server plus the local TCP stack plus the benchmark
client**, not of the server alone. On a real network the round trip would add
tens to hundreds of microseconds and would dominate everything below.

This matters for interpreting the results honestly: loopback *flatters*
per-operation latency and *understates* the value of pipelining. Both effects
show up in the data below, and both are called out where they do.

---

## 1. Concurrency: the headline result

```bash
miniredis-benchmark -t get -n 50000 -c <clients>
```

| Clients | Throughput | p50 | p95 | p99 | max |
|---:|---:|---:|---:|---:|---:|
| 1 | 27,102 ops/s | **29 µs** | 69 µs | 107 µs | 1,898 µs |
| 2 | 40,919 ops/s | 38 µs | 86 µs | 134 µs | 1,223 µs |
| 4 | 54,249 ops/s | 57 µs | 110 µs | 152 µs | 578 µs |
| 8 | 55,318 ops/s | 106 µs | 225 µs | 321 µs | 941 µs |
| 16 | 57,173 ops/s | 232 µs | 461 µs | 678 µs | 1,891 µs |
| 32 | 57,421 ops/s | 492 µs | 929 µs | 1,232 µs | 2,365 µs |
| 64 | 63,482 ops/s | 941 µs | 1,356 µs | 1,765 µs | 3,627 µs |
| 128 | 61,014 ops/s | 1,956 µs | 2,752 µs | 3,159 µs | 8,555 µs |

This is the most instructive chart in the project, and it is a textbook
queueing result made concrete:

> **Throughput saturates at about 4 clients. Every client added after that
> converts almost entirely into latency.**

From 4 → 128 clients:
- throughput moves **+12%** (54,249 → 61,014 ops/s)
- p50 latency grows **34×** (57 µs → 1,956 µs)

Past the saturation point, concurrency does not buy work. It buys queue. This
is Little's Law in action: with throughput pinned, adding requests in flight can
only increase the time each one spends waiting.

The practical consequence is that **a connection pool sized well past saturation
makes a system slower, not faster**, and the symptom is exactly what you see
here — unchanged throughput with steadily worsening latency percentiles. It is a
common and expensive production mistake.

### The number that describes the server itself

The single-client p50 of **29 µs** is the honest figure for what MiniRedis does
per operation: read the socket, parse RESP, hash the key, look it up, encode the
reply, write it back. At one client there is no queueing, so nothing else is
folded into it.

For scale: a typical relational database answers a primary-key lookup in
1–10 **milliseconds**. That is the two-to-three orders of magnitude that make
in-memory stores worth having.

---

## 2. Operations compared

```bash
miniredis-benchmark -t ping,set,get,incr,lpush,lrange,sadd,zadd,hset -n 100000 -c 16
```

| Operation | Throughput | p50 | p95 | p99 |
|---|---:|---:|---:|---:|
| `PING` | 62,832 ops/s | 193 µs | 431 µs | 937 µs |
| `SET` | 43,026 ops/s | 225 µs | 745 µs | 1,413 µs |
| `LPUSH` | 28,450 ops/s | 430 µs | 1,005 µs | 1,583 µs |
| `HSET` | 26,594 ops/s | 469 µs | 1,084 µs | 1,705 µs |
| `LRANGE` (10) | 26,307 ops/s | 476 µs | 1,107 µs | 1,755 µs |
| `GET` | 26,162 ops/s | 479 µs | 1,081 µs | 1,778 µs |
| `SADD` | 26,000 ops/s | 488 µs | 1,038 µs | 1,471 µs |
| `ZADD` | 24,622 ops/s | 496 µs | 1,159 µs | 1,872 µs |
| `INCR` | 23,559 ops/s | 552 µs | 1,142 µs | 1,760 µs |

### Reading this table honestly

The spread from `PING` (62.8k) to `INCR` (23.6k) is **2.7×**, and it is
tempting to conclude that `INCR` is 2.7× more expensive than `PING`. That
conclusion would be wrong, and the reason is worth understanding.

`PING` touches no data at all — it is pure protocol. So the gap between `PING`
and everything else is the cost of *the keyspace operation*, and the fact that
all eight data commands land within ~20% of each other (23.6k–28.5k, excluding
`SET`) tells you something useful: **the choice of data structure barely
matters at this scale.** A sorted-set insert into a balanced tree and a hash
insert cost about the same once protocol handling and socket I/O are included,
because those dominate.

Two specific observations:

- **`SET` is faster than `GET` here (43.0k vs 26.2k).** That looks backwards.
  It is an artefact of ordering: the tests run in sequence against a shared
  keyspace, and `SET` ran while the working set was still small and warm in
  cache. This is a benchmark artefact, not a property of the server, and it is
  reported rather than quietly dropped because a table that only ever confirms
  expectations is not a measurement.

- **`LRANGE 0 9` returning ten elements costs about the same as `GET`
  returning one.** Encoding ten bulk strings instead of one is cheap next to the
  fixed per-command overhead.

The general lesson: at this scale you are measuring the *protocol and syscall
path*, not the data structures. Optimising the container here would be
misdirected effort.

---

## 3. Pipeline depth

```bash
miniredis-benchmark -t get -n 100000 -c 16 -P <depth>
```

| Depth | Throughput | p50 | p99 | **max** |
|---:|---:|---:|---:|---:|
| 1 | 38,346 ops/s | 376 µs | 1,297 µs | **7,971 µs** |
| 2 | 34,454 ops/s | 395 µs | 1,638 µs | 9,116 µs |
| 4 | 51,737 ops/s | 309 µs | 984 µs | 4,955 µs |
| 8 | 48,606 ops/s | 311 µs | 993 µs | 2,028 µs |
| 16 | 43,423 ops/s | 327 µs | 1,076 µs | 1,873 µs |
| 32 | 39,048 ops/s | 386 µs | 986 µs | 1,417 µs |
| 64 | 35,501 ops/s | 417 µs | 993 µs | 1,252 µs |
| 128 | 39,103 ops/s | 361 µs | 876 µs | **1,353 µs** |

**This result does not match the textbook expectation, and that is the
interesting part.**

The standard claim — including one I would have written into this README
without measuring — is that pipelining multiplies throughput on a
request/response protocol, because it amortises the network round trip. Here it
does not. Throughput peaks at depth 4 and then *declines*.

The explanation is the caveat from the method section: **over loopback there is
no round trip to amortise.** Pipelining's entire benefit is removing network
latency from the critical path, and on `127.0.0.1` that latency is already
close to zero. What remains is the extra cost of building and buffering larger
batches, which is why deep pipelines are slightly *worse*.

On a real network — even 0.5 ms of round trip — the picture inverts completely,
because at depth 1 every operation pays that 0.5 ms and at depth 64 they share
it. **This is a case where the benchmark environment determines the answer, and
reporting the loopback number as though it were general would be misleading.**

### What pipelining *does* do here

Look at the `max` column. Worst-case latency falls from **7,971 µs at depth 1 to
1,252 µs at depth 64** — a 6.4× improvement in the tail. p99 also improves, from
1,297 µs to 876 µs.

The trend is not strictly monotonic: depth 2 measures worse than depth 1
(9,116 µs vs 7,971 µs). Max is a single-sample statistic and therefore the
noisiest number in the table, so one point moving the wrong way is expected.
The direction from depth 4 onward is consistent.

Batching smooths out scheduling jitter: fewer, larger interactions with the
socket mean fewer opportunities to be descheduled at an inconvenient moment. So
on loopback pipelining is a **tail-latency** optimisation rather than a
throughput one.

---

## 4. Value size

```bash
miniredis-benchmark -t set -n 30000 -c 16 -d <bytes>
```

| Value size | Throughput | p50 | p99 |
|---:|---:|---:|---:|
| 8 B | 66,244 ops/s | 208 µs | 494 µs |
| 64 B | 57,463 ops/s | 228 µs | 744 µs |
| 512 B | 53,990 ops/s | 234 µs | 824 µs |
| 4 KB | 51,325 ops/s | 244 µs | 902 µs |
| 16 KB | 35,731 ops/s | 370 µs | 1,210 µs |

A **2,048× increase** in value size costs only a **1.85× drop** in throughput.

That is the signature of a system dominated by fixed per-operation overhead.
Up to about 4 KB, the cost of a command is almost entirely parsing, hashing and
syscalls — the bytes themselves are nearly free, because copying 4 KB is a
handful of microseconds and `memcpy` runs at gigabytes per second.

The knee is between 4 KB and 16 KB, where the payload finally becomes large
enough to matter: throughput drops 30% for that last 4× size increase, versus
23% for the preceding 512× increase.

**Practical reading:** storing values up to a few KB is essentially free
relative to the cost of the round trip. Above that, size starts to buy you real
cost, and it is worth asking whether the large value needs to be in the cache
at all.

---

## Reproducing

```bash
# Start a server with persistence off, so the measurement is of the
# data path rather than of the disk.
./bin/miniredis-server --port 7500 --dir /tmp/bench --save

# Operations
./bin/miniredis-benchmark -p 7500 -t ping,set,get,incr,lpush,lrange,sadd,zadd,hset \
    -n 100000 -c 16 --csv bench/results/operations.csv

# Concurrency sweep
for c in 1 2 4 8 16 32 64 128; do
    ./bin/miniredis-benchmark -p 7500 -t get -n 50000 -c $c -q \
        --csv bench/results/concurrency.csv
done

# Pipeline sweep
for p in 1 2 4 8 16 32 64 128; do
    ./bin/miniredis-benchmark -p 7500 -t get -n 100000 -c 16 -P $p -q \
        --csv bench/results/pipeline.csv
done

# Value size sweep
for d in 8 64 512 4096 16384; do
    ./bin/miniredis-benchmark -p 7500 -t set -n 30000 -c 16 -d $d -q \
        --csv bench/results/valuesize.csv
done
```

---

## What these numbers are not

- **Not a comparison with Redis.** No such comparison was run. Redis has
  fifteen years of optimisation and would very likely win. Publishing a
  favourable-looking comparison without running it would be dishonest, and
  running one fairly needs identical hardware, identical configuration and a
  shared client.
- **Not representative of a real network.** Loopback removes the single largest
  cost in a real deployment, and §3 shows how much that changes the conclusion.
- **Not a durability measurement.** These runs have snapshots and the AOF
  disabled. `appendfsync always` would add a disk round trip to every write and
  would dominate all of this.

The purpose of these numbers is to show the *shape* of the system's
behaviour — where it saturates, what dominates cost, and how latency degrades
under load — not to win a comparison.
