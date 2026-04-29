// SPDX-License-Identifier: Apache-2.0
#include "mcp/server.hpp"

#include "mcp/error.hpp"
#include "mcp/log.hpp"
#include "mcp/protocol.hpp"
#include "mcp/session.hpp"
#include "mcp/transport.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <limits>
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

Server& Server::set_page_size(std::size_t n) {
    page_size_ = n;
    return *this;
}

namespace {
// Decode an offset cursor. Empty / missing cursor means start at 0.
// Throws Error(invalid_params) on malformed cursors.
//
// The decode is strict: only ASCII digits, no sign character, no
// leading/trailing whitespace, no overflow. std::stoull accepts "-1"
// (silently wrapping to UINT64_MAX) and trailing junk like "1abc",
// neither of which we want to treat as a valid offset.
std::size_t decode_cursor(const std::optional<std::string>& cursor) {
    if (!cursor.has_value() || cursor->empty()) return 0;
    const auto& s = *cursor;
    if (s.size() > 20) {  // longer than UINT64_MAX's decimal length
        throw Error{error_code::invalid_params,
                    "invalid pagination cursor: " + s};
    }
    std::uint64_t value = 0;
    for (char c : s) {
        if (c < '0' || c > '9') {
            throw Error{error_code::invalid_params,
                        "invalid pagination cursor: " + s};
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            throw Error{error_code::invalid_params,
                        "invalid pagination cursor: " + s};
        }
        value = value * 10 + digit;
    }
    return static_cast<std::size_t>(value);
}

// Slice [items_begin, items_end) into a page. If page_size is 0 or
// items.size() <= offset+page_size, returns the remainder with no
// nextCursor; otherwise returns a slice of size page_size and a
// `nextCursor` for the next call.
template <typename T>
std::pair<std::vector<T>, std::optional<std::string>>
paginate(const std::vector<T>& items, std::size_t offset, std::size_t page_size) {
    if (offset >= items.size()) return {{}, std::nullopt};
    if (page_size == 0 || offset + page_size >= items.size()) {
        return {{items.begin() + static_cast<std::ptrdiff_t>(offset), items.end()},
                std::nullopt};
    }
    const std::size_t next = offset + page_size;
    return {{items.begin() + static_cast<std::ptrdiff_t>(offset),
             items.begin() + static_cast<std::ptrdiff_t>(next)},
            std::to_string(next)};
}
}  // namespace

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

Server& Server::resource(Resource descriptor, ResourceReadHandler handler) {
    std::lock_guard<std::mutex> lk(resources_mu_);
    const std::string uri = descriptor.uri;
    resources_[uri] = ResourceEntry{std::move(descriptor), std::move(handler)};
    return *this;
}

Server& Server::resource_template(ResourceTemplate descriptor) {
    std::lock_guard<std::mutex> lk(resources_mu_);
    resource_templates_.push_back(std::move(descriptor));
    return *this;
}

Server& Server::fallback_resource_handler(ResourceReadHandler handler) {
    std::lock_guard<std::mutex> lk(resources_mu_);
    fallback_resource_handler_ = std::move(handler);
    return *this;
}

Server& Server::prompt(Prompt descriptor, PromptGetHandler handler) {
    std::lock_guard<std::mutex> lk(prompts_mu_);
    const std::string name = descriptor.name;
    prompts_[name] = PromptEntry{std::move(descriptor), std::move(handler)};
    return *this;
}

Server& Server::enable_logging(LoggingLevel initial_level) {
    logging_enabled_.store(true, std::memory_order_release);
    log_level_.store(initial_level, std::memory_order_release);
    return *this;
}

bool Server::log(LoggingLevel               level,
                 nlohmann::json             data,
                 std::optional<std::string> logger) {
    if (!logging_enabled_.load(std::memory_order_acquire)) return false;
    if (static_cast<int>(level) <
        static_cast<int>(log_level_.load(std::memory_order_acquire))) return false;
    auto session = acquire_session();
    if (!session) return false;
    LoggingMessageNotificationParams params{
        .level  = level,
        .logger = std::move(logger),
        .data   = std::move(data),
    };
    auto ec = session->send_notification(
        std::string{method_notifications_message},
        nlohmann::json(params));
    return !ec;
}

