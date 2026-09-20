// miniredis-benchmark - a load generator for MiniRedis.
//
// Modelled on redis-benchmark, with one addition that matters: it emits CSV,
// so the numbers can be charted rather than eyeballed. The notebook in
// notebooks/ reads exactly this output.
//
// What it measures and why:
//
//   throughput     operations per second at a given concurrency and pipeline
//                  depth. The headline number, and the least informative one
//                  on its own.
//
//   latency        p50/p95/p99/max, from a full histogram of every request
//                  rather than a running average. An average hides the tail,
//                  and the tail is what users actually notice.
//
//   pipeline depth batching N commands per round trip. This is the single
//                  biggest lever on a request/response protocol, because it
//                  amortises the network round trip that usually dominates.
//
// Every request is timed individually even when pipelined, so that the latency
// reported is per operation and comparable across depths.
#include "miniredis/common.hpp"
#include "miniredis/net.hpp"
#include "miniredis/resp.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace miniredis;

struct Options {
    std::string host = "127.0.0.1";
    int port = 6380;
    std::string password;
    int clients = 16;
    std::int64_t requests = 50000;
    int pipeline = 1;
    int valueSize = 32;
    std::int64_t keyspace = 10000;
    std::vector<std::string> tests;
    std::string csvPath;
    bool quiet = false;
};

struct TestResult {
    std::string name;
    std::int64_t requests = 0;
    int clients = 0;
    int pipeline = 0;
    int valueSize = 0;
    double seconds = 0;
    double opsPerSecond = 0;
    double p50Micros = 0;
    double p95Micros = 0;
    double p99Micros = 0;
    double maxMicros = 0;
    double meanMicros = 0;
    std::int64_t errors = 0;
};

void printUsage() {
    std::cout <<
        "miniredis-benchmark - load generator for MiniRedis\n"
        "\n"
        "Usage: miniredis-benchmark [options]\n"
        "\n"
        "  -h <host>       server host (default 127.0.0.1)\n"
        "  -p <port>       server port (default 6380)\n"
        "  -a <password>   authenticate on connect\n"
        "  -c <clients>    concurrent connections (default 16)\n"
        "  -n <requests>   total requests per test (default 50000)\n"
        "  -P <pipeline>   commands batched per round trip (default 1)\n"
        "  -d <bytes>      value size for SET (default 32)\n"
        "  -r <keyspace>   number of distinct keys (default 10000)\n"
        "  -t <tests>      comma-separated: ping,set,get,incr,lpush,lrange,sadd,zadd,hset,mixed\n"
        "  --csv <path>    append results to a CSV file\n"
        "  -q              print only the summary table\n"
        "\n"
        "Examples:\n"
        "  miniredis-benchmark -t set,get -n 100000 -c 32\n"
        "  miniredis-benchmark -t get -P 16 --csv bench/results/pipeline.csv\n";
}

std::string makeValue(int size) {
    static const char kAlphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::string value;
    value.reserve(static_cast<size_t>(size));
    for (int i = 0; i < size; ++i) value.push_back(kAlphabet[i % 62]);
    return value;
}

// Builds the command a given test issues for request number `sequence`.
Args buildCommand(const std::string& test, std::int64_t sequence, std::int64_t keyspace,
                  const std::string& value) {
    const std::string key = "key:" + formatInt64(sequence % keyspace);

    if (test == "ping")   return Args{"PING"};
    if (test == "set")    return Args{"SET", key, value};
    if (test == "get")    return Args{"GET", key};
    if (test == "incr")   return Args{"INCR", "counter:" + formatInt64(sequence % keyspace)};
    if (test == "lpush")  return Args{"LPUSH", "list:" + formatInt64(sequence % 100), value};
    if (test == "lrange") return Args{"LRANGE", "list:" + formatInt64(sequence % 100), "0", "9"};
    if (test == "sadd")   return Args{"SADD", "set:" + formatInt64(sequence % 100), value + formatInt64(sequence)};
    if (test == "zadd")   return Args{"ZADD", "zset:" + formatInt64(sequence % 100),
                                      formatInt64(sequence % 1000), "member:" + formatInt64(sequence)};
    if (test == "hset")   return Args{"HSET", "hash:" + formatInt64(sequence % 100),
                                      "field:" + formatInt64(sequence % 50), value};

    if (test == "mixed") {
        // A rough approximation of a cache workload: mostly reads.
        const int roll = static_cast<int>(sequence % 10);
        if (roll < 8) return Args{"GET", key};
        return Args{"SET", key, value};
    }

    return Args{"PING"};
}

