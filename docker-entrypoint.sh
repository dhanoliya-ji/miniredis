#!/bin/sh
# Start the database, wait for it to accept connections, then hand the
# foreground to the web bridge so the container's lifetime follows it.
set -e

miniredis-server /app/miniredis.conf --bind 127.0.0.1 --port "${MINIREDIS_PORT:-6380}" --dir /app/data &
DB_PID=$!

trap 'kill "$DB_PID" 2>/dev/null || true' TERM INT

i=0
while [ "$i" -lt 50 ]; do
  if node -e "require('net').createConnection({host:'127.0.0.1',port:${MINIREDIS_PORT:-6380}}).on('connect',()=>process.exit(0)).on('error',()=>process.exit(1))" 2>/dev/null; then
    echo "[entrypoint] miniredis is accepting connections"
    break
  fi
  i=$((i + 1))
  sleep 0.2
done

cd /app/web
exec node server.js
