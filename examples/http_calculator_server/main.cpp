// SPDX-License-Identifier: Apache-2.0
//
// HTTP-hosted MCP calculator server.
//
// Mirrors examples/calculator_server/main.cpp but exposes the same
// tools over the Streamable HTTP transport instead of stdio. Run it,
// then connect with any MCP client that speaks Streamable HTTP:
//
//   ./http_calculator_server [host] [port] [path]
//
// Defaults to 127.0.0.1:8080 /mcp. Bind 0.0.0.0 only behind a real
// origin allowlist; the example refuses to bind to non-loopback
// without an --origin flag (TODO).

#include "mcp/http_server_host.hpp"
#include "mcp/log.hpp"
#include "mcp/mcp.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {

std::atomic<mcp::HttpServerHost*> g_host{nullptr};
std::atomic_flag                  g_shutdown_requested = ATOMIC_FLAG_INIT;

extern "C" void on_signal(int) {
    g_shutdown_requested.test_and_set(std::memory_order_release);
}

nlohmann::json arith_schema() {
    return {
        {"type", "object"},
        {"properties", {
            {"a", {{"type", "number"}, {"description", "left operand"}}},
            {"b", {{"type", "number"}, {"description", "right operand"}}},
        }},
        {"required", nlohmann::json::array({"a", "b"})},
    };
}

mcp::CallToolResult text_result(std::string s, bool is_err = false) {
    return mcp::CallToolResult{
        .content  = { mcp::TextContent{.text = std::move(s)} },
        .is_error = is_err,
    };
}

}  // namespace

int main(int argc, char** argv) {
    mcp::set_log_level(mcp::LogLevel::info);

    std::string host = "127.0.0.1";
    int         port = 8080;
    std::string path = "/mcp";
    if (argc > 1) host = argv[1];
    if (argc > 2) port = std::atoi(argv[2]);
    if (argc > 3) path = argv[3];

    mcp::HttpServerHost::Options opts;
    opts.host = host;
    opts.port = port;
    opts.path = path;

    mcp::HttpServerHost server_host{
        mcp::Implementation{
            .name        = "mcp-cpp-http-calculator",
            .version     = "0.1.0",
            .description = "Streamable HTTP demo: arithmetic over MCP.",
        },
        opts,
        [](mcp::Server& s) {
            s.set_instructions(
                "This server offers four arithmetic tools (add, subtract, "
                "multiply, divide). Each takes two numbers `a` and `b`.");

            s.tool("add", arith_schema(),
                [](const nlohmann::json& args) {
                    const double a = args.at("a").get<double>();
                    const double b = args.at("b").get<double>();
                    return text_result(std::to_string(a + b));
                },
                "Add", "Returns a + b.");

            s.tool("subtract", arith_schema(),
                [](const nlohmann::json& args) {
                    const double a = args.at("a").get<double>();
                    const double b = args.at("b").get<double>();
                    return text_result(std::to_string(a - b));
                },
                "Subtract", "Returns a - b.");

            s.tool("multiply", arith_schema(),
                [](const nlohmann::json& args) {
                    const double a = args.at("a").get<double>();
                    const double b = args.at("b").get<double>();
                    return text_result(std::to_string(a * b));
                },
                "Multiply", "Returns a * b.");

            s.tool("divide", arith_schema(),
                [](const nlohmann::json& args) {
                    const double a = args.at("a").get<double>();
                    const double b = args.at("b").get<double>();
                    if (b == 0.0) return text_result("division by zero", true);
                    return text_result(std::to_string(a / b));
                },
                "Divide", "Returns a / b. Reports an error result on b == 0.");
        },
    };

    g_host.store(&server_host, std::memory_order_release);
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    server_host.start();
    std::fprintf(stderr,
        "[mcp-cpp-http-calculator] listening on http://%s:%d%s\n",
        host.c_str(), server_host.port(), path.c_str());

    // Park until a signal arrives. The signal handler only flips an
    // atomic flag — the actual stop() call (which uses mutex/cv) runs
    // on this regular thread, where it's safe per POSIX.
    while (!g_shutdown_requested.test(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
    }
    std::fprintf(stderr, "[mcp-cpp-http-calculator] shutting down\n");
    server_host.stop();
    return EXIT_SUCCESS;
}
