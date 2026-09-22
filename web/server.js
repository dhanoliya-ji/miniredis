// Web console for miniredis.
//
// Serves a browser terminal and bridges it to the running miniredis server over
// a real TCP connection speaking RESP2. Each browser tab gets its own socket,
// so `SELECT`, `MULTI` and `SUBSCRIBE` behave per-session exactly as they would
// from redis-cli.
//
//   PORT           port for this web server        (default 8080)
//   MINIREDIS_HOST miniredis host                  (default 127.0.0.1)
//   MINIREDIS_PORT miniredis port                  (default 6380)

import http from 'node:http'
import net from 'node:net'
import { readFile } from 'node:fs/promises'
import { fileURLToPath } from 'node:url'
import { dirname, join, normalize } from 'node:path'
import { WebSocketServer } from 'ws'
import { encodeCommand, tokenize, RespReader } from './resp.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const PORT = Number(process.env.PORT ?? 8080)
const DB_HOST = process.env.MINIREDIS_HOST ?? '127.0.0.1'
const DB_PORT = Number(process.env.MINIREDIS_PORT ?? 6380)

// This console is public, so a few commands are refused at the bridge. They are
// the ones that would take the demo down or reconfigure it for everyone else;
// everything that demonstrates the database is allowed.
const BLOCKED = new Set(['SHUTDOWN', 'DEBUG', 'REPLICAOF', 'SLAVEOF', 'MONITOR'])
const BLOCKED_SUBCOMMANDS = { CONFIG: new Set(['SET', 'REWRITE']) }

const MAX_COMMAND_BYTES = 4096
const MAX_REPLY_ITEMS = 500

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.svg': 'image/svg+xml',
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, 'http://localhost')

  if (url.pathname === '/healthz') {
    res.writeHead(200, { 'content-type': 'application/json' })
    res.end(JSON.stringify({ ok: true }))
    return
  }

  // Static files, confined to public/.
  const rel = url.pathname === '/' ? '/index.html' : url.pathname
  const path = join(HERE, 'public', normalize(rel).replace(/^(\.\.[/\\])+/, ''))
  if (!path.startsWith(join(HERE, 'public'))) {
    res.writeHead(403).end('forbidden')
    return
  }
  try {
    const body = await readFile(path)
    const ext = path.slice(path.lastIndexOf('.'))
    res.writeHead(200, { 'content-type': MIME[ext] ?? 'application/octet-stream' })
    res.end(body)
  } catch {
    res.writeHead(404, { 'content-type': 'text/plain' }).end('not found')
  }
})

const wss = new WebSocketServer({ server })

wss.on('connection', (ws) => {
  const sock = net.createConnection({ host: DB_HOST, port: DB_PORT })
  const reader = new RespReader()
  let closed = false

  const send = (msg) => {
    if (ws.readyState === ws.OPEN) ws.send(JSON.stringify(msg))
  }

  sock.on('connect', () => send({ type: 'ready', host: `${DB_HOST}:${DB_PORT}` }))

  sock.on('data', (chunk) => {
    reader.push(chunk)
    try {
      for (;;) {
        const r = reader.read()
        if (!r) break
        send({ type: 'reply', reply: truncate(r.value) })
      }
    } catch (err) {
      send({ type: 'reply', reply: { kind: 'error', value: `protocol error: ${err.message}` } })
    }
  })

  const fail = (message) => {
    if (closed) return
    closed = true
    send({ type: 'closed', message })
    ws.close()
  }
  sock.on('error', (err) => fail(`connection to miniredis failed: ${err.message}`))
  sock.on('close', () => fail('miniredis closed the connection'))

  ws.on('message', (raw) => {
    let line
    try {
      line = String(JSON.parse(raw).command ?? '')
    } catch {
      return
    }
    if (!line.trim()) return
    if (Buffer.byteLength(line) > MAX_COMMAND_BYTES) {
      send({ type: 'reply', reply: { kind: 'error', value: 'ERR command too long for this console' } })
      return
    }

    let argv
    try {
      argv = tokenize(line)
    } catch (err) {
      send({ type: 'reply', reply: { kind: 'error', value: `ERR ${err.message}` } })
      return
    }
    if (!argv.length) return

    const name = argv[0].toUpperCase()
    const sub = argv[1] ? argv[1].toUpperCase() : ''
    if (BLOCKED.has(name) || BLOCKED_SUBCOMMANDS[name]?.has(sub)) {
      send({
        type: 'reply',
        reply: { kind: 'error', value: `ERR ${name}${sub ? ' ' + sub : ''} is disabled on the public console` },
      })
      return
    }

    sock.write(encodeCommand(argv))
  })

  ws.on('close', () => sock.destroy())
})

/** Keep a runaway reply (a big KEYS or LRANGE) from flooding the browser. */
function truncate(reply) {
  if (reply.kind !== 'array' || !Array.isArray(reply.value)) return reply
  if (reply.value.length <= MAX_REPLY_ITEMS) {
    return { ...reply, value: reply.value.map(truncate) }
  }
  return {
    kind: 'array',
    truncatedFrom: reply.value.length,
    value: reply.value.slice(0, MAX_REPLY_ITEMS).map(truncate),
  }
}

server.listen(PORT, '0.0.0.0', () => {
  console.log(`[web] console on http://0.0.0.0:${PORT}, bridging to ${DB_HOST}:${DB_PORT}`)
})