// Consumes exactly one RESP reply from the buffer. Returns false when more
// bytes are needed. Used only to know when a reply has fully arrived; the
// contents are not inspected, because the benchmark measures timing.
bool skipOneReply(const std::string& buffer, size_t& offset, bool& sawError) {
    if (offset >= buffer.size()) return false;

    const char tag = buffer[offset];
    const size_t lineEnd = buffer.find("\r\n", offset);
    if (lineEnd == std::string::npos) return false;

    if (tag == '-') sawError = true;

    if (tag == '+' || tag == '-' || tag == ':') {
        offset = lineEnd + 2;
        return true;
    }

    if (tag == '$') {
        std::int64_t length = 0;
        if (!parseInt64(buffer.substr(offset + 1, lineEnd - offset - 1), length)) return false;
        if (length < 0) {
            offset = lineEnd + 2;
            return true;
        }
        const size_t needed = lineEnd + 2 + static_cast<size_t>(length) + 2;
        if (buffer.size() < needed) return false;
        offset = needed;
        return true;
    }

    if (tag == '*') {
        std::int64_t count = 0;
        if (!parseInt64(buffer.substr(offset + 1, lineEnd - offset - 1), count)) return false;
        size_t cursor = lineEnd + 2;
        for (std::int64_t i = 0; i < count; ++i) {
            if (!skipOneReply(buffer, cursor, sawError)) return false;
        }
        offset = cursor;
        return true;
    }

    return false;
}

struct WorkerOutcome {
    std::vector<std::int64_t> latenciesMicros;
    std::int64_t errors = 0;
    bool connected = false;
};

void runWorker(const Options& options, const std::string& test, std::int64_t requestsForThisWorker,
               std::int64_t startSequence, const std::string& value, WorkerOutcome& outcome) {
    std::string error;
    SocketHandle fd = Net::connectTo(options.host, options.port, 5000, error);
    if (fd == kInvalidSocket) return;
    outcome.connected = true;

    if (!options.password.empty()) {
        const std::string auth = encodeCommand(Args{"AUTH", options.password});
        Net::writeAll(fd, auth.data(), auth.size(), 5000);
        std::string line;
        Net::readLine(fd, line, 5000);
    }

    outcome.latenciesMicros.reserve(static_cast<size_t>(requestsForThisWorker));

    std::string readBuffer;
    char chunk[64 * 1024];
    std::int64_t issued = 0;

    while (issued < requestsForThisWorker) {
        const std::int64_t batch =
            std::min<std::int64_t>(options.pipeline, requestsForThisWorker - issued);

        // Build the whole batch, then write it once. That is the entire point
        // of pipelining: one round trip instead of `batch` of them.
        std::string request;
        for (std::int64_t i = 0; i < batch; ++i) {
            request += encodeCommand(buildCommand(test, startSequence + issued + i,
                                                  options.keyspace, value));
        }

        const std::int64_t sentAt = monotonicMicros();
        if (!Net::writeAll(fd, request.data(), request.size(), 30000)) {
            outcome.errors += batch;
            break;
        }

        // Wait for exactly `batch` replies.
        std::int64_t received = 0;
        size_t offset = 0;
        readBuffer.clear();
        bool failed = false;

        while (received < batch) {
            size_t got = 0;
            const IoResult result = Net::readSome(fd, chunk, sizeof(chunk), got);
            if (result == IoResult::Ok) {
                readBuffer.append(chunk, got);
            } else if (result == IoResult::WouldBlock) {
                std::vector<PollRequest> request1(1);
                request1[0].fd = fd;
                request1[0].readable = true;
                if (Net::pollSockets(request1, 30000) <= 0) {
                    failed = true;
                    break;
                }
                continue;
            } else {
                failed = true;
                break;
            }

            bool sawError = false;
            while (received < batch && skipOneReply(readBuffer, offset, sawError)) {
                ++received;
            }
            if (sawError) ++outcome.errors;
        }

        if (failed) {
            outcome.errors += (batch - received);
            break;
        }

        // The batch's elapsed time is divided across its requests, so latency
        // stays per operation and is comparable across pipeline depths.
        const std::int64_t elapsed = monotonicMicros() - sentAt;
        const std::int64_t perRequest = batch > 0 ? elapsed / batch : elapsed;
        for (std::int64_t i = 0; i < batch; ++i) {
            outcome.latenciesMicros.push_back(perRequest);
        }

        issued += batch;
    }

    Net::closeSocket(fd);
}

