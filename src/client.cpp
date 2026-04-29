// SPDX-License-Identifier: Apache-2.0
#include "mcp/client.hpp"

#include "mcp/error.hpp"
#include "mcp/log.hpp"
#include "mcp/protocol.hpp"
#include "mcp/session.hpp"
#include "mcp/transport.hpp"

#include <nlohmann/json.hpp>

#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace mcp {

Client::Client(Implementation client_info)
    : client_info_(std::move(client_info)) {}

Client::~Client() {
    disconnect();
}

void Client::connect(std::unique_ptr<Transport> transport) {
    if (!transport) {
        throw Error{error_code::internal_error, "Client::connect: transport is null"};
    }
    disconnect();
    {
        std::lock_guard<std::mutex> lk(session_mu_);
        session_ = std::make_shared<Session>(std::move(transport));
    }
    // Start under the lock-acquired pointer, but don't hold the lock
    // across start() — start spawns the read thread, which can run
    // arbitrary user callbacks.
    auto local = acquire_session();
    if (local) local->start();
    connected_.store(true, std::memory_order_release);
}

void Client::disconnect() {
    if (!connected_.exchange(false, std::memory_order_acq_rel)) return;
    std::shared_ptr<Session> local;
    {
        std::lock_guard<std::mutex> lk(session_mu_);
        local = std::move(session_);
        session_.reset();
    }
    // close() + drop our reference outside the lock; concurrent
    // call sites that took an earlier copy will keep the Session
    // alive via shared_ptr count until they finish.
    if (local) local->close();
    {
        std::lock_guard<std::mutex> lk(server_mu_);
        server_.reset();
    }
}

std::shared_ptr<Session> Client::acquire_session() const {
    std::lock_guard<std::mutex> lk(session_mu_);
    return session_;
}

bool Client::is_connected() const noexcept {
    return connected_.load(std::memory_order_acquire);
}

std::optional<InitializeResult> Client::server() const {
    std::lock_guard<std::mutex> lk(server_mu_);
    return server_;
}

// =====================================================================
// initialize
// =====================================================================

std::future<InitializeResult> Client::initialize() {
    auto session = acquire_session();
    if (!session) {
        throw Error{error_code::internal_error, "client not connected"};
    }
    ClientCapabilities caps;
    {
        std::lock_guard<std::mutex> lk(handlers_mu_);
        // First derive caps from the registered handlers...
        if (sampling_handler_) caps.sampling = SamplingCapability{};
        if (roots_handler_)    caps.roots    = RootsCapability{};
        if (elicitation_handler_) {
            ElicitationCapability ec;
            ec.form = nlohmann::json::object();
            ec.url  = nlohmann::json::object();
            caps.elicitation = std::move(ec);
        }
        // ...then merge any explicit override on top. Each engaged
        // override field replaces the corresponding derived one
        // (typical use: narrow elicitation modes, or attach
        // experimental keys); fields the override leaves unset
        // keep their handler-derived values rather than getting
        // wiped out, which is what users expect from "override"
        // semantics.
        if (capabilities_override_.has_value()) {
            const auto& o = *capabilities_override_;
            if (o.experimental.has_value()) caps.experimental = o.experimental;
            if (o.roots.has_value())        caps.roots        = o.roots;
            if (o.sampling.has_value())     caps.sampling     = o.sampling;
            if (o.elicitation.has_value())  caps.elicitation  = o.elicitation;
            if (o.tasks.has_value())        caps.tasks        = o.tasks;
        }
    }
    InitializeRequestParams params{
        .protocol_version = std::string{kLatestProtocolVersion},
        .capabilities     = std::move(caps),
        .client_info      = client_info_,
    };

    return std::async(std::launch::async,
        [this, session, payload = nlohmann::json(params)]() -> InitializeResult {
            auto raw = session->send_request(std::string{method_initialize},
                                              payload).get();
            auto res = raw.get<InitializeResult>();
            {
                std::lock_guard<std::mutex> lk(server_mu_);
                server_ = res;
            }
            const auto note_ec = session->send_notification(
                std::string{method_notifications_initialized});
            if (note_ec) {
                throw Error{error_code::internal_error,
                            "failed to send notifications/initialized: " +
                                note_ec.message()};
            }
            return res;
        });
}

// =====================================================================
// tools
// =====================================================================

