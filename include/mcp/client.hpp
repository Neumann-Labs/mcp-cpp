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

    /// `resources/list`. Cursor optional; pagination support is
    /// callee-side for Phase 2.
    [[nodiscard]] std::future<ListResourcesResult>
    list_resources(std::optional<std::string> cursor = std::nullopt);

    /// `resources/templates/list`.
    [[nodiscard]] std::future<ListResourceTemplatesResult>
    list_resource_templates(std::optional<std::string> cursor = std::nullopt);

    /// `resources/read` for the given URI.
    [[nodiscard]] std::future<ReadResourceResult>
    read_resource(std::string uri);

    /// `resources/subscribe` — request `notifications/resources/updated`
    /// frames whenever this URI's contents change. Subscribe handlers
    /// for incoming updates: see `set_resource_updated_handler`. The
    /// future resolves on success and throws mcp::Error on failure.
    [[nodiscard]] std::future<void>
    subscribe(std::string uri);

    /// `resources/unsubscribe`.
    [[nodiscard]] std::future<void>
    unsubscribe(std::string uri);

    /// Register a handler for inbound resource-update notifications.
    /// The handler is called whenever the server sends
    /// `notifications/resources/updated`.
    using ResourceUpdatedHandler =
        std::function<void(const ResourceUpdatedNotificationParams&)>;
    void set_resource_updated_handler(ResourceUpdatedHandler handler);

    /// Register a handler for inbound resource-list-changed notifications.
    using ListChangedHandler = std::function<void()>;
    void set_resources_list_changed_handler(ListChangedHandler handler);

    /// `prompts/list`.
    [[nodiscard]] std::future<ListPromptsResult>
    list_prompts(std::optional<std::string> cursor = std::nullopt);

    /// `prompts/get` with the given prompt name and (optional) string-keyed
    /// arguments to fill in template parameters.
    [[nodiscard]] std::future<GetPromptResult>
    get_prompt(std::string                                                  name,
               std::optional<std::unordered_map<std::string, std::string>>  arguments = std::nullopt);

    /// Hook for inbound notifications/prompts/list_changed.
    void set_prompts_list_changed_handler(ListChangedHandler handler);

    /// Send a `notifications/cancelled` for a previously-issued request.
    /// Phase 2 does not yet integrate cancellation with the futures
    /// returned by request methods — the caller must already know the
    /// id (typically obtained via a future Phase 3 cancellation token).
    /// Returns the underlying transport error_code on failure (e.g.
    /// transport closed); empty error_code on success.
    std::error_code cancel_request(RequestId request_id,
                                   std::optional<std::string> reason = std::nullopt);

    /// Send `ping` and wait for a response. Returns void on success;
    /// the future throws on timeout / error.
    [[nodiscard]] std::future<void> ping();

    /// Send `logging/setLevel` to ask the server to change its
    /// log-message threshold. The server may reject if it does not
    /// support the logging capability. The future resolves on success
    /// and throws mcp::Error on failure.
    [[nodiscard]] std::future<void>
    set_log_level(LoggingLevel level);

    /// Hook for inbound `notifications/message` (server-emitted log).
    using LogMessageHandler =
        std::function<void(const LoggingMessageNotificationParams&)>;
    void set_log_message_handler(LogMessageHandler handler);

    /// Hook for inbound `notifications/progress`.
    using ProgressHandler =
        std::function<void(const ProgressNotificationParams&)>;
    void set_progress_handler(ProgressHandler handler);

    /// Handler for `sampling/createMessage` requests from the server.
    /// Setting this also advertises the `sampling` capability when the
    /// next initialize() runs (see set_capabilities()).
    using SamplingHandler =
        std::function<CreateMessageResult(const CreateMessageRequestParams&)>;
    void set_sampling_handler(SamplingHandler handler);

    /// Handler for `roots/list` requests from the server. Setting this
    /// also advertises the `roots` capability.
    using RootsListHandler = std::function<ListRootsResult()>;
    void set_roots_list_handler(RootsListHandler handler);

    /// `completion/complete` — ask the server for autocompletion
    /// suggestions.
    [[nodiscard]] std::future<CompleteResult>
    complete(CompletionReference reference,
             CompleteArgument    argument,
             std::optional<std::unordered_map<std::string, std::string>> context_arguments = std::nullopt);

    /// Override the capabilities advertised on initialize(). The
    /// default reads "what handlers are registered"; override here to
    /// add experimental keys, etc.
    void set_client_capabilities(ClientCapabilities caps);

    /// Notify the server that the client's roots list changed.
    /// Returns the underlying transport error_code on failure.
    std::error_code notify_roots_list_changed();

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

    SamplingHandler                             sampling_handler_;
    RootsListHandler                            roots_handler_;
    std::optional<ClientCapabilities>           capabilities_override_;
    std::mutex                                  handlers_mu_;
};

}  // namespace mcp
