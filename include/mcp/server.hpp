// SPDX-License-Identifier: Apache-2.0
//
// Server: high-level MCP server façade. Composed on top of a Session.
//
// Construct with the server's Implementation metadata, register tools
// (and in later phases: resources, prompts), then run() with a Transport.
// run() blocks until the transport closes or stop() is called.
//
//   mcp::Server server{ mcp::Implementation{
//       .name = "calc", .version = "1.0.0",
//   }};
//   server.tool("add",
//       /*input_schema=*/{{"type", "object"}, ...},
//       [](const nlohmann::json& args) -> mcp::CallToolResult {
//           return { .content = { mcp::TextContent{.text = "..."} } };
//       });
//   server.run(std::make_unique<mcp::StdioTransport>());

#pragma once

#include "mcp/protocol.hpp"
#include "mcp/session.hpp"
#include "mcp/transport.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace mcp {

class Server {
public:
    /// Tool handler. Takes the inbound `arguments` JSON (or null if the
    /// client omitted them) and returns a CallToolResult. Throwing
    /// `mcp::Error` reports a JSON-RPC error to the client; throwing
    /// anything else maps to internal_error.
    ///
    /// The result's `isError` field is the right way to report
    /// in-protocol failures (LLM-visible). MCP errors are reserved for
    /// "I couldn't even find this tool" / out-of-band conditions.
    using ToolHandler =
        std::function<CallToolResult(const nlohmann::json& arguments)>;

    explicit Server(Implementation server_info);

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    /// Optional human-readable instructions returned in InitializeResult.
    Server& set_instructions(std::string s);

    /// Register a tool with the given name. Replaces any prior handler.
    /// `input_schema` MUST be a JSON Schema object with "type": "object".
    Server& tool(std::string                    name,
                 nlohmann::json                 input_schema,
                 ToolHandler                    handler,
                 std::optional<std::string>     title       = std::nullopt,
                 std::optional<std::string>     description = std::nullopt,
                 std::optional<ToolAnnotations> annotations = std::nullopt,
                 std::optional<nlohmann::json>  output_schema = std::nullopt);

    /// Run the server: bind a transport, install handlers, start the
    /// session, then block on a condition variable until `stop()` is
    /// called or the transport closes.
    ///
    /// Note: takes a unique_ptr<Transport>, owned for the run's lifetime.
    void run(std::unique_ptr<Transport> transport);

    /// Signal the running server to stop. Safe to call from a signal
    /// handler? No — `run()` blocks on a condition variable, and
    /// notifying that from a signal handler is undefined. Use a side
    /// channel (e.g. set an atomic flag in the signal handler and call
    /// stop() from the main thread).
    void stop();

private:
    void on_initialize(const InitializeRequestParams& params,
                       JsonRpcResponse&               resp);
    nlohmann::json handle_initialize(const nlohmann::json& params);
    nlohmann::json handle_list_tools(const nlohmann::json& params);
    nlohmann::json handle_call_tool(const nlohmann::json& params);

    struct ToolEntry {
        Tool        descriptor;
        ToolHandler handler;
    };

    Implementation                              server_info_;
    std::optional<std::string>                  instructions_;
    std::unordered_map<std::string, ToolEntry>  tools_;
    std::mutex                                  tools_mu_;

    std::unique_ptr<Session>                    session_;
    std::atomic<bool>                           initialized_{false};
    std::atomic<bool>                           stop_requested_{false};
    std::mutex                                  stop_mu_;
    std::condition_variable                     stop_cv_;
};

}  // namespace mcp
