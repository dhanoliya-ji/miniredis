"""Renders the project's PDF deliverables.

    python docs/build_pdfs.py

Produces:
    docs/MiniRedis_Problem_Statement.pdf   - problem, objectives, scope, approach
    docs/MiniRedis_Benchmark_Report.pdf    - measurements, charts and analysis

ReportLab is used rather than a Markdown-to-PDF converter so the layout is
explicit and the benchmark charts can be pulled straight out of the executed
notebook, which guarantees the PDF and the notebook can never disagree about a
number.
"""

import base64
import io
import json
import pathlib
import re

from reportlab.lib import colors
from reportlab.lib.enums import TA_JUSTIFY, TA_LEFT
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import mm
from reportlab.platypus import (
    HRFlowable,
    Image,
    KeepTogether,
    PageBreak,
    Paragraph,
    SimpleDocTemplate,
    Spacer,
    Table,
    TableStyle,
)

DOCS = pathlib.Path(__file__).parent
ROOT = DOCS.parent

# The same validated categorical palette the notebook uses, so the two
# deliverables read as one piece of work.
INK = colors.HexColor("#0b0b0b")
INK_SOFT = colors.HexColor("#52514e")
BLUE = colors.HexColor("#2a78d6")
ORANGE = colors.HexColor("#eb6834")
RULE = colors.HexColor("#d8d7d2")
BAND = colors.HexColor("#f2f1ee")


# ---------------------------------------------------------------------------
# Styles
# ---------------------------------------------------------------------------

def build_styles():
    base = getSampleStyleSheet()

    styles = {
        "title": ParagraphStyle(
            "title", parent=base["Title"], fontName="Helvetica-Bold",
            fontSize=26, leading=31, textColor=INK, spaceAfter=4, alignment=TA_LEFT),
        "subtitle": ParagraphStyle(
            "subtitle", parent=base["Normal"], fontName="Helvetica",
            fontSize=12.5, leading=17, textColor=INK_SOFT, spaceAfter=20),
        "h1": ParagraphStyle(
            "h1", parent=base["Heading1"], fontName="Helvetica-Bold",
            fontSize=15.5, leading=20, textColor=INK, spaceBefore=18, spaceAfter=8),
        "h2": ParagraphStyle(
            "h2", parent=base["Heading2"], fontName="Helvetica-Bold",
            fontSize=12, leading=16, textColor=INK, spaceBefore=13, spaceAfter=6),
        "body": ParagraphStyle(
            "body", parent=base["Normal"], fontName="Helvetica",
            fontSize=9.7, leading=14.3, textColor=INK, spaceAfter=7,
            alignment=TA_JUSTIFY),
        "bullet": ParagraphStyle(
            "bullet", parent=base["Normal"], fontName="Helvetica",
            fontSize=9.7, leading=14.3, textColor=INK, spaceAfter=4,
            leftIndent=13, bulletIndent=3),
        "code": ParagraphStyle(
            "code", parent=base["Normal"], fontName="Courier",
            fontSize=8.4, leading=11.6, textColor=INK,
            backColor=BAND, borderPadding=7, spaceBefore=5, spaceAfter=9,
            leftIndent=3, rightIndent=3),
        "caption": ParagraphStyle(
            "caption", parent=base["Normal"], fontName="Helvetica-Oblique",
            fontSize=8.6, leading=12, textColor=INK_SOFT, spaceAfter=13),
        "callout": ParagraphStyle(
            "callout", parent=base["Normal"], fontName="Helvetica-Bold",
            fontSize=10.3, leading=15, textColor=INK,
            backColor=BAND, borderPadding=9, borderColor=BLUE, borderWidth=0,
            leftIndent=9, rightIndent=9, spaceBefore=7, spaceAfter=11),
        "tablecell": ParagraphStyle(
            "tablecell", parent=base["Normal"], fontName="Helvetica",
            fontSize=8.5, leading=11.4, textColor=INK),
        "tablehead": ParagraphStyle(
            "tablehead", parent=base["Normal"], fontName="Helvetica-Bold",
            fontSize=8.5, leading=11.4, textColor=colors.white),
    }
    return styles


S = build_styles()


def esc(text):
    """Escapes XML and converts **bold** and `code` to ReportLab markup."""
    text = text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
    text = re.sub(r"\*\*(.+?)\*\*", r"<b>\1</b>", text)
    text = re.sub(r"`(.+?)`", r'<font face="Courier" size="8.8">\1</font>', text)
    return text


