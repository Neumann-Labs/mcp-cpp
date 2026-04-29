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

    /// `tools/call` with a `task` augmentation. The server returns a
    /// `CreateTaskResult` envelope right away; the actual
    /// `CallToolResult` is fetched later via `task_result()`. The
    /// server must advertise the tasks capability for tools/call.
    /// `ttl_ms` is the per-call retention override; nullopt uses the
    /// server's default.
    [[nodiscard]] std::future<CreateTaskResult>
    call_tool_as_task(std::string                 name,
                      nlohmann::json              arguments = nullptr,
                      std::optional<std::int64_t> ttl_ms    = std::nullopt);

    /// `tasks/get` — fetch the current envelope for a task.
    [[nodiscard]] std::future<Task> task_get(std::string task_id);

    /// `tasks/result` — block on the server until the task is
    /// terminal, then return the underlying request's result as raw
    /// JSON. Decode it as the appropriate result type (e.g.
    /// `CallToolResult`) by calling `.get<T>()`.
    [[nodiscard]] std::future<nlohmann::json>
    task_result(std::string task_id);

    /// `tasks/list` — paginated enumeration of all tasks the server
    /// currently knows about.
    [[nodiscard]] std::future<ListTasksResult>
    task_list(std::optional<std::string> cursor = std::nullopt);

    /// `tasks/cancel` — request cancellation. Returns the task
    /// envelope post-transition (status: cancelled).
    [[nodiscard]] std::future<Task> task_cancel(std::string task_id);

    /// Hook for inbound `notifications/tasks/status`. The handler
    /// receives the latest Task projection on every transition.
    using TaskStatusHandler = std::function<void(const Task&)>;
    void set_task_status_handler(TaskStatusHandler handler);

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

    /// Handler for `elicitation/create` requests from the server. Setting
    /// this also advertises the `elicitation` capability (form+url) on
    /// the next initialize(). Use set_client_capabilities() to declare a
    /// narrower mode set.
    using ElicitationHandler =
        std::function<ElicitResult(const ElicitRequestParams&)>;
    void set_elicitation_handler(ElicitationHandler handler);

    /// Handler for `notifications/elicitation/complete` (URL-mode
    /// completion). Receives the elicitation_id of the flow that
    /// finished out of band. Pass nullptr to clear.
    using ElicitationCompleteHandler =
        std::function<void(std::string elicitation_id)>;
    void set_elicitation_complete_handler(ElicitationCompleteHandler handler);

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
    /// Acquire a strong reference to the live Session, or empty if
    /// disconnected. Centralises the lock+copy that protects against
    /// races with disconnect() resetting `session_`. Returns by value
    /// so callers can drop the lock immediately and still keep the
    /// session alive for the duration of their operation.
    [[nodiscard]] std::shared_ptr<Session> acquire_session() const;

    Implementation                              client_info_;
    std::shared_ptr<Session>                    session_;
    mutable std::mutex                          session_mu_;
    std::optional<InitializeResult>             server_;
    mutable std::mutex                          server_mu_;
    std::atomic<bool>                           connected_{false};

    SamplingHandler                             sampling_handler_;
    RootsListHandler                            roots_handler_;
    ElicitationHandler                          elicitation_handler_;
    ElicitationCompleteHandler                  elicitation_complete_handler_;
    TaskStatusHandler                           task_status_handler_;
    std::optional<ClientCapabilities>           capabilities_override_;
    std::mutex                                  handlers_mu_;
};

}  // namespace mcp