double percentile(const std::vector<std::int64_t>& sorted, double fraction) {
    if (sorted.empty()) return 0;
    size_t index = static_cast<size_t>(fraction * static_cast<double>(sorted.size()));
    if (index >= sorted.size()) index = sorted.size() - 1;
    return static_cast<double>(sorted[index]);
}

TestResult runTest(const Options& options, const std::string& test) {
    TestResult result;
    result.name = test;
    result.requests = options.requests;
    result.clients = options.clients;
    result.pipeline = options.pipeline;
    result.valueSize = options.valueSize;

    const std::string value = makeValue(options.valueSize);

    std::vector<WorkerOutcome> outcomes(static_cast<size_t>(options.clients));
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(options.clients));

    const std::int64_t perWorker = options.requests / options.clients;
    const std::int64_t remainder = options.requests % options.clients;

    const auto started = std::chrono::steady_clock::now();

    for (int i = 0; i < options.clients; ++i) {
        const std::int64_t share = perWorker + (i < remainder ? 1 : 0);
        const std::int64_t startSequence = static_cast<std::int64_t>(i) * perWorker;
        workers.emplace_back(runWorker, std::cref(options), std::cref(test), share, startSequence,
                             std::cref(value), std::ref(outcomes[static_cast<size_t>(i)]));
    }
    for (auto& worker : workers) worker.join();

    const auto finished = std::chrono::steady_clock::now();
    result.seconds = std::chrono::duration<double>(finished - started).count();

    std::vector<std::int64_t> allLatencies;
    allLatencies.reserve(static_cast<size_t>(options.requests));
    int connectedWorkers = 0;

    for (const auto& outcome : outcomes) {
        if (outcome.connected) ++connectedWorkers;
        result.errors += outcome.errors;
        allLatencies.insert(allLatencies.end(), outcome.latenciesMicros.begin(),
                            outcome.latenciesMicros.end());
    }

    if (connectedWorkers == 0) {
        std::cerr << "miniredis-benchmark: could not connect any client to "
                  << options.host << ":" << options.port << "\n";
        return result;
    }

    std::sort(allLatencies.begin(), allLatencies.end());

    result.opsPerSecond = result.seconds > 0
        ? static_cast<double>(allLatencies.size()) / result.seconds
        : 0;
    result.p50Micros = percentile(allLatencies, 0.50);
    result.p95Micros = percentile(allLatencies, 0.95);
    result.p99Micros = percentile(allLatencies, 0.99);
    result.maxMicros = allLatencies.empty() ? 0 : static_cast<double>(allLatencies.back());

    double total = 0;
    for (const std::int64_t sample : allLatencies) total += static_cast<double>(sample);
    result.meanMicros = allLatencies.empty() ? 0 : total / static_cast<double>(allLatencies.size());

    return result;
}

void printResult(const TestResult& result) {
    std::printf("%-8s %10.0f ops/s  p50 %7.1fus  p95 %8.1fus  p99 %9.1fus  max %10.1fus  %6.2fs",
                result.name.c_str(), result.opsPerSecond, result.p50Micros, result.p95Micros,
                result.p99Micros, result.maxMicros, result.seconds);
    if (result.errors > 0) std::printf("  errors=%lld", static_cast<long long>(result.errors));
    std::printf("\n");
}

