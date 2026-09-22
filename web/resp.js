// RESP2 codec.
//
// The console speaks to miniredis over the same wire protocol redis-cli uses,
// so nothing here is a shortcut around the server: a command typed in the
// browser is encoded exactly as a real client would send it.

/** Encode an argv array as a RESP2 array of bulk strings. */
export function encodeCommand(args) {
  let out = `*${args.length}\r\n`
  for (const a of args) {
    const buf = Buffer.from(String(a), 'utf8')
    out += `$${buf.length}\r\n${buf.toString('utf8')}\r\n`
  }
  return Buffer.from(out, 'utf8')
}

/**
 * Split a command line into argv, honouring single and double quotes so
 * `SET greeting "hello world"` is three arguments rather than four.
 */
export function tokenize(line) {
  const out = []
  let cur = ''
  let quote = null
  let started = false
  for (let i = 0; i < line.length; i++) {
    const c = line[i]
    if (quote) {
      if (c === '\\' && i + 1 < line.length) {
        cur += line[++i]
      } else if (c === quote) {
        quote = null
      } else {
        cur += c
      }
      continue
    }
    if (c === '"' || c === "'") {
      quote = c
      started = true
      continue
    }
    if (/\s/.test(c)) {
      if (started) out.push(cur)
      cur = ''
      started = false
      continue
    }
    cur += c
    started = true
  }
  if (started) out.push(cur)
  if (quote) throw new Error('unbalanced quotes')
  return out
}

/**
 * Incremental RESP2 reader. Feed it bytes; it yields one decoded reply at a
 * time and keeps whatever remains for the next chunk, because a TCP read can
 * split a reply anywhere.
 */
export class RespReader {
  constructor() {
    this.buf = Buffer.alloc(0)
  }

  push(chunk) {
    this.buf = Buffer.concat([this.buf, chunk])
  }

  /** @returns {{value: any}|null} null when more bytes are needed */
  read() {
    const r = this.#parse(0)
    if (!r) return null
    this.buf = this.buf.subarray(r.next)
    return { value: r.value }
  }

  #lineEnd(from) {
    const i = this.buf.indexOf('\r\n', from, 'utf8')
    return i === -1 ? null : i
  }

  #parse(pos) {
    if (pos >= this.buf.length) return null
    const type = String.fromCharCode(this.buf[pos])
    const end = this.#lineEnd(pos + 1)
    if (end === null) return null
    const line = this.buf.toString('utf8', pos + 1, end)
    const after = end + 2

    switch (type) {
      case '+':
        return { value: { kind: 'status', value: line }, next: after }
      case '-':
        return { value: { kind: 'error', value: line }, next: after }
      case ':':
        return { value: { kind: 'integer', value: Number(line) }, next: after }
      case '$': {
        const len = Number(line)
        if (len === -1) return { value: { kind: 'nil' }, next: after }
        if (this.buf.length < after + len + 2) return null
        return {
          value: { kind: 'bulk', value: this.buf.toString('utf8', after, after + len) },
          next: after + len + 2,
        }
      }
      case '*': {
        const n = Number(line)
        if (n === -1) return { value: { kind: 'nil' }, next: after }
        const items = []
        let p = after
        for (let i = 0; i < n; i++) {
          const r = this.#parse(p)
          if (!r) return null
          items.push(r.value)
          p = r.next
        }
        return { value: { kind: 'array', value: items }, next: p }
      }
      default:
        throw new Error(`unexpected RESP type byte: ${JSON.stringify(type)}`)
    }
  }
}
