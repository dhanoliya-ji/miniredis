// miniredis-server entry point.
#include "miniredis/net.hpp"
#include "miniredis/server.hpp"

#include <csignal>
#include <iostream>

namespace {

miniredis::Server* g_server = nullptr;

// Ctrl-C and SIGTERM set a flag rather than tearing the process down, so the
// append-only file is flushed and closed on the way out instead of losing
// whatever was buffered.
void handleSignal(int) {
    if (g_server != nullptr) g_server->stop();
}

void printUsage() {
    std::cout <<
        "MiniRedis 1.0.0 - a Redis-style in-memory database built from scratch\n"
        "\n"
        "Usage: miniredis-server [config-file] [--directive value ...]\n"
        "\n"
        "Common options:\n"
        "  --port <n>                 port to listen on (default 6380)\n"
        "  --bind <address>           interface to bind (default 127.0.0.1)\n"
        "  --dir <path>               working directory for data files\n"
        "  --nodeid <name>            name used in logs and INFO\n"
        "  --databases <n>            number of logical databases (default 16)\n"
        "  --requirepass <password>   require AUTH before any command\n"
        "\n"
        "Memory:\n"
        "  --maxmemory <size>         e.g. 512mb; 0 means unlimited\n"
        "  --maxmemory-policy <p>     noeviction | allkeys-lru | allkeys-lfu |\n"
        "                             allkeys-random | volatile-lru | volatile-lfu |\n"
        "                             volatile-random | volatile-ttl\n"
        "\n"
        "Persistence:\n"
        "  --appendonly <yes|no>      enable the append-only log\n"
        "  --appendfsync <policy>     always | everysec | no\n"
        "  --dbfilename <name>        snapshot filename\n"
        "  --save \"\"                  disable scheduled snapshots\n"
        "\n"
        "Replication:\n"
        "  --replicaof <host> <port>  follow another node\n"
        "  --repl-backlog-size <size> how much history to keep for partial resync\n"
        "\n"
        "Cluster:\n"
        "  --cluster-enabled <yes|no> enable hash-slot sharding\n"
        "\n"
        "Every directive accepted in a config file is also accepted on the command\n"
        "line with a leading '--'. A config file is one 'directive value' per line.\n"
        "\n"
        "  miniredis-server miniredis.conf --port 6390\n"
        "\n";
}

} // namespace

int main(int argc, char* argv[]) {
    using namespace miniredis;

    ServerConfig config;

    int argIndex = 1;

    // A bare first argument is a config file path, matching redis-server.
    if (argc > 1 && argv[1][0] != '-') {
        std::string error;
        if (!config.loadFromFile(argv[1], error)) {
            std::cerr << "miniredis-server: " << error << "\n";
            return 1;
        }
        argIndex = 2;
    }

    for (; argIndex < argc; ++argIndex) {
        std::string token = argv[argIndex];

        if (token == "--help" || token == "-h") {
            printUsage();
            return 0;
        }
        if (token == "--version" || token == "-v") {
            std::cout << "MiniRedis 1.0.0\n";
            return 0;
        }
        if (token.rfind("--", 0) != 0) {
            std::cerr << "miniredis-server: unexpected argument '" << token << "'\n";
            return 1;
        }

        const std::string name = token.substr(2);

        // Collect every following token that is not itself a directive, so
        // multi-value directives such as --replicaof and --save work.
        std::vector<std::string> values;
        while (argIndex + 1 < argc && std::string(argv[argIndex + 1]).rfind("--", 0) != 0) {
            values.push_back(argv[++argIndex]);
        }

        std::string error;
        if (!config.applyDirective(name, values, error)) {
            std::cerr << "miniredis-server: " << error << "\n";
            return 1;
        }
    }

    if (!Net::startup()) {
        std::cerr << "miniredis-server: cannot initialise networking: " << Net::lastErrorString() << "\n";
        return 1;
    }

    Server server(std::move(config));
    g_server = &server;

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    std::string error;
    if (!server.start(error)) {
        std::cerr << "miniredis-server: " << error << "\n";
        Net::shutdown();
        return 1;
    }

    const int exitCode = server.run();

    g_server = nullptr;
    Net::shutdown();
    return exitCode;
}