std::future<ListToolsResult>
Client::list_tools(std::optional<std::string> cursor) {
    auto session = acquire_session();
    if (!session) {
        throw Error{error_code::internal_error, "client not connected"};
    }
    ListToolsRequestParams params{.cursor = std::move(cursor)};
    auto inner = session->send_request(std::string{method_tools_list},
                                        nlohmann::json(params));
    return std::async(std::launch::async,
        [inner = std::move(inner)]() mutable -> ListToolsResult {
            return inner.get().get<ListToolsResult>();
        });
}

std::future<CallToolResult>
Client::call_tool(std::string name, nlohmann::json arguments) {
    auto session = acquire_session();
    if (!session) {
        throw Error{error_code::internal_error, "client not connected"};
    }
    CallToolRequestParams params{
        .name      = std::move(name),
        .arguments = arguments.is_null() ? std::nullopt
                                         : std::optional<nlohmann::json>(std::move(arguments)),
        .task      = std::nullopt,
    };
    auto inner = session->send_request(std::string{method_tools_call},
                                        nlohmann::json(params));
    return std::async(std::launch::async,
        [inner = std::move(inner)]() mutable -> CallToolResult {
            return inner.get().get<CallToolResult>();
        });
}

std::future<CreateTaskResult>
Client::call_tool_as_task(std::string                 name,
                          nlohmann::json              arguments,
                          std::optional<std::int64_t> ttl_ms) {
    auto session = acquire_session();
    if (!session) {
        throw Error{error_code::internal_error, "client not connected"};
    }
    CallToolRequestParams params{
        .name      = std::move(name),
        .arguments = arguments.is_null() ? std::nullopt
                                         : std::optional<nlohmann::json>(std::move(arguments)),
        .task      = TaskAugmentation{.ttl = ttl_ms},
    };
    auto inner = session->send_request(std::string{method_tools_call},
                                        nlohmann::json(params));
    return std::async(std::launch::async,
        [inner = std::move(inner)]() mutable -> CreateTaskResult {
            return inner.get().get<CreateTaskResult>();
        });
}

std::future<Task> Client::task_get(std::string task_id) {
    auto session = acquire_session();
    if (!session) {
        throw Error{error_code::internal_error, "client not connected"};
    }
    auto inner = session->send_request(std::string{method_tasks_get},
        nlohmann::json(GetTaskRequestParams{.task_id = std::move(task_id)}));
    return std::async(std::launch::async,
        [inner = std::move(inner)]() mutable -> Task {
            return inner.get().get<Task>();
        });
}

std::future<nlohmann::json> Client::task_result(std::string task_id) {
    auto session = acquire_session();
    if (!session) {
        throw Error{error_code::internal_error, "client not connected"};
    }
    auto inner = session->send_request(std::string{method_tasks_result},
        nlohmann::json(GetTaskResultRequestParams{.task_id = std::move(task_id)}));
    return std::async(std::launch::async,
        [inner = std::move(inner)]() mutable -> nlohmann::json {
            return inner.get();
        });
}

std::future<ListTasksResult>
Client::task_list(std::optional<std::string> cursor) {
    auto session = acquire_session();
    if (!session) {
        throw Error{error_code::internal_error, "client not connected"};
    }
    ListTasksRequestParams params{.cursor = std::move(cursor)};
    auto inner = session->send_request(std::string{method_tasks_list},
                                        nlohmann::json(params));
    return std::async(std::launch::async,
        [inner = std::move(inner)]() mutable -> ListTasksResult {
            return inner.get().get<ListTasksResult>();
        });
}

std::future<Task> Client::task_cancel(std::string task_id) {
    auto session = acquire_session();
    if (!session) {
        throw Error{error_code::internal_error, "client not connected"};
    }
    auto inner = session->send_request(std::string{method_tasks_cancel},
        nlohmann::json(CancelTaskRequestParams{.task_id = std::move(task_id)}));
    return std::async(std::launch::async,
        [inner = std::move(inner)]() mutable -> Task {
            return inner.get().get<Task>();
        });
}

