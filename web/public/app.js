// Browser side of the console: one WebSocket to the bridge, which holds one TCP
// connection to miniredis. Replies arrive in order, so they are matched to the
// commands still in flight by a simple queue.

const out = document.getElementById('out')
const input = document.getElementById('input')
const form = document.getElementById('form')
const dot = document.getElementById('status-dot')
const statusText = document.getElementById('status-text')

const history = []
let historyAt = 0
/** Commands awaiting a reply, in send order. */
const pending = []
let ws

/** Guided tours: each button sends one command, in a sensible order. */
const TOURS = [
  {
    title: 'Data types',
    note: 'Strings, hashes and sorted sets, all hand-written.',
    commands: [
      'SET user:1 alice',
      'GET user:1',
      'HSET car:1 make Tata model Nexon',
      'HGETALL car:1',
      'ZADD leaderboard 100 alice 90 bob 120 carol',
      'ZREVRANGE leaderboard 0 -1 WITHSCORES',
    ],
  },
  {
    title: 'Expiry',
    note: 'TTLs are reclaimed both lazily and by active sampling.',
    commands: ['SET session:1 token EX 60', 'TTL session:1', 'PERSIST session:1', 'TTL session:1'],
  },
  {
    title: 'Transactions',
    note: 'MULTI queues; EXEC runs the batch as one unit.',
    commands: ['MULTI', 'INCR visits', 'INCR visits', 'EXEC', 'GET visits'],
  },
  {
    title: 'Introspection',
    note: 'The server reports on itself.',
    commands: ['INFO server', 'DBSIZE', 'COMMAND COUNT', 'SLOWLOG LEN'],
  },
]

function el(tag, cls, text) {
  const n = document.createElement(tag)
  if (cls) n.className = cls
  if (text !== undefined) n.textContent = text
  return n
}

function scroll() {
  out.scrollTop = out.scrollHeight
}

function echoCommand(text) {
  const row = el('div', 'row cmd')
  row.append(el('span', 'c', '> '), document.createTextNode(text))
  out.append(row)
  scroll()
}

function note(text) {
  out.append(el('div', 'row note', text))
  scroll()
}

/** Render one decoded RESP reply, indenting nested arrays like redis-cli. */
function renderReply(reply, depth = 0, index = null) {
  const row = el('div', 'row')
  const pad = '  '.repeat(depth)

  if (index !== null) row.append(el('span', 'idx', `${pad}${index}) `))
  else if (depth > 0) row.append(document.createTextNode(pad))

  switch (reply.kind) {
    case 'status':
      row.append(el('span', 'val-status', reply.value))
      break
    case 'error':
      row.append(el('span', 'val-error', `(error) ${reply.value}`))
      break
    case 'integer':
      row.append(el('span', 'val-int', `(integer) ${reply.value}`))
      break
    case 'bulk':
      row.append(el('span', 'val-bulk', JSON.stringify(reply.value)))
      break
    case 'nil':
      row.append(el('span', 'val-nil', '(nil)'))
      break
    case 'array': {
      if (!reply.value.length) {
        row.append(el('span', 'val-nil', '(empty array)'))
        break
      }
      out.append(row)
      reply.value.forEach((item, i) => renderReply(item, depth + 1, i + 1))
      if (reply.truncatedFrom) {
        out.append(el('div', 'row note', `  … ${reply.truncatedFrom - reply.value.length} more (truncated by the console)`))
      }
      scroll()
      return
    }
    default:
      row.append(document.createTextNode(String(reply.value)))
  }
  out.append(row)
  scroll()
}

function setStatus(state, text) {
  dot.className = `dot ${state}`
  statusText.textContent = text
}

function connect() {
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:'
  ws = new WebSocket(`${proto}//${location.host}`)

  ws.onopen = () => setStatus('', 'handshaking…')

  ws.onmessage = (ev) => {
    const msg = JSON.parse(ev.data)
    if (msg.type === 'ready') {
      setStatus('ok', `connected · ${msg.host}`)
      note('Connected to miniredis over RESP2. Type a command, or use the panel on the right.')
      refreshInfo()
      return
    }
    if (msg.type === 'closed') {
      setStatus('bad', 'disconnected')
      note(msg.message)
      return
    }
    if (msg.type === 'reply') {
      const job = pending.shift()
      if (job?.silent) {
        job.resolve(msg.reply)
        return
      }
      renderReply(msg.reply)
    }
  }

  ws.onclose = () => {
    setStatus('bad', 'disconnected — retrying')
    setTimeout(connect, 2500)
  }
  ws.onerror = () => setStatus('bad', 'connection error')
}

function send(command, { silent = false } = {}) {
  if (!ws || ws.readyState !== WebSocket.OPEN) {
    note('not connected yet')
    return Promise.resolve(null)
  }
  if (!silent) echoCommand(command)
  return new Promise((resolve) => {
    pending.push({ silent, resolve })
    ws.send(JSON.stringify({ command }))
  })
}

form.addEventListener('submit', (e) => {
  e.preventDefault()
  const value = input.value.trim()
  if (!value) return
  history.push(value)
  historyAt = history.length
  input.value = ''
  send(value)
})

input.addEventListener('keydown', (e) => {
  if (e.key === 'ArrowUp') {
    if (historyAt > 0) input.value = history[--historyAt] ?? ''
    e.preventDefault()
  } else if (e.key === 'ArrowDown') {
    if (historyAt < history.length - 1) input.value = history[++historyAt] ?? ''
    else {
      historyAt = history.length
      input.value = ''
    }
    e.preventDefault()
  }
})

// Build the guided-tour buttons.
const tours = document.getElementById('tours')
for (const tour of TOURS) {
  const box = el('div', 'tour')
  box.append(el('h3', null, tour.title), el('p', null, tour.note))
  for (const cmd of tour.commands) {
    const b = el('button', null, cmd)
    b.type = 'button'
    b.addEventListener('click', () => {
      send(cmd)
      input.focus()
    })
    box.append(b)
  }
  tours.append(box)
}

/** Poll a few INFO fields for the sidebar, without printing them to the log. */
async function refreshInfo() {
  const reply = await send('INFO', { silent: true })
  if (!reply || reply.kind !== 'bulk') return

  const fields = {}
  for (const line of reply.value.split(/\r?\n/)) {
    const i = line.indexOf(':')
    if (i > 0 && !line.startsWith('#')) fields[line.slice(0, i)] = line.slice(i + 1)
  }

  const want = [
    ['miniredis_version', 'version'],
    ['uptime_in_seconds', 'uptime'],
    ['connected_clients', 'clients'],
    ['total_commands_processed', 'commands'],
    ['used_memory_human', 'memory'],
    ['db0', 'keyspace'],
  ]

  const dl = document.getElementById('info')
  dl.textContent = ''
  let shown = 0
  for (const [key, label] of want) {
    if (fields[key] === undefined) continue
    let value = fields[key]
    if (key === 'uptime_in_seconds') value = `${Number(value)}s`
    const rowEl = el('div')
    rowEl.append(el('dt', null, label), el('dd', null, value))
    dl.append(rowEl)
    shown++
  }
  if (!shown) dl.append(el('dd', 'muted', 'INFO returned no recognised fields'))

  setTimeout(refreshInfo, 5000)
}

connect()
input.focus()