void Server::report_progress(const ProgressToken&        token,
                             double                      progress,
                             std::optional<double>       total,
                             std::optional<std::string>  message) {
    auto session = acquire_session();
    if (!session) return;
    ProgressNotificationParams params{
        .progress_token = token,
        .progress       = progress,
        .total          = total,
        .message        = std::move(message),
    };
    (void)session->send_notification(
        std::string{method_notifications_progress},
        nlohmann::json(params));
}

std::future<CreateMessageResult>
Server::sample(CreateMessageRequestParams params) {
    auto session = acquire_session();
    if (!session) {
        throw Error{error_code::internal_error,
                    "Server::sample: server is not running"};
    }
    auto inner = session->send_request(
        std::string{method_sampling_create_message},
        nlohmann::json(params));
    return std::async(std::launch::async,
        [inner = std::move(inner)]() mutable -> CreateMessageResult {
            return inner.get().get<CreateMessageResult>();
        });
}

std::future<ListRootsResult> Server::list_roots() {
    auto session = acquire_session();
    if (!session) {
        throw Error{error_code::internal_error,
                    "Server::list_roots: server is not running"};
    }
    auto inner = session->send_request(std::string{method_roots_list}, nullptr);
    return std::async(std::launch::async,
        [inner = std::move(inner)]() mutable -> ListRootsResult {
            return inner.get().get<ListRootsResult>();
        });
}

std::shared_ptr<Session> Server::acquire_session() const {
    std::lock_guard<std::mutex> lk(session_mu_);
    return session_;
}

Server& Server::enable_completion(CompletionHandler handler) {
    std::lock_guard<std::mutex> lk(completion_mu_);
    completion_handler_ = std::move(handler);
    return *this;
}

// =====================================================================
// Handlers
// =====================================================================

nlohmann::json Server::handle_initialize(const nlohmann::json& params) {
    InitializeRequestParams parsed;
    try {
        parsed = params.get<InitializeRequestParams>();
    } catch (const std::exception& e) {
        throw Error{error_code::invalid_params,
                    std::string{"invalid initialize params: "} + e.what()};
    }

    // Atomically claim the "initialized" slot — only the first concurrent
    // initialize call may pass through, even if multiple arrived on
    // different worker threads.
    bool expected = false;
    if (!initialized_.compare_exchange_strong(expected, true,
                                              std::memory_order_acq_rel)) {
        throw Error{error_code::invalid_request, "already initialized"};
    }

    // Per spec: if we don't support the client's protocol version, we
    // respond with the version we *do* support; the client may then
    // disconnect. For Phase 1 we simply echo our own latest.
    InitializeResult result{};
    result.protocol_version = std::string{kLatestProtocolVersion};
    result.server_info      = server_info_;
    result.instructions     = instructions_;

    // Capabilities: we offer each capability iff something is registered
    // for it. listChanged is unset because we don't yet emit those
    // notifications (Phase 3).
    {
        std::lock_guard<std::mutex> lk(tools_mu_);
        if (!tools_.empty()) result.capabilities.tools = ToolsCapability{};
    }
    {
        std::lock_guard<std::mutex> lk(resources_mu_);
        if (!resources_.empty() || !resource_templates_.empty() ||
            fallback_resource_handler_) {
            result.capabilities.resources = ResourcesCapability{};
        }
    }
    {
        std::lock_guard<std::mutex> lk(prompts_mu_);
        if (!prompts_.empty()) result.capabilities.prompts = PromptsCapability{};
    }
    if (logging_enabled_.load(std::memory_order_acquire)) {
        result.capabilities.logging = nlohmann::json::object();
    }
    {
        std::lock_guard<std::mutex> lk(completion_mu_);
        if (completion_handler_) {
            result.capabilities.completions = nlohmann::json::object();
        }
    }

    return result;
}

