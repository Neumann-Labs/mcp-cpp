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

namespace detail { class TaskStore; }

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

    /// Resource read handler. Receives the requested URI and returns
    /// the resource contents. Throwing mcp::Error is reported back as a
    /// JSON-RPC error.
    using ResourceReadHandler =
        std::function<ReadResourceResult(const std::string& uri)>;

    /// Prompt-get handler. Receives the (string-keyed, string-valued)
    /// argument map (which may be empty) and returns the rendered
    /// prompt as a sequence of PromptMessages.
    using PromptGetHandler =
        std::function<GetPromptResult(const std::unordered_map<std::string, std::string>& arguments)>;

    explicit Server(Implementation server_info);
    /// Out-of-line because the optional `tasks_` member holds a
    /// pImpl-style forward-declared TaskStore.
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    /// Optional human-readable instructions returned in InitializeResult.
    Server& set_instructions(std::string s);

    /// Maximum items per list response. Pages beyond this size return a
    /// `nextCursor` that the client passes back to fetch the next page.
    /// Default 0 means "no pagination — return everything in one page."
    Server& set_page_size(std::size_t n);

    /// Register a tool with the given name. Replaces any prior handler.
    /// `input_schema` MUST be a JSON Schema object with "type": "object".
    Server& tool(std::string                    name,
                 nlohmann::json                 input_schema,
                 ToolHandler                    handler,
                 std::optional<std::string>     title       = std::nullopt,
                 std::optional<std::string>     description = std::nullopt,
                 std::optional<ToolAnnotations> annotations = std::nullopt,
                 std::optional<nlohmann::json>  output_schema = std::nullopt);

    /// Register a resource the server exposes. The handler is invoked
    /// when a client calls `resources/read` for this URI.
    Server& resource(Resource           descriptor,
                     ResourceReadHandler handler);

    /// Add a resource template (for parameterised URIs). The handler
    /// for a templated read isn't bound to a single URI — register the
    /// matching read with a fallback resource handler if you need
    /// dispatch by pattern. (URI-template matching lives in the
    /// application layer for now; we may grow it into the SDK later.)
    Server& resource_template(ResourceTemplate descriptor);

    /// Optional: a fallback resource read handler invoked when no
    /// specifically-registered URI matches. Useful for templated
    /// resources or generic-protocol URIs.
    Server& fallback_resource_handler(ResourceReadHandler handler);

    /// Register a prompt under the given name. Replaces any prior
    /// registration with the same name.
    Server& prompt(Prompt           descriptor,
                   PromptGetHandler handler);

    /// Enable the protocol-level "logging" capability. After
    /// initialization, the server may emit log messages to the client
    /// via `log()`, and the client may adjust the level via
    /// `logging/setLevel`. The level filter is applied server-side
    /// before each emission.
    Server& enable_logging(LoggingLevel initial_level = LoggingLevel::info);

    /// Emit a `notifications/message` log entry to the client. Dropped
    /// if logging is not enabled or `level` is below the negotiated
    /// minimum. `data` is free-form JSON (typically a string or
    /// object). Returns `true` if the notification was sent.
    [[nodiscard]] bool log(LoggingLevel level,
                           nlohmann::json data,
                           std::optional<std::string> logger = std::nullopt);

    /// Emit a `notifications/progress` for the given progress token.
    /// `total` and `message` are optional per spec.
    void report_progress(const ProgressToken&        token,
                         double                      progress,
                         std::optional<double>       total   = std::nullopt,
                         std::optional<std::string>  message = std::nullopt);

    /// Send `sampling/createMessage` to the client and return a future
    /// for the result. The caller (typically a tool handler) MUST run on
    /// the Session's worker thread, not the read thread, or it would
    /// deadlock. Session::handle_frame ensures this by dispatching
    /// inbound requests to a detached worker.
    [[nodiscard]] std::future<CreateMessageResult>
    sample(CreateMessageRequestParams params);

    /// Send `roots/list` to the client and return a future for the
    /// result. The client must declare the roots capability.
    [[nodiscard]] std::future<ListRootsResult> list_roots();

    /// Send `elicitation/create` to the client and return a future for
    /// the result. Pass either an `ElicitFormRequestParams` (in-band
    /// form rendered by the client) or an `ElicitUrlRequestParams`
    /// (out-of-band browser navigation; you can later poll the
    /// `notifications/elicitation/complete` notification to learn the
    /// flow finished). Same threading rule as `sample()`: must run on
    /// the Session worker.
    [[nodiscard]] std::future<ElicitResult>
    elicit(ElicitRequestParams params);

    /// Best-effort `notifications/elicitation/complete` from the server
    /// side, used after a URL-mode elicitation finishes out of band.
    /// Returns std::errc::not_connected if no session is bound.
    std::error_code
    notify_elicitation_complete(std::string elicitation_id);

    /// Hook for inbound `notifications/elicitation/complete` from the
    /// client. The handler receives the `elicitationId` of the URL
    /// flow that finished out of band. Pass nullptr to clear.
    ///
    /// The spec lets either side emit the completion notification
    /// (the originator depends on which side actually witnesses the
    /// out-of-band flow conclude) — the SDK accepts it on either.
    using ElicitationCompleteHandler =
        std::function<void(std::string elicitation_id)>;
    void set_elicitation_complete_handler(ElicitationCompleteHandler handler);

    /// `Server::elicit` with an explicit per-call timeout. Useful when
    /// the natural human-in-the-loop response window exceeds the
    /// Session's default 30s request timeout (URL flows in particular).
    [[nodiscard]] std::future<ElicitResult>
    elicit(ElicitRequestParams       params,
           std::chrono::milliseconds timeout);

    /// Register an autocompletion handler for prompt args or resource
    /// template URIs. The handler returns a list of candidate values.
    using CompletionHandler =
        std::function<CompletionValues(const CompleteRequestParams&)>;
    Server& enable_completion(CompletionHandler handler);

    /// Enable the `tasks` capability for tool calls. Once enabled, a
    /// `tools/call` request that carries a `task` augmentation in
    /// params is dispatched onto a worker thread; the synchronous reply
    /// is a `CreateTaskResult` envelope, and the actual `CallToolResult`
    /// is fetched later via `tasks/result`.
    ///
    /// `default_ttl_ms` controls how long completed tasks are retained
    /// before garbage collection (0 / nullopt ⇒ unlimited). Per spec,
    /// the requester may also override the TTL per call.
    ///
    /// `max_concurrent` caps the number of in-flight task workers; a
    /// task-augmented request received while at the cap is rejected
    /// with `invalid_request` instead of spawning yet another thread
    /// (DoS hardening). 0 = unlimited.
    ///
    /// Calling enable_tasks() takes effect at the next `run()`. Calling
    /// it again replaces the configured limits. Calling it after
    /// `run()` is in progress is racy and not supported.
    Server& enable_tasks(std::optional<std::int64_t> default_ttl_ms = std::nullopt,
                         std::size_t                 max_concurrent = 0);

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
    nlohmann::json handle_list_resources(const nlohmann::json& params);
    nlohmann::json handle_list_resource_templates(const nlohmann::json& params);
    nlohmann::json handle_read_resource(const nlohmann::json& params);
    nlohmann::json handle_list_prompts(const nlohmann::json& params);
    nlohmann::json handle_get_prompt(const nlohmann::json& params);
    nlohmann::json handle_ping(const nlohmann::json& params);
    nlohmann::json handle_set_level(const nlohmann::json& params);
    void           handle_cancelled(const nlohmann::json& params);
    nlohmann::json handle_complete(const nlohmann::json& params);

    nlohmann::json handle_get_task(const nlohmann::json& params);
    nlohmann::json handle_get_task_result(const nlohmann::json& params);
    nlohmann::json handle_list_tasks(const nlohmann::json& params);
    nlohmann::json handle_cancel_task(const nlohmann::json& params);

    struct ToolEntry {
        Tool        descriptor;
        ToolHandler handler;
    };
    struct ResourceEntry {
        Resource             descriptor;
        ResourceReadHandler  handler;
    };

    Implementation                              server_info_;
    std::optional<std::string>                  instructions_;
    std::size_t                                 page_size_{0};
    std::unordered_map<std::string, ToolEntry>  tools_;
    std::mutex                                  tools_mu_;

    std::unordered_map<std::string, ResourceEntry> resources_;
    std::vector<ResourceTemplate>                  resource_templates_;
    ResourceReadHandler                            fallback_resource_handler_;
    std::mutex                                     resources_mu_;

    struct PromptEntry {
        Prompt           descriptor;
        PromptGetHandler handler;
    };
    std::unordered_map<std::string, PromptEntry>   prompts_;
    std::mutex                                     prompts_mu_;

    std::atomic<bool>                              logging_enabled_{false};
    std::atomic<LoggingLevel>                      log_level_{LoggingLevel::info};

    CompletionHandler                              completion_handler_;
    std::mutex                                     completion_mu_;

    /// Inbound `notifications/elicitation/complete` from the client.
    /// Set via set_elicitation_complete_handler(); guards live under
    /// completion_mu_ to keep the handler-registration mutexing
    /// surface narrow.
    ElicitationCompleteHandler                     elicitation_complete_handler_;

    /// Tasks: nullable. Allocated by enable_tasks(). When null, the
    /// `tasks` capability is not advertised and `tools/call` requests
    /// with a `task` augmentation receive a method_not_supported
    /// error.
    std::unique_ptr<detail::TaskStore>             tasks_;
    std::optional<std::int64_t>                    tasks_default_ttl_ms_;
    std::size_t                                    tasks_max_concurrent_{0};

    /// Acquire a strong reference to the live Session; returns empty
    /// when run() has not started or has already returned. Lets
    /// log/sample/list_roots/etc. be called from any thread without
    /// racing with the run() teardown.
    [[nodiscard]] std::shared_ptr<Session> acquire_session() const;

    std::shared_ptr<Session>                    session_;
    mutable std::mutex                          session_mu_;
    std::atomic<bool>                           initialized_{false};
    std::atomic<bool>                           stop_requested_{false};
    std::mutex                                  stop_mu_;
    std::condition_variable                     stop_cv_;
};

}  // namespace mcp
