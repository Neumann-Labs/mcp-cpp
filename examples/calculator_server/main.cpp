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
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace {

std::atomic<mcp::Server*> g_server{nullptr};

extern "C" void on_signal(int) {
    if (auto* s = g_server.load(std::memory_order_acquire)) s->stop();
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

    MCP_LOG_INFO("calculator_server starting");
    server.run(std::make_unique<mcp::StdioTransport>());
    MCP_LOG_INFO("calculator_server stopped");
    return EXIT_SUCCESS;
}