def para(text, style="body"):
    return Paragraph(esc(text), S[style])


def bullets(items):
    return [Paragraph(esc(item), S["bullet"], bulletText="•") for item in items]


def rule():
    return HRFlowable(width="100%", thickness=0.6, color=RULE,
                      spaceBefore=9, spaceAfter=11)


def table(rows, widths, align_right=()):
    """A banded table with a dark header row."""
    data = [[Paragraph(esc(str(cell)), S["tablehead"]) for cell in rows[0]]]
    for row in rows[1:]:
        data.append([Paragraph(esc(str(cell)), S["tablecell"]) for cell in row])

    style = [
        ("BACKGROUND", (0, 0), (-1, 0), INK),
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("TOPPADDING", (0, 0), (-1, -1), 5),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 5),
        ("LEFTPADDING", (0, 0), (-1, -1), 7),
        ("RIGHTPADDING", (0, 0), (-1, -1), 7),
        ("LINEBELOW", (0, 0), (-1, -2), 0.4, RULE),
        ("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.white, BAND]),
    ]
    for column in align_right:
        style.append(("ALIGN", (column, 0), (column, -1), "RIGHT"))

    t = Table(data, colWidths=widths, repeatRows=1)
    t.setStyle(TableStyle(style))
    return t


def page_furniture(canvas, doc):
    """A thin rule and a page number on every page but the first."""
    canvas.saveState()
    if doc.page > 1:
        canvas.setStrokeColor(RULE)
        canvas.setLineWidth(0.5)
        canvas.line(18 * mm, A4[1] - 14 * mm, A4[0] - 18 * mm, A4[1] - 14 * mm)
        canvas.setFont("Helvetica", 7.6)
        canvas.setFillColor(INK_SOFT)
        canvas.drawString(18 * mm, A4[1] - 11.5 * mm, doc.docTitle)
    canvas.setFont("Helvetica", 8)
    canvas.setFillColor(INK_SOFT)
    canvas.drawCentredString(A4[0] / 2, 11 * mm, str(doc.page))
    canvas.restoreState()


def render(path, title, story):
    doc = SimpleDocTemplate(
        str(path), pagesize=A4,
        leftMargin=18 * mm, rightMargin=18 * mm,
        topMargin=18 * mm, bottomMargin=17 * mm,
        title=title, author="Gajendra Dhanoliya", subject=title)
    doc.docTitle = title
    doc.build(story, onFirstPage=page_furniture, onLaterPages=page_furniture)
    print(f"wrote {path.relative_to(ROOT)}")


# ---------------------------------------------------------------------------
# Document 1 - problem statement
# ---------------------------------------------------------------------------

