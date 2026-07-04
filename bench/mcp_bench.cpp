// SPDX-License-Identifier: Apache-2.0
//
// mcp-cpp microbenchmarks.
//
// Measures the SDK's own per-call overhead with zero network/OS noise by
// wiring a Server and Client together over the in-process paired transport
// used by the test suite. Every number here is "what the library costs",
// not "what the kernel/network costs":
//
//   client.call_tool() → serialize → in-proc queue → server dispatch thread
//     → parse → session worker → tool handler → serialize result
//     → in-proc queue → client dispatch thread → parse → resolve future
//
// Three benchmarks:
//   1. tools/call round-trip latency + throughput (the headline)
//   2. ping round-trip (protocol floor: tiny payload)
//   3. JSON-RPC serialize + parse codec microbench (transport-independent)
//
// Build: see bench/README.md (standalone compile against build-rel/libmcp.a).

#include "in_memory_transport.hpp"

#include "mcp/client.hpp"
#include "mcp/protocol.hpp"
#include "mcp/server.hpp"
#include "mcp/transport.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using nlohmann::json;
using clk = std::chrono::steady_clock;

namespace {

double ns(clk::duration d) {
    return std::chrono::duration_cast<std::chrono::duration<double, std::nano>>(d).count();
}

struct Stats {
    double mean, p50, p90, p99, p999, max, min;
};

Stats summarize(std::vector<double>& v) {  // v in nanoseconds, mutated (sorted)
    std::sort(v.begin(), v.end());
    auto pct = [&](double p) {
        if (v.empty()) return 0.0;
        auto idx = static_cast<std::size_t>(p * (v.size() - 1));
        return v[idx];
    };
    double sum = 0;
    for (double x : v) sum += x;
    return Stats{
        .mean = sum / static_cast<double>(v.size()),
        .p50  = pct(0.50),
        .p90  = pct(0.90),
        .p99  = pct(0.99),
        .p999 = pct(0.999),
        .max  = v.back(),
        .min  = v.front(),
    };
}

void print_latency(const char* name, std::vector<double>& samples, double wall_s) {
    Stats s = summarize(samples);
    double per_us = 1000.0;  // ns per us
    std::printf("\n%s  (n=%zu)\n", name, samples.size());
    std::printf("  throughput : %10.0f calls/sec\n",
                static_cast<double>(samples.size()) / wall_s);
    std::printf("  mean       : %8.2f us\n", s.mean / per_us);
    std::printf("  min        : %8.2f us\n", s.min / per_us);
    std::printf("  p50        : %8.2f us\n", s.p50 / per_us);
    std::printf("  p90        : %8.2f us\n", s.p90 / per_us);
    std::printf("  p99        : %8.2f us\n", s.p99 / per_us);
    std::printf("  p99.9      : %8.2f us\n", s.p999 / per_us);
    std::printf("  max        : %8.2f us\n", s.max / per_us);
}

// ---- Server fixture -------------------------------------------------------

struct ServerThread {
    std::shared_ptr<mcp::Server> server;
    std::thread                  thread;

    ServerThread(std::unique_ptr<mcp::Transport> transport) {
        server = std::make_shared<mcp::Server>(mcp::Implementation{
            .name = "bench", .version = "1.0.0"});
        server->tool("add",
            json{{"type", "object"},
                 {"properties", {{"a", {{"type", "number"}}},
                                 {"b", {{"type", "number"}}}}},
                 {"required", json::array({"a", "b"})}},
            [](const json& args) -> mcp::CallToolResult {
                const double a = args.at("a").get<double>();
                const double b = args.at("b").get<double>();
                return mcp::CallToolResult{
                    .content = {mcp::TextContent{.text = std::to_string(a + b)}}};
            });
        thread = std::thread([s = server, t = std::move(transport)]() mutable {
            s->run(std::move(t));
        });
    }
    ~ServerThread() {
        server->stop();
        if (thread.joinable()) thread.join();
    }
};

}  // namespace

int main(int argc, char** argv) {
    const std::size_t N      = argc > 1 ? std::stoul(argv[1]) : 100000;
    const std::size_t WARMUP = 2000;

    auto pair = mcp::test::make_in_memory_pair();
    ServerThread srv(std::move(pair.a));

    mcp::Client client{mcp::Implementation{.name = "bench-client", .version = "1.0.0"}};
    client.connect(std::move(pair.b));
    client.initialize().get();

    std::printf("mcp-cpp microbenchmark  (in-process, single client thread)\n");
    std::printf("hardware concurrency: %u\n", std::thread::hardware_concurrency());

    // ---- 1. tools/call round-trip ----------------------------------------
    {
        for (std::size_t i = 0; i < WARMUP; ++i)
            (void)client.call_tool("add", {{"a", 1}, {"b", 2}}).get();

        std::vector<double> samples;
        samples.reserve(N);
        auto wall0 = clk::now();
        for (std::size_t i = 0; i < N; ++i) {
            auto t0  = clk::now();
            auto fut = client.call_tool("add", {{"a", 1.5}, {"b", 2.5}});
            auto res = fut.get();
            auto t1  = clk::now();
            samples.push_back(ns(t1 - t0));
        }
        double wall_s = ns(clk::now() - wall0) / 1e9;
        print_latency("tools/call  (add, text result)", samples, wall_s);
    }

    // ---- 2. ping round-trip (protocol floor) -----------------------------
    {
        for (std::size_t i = 0; i < WARMUP; ++i) client.ping().get();

        std::vector<double> samples;
        samples.reserve(N);
        auto wall0 = clk::now();
        for (std::size_t i = 0; i < N; ++i) {
            auto t0  = clk::now();
            client.ping().get();
            auto t1  = clk::now();
            samples.push_back(ns(t1 - t0));
        }
        double wall_s = ns(clk::now() - wall0) / 1e9;
        print_latency("ping  (empty payload)", samples, wall_s);
    }

    client.disconnect();

    // ---- 3. JSON-RPC codec microbench (transport-independent) ------------
    {
        const std::size_t M = N * 4;

        // Representative tools/call request frame.
        mcp::JsonRpcRequest req;
        req.id     = mcp::RequestId{42};
        req.method = "tools/call";
        req.params = json{{"name", "add"}, {"arguments", {{"a", 1.5}, {"b", 2.5}}}};
        mcp::JsonRpcMessage req_msg = req;
        std::string req_wire = mcp::serialize_message(req_msg).dump();

        // Serialize: typed -> json -> string.
        std::uint64_t sink = 0;
        auto s0 = clk::now();
        for (std::size_t i = 0; i < M; ++i) {
            std::string out = mcp::serialize_message(req_msg).dump();
            sink += out.size();
        }
        double ser_ns = ns(clk::now() - s0) / static_cast<double>(M);

        // Parse: string -> json -> typed.
        auto p0 = clk::now();
        for (std::size_t i = 0; i < M; ++i) {
            auto msg = mcp::parse_message(json::parse(req_wire));
            sink += msg.index();
        }
        double par_ns = ns(clk::now() - p0) / static_cast<double>(M);

        std::printf("\nJSON-RPC codec  (tools/call frame, %zu B, n=%zu)\n",
                    req_wire.size(), M);
        std::printf("  serialize  : %8.3f us/op   %10.0f ops/sec\n",
                    ser_ns / 1000.0, 1e9 / ser_ns);
        std::printf("  parse      : %8.3f us/op   %10.0f ops/sec\n",
                    par_ns / 1000.0, 1e9 / par_ns);
        std::printf("  (checksum %llu)\n", static_cast<unsigned long long>(sink));
    }

    return 0;
}