void Client::set_task_status_handler(TaskStatusHandler handler) {
    auto session = acquire_session();
    if (!session) throw Error{error_code::internal_error, "client not connected"};
    {
        std::lock_guard<std::mutex> lk(handlers_mu_);
        task_status_handler_ = handler;
    }
    const std::string method{method_notifications_tasks_status};
    if (handler) {
        session->set_notification_handler(method,
            [h = std::move(handler)](const nlohmann::json& params) {
                if (params.is_null()) return;
                Task projection;
                try {
                    // notifications/tasks/status carries the Task
                    // projection inline (no wrapper object).
                    projection = params.get<Task>();
                } catch (...) {
                    return;
                }
                h(projection);
            });
    } else {
        session->clear_notification_handler(method);
    }
}

// =====================================================================
// resources
// =====================================================================

std::future<ListResourcesResult>
Client::list_resources(std::optional<std::string> cursor) {
    auto session = acquire_session();
    if (!session) throw Error{error_code::internal_error, "client not connected"};
    ListResourcesRequestParams params{.cursor = std::move(cursor)};
    auto inner = session->send_request(std::string{method_resources_list},
                                        nlohmann::json(params));
    return std::async(std::launch::async,
        [inner = std::move(inner)]() mutable -> ListResourcesResult {
            return inner.get().get<ListResourcesResult>();
        });
}

std::future<ListResourceTemplatesResult>
Client::list_resource_templates(std::optional<std::string> cursor) {
    auto session = acquire_session();
    if (!session) throw Error{error_code::internal_error, "client not connected"};
    ListResourceTemplatesRequestParams params{.cursor = std::move(cursor)};
    auto inner = session->send_request(std::string{method_resources_templates_list},
                                        nlohmann::json(params));
    return std::async(std::launch::async,
        [inner = std::move(inner)]() mutable -> ListResourceTemplatesResult {
            return inner.get().get<ListResourceTemplatesResult>();
        });
}

std::future<ReadResourceResult>
Client::read_resource(std::string uri) {
    auto session = acquire_session();
    if (!session) throw Error{error_code::internal_error, "client not connected"};
    ReadResourceRequestParams params{.uri = std::move(uri)};
    auto inner = session->send_request(std::string{method_resources_read},
                                        nlohmann::json(params));
    return std::async(std::launch::async,
        [inner = std::move(inner)]() mutable -> ReadResourceResult {
            return inner.get().get<ReadResourceResult>();
        });
}

std::future<void>
Client::subscribe(std::string uri) {
    auto session = acquire_session();
    if (!session) throw Error{error_code::internal_error, "client not connected"};
    SubscribeRequestParams params{.uri = std::move(uri)};
    auto inner = session->send_request(std::string{method_resources_subscribe},
                                        nlohmann::json(params));
    return std::async(std::launch::async,
        [inner = std::move(inner)]() mutable { (void)inner.get(); });
}

std::future<void>
Client::unsubscribe(std::string uri) {
    auto session = acquire_session();
    if (!session) throw Error{error_code::internal_error, "client not connected"};
    UnsubscribeRequestParams params{.uri = std::move(uri)};
    auto inner = session->send_request(std::string{method_resources_unsubscribe},
                                        nlohmann::json(params));
    return std::async(std::launch::async,
        [inner = std::move(inner)]() mutable { (void)inner.get(); });
}

void Client::set_resource_updated_handler(ResourceUpdatedHandler handler) {
    auto session = acquire_session();
    if (!session) throw Error{error_code::internal_error, "client not connected"};
    const std::string method{method_notifications_resources_updated};
    if (handler) {
        session->set_notification_handler(method,
            [h = std::move(handler)](const nlohmann::json& params) {
                if (params.is_null()) return;
                h(params.get<ResourceUpdatedNotificationParams>());
            });
    } else {
        session->clear_notification_handler(method);
    }
}

void Client::set_resources_list_changed_handler(ListChangedHandler handler) {
    auto session = acquire_session();
    if (!session) throw Error{error_code::internal_error, "client not connected"};
    const std::string method{method_notifications_resources_list_changed};
    if (handler) {
        session->set_notification_handler(method,
            [h = std::move(handler)](const nlohmann::json&) { h(); });
    } else {
        session->clear_notification_handler(method);
    }
}

// =====================================================================
// prompts
// =====================================================================

std::future<ListPromptsResult>
Client::list_prompts(std::optional<std::string> cursor) {
    auto session = acquire_session();
    if (!session) throw Error{error_code::internal_error, "client not connected"};
    ListPromptsRequestParams params{.cursor = std::move(cursor)};
    auto inner = session->send_request(std::string{method_prompts_list},
                                        nlohmann::json(params));
    return std::async(std::launch::async,
        [inner = std::move(inner)]() mutable -> ListPromptsResult {
            return inner.get().get<ListPromptsResult>();
        });
}

