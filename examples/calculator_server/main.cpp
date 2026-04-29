// SPDX-License-Identifier: Apache-2.0
//
// A minimal MCP calculator server. Communicates over stdio per the spec:
// stdin/stdout carry JSON-RPC frames; logs go to stderr.
//
// Run from a hosting application (e.g. Claude Desktop) by configuring it
// to spawn this binary. To poke it by hand:
//
//   { ./calculator_server <<EOF
//   {"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"cli","version":"0"}}}
//   {"jsonrpc":"2.0","method":"notifications/initialized"}
//   {"jsonrpc":"2.0","id":2,"method":"tools/list"}
//   {"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"add","arguments":{"a":2,"b":3}}}
//   EOF
//   }

#include "mcp/mcp.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

namespace {

// Signal-safe shutdown: the signal handler only flips an atomic flag
// (the only async-signal-safe primitives available to us are atomic
// stores and write(2)). A small supervisor thread polls the flag and,
// once set, calls Server::stop() from a regular thread context where
// mutexes and condition variables are legal.
std::atomic<mcp::Server*>      g_server{nullptr};
std::atomic_flag               g_shutdown_requested = ATOMIC_FLAG_INIT;

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
        .content = { mcp::TextContent{.text = std::move(s)} },
        .is_error = is_err,
    };
}

}  // namespace

int main() {
    mcp::set_log_level(mcp::LogLevel::info);

    mcp::Server server{ mcp::Implementation{
        .name        = "mcp-cpp-calculator",
        .version     = "0.1.0",
        .description = "Tiny demo: arithmetic over MCP.",
    }};
    server.set_instructions(
        "This server offers four arithmetic tools (add, subtract, "
        "multiply, divide). Each takes two numbers `a` and `b`.");

    server.tool("add", arith_schema(),
        [](const nlohmann::json& args) {
            const double a = args.at("a").get<double>();
            const double b = args.at("b").get<double>();
            return text_result(std::to_string(a + b));
        },
        "Add",
        "Returns a + b.");

    server.tool("subtract", arith_schema(),
        [](const nlohmann::json& args) {
            const double a = args.at("a").get<double>();
            const double b = args.at("b").get<double>();
            return text_result(std::to_string(a - b));
        },
        "Subtract",
        "Returns a - b.");

    server.tool("multiply", arith_schema(),
        [](const nlohmann::json& args) {
            const double a = args.at("a").get<double>();
            const double b = args.at("b").get<double>();
            return text_result(std::to_string(a * b));
        },
        "Multiply",
        "Returns a * b.");

    server.tool("divide", arith_schema(),
        [](const nlohmann::json& args) {
            const double a = args.at("a").get<double>();
            const double b = args.at("b").get<double>();
            if (b == 0.0) return text_result("division by zero", /*is_err=*/true);
            return text_result(std::to_string(a / b));
        },
        "Divide",
        "Returns a / b. Reports an error result on b == 0.");

    g_server.store(&server, std::memory_order_release);
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    // Supervisor watches the signal flag and calls Server::stop() from
    // a normal thread when a signal arrives; mutex/cv operations there
    // are legal whereas calling them directly from the signal handler
    // is undefined per POSIX.
    std::atomic<bool> supervisor_done{false};
    std::thread supervisor([&]() {
        while (!supervisor_done.load(std::memory_order_acquire)) {
            if (g_shutdown_requested.test(std::memory_order_acquire)) {
                if (auto* s = g_server.load(std::memory_order_acquire)) s->stop();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
    });

    MCP_LOG_INFO("calculator_server starting");
    server.run(std::make_unique<mcp::StdioTransport>());
    MCP_LOG_INFO("calculator_server stopped");

    // Wake the supervisor and join it before exiting.
    supervisor_done.store(true, std::memory_order_release);
    if (supervisor.joinable()) supervisor.join();
    return EXIT_SUCCESS;
}
