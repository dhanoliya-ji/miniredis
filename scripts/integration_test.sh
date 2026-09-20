#!/usr/bin/env bash
# End-to-end integration tests for MiniRedis (Linux / macOS).
#
# The C++ suite in tests/ covers the pieces in isolation. This script covers
# what only shows up when real processes talk over real sockets: crash recovery
# from the append-only file, a replica synchronising with a master, read-only
# enforcement, eviction under a memory limit, and transaction aborts.
#
#   ./scripts/integration_test.sh
#   BASE_PORT=7400 KEEP_LOGS=1 ./scripts/integration_test.sh

set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

server="$root/bin/miniredis-server"
cli="$root/bin/miniredis-cli"
base_port="${BASE_PORT:-7350}"

for binary in "$server" "$cli"; do
    if [ ! -x "$binary" ]; then
        printf '\033[31mMissing %s - run ./scripts/build.sh first\033[0m\n' "$binary" >&2
        exit 1
    fi
done

work_dir="$(mktemp -d "${TMPDIR:-/tmp}/miniredis-integration.XXXXXX")"
passed=0
failed=0
failures=()
pids=()

start_node() {
    local name="$1" port="$2"
    shift 2

    local data_dir="$work_dir/$name"
    mkdir -p "$data_dir"

    # A bare --save with no value disables scheduled snapshots, so the tests
    # measure the data path rather than the disk.
    "$server" --port "$port" --dir "$data_dir" --nodeid "$name" "$@" --save \
        >"$work_dir/$name.log" 2>&1 &
    local pid=$!
    pids+=("$pid")

    # Wait for the port to answer rather than sleeping a fixed amount: a fixed
    # sleep is either slow or flaky, and usually both on a loaded machine.
    for _ in $(seq 1 100); do
        sleep 0.1
        if "$cli" -p "$port" PING 2>/dev/null | grep -q PONG; then
            echo "$pid"
            return 0
        fi
    done

    printf '\033[31m  node %s did not come up on port %s\033[0m\n' "$name" "$port" >&2
    tail -20 "$work_dir/$name.log" >&2 || true
    return 1
}