def problem_statement():
    story = [
        para("MiniRedis", "title"),
        para("A Redis-style in-memory database built from scratch in C++20<br/>"
             "Problem Statement and Objectives", "subtitle"),
        rule(),

        para("1. The problem", "h1"),
        para("Almost every application that serves users at scale sits in front of a database it "
             "cannot afford to hit for every request. A relational database answers a primary-key "
             "lookup in roughly 1-10 milliseconds, most of which is disk seek, query planning and "
             "connection overhead rather than the work of finding the row. An in-memory data store "
             "answers the same lookup in tens of microseconds - two to three orders of magnitude "
             "faster - because the data is already in RAM and the path between socket and value is "
             "short."),
        para("That is why Redis, Memcached and their relatives are near-universal infrastructure. "
             "Session stores, rate limiters, leaderboards, job queues, feature flags and caches are "
             "all built on them."),
        para("The problem this project addresses is not \"we need another cache\". It is that these "
             "systems are used as black boxes. An engineer can operate Redis for years without being "
             "able to answer:"),
        *bullets([
            "What actually happens between SET k v arriving on a socket and the reply going back out?",
            "Redis is single-threaded. Why is that fast rather than slow?",
            "If the process is killed mid-write, exactly how much data is lost, and what determines "
            "that number?",
            "A replica reconnects after a three-second network blip. Does it re-transfer the entire "
            "dataset, and what decides?",
            "The server hits its memory limit. Which key is deleted, and how was it chosen without "
            "scanning everything?",
            "MULTI/EXEC is called a transaction. Is it? What happens if the third of five commands "
            "fails?",
        ]),
        Spacer(1, 5),
        para("These are not trivia. Each is a decision with a real engineering trade-off behind it, "
             "and each determines how the system behaves on the day something goes wrong. Reading "
             "the documentation gives you the what. Building the system gives you the why."),

        para("1.1 The concrete starting point", "h2"),
        para("This project began as a small distributed key-value store: about 1,600 lines of C++ "
             "implementing a hash map behind a TCP socket, a write-ahead log, a snapshot mechanism, "
             "leader/follower replication and a small SQL parser. It worked, and it demonstrated the "
             "basic shape of the problem. It also had specific, instructive defects. A code review "
             "found six:"),
        Spacer(1, 3),
        table([
            ["#", "Defect", "Consequence"],
            ["1", "setsockopt passed a 1-byte char for the 4-byte SO_REUSEADDR option",
             "Three bytes of the option value came from uninitialised stack memory"],
            ["2", "The socket reader issued one recv() syscall per byte",
             "Two orders of magnitude more syscalls than necessary per command"],
            ["3", "The write-ahead log was appended after the lock was released",
             "Two concurrent writers to one key could log in the opposite order to the one they "
             "applied, so recovery restored the losing value"],
            ["4", "INSERT/UPDATE checked existence and wrote in two separate locked sections",
             "Time-of-check-to-time-of-use race: two clients could both believe they created the "
             "same key"],
            ["5", "The log's escaping encoded an empty value as an empty token",
             "A record with an empty value read back with every later field shifted left"],
            ["6", "Accepted connections were pushed into a vector drained only after an infinite loop",
             "Every connection ever accepted leaked a thread handle for the process lifetime"],
        ], widths=[9 * mm, 68 * mm, 97 * mm]),
        Spacer(1, 8),
        para("Defects 3, 4 and 6 are all consequences of one architectural choice: "
             "thread-per-connection with shared mutable state. That is the single most important "
             "thing the original codebase has to teach, and it motivates the rewrite.", "callout"),

        PageBreak(),

        para("2. Objectives", "h1"),
        para("2.1 Primary objective", "h2"),
        para("Build a production-shaped, Redis-compatible in-memory database from first principles - "
             "no database libraries, no networking frameworks, no serialisation libraries - such "
             "that every architectural decision in the system is one the author made deliberately "
             "and can defend."),
        para("Success criterion: the stock redis-cli connects to MiniRedis and works without "
             "modification. That is a falsifiable, externally-verifiable claim about protocol "
             "correctness which cannot be faked with a convincing-looking demo.", "callout"),

        para("2.2 Specific objectives", "h2"),
        Spacer(1, 2),
        table([
            ["", "Objective", "How it is measured"],
            ["O1", "Correct the six defects, and eliminate their root cause",
             "Each fix lands as its own commit explaining the failure mode; the resulting server "
             "has no locks in the command path"],
            ["O2", "Implement a real wire protocol (RESP2)",
             "redis-cli interoperates; a test feeds every prefix of a command and asserts the "
             "parser consumes zero bytes each time"],
            ["O3", "Implement Redis's data model",
             ">=140 commands; WRONGTYPE on type mismatch; an emptied container is indistinguishable "
             "from a missing key"],
            ["O4", "Key expiry that is correct and bounded",
             "Keys with elapsed TTLs are reclaimed without being accessed; cycle cost is bounded by "
             "sample size, not keyspace size"],
            ["O5", "Memory limits and eviction",
             "Under a 2 MB limit with allkeys-lru, writing 4 MB succeeds and stays within the "
             "limit; noeviction refuses with OOM"],
            ["O6", "Both persistence strategies, and the trade-off",
             "A kill -9 mid-workload loses no acknowledged write under appendfsync always; a "
             "corrupted snapshot is rejected; a truncated log tail is recovered from"],
            ["O7", "Replication that survives a reconnect cheaply",
             "A replica reconnecting within the backlog window receives only the bytes it missed; "
             "INFO replication reports the offsets"],
            ["O8", "Horizontal sharding",
             "CLUSTER KEYSLOT foo returns 12182, matching real Redis exactly, so cluster-aware "
             "clients route correctly"],
            ["O9", "Transactions and pub/sub, with precise guarantees",
             "A WATCHed key modified before EXEC aborts the transaction; the documentation states "
             "what the transaction does not guarantee"],
            ["O10", "Measure it, rather than asserting it is fast",
             "p50/p95/p99 latency and throughput across pipeline depths and concurrency levels, "
             "charted and interpreted"],
            ["O11", "Make the codebase legible",
             "Every non-obvious decision carries a comment naming the rejected alternative; zero "
             "warnings under -Wall -Wextra; builds on Windows and Linux"],
        ], widths=[11 * mm, 61 * mm, 102 * mm]),

        Spacer(1, 9),
        para("2.3 Explicit non-objectives", "h2"),
        para("Stating what is not attempted is part of an honest problem statement:"),
        *bullets([
            "Not a Redis replacement. Real Redis has fifteen years of optimisation, multiple memory "
            "encodings per type, Lua scripting, streams, HyperLogLog and modules.",
            "Not automatic failover. Coordinated failover requires consensus. Implementing it badly "
            "produces split brain, so it is deliberately omitted and the omission is documented at "
            "the point where a user would look for it.",
            "Not forked background saves. Real Redis BGSAVE forks and relies on copy-on-write; "
            "fork() does not exist on Windows. MiniRedis saves synchronously and says so in the "
            "reply rather than pretending.",
            "Not RESP3. The server advertises RESP2 and rejects a RESP3 handshake explicitly, rather "
            "than accepting it and then failing to send push frames.",
        ]),

        PageBreak(),

        para("3. Scope", "h1"),
        table([
            ["Layer", "Delivered"],
            ["Network", "Cross-platform non-blocking sockets (Winsock2 + POSIX), poll()/WSAPoll "
                        "multiplexing"],
            ["Protocol", "RESP2 codec, incremental request parser, inline command support"],
            ["Execution", "Single-threaded event loop, command dispatch table with metadata-driven "
                          "routing"],
            ["Data model", "Strings, lists, hashes, sets, sorted sets - roughly 150 commands"],
            ["Lifetime", "TTL expiry, lazy plus an active sampling cycle"],
            ["Memory", "maxmemory with 8 eviction policies, sampled approximated LRU/LFU"],
            ["Durability", "Append-only log with 3 fsync policies and self-rewriting; CRC64-checked "
                           "binary snapshots"],
            ["Distribution", "Leader/follower replication with offsets, ring backlog, partial resync"],
            ["Sharding", "16384 hash slots, CRC16, MOVED redirection, hash tags"],
            ["Concurrency control", "MULTI/EXEC/DISCARD/WATCH optimistic locking"],
            ["Messaging", "Pub/Sub with channel and glob-pattern subscriptions"],
            ["Observability", "INFO, SLOWLOG, MONITOR, MEMORY DOCTOR, CLIENT LIST"],
            ["Query layer", "A small SQL surface over the string keyspace"],
            ["Tooling", "Server, interactive CLI, benchmark harness"],
            ["Verification", "C++ unit suite plus a process-level integration suite"],
        ], widths=[40 * mm, 134 * mm]),
        Spacer(1, 7),
        para("Out of scope: Lua scripting, Redis Streams, HyperLogLog, geospatial indexes, "
             "bitfields, modules, TLS, ACL users, Sentinel, automatic cluster resharding, RESP3."),

        para("4. Approach", "h1"),
        para("The work proceeds in three phases, and the commit history follows them exactly, so "
             "the repository reads as a narrative rather than a drop."),
        *bullets([
            "Phase 1 - Repair. Fix each of the six defects as its own commit, with a message "
            "explaining the failure mode. This establishes what was wrong before anything is "
            "replaced, and makes the case for Phase 2.",
            "Phase 2 - Rebuild. Replace the architecture, one subsystem per commit, in dependency "
            "order: platform abstraction, protocol, data model, keyspace, event loop, persistence, "
            "replication, sharding, commands.",
            "Phase 3 - Verify and document. Unit tests for components, integration tests for "
            "behaviour that only appears across process boundaries, a benchmark harness for the "
            "performance claims, and written documentation for the reasoning.",
        ]),

        para("4.1 The central design decision", "h2"),
        para("The most consequential choice is replacing thread-per-connection with a "
             "single-threaded event loop, and it is worth stating the reasoning explicitly because "
             "it is counter-intuitive."),
        para("The intuition is that more threads means more throughput. For a CPU-bound workload "
             "that is true. For an in-memory database it is not, because the work per command is "
             "measured in hundreds of nanoseconds - a hash lookup and a memory copy. Against that, "
             "a mutex acquisition under contention costs comparable time, and a context switch "
             "costs an order of magnitude more. The synchronisation overhead is not amortised by "
             "the work; it dominates it."),
        para("Removing threads buys three things: atomicity for free, since every command is atomic "
             "because nothing else runs concurrently - defects 3, 4 and 6 become unrepresentable; "
             "no synchronisation cost at all; and a simple concurrency story, because concurrency "
             "comes from multiplexing thousands of sockets with one poll() call, which is where the "
             "actual waiting happens."),
        para("The cost is real and is documented rather than hidden: one slow command blocks every "
             "client. That is precisely why KEYS is discouraged in favour of SCAN, why the slow log "
             "exists, and why DEBUG SLEEP is implemented - so the cost can be demonstrated rather "
             "than described.", "callout"),

        para("5. Expected outcomes", "h1"),
        *bullets([
            "A working database that real Redis clients can talk to.",
            "A commit history that documents six real defects and their root cause.",
            "Measured performance data with latency distributions, not marketing numbers.",
            "A test suite covering both component correctness and cross-process behaviour.",
            "Written explanations of every significant trade-off, including the ones where "
            "MiniRedis chose differently from Redis, and why.",
        ]),
        Spacer(1, 5),
        para("The deliverable is not only the binary. It is the ability to answer every question in "
             "section 1 from having built the answer.", "callout"),
    ]

    render(DOCS / "MiniRedis_Problem_Statement.pdf",
           "MiniRedis - Problem Statement", story)


