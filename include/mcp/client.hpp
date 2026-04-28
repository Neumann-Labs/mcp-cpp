// SPDX-License-Identifier: Apache-2.0
//
// Client: high-level MCP client façade. Composed on top of a Session.
//
// Typical usage:
//
//   mcp::Client client{ mcp::Implementation{
//       .name = "my-app", .version = "0.1.0",
//   }};
//   client.connect(std::make_unique<mcp::StdioTransport>(...));
//   auto info = client.initialize().get();
//   auto tools = client.list_tools().get();
//   auto out = client.call_tool("add", {{"a", 1}, {"b", 2}}).get();
//   client.shutdown();

#pragma once

#include "mcp/protocol.hpp"
#include "mcp/session.hpp"
#include "mcp/transport.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <future>
#include <memory>
#include <optional>
#include <string>

namespace mcp {

class Client {
public:
    explicit Client(Implementation client_info);

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&) = delete;
    Client& operator=(Client&&) = delete;

    ~Client();

    /// Bind a transport and start the underlying session. Idempotent
    /// per Client instance — calling twice rebinds (after disconnect).
    void connect(std::unique_ptr<Transport> transport);

    /// Tear down: close the session and drop the transport.
    void disconnect();

    /// Send `initialize` and (on resolution) the `notifications/initialized`
    /// notification. The future resolves to the InitializeResult or
    /// throws on error.
    [[nodiscard]] std::future<InitializeResult> initialize();

    /// `tools/list`. Optional cursor for pagination (Phase 3 will fully
    /// support paginated iteration; for now you get the whole list).
    [[nodiscard]] std::future<ListToolsResult>
    list_tools(std::optional<std::string> cursor = std::nullopt);

    /// `tools/call` with the given name and JSON-shaped arguments.
    [[nodiscard]] std::future<CallToolResult>
    call_tool(std::string name, nlohmann::json arguments = nullptr);

    /// True between connect() and disconnect().
    [[nodiscard]] bool is_connected() const noexcept;

    /// Cached server information from the last successful initialize().
    [[nodiscard]] std::optional<InitializeResult> server() const;

private:
    Implementation                              client_info_;
    std::unique_ptr<Session>                    session_;
    std::optional<InitializeResult>             server_;
    mutable std::mutex                          server_mu_;
    std::atomic<bool>                           connected_{false};
};

}  // namespace mcp