std::future<GetPromptResult>
Client::get_prompt(std::string name,
                   std::optional<std::unordered_map<std::string, std::string>> arguments) {
    auto session = acquire_session();
    if (!session) throw Error{error_code::internal_error, "client not connected"};
    GetPromptRequestParams params{
        .name      = std::move(name),
        .arguments = std::move(arguments),
    };
    auto inner = session->send_request(std::string{method_prompts_get},
                                        nlohmann::json(params));
    return std::async(std::launch::async,
        [inner = std::move(inner)]() mutable -> GetPromptResult {
            return inner.get().get<GetPromptResult>();
        });
}

void Client::set_prompts_list_changed_handler(ListChangedHandler handler) {
    auto session = acquire_session();
    if (!session) throw Error{error_code::internal_error, "client not connected"};
    const std::string method{method_notifications_prompts_list_changed};
    if (handler) {
        session->set_notification_handler(method,
            [h = std::move(handler)](const nlohmann::json&) { h(); });
    } else {
        session->clear_notification_handler(method);
    }
}

// =====================================================================
// Cancellation, ping, log, progress
// =====================================================================

std::error_code Client::cancel_request(RequestId request_id,
                                       std::optional<std::string> reason) {
    auto session = acquire_session();
    if (!session) {
        throw Error{error_code::internal_error, "client not connected"};
    }
    CancelledNotificationParams params{
        .request_id = std::move(request_id),
        .reason     = std::move(reason),
    };
    return session->send_notification(
        std::string{method_notifications_cancelled},
        nlohmann::json(params));
}

std::future<void> Client::ping() {
    auto session = acquire_session();
    if (!session) throw Error{error_code::internal_error, "client not connected"};
    auto inner = session->send_request(std::string{method_ping}, nullptr);
    return std::async(std::launch::async,
        [inner = std::move(inner)]() mutable {
            (void)inner.get();
        });
}

std::future<void> Client::set_log_level(LoggingLevel level) {
    auto session = acquire_session();
    if (!session) throw Error{error_code::internal_error, "client not connected"};
    SetLevelRequestParams params{.level = level};
    auto inner = session->send_request(std::string{method_logging_set_level},
                                        nlohmann::json(params));
    return std::async(std::launch::async,
        [inner = std::move(inner)]() mutable { (void)inner.get(); });
}

void Client::set_log_message_handler(LogMessageHandler handler) {
    auto session = acquire_session();
    if (!session) throw Error{error_code::internal_error, "client not connected"};
    const std::string method{method_notifications_message};
    if (handler) {
        session->set_notification_handler(method,
            [h = std::move(handler)](const nlohmann::json& params) {
                if (params.is_null()) return;
                h(params.get<LoggingMessageNotificationParams>());
            });
    } else {
        session->clear_notification_handler(method);
    }
}

void Client::set_progress_handler(ProgressHandler handler) {
    auto session = acquire_session();
    if (!session) throw Error{error_code::internal_error, "client not connected"};
    const std::string method{method_notifications_progress};
    if (handler) {
        session->set_notification_handler(method,
            [h = std::move(handler)](const nlohmann::json& params) {
                if (params.is_null()) return;
                h(params.get<ProgressNotificationParams>());
            });
    } else {
        session->clear_notification_handler(method);
    }
}

// =====================================================================
// Sampling, roots, completion
// =====================================================================

void Client::set_sampling_handler(SamplingHandler handler) {
    auto session = acquire_session();
    if (!session) throw Error{error_code::internal_error, "client not connected"};
    {
        std::lock_guard<std::mutex> lk(handlers_mu_);
        sampling_handler_ = handler;
    }
    const std::string method{method_sampling_create_message};
    if (handler) {
        session->set_request_handler(method,
            [h = std::move(handler)](const nlohmann::json& params) -> nlohmann::json {
                if (params.is_null()) {
                    throw Error{error_code::invalid_params,
                                "sampling/createMessage requires params"};
                }
                CreateMessageRequestParams parsed;
                try {
                    parsed = params.get<CreateMessageRequestParams>();
                } catch (const std::exception& e) {
                    throw Error{error_code::invalid_params,
                                std::string{"invalid sampling params: "} + e.what()};
                }
                return h(parsed);
            });
    } else {
        session->clear_request_handler(method);
    }
}