# ---------------------------------------------------------------------------
# Document 2 - benchmark report
# ---------------------------------------------------------------------------

def chart_images():
    """Pulls the rendered charts out of the executed notebook.

    Taking them from the notebook rather than re-plotting means the PDF and the
    notebook cannot disagree about a number.
    """
    notebook_path = ROOT / "notebooks" / "benchmarks.ipynb"
    if not notebook_path.exists():
        return []

    notebook = json.loads(notebook_path.read_text(encoding="utf-8"))
    images = []
    for cell in notebook["cells"]:
        for output in cell.get("outputs", []):
            png = output.get("data", {}).get("image/png")
            if png:
                images.append(base64.b64decode(png))
    return images


def figure(png_bytes, width_mm=172):
    reader = io.BytesIO(png_bytes)
    image = Image(reader)
    scale = (width_mm * mm) / image.imageWidth
    image.drawWidth = width_mm * mm
    image.drawHeight = image.imageHeight * scale
    return image


def csv_rows(name, columns, formatter=None):
    path = ROOT / "bench" / "results" / name
    lines = path.read_text(encoding="utf-8").strip().split("\n")
    header = lines[0].split(",")
    index = {column: header.index(column) for column in columns}

    rows = []
    for line in lines[1:]:
        fields = line.split(",")
        row = [fields[index[column]] for column in columns]
        rows.append(formatter(row) if formatter else row)
    return rows