stop_node() {
    local pid="${1:-}"
    [ -n "$pid" ] || return 0
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

run() {
    local port="$1"
    shift
    "$cli" -p "$port" "$@" 2>&1
}

check() {
    local what="$1" actual="$2" expected="$3"
    if printf '%s' "$actual" | grep -qF -- "$expected"; then
        printf '\033[32m  [ ok ] %s\033[0m\n' "$what"
        passed=$((passed + 1))
    else
        printf '\033[31m  [FAIL] %s\033[0m\n' "$what"
        printf '         expected to contain: %s\n' "$expected"
        printf '         actual:              %s\n' "$actual"
        failed=$((failed + 1))
        failures+=("$what")
    fi
}

check_regex() {
    local what="$1" actual="$2" pattern="$3"
    if printf '%s' "$actual" | grep -qE -- "$pattern"; then
        printf '\033[32m  [ ok ] %s\033[0m\n' "$what"
        passed=$((passed + 1))
    else
        printf '\033[31m  [FAIL] %s\033[0m\n' "$what"
        printf '         expected to match: %s\n' "$pattern"
        printf '         actual:            %s\n' "$actual"
        failed=$((failed + 1))
        failures+=("$what")
    fi
}

cleanup() {
    for pid in "${pids[@]:-}"; do stop_node "$pid"; done
    if [ -z "${KEEP_LOGS:-}" ]; then
        rm -rf "$work_dir"
    else
        printf '\nLogs kept in %s\n' "$work_dir"
    fi
}
trap cleanup EXIT

printf '\033[36mMiniRedis integration tests\033[0m\n'
printf '  working directory: %s\n\n' "$work_dir"

# ---------------------------------------------------------------------------
printf '\033[33mCore data types\033[0m\n'
port=$base_port
node=$(start_node core "$port") || exit 1

check "PING answers PONG" "$(run "$port" PING)" "PONG"
run "$port" SET greeting hello >/dev/null
check "SET then GET returns the value" "$(run "$port" GET greeting)" "hello"
check "GET of a missing key is nil" "$(run "$port" GET nothing)" "nil"

run "$port" RPUSH q a b c >/dev/null
check "LLEN counts list elements" "$(run "$port" LLEN q)" "3"
check "LPOP removes from the head" "$(run "$port" LPOP q)" "a"

run "$port" HSET user name alice age 30 >/dev/null
check "HGET reads a field" "$(run "$port" HGET user name)" "alice"

run "$port" ZADD board 100 alice 90 bob >/dev/null
check "ZRANGE orders by score" "$(run "$port" ZRANGE board 0 0)" "bob"
check "ZSCORE reads a score" "$(run "$port" ZSCORE board alice)" "100"

check "wrong type is refused" "$(run "$port" LPUSH greeting x)" "WRONGTYPE"
check "unknown command is refused" "$(run "$port" NOTACOMMAND)" "unknown command"
check "wrong arity is refused" "$(run "$port" GET)" "wrong number of arguments"

# ---------------------------------------------------------------------------
printf '\n\033[33mExpiry\033[0m\n'
run "$port" SET temp v PX 300 >/dev/null
check_regex "TTL is reported before expiry" "$(run "$port" PTTL temp)" '\(integer\) [0-9]+'
sleep 0.6
check "the key is gone after its TTL" "$(run "$port" GET temp)" "nil"
check "TTL of a missing key is -2" "$(run "$port" TTL temp)" "-2"

run "$port" SET keeper v >/dev/null
run "$port" EXPIRE keeper 100 >/dev/null
run "$port" PERSIST keeper >/dev/null
check "PERSIST clears the TTL" "$(run "$port" TTL keeper)" "-1"

# ---------------------------------------------------------------------------
printf '\n\033[33mTransactions\033[0m\n'
multi_output=$(printf 'MULTI\nSET tx 1\nINCR tx\nEXEC\nGET tx\nexit\n' | "$cli" -p "$port" 2>&1)
check "queued commands run on EXEC" "$multi_output" '"2"'

abort_output=$(printf 'MULTI\nNOSUCHCOMMAND x\nEXEC\nexit\n' | "$cli" -p "$port" 2>&1)
check "a bad command aborts the transaction" "$abort_output" "EXECABORT"

stop_node "$node"

# ---------------------------------------------------------------------------
printf '\n\033[33mCrash recovery from the append-only file\033[0m\n'
port=$((base_port + 1))
node=$(start_node aof "$port" --appendonly yes --appendfsync always) || exit 1

run "$port" SET durable survives >/dev/null
run "$port" RPUSH items x y z >/dev/null
run "$port" HSET profile city pune >/dev/null
run "$port" ZADD ranks 5 first >/dev/null
run "$port" SET expiring v EX 3600 >/dev/null

# Kill rather than shut down: this has to look like a crash, not a clean exit,
# or the test proves nothing about recovery.
kill -9 "$node" 2>/dev/null || true
wait "$node" 2>/dev/null || true
sleep 0.3

node=$(start_node aof "$port" --appendonly yes) || exit 1
check "a string survives a crash" "$(run "$port" GET durable)" "survives"
check "a list survives a crash" "$(run "$port" LLEN items)" "3"
check "a hash survives a crash" "$(run "$port" HGET profile city)" "pune"
check "a sorted set survives a crash" "$(run "$port" ZSCORE ranks first)" "5"
check_regex "a TTL survives a crash" "$(run "$port" TTL expiring)" '\(integer\) 3[0-9]{3}'

run "$port" BGREWRITEAOF >/dev/null
check "the dataset survives an AOF rewrite" "$(run "$port" GET durable)" "survives"
stop_node "$node"

# ---------------------------------------------------------------------------
printf '\n\033[33mSnapshot persistence\033[0m\n'
port=$((base_port + 2))
node=$(start_node rdb "$port") || exit 1

run "$port" SET snapshotted yes >/dev/null
run "$port" SAVE >/dev/null
stop_node "$node"
sleep 0.3

node=$(start_node rdb "$port") || exit 1
check "the snapshot reloads on startup" "$(run "$port" GET snapshotted)" "yes"
stop_node "$node"

# ---------------------------------------------------------------------------
printf '\n\033[33mReplication\033[0m\n'
master_port=$((base_port + 3))
replica_port=$((base_port + 4))

master=$(start_node master "$master_port") || exit 1
run "$master_port" SET before-replica existing >/dev/null

replica=$(start_node replica "$replica_port" --replicaof 127.0.0.1 "$master_port") || exit 1

synced="not synced"
for _ in $(seq 1 60); do
    sleep 0.2
    if run "$replica_port" GET before-replica | grep -q existing; then
        synced="synced"
        break
    fi
done
check "the replica receives the existing dataset" "$synced" "synced"

run "$master_port" SET after-replica streamed >/dev/null
streamed="not streamed"
for _ in $(seq 1 60); do
    sleep 0.1
    if run "$replica_port" GET after-replica | grep -q streamed; then
        streamed="streamed"
        break
    fi
done
check "later writes stream to the replica" "$streamed" "streamed"

check "the replica refuses writes" "$(run "$replica_port" SET nope x)" "READONLY"
check "the replica still serves reads" "$(run "$replica_port" GET after-replica)" "streamed"
check "the master reports its replica" "$(run "$master_port" INFO replication)" "connected_slaves:1"
check "the replica reports its role" "$(run "$replica_port" INFO replication)" "role:slave"

run "$master_port" DEL after-replica >/dev/null
deleted="still present"
for _ in $(seq 1 60); do
    sleep 0.1
    if run "$replica_port" GET after-replica | grep -q nil; then
        deleted="deleted"
        break
    fi
done
check "deletes propagate to the replica" "$deleted" "deleted"

run "$replica_port" REPLICAOF NO ONE >/dev/null
sleep 0.3
check "a promoted replica accepts writes" "$(run "$replica_port" SET promoted yes)" "OK"

stop_node "$replica"
stop_node "$master"

# ---------------------------------------------------------------------------
printf '\n\033[33mEviction under a memory limit\033[0m\n'
port=$((base_port + 5))
node=$(start_node evict "$port" --maxmemory 2mb --maxmemory-policy allkeys-lru) || exit 1

filler=$(printf 'x%.0s' $(seq 1 1024))
for i in $(seq 0 3999); do
    "$cli" -p "$port" SET "fill:$i" "$filler" >/dev/null 2>&1
done

info=$(run "$port" INFO stats)
check "keys were evicted to stay under maxmemory" "$info" "evicted_keys"
evicted=$(printf '%s' "$info" | grep -oE 'evicted_keys:[0-9]+' | cut -d: -f2)
check "the eviction count is non-zero" "$([ "${evicted:-0}" -gt 0 ] && echo evicted || echo none)" "evicted"
check "writes still succeed while evicting" "$(run "$port" SET still working)" "OK"

stop_node "$node"

# ---------------------------------------------------------------------------
printf '\n\033[33mNo eviction means writes are refused\033[0m\n'
port=$((base_port + 6))
node=$(start_node oom "$port" --maxmemory 1mb --maxmemory-policy noeviction) || exit 1

saw_oom="no OOM seen"
for i in $(seq 0 3999); do
    if "$cli" -p "$port" SET "fill:$i" "$filler" 2>&1 | grep -q OOM; then
        saw_oom="OOM"
        break
    fi
done
check "noeviction refuses writes with OOM" "$saw_oom" "OOM"
check_regex "reads still work when out of memory" "$(run "$port" DBSIZE)" '\(integer\) [0-9]+'

stop_node "$node"

# ---------------------------------------------------------------------------
printf '\n\033[33mSQL layer\033[0m\n'
port=$((base_port + 7))
node=$(start_node sql "$port") || exit 1

run "$port" SQL "INSERT INTO kv VALUES ('user:1', 'alice')" >/dev/null
check "SQL INSERT stores a value" "$(run "$port" GET user:1)" "alice"
check "SQL SELECT renders a table" "$(run "$port" SQL "SELECT * FROM kv")" "user:1"
run "$port" SQL "UPDATE kv SET value = 'alicia' WHERE key = 'user:1'" >/dev/null
check "SQL UPDATE changes the value" "$(run "$port" GET user:1)" "alicia"
run "$port" SQL "DELETE FROM kv WHERE key = 'user:1'" >/dev/null
check "SQL DELETE removes the key" "$(run "$port" GET user:1)" "nil"
check "an unqualified DELETE is refused" "$(run "$port" SQL "DELETE FROM kv")" "WHERE"

stop_node "$node"

# ---------------------------------------------------------------------------
printf '\n\033[33mCluster mode\033[0m\n'
port=$((base_port + 8))
node=$(start_node cluster "$port" --cluster-enabled yes) || exit 1

check "CLUSTER INFO reports full slot coverage" "$(run "$port" CLUSTER INFO)" "cluster_slots_assigned:16384"
check "CLUSTER KEYSLOT matches Redis for 'foo'" "$(run "$port" CLUSTER KEYSLOT foo)" "12182"
check "a single node serves its own slots" "$(run "$port" SET clustered yes)" "OK"
tagged_a=$(run "$port" CLUSTER KEYSLOT '{u}:a')
tagged_b=$(run "$port" CLUSTER KEYSLOT '{u}:b')
check "hash tags co-locate keys" "$tagged_a" "$tagged_b"

stop_node "$node"

# ---------------------------------------------------------------------------
if [ "$failed" -eq 0 ]; then
    printf '\n\033[32m%d passed, %d failed\033[0m\n' "$passed" "$failed"
else
    printf '\n\033[31m%d passed, %d failed\033[0m\n' "$passed" "$failed"
    printf '\n\033[31mFailing checks:\033[0m\n'
    for failure in "${failures[@]}"; do printf '\033[31m  %s\033[0m\n' "$failure"; done
fi

exit "$failed"