void writeCsv(const std::string& path, const std::vector<TestResult>& results) {
    // The header is written only when the file is new, so repeated runs append
    // into one file the notebook can read directly.
    const bool exists = std::ifstream(path).good();

    std::ofstream file(path, std::ios::app);
    if (!file.is_open()) {
        std::cerr << "miniredis-benchmark: cannot write " << path << "\n";
        return;
    }

    if (!exists) {
        file << "test,requests,clients,pipeline,value_size,seconds,ops_per_sec,"
                "p50_us,p95_us,p99_us,max_us,mean_us,errors\n";
    }

    for (const auto& r : results) {
        file << r.name << ',' << r.requests << ',' << r.clients << ',' << r.pipeline << ','
             << r.valueSize << ',' << r.seconds << ',' << r.opsPerSecond << ',' << r.p50Micros
             << ',' << r.p95Micros << ',' << r.p99Micros << ',' << r.maxMicros << ','
             << r.meanMicros << ',' << r.errors << '\n';
    }

    std::cout << "results appended to " << path << "\n";
}

} // namespace

int main(int argc, char* argv[]) {
    Options options;
    options.tests = {"ping", "set", "get", "incr", "lpush", "lrange", "sadd", "zadd", "hset"};

    for (int i = 1; i < argc; ++i) {
        const std::string token = argv[i];

        if (token == "--help") { printUsage(); return 0; }
        if (token == "-q") { options.quiet = true; continue; }

        if (i + 1 >= argc) {
            std::cerr << "miniredis-benchmark: " << token << " needs a value\n";
            return 1;
        }
        const std::string value = argv[++i];
        std::int64_t number = 0;

        if (token == "-h") options.host = value;
        else if (token == "-a") options.password = value;
        else if (token == "--csv") options.csvPath = value;
        else if (token == "-p") { if (!parseInt64(value, number)) return 1; options.port = static_cast<int>(number); }
        else if (token == "-c") { if (!parseInt64(value, number) || number < 1) return 1; options.clients = static_cast<int>(number); }
        else if (token == "-n") { if (!parseInt64(value, number) || number < 1) return 1; options.requests = number; }
        else if (token == "-P") { if (!parseInt64(value, number) || number < 1) return 1; options.pipeline = static_cast<int>(number); }
        else if (token == "-d") { if (!parseInt64(value, number) || number < 1) return 1; options.valueSize = static_cast<int>(number); }
        else if (token == "-r") { if (!parseInt64(value, number) || number < 1) return 1; options.keyspace = number; }
        else if (token == "-t") {
            options.tests.clear();
            std::string current;
            for (const char c : value) {
                if (c == ',') {
                    if (!current.empty()) options.tests.push_back(toLower(current));
                    current.clear();
                } else {
                    current.push_back(c);
                }
            }
            if (!current.empty()) options.tests.push_back(toLower(current));
        } else {
            std::cerr << "miniredis-benchmark: unknown option " << token << "\n";
            return 1;
        }
    }

    if (options.clients > options.requests) {
        options.clients = static_cast<int>(options.requests);
    }

    if (!Net::startup()) {
        std::cerr << "miniredis-benchmark: cannot initialise networking\n";
        return 1;
    }

    if (!options.quiet) {
        std::cout << "MiniRedis benchmark\n"
                  << "  target    " << options.host << ":" << options.port << "\n"
                  << "  requests  " << options.requests << " per test\n"
                  << "  clients   " << options.clients << "\n"
                  << "  pipeline  " << options.pipeline << "\n"
                  << "  value     " << options.valueSize << " bytes\n"
                  << "  keyspace  " << options.keyspace << " keys\n\n";
    }

    std::vector<TestResult> results;
    for (const auto& test : options.tests) {
        TestResult result = runTest(options, test);
        results.push_back(result);
        printResult(result);
    }

    if (!options.csvPath.empty()) writeCsv(options.csvPath, results);

    Net::shutdown();
    return 0;
}