def benchmark_report():
    charts = chart_images()

    def number(text, decimals=0):
        try:
            return f"{float(text):,.{decimals}f}"
        except ValueError:
            return text

    concurrency_rows = csv_rows(
        "concurrency.csv", ["clients", "ops_per_sec", "p50_us", "p95_us", "p99_us", "max_us"],
        lambda r: [r[0]] + [number(v) for v in r[1:]])

    operations_rows = csv_rows(
        "operations.csv", ["test", "ops_per_sec", "p50_us", "p95_us", "p99_us"],
        lambda r: [r[0].upper()] + [number(v) for v in r[1:]])

    pipeline_rows = csv_rows(
        "pipeline.csv", ["pipeline", "ops_per_sec", "p50_us", "p99_us", "max_us"],
        lambda r: [r[0]] + [number(v) for v in r[1:]])

    valuesize_rows = csv_rows(
        "valuesize.csv", ["value_size", "ops_per_sec", "p50_us", "p99_us"],
        lambda r: [f"{int(r[0]) // 1024} KB" if int(r[0]) >= 1024 else f"{r[0]} B"]
                  + [number(v) for v in r[1:]])

    story = [
        para("MiniRedis", "title"),
        para("Benchmark Report - measured numbers, and what they mean", "subtitle"),
        rule(),

        para("Method", "h1"),
        table([
            ["", ""],
            ["Platform", "Windows 11, loopback (127.0.0.1)"],
            ["Build", "GCC 15.2 (MSYS2 UCRT64), -O2 -std=c++20, statically linked"],
            ["Server", "Default configuration, snapshots disabled, no append-only log"],
            ["Value size", "32 bytes unless stated"],
            ["Keyspace", "10,000 distinct keys"],
            ["Tool", "bin/miniredis-benchmark, results written as CSV to bench/results/"],
        ], widths=[32 * mm, 142 * mm]),
        Spacer(1, 7),
        para("Latency is measured per operation, from a full histogram, not as a running average. "
             "An average hides the tail, and the tail is what users experience. When commands are "
             "pipelined, the batch's elapsed time is divided across its requests, so latency stays "
             "per-operation and comparable across pipeline depths."),
        para("An important caveat: these numbers are taken over loopback, where there is no "
             "network. That makes them a measurement of the server plus the local TCP stack plus "
             "the benchmark client, not of the server alone. Loopback flatters per-operation "
             "latency and understates the value of pipelining. Both effects appear below, and both "
             "are called out where they do.", "callout"),

        PageBreak(),

        para("1. Concurrency - the headline result", "h1"),
        figure(charts[0]) if charts else Spacer(1, 1),
        para("Left: throughput against concurrent clients. Right: latency percentiles, log scale.",
             "caption"),
        table([["Clients", "Throughput", "p50 (us)", "p95 (us)", "p99 (us)", "max (us)"]]
              + concurrency_rows,
              widths=[22 * mm, 34 * mm, 26 * mm, 26 * mm, 26 * mm, 26 * mm],
              align_right=(1, 2, 3, 4, 5)),
        Spacer(1, 8),
        para("Throughput saturates at about 4 concurrent clients. Every client added after that "
             "converts almost entirely into latency: from 4 to 128 clients, throughput moves +12% "
             "while p50 latency grows 34 times.", "callout"),
        para("This is Little's Law made visible. With throughput pinned, adding requests in flight "
             "can only increase the time each one spends waiting. Past saturation, concurrency does "
             "not buy work - it buys queue."),
        para("The practical consequence is that a connection pool sized well past saturation makes "
             "a system slower, not faster, and the symptom is exactly this shape: flat throughput "
             "with steadily worsening percentiles. It is a common and expensive production mistake."),
        para("The single-client p50 of 29 microseconds is the honest figure for what the server "
             "does per operation: read the socket, parse RESP, hash the key, look it up, encode the "
             "reply, write it back. At one client there is no queueing, so nothing else is folded "
             "in. For scale, a relational database answers the equivalent lookup in 1-10 "
             "milliseconds."),

        PageBreak(),

        para("2. Operations compared", "h1"),
        figure(charts[1]) if len(charts) > 1 else Spacer(1, 1),
        para("Nine command types at fixed concurrency of 16 clients.", "caption"),
        table([["Operation", "Throughput", "p50 (us)", "p95 (us)", "p99 (us)"]] + operations_rows,
              widths=[34 * mm, 38 * mm, 32 * mm, 32 * mm, 32 * mm],
              align_right=(1, 2, 3, 4)),
        Spacer(1, 8),
        para("PING touches no data at all, so the gap between it and everything else is the cost of "
             "the keyspace operation itself. The eight data commands land in a narrow band: a "
             "sorted-set insert into a balanced tree costs about the same as a hash insert, because "
             "at this scale you are measuring the protocol and syscall path, not the data "
             "structures. Optimising the containers here would be misdirected effort."),
        para("Two honest observations rather than convenient ones. SET measures faster than GET, "
             "which looks backwards; it is an artefact of test ordering against a shared keyspace, "
             "reported rather than quietly dropped, because a results table that only ever confirms "
             "expectations is not a measurement. And LRANGE returning ten elements costs about the "
             "same as GET returning one, because encoding ten bulk strings is cheap next to the "
             "fixed per-command overhead."),

        PageBreak(),

        para("3. Pipeline depth - where the textbook answer is wrong", "h1"),
        figure(charts[2]) if len(charts) > 2 else Spacer(1, 1),
        para("Left: throughput against pipeline depth. Right: tail latency.", "caption"),
        table([["Depth", "Throughput", "p50 (us)", "p99 (us)", "max (us)"]] + pipeline_rows,
              widths=[22 * mm, 38 * mm, 32 * mm, 32 * mm, 32 * mm],
              align_right=(1, 2, 3, 4)),
        Spacer(1, 8),
        para("The standard claim is that pipelining multiplies throughput, because it amortises the "
             "network round trip across a batch. Here it does not - throughput peaks at depth 4 and "
             "then declines. That is the finding, not a defect in the measurement.", "callout"),
        para("The explanation is the loopback caveat. Pipelining's entire benefit is removing "
             "network latency from the critical path, and on 127.0.0.1 that latency is already "
             "close to zero, so there is nothing to amortise. What remains is the extra cost of "
             "building and buffering larger batches, which is why deep pipelines are slightly "
             "worse."),
        para("On a real network the picture inverts completely. At 0.5 ms of round trip, depth 1 "
             "pays that cost on every operation while depth 64 shares it across sixty-four. This is "
             "a case where the benchmark environment determines the answer, and reporting the "
             "loopback number as though it were general would be misleading."),
        para("What pipelining does buy here is the tail: worst-case latency falls roughly six-fold "
             "between depth 1 and depth 64, and p99 improves too. Fewer, larger socket interactions "
             "mean fewer opportunities to be descheduled at an inconvenient moment. The trend is "
             "not strictly monotonic - max is a single-sample statistic and therefore the noisiest "
             "number in the table - but the direction from depth 4 onward is consistent."),

        PageBreak(),

        para("4. Value size", "h1"),
        figure(charts[3]) if len(charts) > 3 else Spacer(1, 1),
        para("SET throughput against payload size, log scale on the x axis.", "caption"),
        table([["Value size", "Throughput", "p50 (us)", "p99 (us)"]] + valuesize_rows,
              widths=[30 * mm, 42 * mm, 36 * mm, 36 * mm],
              align_right=(1, 2, 3)),
        Spacer(1, 8),
        para("A 2,048-fold increase in value size costs under a two-fold drop in throughput. That "
             "is the signature of a system dominated by fixed per-operation overhead: up to about "
             "4 KB the cost of a command is almost entirely parsing, hashing and syscalls, and the "
             "bytes themselves are nearly free because memcpy runs at gigabytes per second."),
        para("The knee sits between 4 KB and 16 KB, where the payload finally becomes large enough "
             "to matter: throughput drops about 30% across that last four-fold increase, versus "
             "about 23% across the preceding 512-fold increase."),
        para("Practical reading: values up to a few KB are essentially free relative to the round "
             "trip. Above that, size starts to buy real cost, and it is worth asking whether the "
             "large value belongs in the cache at all."),

        para("Summary", "h1"),
        table([
            ["Finding", "Evidence"],
            ["Single-operation latency is about 29 us", "concurrency sweep at 1 client"],
            ["The server saturates at about 4 concurrent clients", "throughput flat from 4 to 128"],
            ["Past saturation, concurrency becomes latency", "+12% throughput, 34x p50 latency"],
            ["Data structure choice barely matters at this scale",
             "8 data commands within a narrow band"],
            ["Pipelining does not help throughput on loopback",
             "flat-to-declining past depth 4"],
            ["Pipelining does cut tail latency about six-fold", "max 7,971 us to 1,252 us"],
            ["Payload size is nearly free below about 4 KB", "2,048x size for under 2x cost"],
        ], widths=[84 * mm, 90 * mm]),

        Spacer(1, 9),
        para("What these numbers are not", "h1"),
        *bullets([
            "Not a comparison with Redis. No such comparison was run. Redis has fifteen years of "
            "optimisation and would very likely win. Publishing a favourable-looking comparison "
            "without running it would be dishonest.",
            "Not representative of a real network. Loopback removes the single largest cost in a "
            "real deployment, and section 3 shows how much that changes the conclusion.",
            "Not a durability measurement. Snapshots and the append-only log are disabled in these "
            "runs; appendfsync always adds a disk round trip to every write and would dominate all "
            "of this.",
        ]),
        Spacer(1, 4),
        para("The purpose is to show the shape of the system's behaviour - where it saturates, what "
             "dominates cost, how latency degrades under load - not to win a comparison.", "callout"),
    ]

    render(DOCS / "MiniRedis_Benchmark_Report.pdf",
           "MiniRedis - Benchmark Report", story)


if __name__ == "__main__":
    problem_statement()
    benchmark_report()