nlohmann::json Server::handle_list_tools(const nlohmann::json& params) {
    if (!initialized_.load(std::memory_order_acquire)) {
        throw Error{error_code::invalid_request, "not initialized"};
    }
    auto parsed = params.is_null()
                      ? ListToolsRequestParams{}
                      : params.get<ListToolsRequestParams>();
    std::vector<Tool> all;
    {
        std::lock_guard<std::mutex> lk(tools_mu_);
        all.reserve(tools_.size());
        for (const auto& [name, entry] : tools_) all.push_back(entry.descriptor);
    }
    auto [page, next] = paginate(all, decode_cursor(parsed.cursor), page_size_);
    return ListToolsResult{.tools = std::move(page), .next_cursor = std::move(next)};
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

nlohmann::json Server::handle_list_resources(const nlohmann::json& params) {
    if (!initialized_.load(std::memory_order_acquire)) {
        throw Error{error_code::invalid_request, "not initialized"};
    }
    auto parsed = params.is_null()
                      ? ListResourcesRequestParams{}
                      : params.get<ListResourcesRequestParams>();
    std::vector<Resource> all;
    {
        std::lock_guard<std::mutex> lk(resources_mu_);
        all.reserve(resources_.size());
        for (const auto& [uri, entry] : resources_) all.push_back(entry.descriptor);
    }
    auto [page, next] = paginate(all, decode_cursor(parsed.cursor), page_size_);
    return ListResourcesResult{.resources = std::move(page), .next_cursor = std::move(next)};
}

nlohmann::json Server::handle_list_resource_templates(const nlohmann::json& params) {
    if (!initialized_.load(std::memory_order_acquire)) {
        throw Error{error_code::invalid_request, "not initialized"};
    }
    auto parsed = params.is_null()
                      ? ListResourceTemplatesRequestParams{}
                      : params.get<ListResourceTemplatesRequestParams>();
    std::vector<ResourceTemplate> all;
    {
        std::lock_guard<std::mutex> lk(resources_mu_);
        all = resource_templates_;
    }
    auto [page, next] = paginate(all, decode_cursor(parsed.cursor), page_size_);
    return ListResourceTemplatesResult{.resource_templates = std::move(page),
                                       .next_cursor        = std::move(next)};
}

nlohmann::json Server::handle_read_resource(const nlohmann::json& params) {
    if (!initialized_.load(std::memory_order_acquire)) {
        throw Error{error_code::invalid_request, "not initialized"};
    }
    ReadResourceRequestParams parsed;
    try {
        parsed = params.get<ReadResourceRequestParams>();
    } catch (const std::exception& e) {
        throw Error{error_code::invalid_params,
                    std::string{"invalid resources/read params: "} + e.what()};
    }

    ResourceReadHandler h;
    {
        std::lock_guard<std::mutex> lk(resources_mu_);
        auto it = resources_.find(parsed.uri);
        if (it != resources_.end()) {
            h = it->second.handler;
        } else {
            h = fallback_resource_handler_;
        }
    }
    if (!h) {
        throw Error{error_code::method_not_found,
                    "resource not found: " + parsed.uri};
    }
    return h(parsed.uri);
}

nlohmann::json Server::handle_list_prompts(const nlohmann::json& params) {
    if (!initialized_.load(std::memory_order_acquire)) {
        throw Error{error_code::invalid_request, "not initialized"};
    }
    auto parsed = params.is_null()
                      ? ListPromptsRequestParams{}
                      : params.get<ListPromptsRequestParams>();
    std::vector<Prompt> all;
    {
        std::lock_guard<std::mutex> lk(prompts_mu_);
        all.reserve(prompts_.size());
        for (const auto& [name, entry] : prompts_) all.push_back(entry.descriptor);
    }
    auto [page, next] = paginate(all, decode_cursor(parsed.cursor), page_size_);
    return ListPromptsResult{.prompts = std::move(page), .next_cursor = std::move(next)};
}

nlohmann::json Server::handle_get_prompt(const nlohmann::json& params) {
    if (!initialized_.load(std::memory_order_acquire)) {
        throw Error{error_code::invalid_request, "not initialized"};
    }
    GetPromptRequestParams parsed;
    try {
        parsed = params.get<GetPromptRequestParams>();
    } catch (const std::exception& e) {
        throw Error{error_code::invalid_params,
                    std::string{"invalid prompts/get params: "} + e.what()};
    }

    PromptGetHandler h;
    {
        std::lock_guard<std::mutex> lk(prompts_mu_);
        auto it = prompts_.find(parsed.name);
        if (it != prompts_.end()) h = it->second.handler;
    }
    if (!h) {
        throw Error{error_code::method_not_found,
                    "prompt not found: " + parsed.name};
    }
    const auto args = parsed.arguments.value_or(
        std::unordered_map<std::string, std::string>{});
    return h(args);
}

nlohmann::json Server::handle_ping(const nlohmann::json& /*params*/) {
    return nlohmann::json::object();
}

nlohmann::json Server::handle_set_level(const nlohmann::json& params) {
    if (!logging_enabled_.load(std::memory_order_acquire)) {
        throw Error{error_code::method_not_found,
                    "logging capability is not enabled"};
    }
    SetLevelRequestParams parsed;
    try {
        parsed = params.get<SetLevelRequestParams>();
    } catch (const std::exception& e) {
        throw Error{error_code::invalid_params,
                    std::string{"invalid logging/setLevel: "} + e.what()};
    }
    log_level_.store(parsed.level, std::memory_order_release);
    return nlohmann::json::object();
}

nlohmann::json Server::handle_complete(const nlohmann::json& params) {
    if (!initialized_.load(std::memory_order_acquire)) {
        throw Error{error_code::invalid_request, "not initialized"};
    }
    CompletionHandler h;
    {
        std::lock_guard<std::mutex> lk(completion_mu_);
        h = completion_handler_;
    }
    if (!h) {
        throw Error{error_code::method_not_found,
                    "completion is not enabled on this server"};
    }
    CompleteRequestParams parsed;
    try {
        parsed = params.get<CompleteRequestParams>();
    } catch (const std::exception& e) {
        throw Error{error_code::invalid_params,
                    std::string{"invalid completion/complete params: "} + e.what()};
    }
    return CompleteResult{.completion = h(parsed)};
}

void Server::handle_cancelled(const nlohmann::json& params) {
    // The Server cannot abort an in-flight handler in Phase 2; we log
    // the cancellation so users know it arrived. Phase 3 will plumb a
    // cancellation token through to ToolHandler etc.
    try {
        auto p = params.get<CancelledNotificationParams>();
        if (p.request_id.has_value()) {
            MCP_LOG_INFO("client cancelled request id=" + p.request_id->canonical()
                         + (p.reason ? (" reason=" + *p.reason) : ""));
        }
    } catch (...) {
        MCP_LOG_WARN("malformed notifications/cancelled payload");
    }
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

    auto local = std::make_shared<Session>(std::move(transport));
    {
        std::lock_guard<std::mutex> lk(session_mu_);
        session_ = local;
    }

    local->set_request_handler(std::string{method_initialize},
        [this](const nlohmann::json& p) { return handle_initialize(p); });
    local->set_request_handler(std::string{method_tools_list},
        [this](const nlohmann::json& p) { return handle_list_tools(p); });
    local->set_request_handler(std::string{method_tools_call},
        [this](const nlohmann::json& p) { return handle_call_tool(p); });
    local->set_request_handler(std::string{method_resources_list},
        [this](const nlohmann::json& p) { return handle_list_resources(p); });
    local->set_request_handler(std::string{method_resources_templates_list},
        [this](const nlohmann::json& p) { return handle_list_resource_templates(p); });
    local->set_request_handler(std::string{method_resources_read},
        [this](const nlohmann::json& p) { return handle_read_resource(p); });
    local->set_request_handler(std::string{method_prompts_list},
        [this](const nlohmann::json& p) { return handle_list_prompts(p); });
    local->set_request_handler(std::string{method_prompts_get},
        [this](const nlohmann::json& p) { return handle_get_prompt(p); });
    local->set_request_handler(std::string{method_ping},
        [this](const nlohmann::json& p) { return handle_ping(p); });
    local->set_request_handler(std::string{method_logging_set_level},
        [this](const nlohmann::json& p) { return handle_set_level(p); });
    local->set_request_handler(std::string{method_completion_complete},
        [this](const nlohmann::json& p) { return handle_complete(p); });

    local->set_notification_handler(std::string{method_notifications_cancelled},
        [this](const nlohmann::json& p) { handle_cancelled(p); });

    local->set_notification_handler(std::string{method_notifications_initialized},
        [](const nlohmann::json&) {
            // Per the spec, this notification just confirms the client is
            // ready for normal operation. We simply log it.
            MCP_LOG_DEBUG("client sent notifications/initialized");
        });

    local->set_on_closed([this]() {
        std::lock_guard<std::mutex> lk(stop_mu_);
        stop_cv_.notify_all();
    });

    local->start();

    {
        std::unique_lock<std::mutex> lk(stop_mu_);
        stop_cv_.wait(lk, [this, &local] {
            return stop_requested_.load(std::memory_order_acquire) ||
                   !local->is_open();
        });
    }
    local->close();
    {
        std::lock_guard<std::mutex> lk(session_mu_);
        session_.reset();
    }
}

void Server::stop() {
    {
        std::lock_guard<std::mutex> lk(stop_mu_);
        stop_requested_.store(true, std::memory_order_release);
    }
    stop_cv_.notify_all();
}

}  // namespace mcp