void Client::set_elicitation_handler(ElicitationHandler handler) {
    auto session = acquire_session();
    if (!session) throw Error{error_code::internal_error, "client not connected"};
    {
        std::lock_guard<std::mutex> lk(handlers_mu_);
        elicitation_handler_ = handler;
    }
    const std::string method{method_elicitation_create};
    if (handler) {
        session->set_request_handler(method,
            [h = std::move(handler)](const nlohmann::json& params)
                    -> nlohmann::json {
                if (params.is_null()) {
                    throw Error{error_code::invalid_params,
                                "elicitation/create requires params"};
                }
                ElicitRequestParams parsed;
                try {
                    parsed = params.get<ElicitRequestParams>();
                } catch (const Error&) {
                    throw;
                } catch (const std::exception& e) {
                    throw Error{error_code::invalid_params,
                                std::string{"invalid elicitation params: "}
                                    + e.what()};
                }
                return h(parsed);
            });
    } else {
        session->clear_request_handler(method);
    }
}

void Client::set_elicitation_complete_handler(
    ElicitationCompleteHandler handler) {
    auto session = acquire_session();
    if (!session) throw Error{error_code::internal_error, "client not connected"};
    {
        std::lock_guard<std::mutex> lk(handlers_mu_);
        elicitation_complete_handler_ = handler;
    }
    const std::string method{method_notifications_elicitation_complete};
    if (handler) {
        session->set_notification_handler(method,
            [h = std::move(handler)](const nlohmann::json& params) {
                if (!params.is_object()) return;
                ElicitationCompleteNotificationParams parsed;
                try {
                    parsed = params.get<ElicitationCompleteNotificationParams>();
                } catch (const std::exception& e) {
                    MCP_LOG_WARN(std::string{"malformed elicitation/complete: "}
                                 + e.what());
                    return;
                } catch (...) {
                    MCP_LOG_WARN("malformed elicitation/complete");
                    return;
                }
                h(std::move(parsed.elicitation_id));
            });
    } else {
        session->clear_notification_handler(method);
    }
}

void Client::set_roots_list_handler(RootsListHandler handler) {
    auto session = acquire_session();
    if (!session) throw Error{error_code::internal_error, "client not connected"};
    {
        std::lock_guard<std::mutex> lk(handlers_mu_);
        roots_handler_ = handler;
    }
    const std::string method{method_roots_list};
    if (handler) {
        session->set_request_handler(method,
            [h = std::move(handler)](const nlohmann::json&) -> nlohmann::json {
                return h();
            });
    } else {
        session->clear_request_handler(method);
    }
}

std::future<CompleteResult>
Client::complete(CompletionReference reference,
                 CompleteArgument    argument,
                 std::optional<std::unordered_map<std::string, std::string>> context_arguments) {
    auto session = acquire_session();
    if (!session) throw Error{error_code::internal_error, "client not connected"};
    CompleteRequestParams params{
        .reference         = std::move(reference),
        .argument          = std::move(argument),
        .context_arguments = std::move(context_arguments),
    };
    auto inner = session->send_request(std::string{method_completion_complete},
                                        nlohmann::json(params));
    return std::async(std::launch::async,
        [inner = std::move(inner)]() mutable -> CompleteResult {
            return inner.get().get<CompleteResult>();
        });
}

void Client::set_client_capabilities(ClientCapabilities caps) {
    std::lock_guard<std::mutex> lk(handlers_mu_);
    capabilities_override_ = std::move(caps);
}

std::error_code Client::notify_roots_list_changed() {
    auto session = acquire_session();
    if (!session) {
        return std::make_error_code(std::errc::not_connected);
    }
    return session->send_notification(
        std::string{method_notifications_roots_list_changed});
}

std::error_code
Client::notify_elicitation_complete(std::string elicitation_id) {
    auto session = acquire_session();
    if (!session) return std::make_error_code(std::errc::not_connected);
    return session->send_notification(
        std::string{method_notifications_elicitation_complete},
        nlohmann::json(ElicitationCompleteNotificationParams{
            .elicitation_id = std::move(elicitation_id),
        }));
}

}  // namespace mcp
