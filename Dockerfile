# Two stages: compile the C++20 server, then ship it next to a small Node
# bridge that serves the web console and proxies RESP2 over a TCP socket.

FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
COPY tools ./tools
# Only the server is needed at runtime; skipping the CLI, benchmark and tests
# keeps the build well inside a free-tier build window. -j2 rather than
# $(nproc): the builder advertises many cores but not the memory to match, and
# parallel C++20 translation units get the build OOM-killed.
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --target miniredis-server -j 2

FROM node:20-bookworm-slim AS runtime
ENV NODE_ENV=production \
    MINIREDIS_HOST=127.0.0.1 \
    MINIREDIS_PORT=6380 \
    PORT=8080
WORKDIR /app

COPY --from=build /src/build/bin/miniredis-server /usr/local/bin/miniredis-server
COPY miniredis.conf /app/miniredis.conf

COPY web/package.json web/package-lock.json* ./web/
RUN cd web && npm install --omit=dev --no-audit --no-fund
COPY web ./web

# The console is public, so the database itself listens only on loopback inside
# the container; the Node bridge is the only thing bound to the outside.
COPY docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
RUN chmod +x /usr/local/bin/docker-entrypoint.sh \
    && mkdir -p /app/data \
    && useradd --create-home --uid 1000 miniredis \
    && chown -R miniredis:miniredis /app
USER miniredis

EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=5s --start-period=15s --retries=3 \
    CMD node -e "require('http').get('http://127.0.0.1:'+(process.env.PORT||8080)+'/healthz',r=>process.exit(r.statusCode===200?0:1)).on('error',()=>process.exit(1))"

CMD ["/usr/local/bin/docker-entrypoint.sh"]
