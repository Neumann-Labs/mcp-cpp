// SPDX-License-Identifier: Apache-2.0
#include "mcp/server.hpp"

#include "mcp/error.hpp"
#include "mcp/log.hpp"
#include "mcp/protocol.hpp"
#include "mcp/session.hpp"
#include "mcp/transport.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace mcp {

Server::Server(Implementation server_info)
    : server_info_(std::move(server_info)) {}

Server& Server::set_instructions(std::string s) {
    instructions_ = std::move(s);
    return *this;
}

Server& Server::tool(std::string                    name,
                     nlohmann::json                 input_schema,
                     ToolHandler                    handler,
                     std::optional<std::string>     title,
                     std::optional<std::string>     description,
                     std::optional<ToolAnnotations> annotations,
                     std::optional<nlohmann::json>  output_schema) {
    Tool t{
        .name          = name,
        .title         = std::move(title),
        .description   = std::move(description),
        .input_schema  = std::move(input_schema),
        .output_schema = std::move(output_schema),
        .annotations   = std::move(annotations),
    };
    std::lock_guard<std::mutex> lk(tools_mu_);
    tools_[name] = ToolEntry{std::move(t), std::move(handler)};
    return *this;
}

// =====================================================================
// Handlers
// =====================================================================

nlohmann::json Server::handle_initialize(const nlohmann::json& params) {
    if (initialized_.load(std::memory_order_acquire)) {
        throw Error{error_code::invalid_request, "already initialized"};
    }
    InitializeRequestParams parsed;
    try {
        parsed = params.get<InitializeRequestParams>();
    } catch (const std::exception& e) {
        throw Error{error_code::invalid_params,
                    std::string{"invalid initialize params: "} + e.what()};
    }

    // Per spec: if we don't support the client's protocol version, we
    // respond with the version we *do* support; the client may then
    // disconnect. For Phase 1 we simply echo our own latest.
    InitializeResult result{};
    result.protocol_version = std::string{kLatestProtocolVersion};
    result.server_info      = server_info_;
    result.instructions     = instructions_;

    // Capabilities: we offer "tools" iff at least one tool is registered.
    std::lock_guard<std::mutex> lk(tools_mu_);
    if (!tools_.empty()) {
        result.capabilities.tools = ToolsCapability{};
    }

    initialized_.store(true, std::memory_order_release);
    return result;
}

nlohmann::json Server::handle_list_tools(const nlohmann::json& params) {
    if (!initialized_.load(std::memory_order_acquire)) {
        throw Error{error_code::invalid_request, "not initialized"};
    }
    // Pagination is not yet implemented; we ignore an inbound cursor and
    // always return the full list with no nextCursor.
    (void)params;

    ListToolsResult res;
    {
        std::lock_guard<std::mutex> lk(tools_mu_);
        res.tools.reserve(tools_.size());
        for (const auto& [name, entry] : tools_) res.tools.push_back(entry.descriptor);
    }
    return res;
}

nlohmann::json Server::handle_call_tool(const nlohmann::json& params) {
    if (!initialized_.load(std::memory_order_acquire)) {
        throw Error{error_code::invalid_request, "not initialized"};
    }
    CallToolRequestParams parsed;
    try {
        parsed = params.get<CallToolRequestParams>();
    } catch (const std::exception& e) {
        throw Error{error_code::invalid_params,
                    std::string{"invalid tools/call params: "} + e.what()};
    }

    ToolHandler h;
    {
        std::lock_guard<std::mutex> lk(tools_mu_);
        auto it = tools_.find(parsed.name);
        if (it == tools_.end()) {
            throw Error{error_code::method_not_found,
                        "tool not found: " + parsed.name};
        }
        h = it->second.handler;
    }
    const auto args = parsed.arguments.value_or(nlohmann::json::object());
    auto result = h(args);
    return result;
}

// =====================================================================
// Lifecycle
// =====================================================================

void Server::run(std::unique_ptr<Transport> transport) {
    if (!transport) {
        throw Error{error_code::internal_error, "Server::run: transport is null"};
    }
    initialized_.store(false, std::memory_order_release);
    stop_requested_.store(false, std::memory_order_release);

    session_ = std::make_unique<Session>(std::move(transport));

    session_->set_request_handler(std::string{method_initialize},
        [this](const nlohmann::json& p) { return handle_initialize(p); });
    session_->set_request_handler(std::string{method_tools_list},
        [this](const nlohmann::json& p) { return handle_list_tools(p); });
    session_->set_request_handler(std::string{method_tools_call},
        [this](const nlohmann::json& p) { return handle_call_tool(p); });

    session_->set_notification_handler(std::string{method_notifications_initialized},
        [](const nlohmann::json&) {
            // Per the spec, this notification just confirms the client is
            // ready for normal operation. We simply log it.
            MCP_LOG_DEBUG("client sent notifications/initialized");
        });

    session_->set_on_closed([this]() {
        std::lock_guard<std::mutex> lk(stop_mu_);
        stop_cv_.notify_all();
    });

    session_->start();

    {
        std::unique_lock<std::mutex> lk(stop_mu_);
        stop_cv_.wait(lk, [this] {
            return stop_requested_.load(std::memory_order_acquire) ||
                   !session_->is_open();
        });
    }
    session_->close();
    session_.reset();
}

void Server::stop() {
    {
        std::lock_guard<std::mutex> lk(stop_mu_);
        stop_requested_.store(true, std::memory_order_release);
    }
    stop_cv_.notify_all();
}

}  // namespace mcp
