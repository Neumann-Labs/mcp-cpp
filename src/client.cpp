// SPDX-License-Identifier: Apache-2.0
#include "mcp/client.hpp"

#include "mcp/error.hpp"
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
    session_ = std::make_unique<Session>(std::move(transport));
    session_->start();
    connected_.store(true, std::memory_order_release);
}

void Client::disconnect() {
    if (!connected_.exchange(false, std::memory_order_acq_rel)) return;
    if (session_) {
        session_->close();
        session_.reset();
    }
    {
        std::lock_guard<std::mutex> lk(server_mu_);
        server_.reset();
    }
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
    if (!session_) {
        throw Error{error_code::internal_error, "client not connected"};
    }
    InitializeRequestParams params{
        .protocol_version = std::string{kLatestProtocolVersion},
        .capabilities     = {},
        .client_info      = client_info_,
    };

    auto* session = session_.get();
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
    if (!session_) {
        throw Error{error_code::internal_error, "client not connected"};
    }
    ListToolsRequestParams params{.cursor = std::move(cursor)};
    auto inner = session_->send_request(std::string{method_tools_list},
                                        nlohmann::json(params));
    return std::async(std::launch::async,
        [inner = std::move(inner)]() mutable -> ListToolsResult {
            return inner.get().get<ListToolsResult>();
        });
}

std::future<CallToolResult>
Client::call_tool(std::string name, nlohmann::json arguments) {
    if (!session_) {
        throw Error{error_code::internal_error, "client not connected"};
    }
    CallToolRequestParams params{
        .name      = std::move(name),
        .arguments = arguments.is_null() ? std::nullopt
                                         : std::optional<nlohmann::json>(std::move(arguments)),
    };
    auto inner = session_->send_request(std::string{method_tools_call},
                                        nlohmann::json(params));
    return std::async(std::launch::async,
        [inner = std::move(inner)]() mutable -> CallToolResult {
            return inner.get().get<CallToolResult>();
        });
}

// =====================================================================
// resources
// =====================================================================

std::future<ListResourcesResult>
Client::list_resources(std::optional<std::string> cursor) {
    if (!session_) throw Error{error_code::internal_error, "client not connected"};
    ListResourcesRequestParams params{.cursor = std::move(cursor)};
    auto inner = session_->send_request(std::string{method_resources_list},
                                        nlohmann::json(params));
    return std::async(std::launch::async,
        [inner = std::move(inner)]() mutable -> ListResourcesResult {
            return inner.get().get<ListResourcesResult>();
        });
}

std::future<ListResourceTemplatesResult>
Client::list_resource_templates(std::optional<std::string> cursor) {
    if (!session_) throw Error{error_code::internal_error, "client not connected"};
    ListResourceTemplatesRequestParams params{.cursor = std::move(cursor)};
    auto inner = session_->send_request(std::string{method_resources_templates_list},
                                        nlohmann::json(params));
    return std::async(std::launch::async,
        [inner = std::move(inner)]() mutable -> ListResourceTemplatesResult {
            return inner.get().get<ListResourceTemplatesResult>();
        });
}

std::future<ReadResourceResult>
Client::read_resource(std::string uri) {
    if (!session_) throw Error{error_code::internal_error, "client not connected"};
    ReadResourceRequestParams params{.uri = std::move(uri)};
    auto inner = session_->send_request(std::string{method_resources_read},
                                        nlohmann::json(params));
    return std::async(std::launch::async,
        [inner = std::move(inner)]() mutable -> ReadResourceResult {
            return inner.get().get<ReadResourceResult>();
        });
}

std::future<nlohmann::json>
Client::subscribe(std::string uri) {
    if (!session_) throw Error{error_code::internal_error, "client not connected"};
    SubscribeRequestParams params{.uri = std::move(uri)};
    return session_->send_request(std::string{method_resources_subscribe},
                                  nlohmann::json(params));
}

std::future<nlohmann::json>
Client::unsubscribe(std::string uri) {
    if (!session_) throw Error{error_code::internal_error, "client not connected"};
    UnsubscribeRequestParams params{.uri = std::move(uri)};
    return session_->send_request(std::string{method_resources_unsubscribe},
                                  nlohmann::json(params));
}

void Client::set_resource_updated_handler(ResourceUpdatedHandler handler) {
    if (!session_) throw Error{error_code::internal_error, "client not connected"};
    if (handler) {
        session_->set_notification_handler(
            std::string{method_notifications_resources_updated},
            [h = std::move(handler)](const nlohmann::json& params) {
                if (params.is_null()) return;
                ResourceUpdatedNotificationParams parsed = params.get<ResourceUpdatedNotificationParams>();
                h(parsed);
            });
    }
}

void Client::set_resources_list_changed_handler(ListChangedHandler handler) {
    if (!session_) throw Error{error_code::internal_error, "client not connected"};
    if (handler) {
        session_->set_notification_handler(
            std::string{method_notifications_resources_list_changed},
            [h = std::move(handler)](const nlohmann::json&) { h(); });
    }
}

// =====================================================================
// prompts
// =====================================================================

std::future<ListPromptsResult>
Client::list_prompts(std::optional<std::string> cursor) {
    if (!session_) throw Error{error_code::internal_error, "client not connected"};
    ListPromptsRequestParams params{.cursor = std::move(cursor)};
    auto inner = session_->send_request(std::string{method_prompts_list},
                                        nlohmann::json(params));
    return std::async(std::launch::async,
        [inner = std::move(inner)]() mutable -> ListPromptsResult {
            return inner.get().get<ListPromptsResult>();
        });
}

std::future<GetPromptResult>
Client::get_prompt(std::string name,
                   std::optional<std::unordered_map<std::string, std::string>> arguments) {
    if (!session_) throw Error{error_code::internal_error, "client not connected"};
    GetPromptRequestParams params{
        .name      = std::move(name),
        .arguments = std::move(arguments),
    };
    auto inner = session_->send_request(std::string{method_prompts_get},
                                        nlohmann::json(params));
    return std::async(std::launch::async,
        [inner = std::move(inner)]() mutable -> GetPromptResult {
            return inner.get().get<GetPromptResult>();
        });
}

void Client::set_prompts_list_changed_handler(ListChangedHandler handler) {
    if (!session_) throw Error{error_code::internal_error, "client not connected"};
    if (handler) {
        session_->set_notification_handler(
            std::string{method_notifications_prompts_list_changed},
            [h = std::move(handler)](const nlohmann::json&) { h(); });
    }
}

}  // namespace mcp
