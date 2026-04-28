// SPDX-License-Identifier: Apache-2.0
//
// Wire types for the Model Context Protocol, revision 2025-11-25.
//
// The C++ struct fields are snake_case; the JSON wire format uses the spec's
// camelCase names. JSON serialization is implemented via ADL `to_json` /
// `from_json` free functions defined alongside each type so they can be
// reached through nlohmann::json's `.get<T>()` and implicit-construct paths.
//
// Forward compatibility: unknown JSON fields are silently dropped on read.
// Optional spec fields are represented with `std::optional<T>`; their
// absence on the wire matches `std::nullopt` in C++. Capability presence is
// itself optional ("the capability is offered iff the optional has a value").

#pragma once

#include "mcp/error.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace mcp {

// --------------------------------------------------------------------------
// Constants
// --------------------------------------------------------------------------

inline constexpr std::string_view kLatestProtocolVersion = "2025-11-25";
inline constexpr std::string_view kJsonrpcVersion        = "2.0";

// --------------------------------------------------------------------------
// JSON-RPC envelope
// --------------------------------------------------------------------------

/// A JSON-RPC request id. The spec permits string or number; we preserve the
/// original kind on round-trip so a client that emits integer ids gets back
/// integer ids, and likewise for strings.
class RequestId {
public:
    using Value = std::variant<std::string, std::int64_t>;

    RequestId() : value_(std::int64_t{0}) {}
    RequestId(std::string s)   : value_(std::move(s)) {}
    RequestId(const char* s)   : value_(std::string{s}) {}
    RequestId(std::int64_t i)  : value_(i) {}
    RequestId(int i)           : value_(static_cast<std::int64_t>(i)) {}

    [[nodiscard]] const Value& value() const noexcept { return value_; }

    [[nodiscard]] bool is_string()  const noexcept { return value_.index() == 0; }
    [[nodiscard]] bool is_integer() const noexcept { return value_.index() == 1; }

    [[nodiscard]] const std::string& as_string() const { return std::get<0>(value_); }
    [[nodiscard]] std::int64_t       as_integer() const { return std::get<1>(value_); }

    /// Canonical string form used for hash/map keys. String ids are returned
    /// as-is with a leading 's:' marker; integer ids get an 'i:' marker. This
    /// guarantees that the integer 5 and the string "5" do not collide.
    [[nodiscard]] std::string canonical() const;

    friend bool operator==(const RequestId&, const RequestId&) = default;

private:
    Value value_;
};

void to_json(nlohmann::json& j, const RequestId& id);
void from_json(const nlohmann::json& j, RequestId& id);

/// A JSON-RPC 2.0 request. Always has an id.
struct JsonRpcRequest {
    RequestId                     id;
    std::string                   method;
    std::optional<nlohmann::json> params;
};

void to_json(nlohmann::json& j, const JsonRpcRequest& r);
void from_json(const nlohmann::json& j, JsonRpcRequest& r);

/// A JSON-RPC 2.0 notification. Has no id; sender expects no response.
struct JsonRpcNotification {
    std::string                   method;
    std::optional<nlohmann::json> params;
};

void to_json(nlohmann::json& j, const JsonRpcNotification& n);
void from_json(const nlohmann::json& j, JsonRpcNotification& n);

/// A JSON-RPC 2.0 response. Carries either a `result` or an `error`. Per
/// the spec the id is REQUIRED on success, and `null`/absent only on parse
/// errors that prevented identifying the request — we model that with
/// `std::optional<RequestId>`.
struct JsonRpcResponse {
    std::optional<RequestId>                       id;
    std::variant<nlohmann::json /*result*/,
                 ErrorObject    /*error*/>         outcome;

    [[nodiscard]] bool is_error()   const noexcept { return outcome.index() == 1; }
    [[nodiscard]] bool is_success() const noexcept { return outcome.index() == 0; }

    [[nodiscard]] const nlohmann::json& result() const {
        return std::get<0>(outcome);
    }
    [[nodiscard]] const ErrorObject& error() const {
        return std::get<1>(outcome);
    }
};

void to_json(nlohmann::json& j, const JsonRpcResponse& r);
void from_json(const nlohmann::json& j, JsonRpcResponse& r);

/// Tagged union over every JSON-RPC frame we accept off the wire. Use
/// `std::visit` to dispatch on the concrete kind.
using JsonRpcMessage = std::variant<JsonRpcRequest,
                                    JsonRpcNotification,
                                    JsonRpcResponse>;

/// Parse a single JSON-RPC frame. Returns the variant; throws
/// `nlohmann::json::exception` on parse error or `mcp::Error(parse_error)` on
/// envelope-shape errors that prevent classification.
[[nodiscard]] JsonRpcMessage parse_message(const nlohmann::json& j);

/// Serialize a JSON-RPC frame.
[[nodiscard]] nlohmann::json serialize_message(const JsonRpcMessage& m);

// --------------------------------------------------------------------------
// Implementation metadata
// --------------------------------------------------------------------------

struct Implementation {
    std::string                name;
    std::optional<std::string> title;
    std::string                version;
    std::optional<std::string> description;
    std::optional<std::string> website_url;
};

void to_json(nlohmann::json& j, const Implementation& i);
void from_json(const nlohmann::json& j, Implementation& i);

// --------------------------------------------------------------------------
// Capabilities
// --------------------------------------------------------------------------

struct RootsCapability {
    std::optional<bool> list_changed;
};
void to_json(nlohmann::json& j, const RootsCapability& c);
void from_json(const nlohmann::json& j, RootsCapability& c);

struct SamplingCapability {
    // Per spec: `context?: object`, `tools?: object`. Presence-only flags;
    // we capture the raw object so future fields round-trip.
    std::optional<nlohmann::json> context;
    std::optional<nlohmann::json> tools;
};
void to_json(nlohmann::json& j, const SamplingCapability& c);
void from_json(const nlohmann::json& j, SamplingCapability& c);

struct ElicitationCapability {
    std::optional<nlohmann::json> form;  // form-based elicitation
    std::optional<nlohmann::json> url;   // URL-based elicitation
};
void to_json(nlohmann::json& j, const ElicitationCapability& c);
void from_json(const nlohmann::json& j, ElicitationCapability& c);

struct ClientCapabilities {
    std::optional<nlohmann::json>        experimental;
    std::optional<RootsCapability>       roots;
    std::optional<SamplingCapability>    sampling;
    std::optional<ElicitationCapability> elicitation;
};
void to_json(nlohmann::json& j, const ClientCapabilities& c);
void from_json(const nlohmann::json& j, ClientCapabilities& c);

struct PromptsCapability {
    std::optional<bool> list_changed;
};
void to_json(nlohmann::json& j, const PromptsCapability& c);
void from_json(const nlohmann::json& j, PromptsCapability& c);

struct ResourcesCapability {
    std::optional<bool> subscribe;
    std::optional<bool> list_changed;
};
void to_json(nlohmann::json& j, const ResourcesCapability& c);
void from_json(const nlohmann::json& j, ResourcesCapability& c);

struct ToolsCapability {
    std::optional<bool> list_changed;
};
void to_json(nlohmann::json& j, const ToolsCapability& c);
void from_json(const nlohmann::json& j, ToolsCapability& c);

struct ServerCapabilities {
    std::optional<nlohmann::json>      experimental;
    std::optional<nlohmann::json>      logging;     // presence-only
    std::optional<nlohmann::json>      completions; // presence-only
    std::optional<PromptsCapability>   prompts;
    std::optional<ResourcesCapability> resources;
    std::optional<ToolsCapability>     tools;
};
void to_json(nlohmann::json& j, const ServerCapabilities& c);
void from_json(const nlohmann::json& j, ServerCapabilities& c);

// --------------------------------------------------------------------------
// initialize / initialized
// --------------------------------------------------------------------------

struct InitializeRequestParams {
    std::string        protocol_version;
    ClientCapabilities capabilities;
    Implementation     client_info;
};
void to_json(nlohmann::json& j, const InitializeRequestParams& p);
void from_json(const nlohmann::json& j, InitializeRequestParams& p);

struct InitializeResult {
    std::string                protocol_version;
    ServerCapabilities         capabilities;
    Implementation             server_info;
    std::optional<std::string> instructions;
};
void to_json(nlohmann::json& j, const InitializeResult& r);
void from_json(const nlohmann::json& j, InitializeResult& r);

inline constexpr std::string_view method_initialize             = "initialize";
inline constexpr std::string_view method_notifications_initialized
    = "notifications/initialized";

// --------------------------------------------------------------------------
// Content blocks
// --------------------------------------------------------------------------

enum class Role : int { user, assistant };
[[nodiscard]] std::string_view to_string(Role r) noexcept;

struct Annotations {
    std::optional<std::vector<Role>> audience;
    std::optional<double>            priority;
    std::optional<std::string>       last_modified;
};
void to_json(nlohmann::json& j, const Annotations& a);
void from_json(const nlohmann::json& j, Annotations& a);

struct TextContent {
    std::string                text;
    std::optional<Annotations> annotations;
};
void to_json(nlohmann::json& j, const TextContent& c);
void from_json(const nlohmann::json& j, TextContent& c);

struct ImageContent {
    std::string                data;       // base64
    std::string                mime_type;
    std::optional<Annotations> annotations;
};
void to_json(nlohmann::json& j, const ImageContent& c);
void from_json(const nlohmann::json& j, ImageContent& c);

struct AudioContent {
    std::string                data;       // base64
    std::string                mime_type;
    std::optional<Annotations> annotations;
};
void to_json(nlohmann::json& j, const AudioContent& c);
void from_json(const nlohmann::json& j, AudioContent& c);

// --------------------------------------------------------------------------
// Resources (must come before ContentBlock; ResourceLink and
// EmbeddedResource are content variants that reference resource shapes)
// --------------------------------------------------------------------------

struct TextResourceContents {
    std::string                uri;
    std::optional<std::string> mime_type;
    std::string                text;
};
void to_json(nlohmann::json& j, const TextResourceContents& c);
void from_json(const nlohmann::json& j, TextResourceContents& c);

struct BlobResourceContents {
    std::string                uri;
    std::optional<std::string> mime_type;
    std::string                blob;  // base64
};
void to_json(nlohmann::json& j, const BlobResourceContents& c);
void from_json(const nlohmann::json& j, BlobResourceContents& c);

/// Resource contents carry either text or a base64 blob; on the wire
/// the discriminator is which member ("text" or "blob") is present.
using ResourceContents = std::variant<TextResourceContents,
                                      BlobResourceContents>;

void to_json(nlohmann::json& j, const ResourceContents& c);
void from_json(const nlohmann::json& j, ResourceContents& c);

struct Resource {
    std::string                 uri;
    std::string                 name;
    std::optional<std::string>  title;
    std::optional<std::string>  description;
    std::optional<std::string>  mime_type;
    std::optional<Annotations>  annotations;
    std::optional<std::int64_t> size;  // bytes
};
void to_json(nlohmann::json& j, const Resource& r);
void from_json(const nlohmann::json& j, Resource& r);

struct ResourceTemplate {
    std::string                uri_template;
    std::string                name;
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::optional<std::string> mime_type;
    std::optional<Annotations> annotations;
};
void to_json(nlohmann::json& j, const ResourceTemplate& r);
void from_json(const nlohmann::json& j, ResourceTemplate& r);

struct ResourceLink {
    std::string                 uri;
    std::string                 name;
    std::optional<std::string>  title;
    std::optional<std::string>  description;
    std::optional<std::string>  mime_type;
    std::optional<Annotations>  annotations;
    std::optional<std::int64_t> size;
};
void to_json(nlohmann::json& j, const ResourceLink& r);
void from_json(const nlohmann::json& j, ResourceLink& r);

struct EmbeddedResource {
    ResourceContents           resource;
    std::optional<Annotations> annotations;
};
void to_json(nlohmann::json& j, const EmbeddedResource& r);
void from_json(const nlohmann::json& j, EmbeddedResource& r);

/// Content blocks used by tools/calls, prompts, and sampling. The
/// type discriminator on the wire is `"text" | "image" | "audio"
/// | "resource_link" | "resource"`.
using ContentBlock = std::variant<TextContent,
                                  ImageContent,
                                  AudioContent,
                                  ResourceLink,
                                  EmbeddedResource>;

void to_json(nlohmann::json& j, const ContentBlock& c);
void from_json(const nlohmann::json& j, ContentBlock& c);

// --------------------------------------------------------------------------
// Tools
// --------------------------------------------------------------------------

struct ToolAnnotations {
    std::optional<std::string> title;
    std::optional<bool>        read_only_hint;
    std::optional<bool>        destructive_hint;
    std::optional<bool>        idempotent_hint;
    std::optional<bool>        open_world_hint;
};
void to_json(nlohmann::json& j, const ToolAnnotations& a);
void from_json(const nlohmann::json& j, ToolAnnotations& a);

struct Tool {
    std::string                    name;
    std::optional<std::string>     title;
    std::optional<std::string>     description;
    nlohmann::json                 input_schema;   // JSON Schema; type:"object"
    std::optional<nlohmann::json>  output_schema;  // optional JSON Schema
    std::optional<ToolAnnotations> annotations;
};
void to_json(nlohmann::json& j, const Tool& t);
void from_json(const nlohmann::json& j, Tool& t);

struct ListToolsRequestParams {
    std::optional<std::string> cursor;
};
void to_json(nlohmann::json& j, const ListToolsRequestParams& p);
void from_json(const nlohmann::json& j, ListToolsRequestParams& p);

struct ListToolsResult {
    std::vector<Tool>          tools;
    std::optional<std::string> next_cursor;
};
void to_json(nlohmann::json& j, const ListToolsResult& r);
void from_json(const nlohmann::json& j, ListToolsResult& r);

struct CallToolRequestParams {
    std::string                   name;
    std::optional<nlohmann::json> arguments;  // JSON object
};
void to_json(nlohmann::json& j, const CallToolRequestParams& p);
void from_json(const nlohmann::json& j, CallToolRequestParams& p);

struct CallToolResult {
    std::vector<ContentBlock>     content;
    std::optional<nlohmann::json> structured_content;  // matches outputSchema
    std::optional<bool>           is_error;
};
void to_json(nlohmann::json& j, const CallToolResult& r);
void from_json(const nlohmann::json& j, CallToolResult& r);

inline constexpr std::string_view method_tools_list = "tools/list";
inline constexpr std::string_view method_tools_call = "tools/call";

// --------------------------------------------------------------------------
// Resource requests / responses
// --------------------------------------------------------------------------

struct ListResourcesRequestParams {
    std::optional<std::string> cursor;
};
void to_json(nlohmann::json& j, const ListResourcesRequestParams& p);
void from_json(const nlohmann::json& j, ListResourcesRequestParams& p);

struct ListResourcesResult {
    std::vector<Resource>      resources;
    std::optional<std::string> next_cursor;
};
void to_json(nlohmann::json& j, const ListResourcesResult& r);
void from_json(const nlohmann::json& j, ListResourcesResult& r);

struct ListResourceTemplatesRequestParams {
    std::optional<std::string> cursor;
};
void to_json(nlohmann::json& j, const ListResourceTemplatesRequestParams& p);
void from_json(const nlohmann::json& j, ListResourceTemplatesRequestParams& p);

struct ListResourceTemplatesResult {
    std::vector<ResourceTemplate> resource_templates;
    std::optional<std::string>    next_cursor;
};
void to_json(nlohmann::json& j, const ListResourceTemplatesResult& r);
void from_json(const nlohmann::json& j, ListResourceTemplatesResult& r);

struct ReadResourceRequestParams {
    std::string uri;
};
void to_json(nlohmann::json& j, const ReadResourceRequestParams& p);
void from_json(const nlohmann::json& j, ReadResourceRequestParams& p);

struct ReadResourceResult {
    std::vector<ResourceContents> contents;
};
void to_json(nlohmann::json& j, const ReadResourceResult& r);
void from_json(const nlohmann::json& j, ReadResourceResult& r);

struct SubscribeRequestParams {
    std::string uri;
};
void to_json(nlohmann::json& j, const SubscribeRequestParams& p);
void from_json(const nlohmann::json& j, SubscribeRequestParams& p);

struct UnsubscribeRequestParams {
    std::string uri;
};
void to_json(nlohmann::json& j, const UnsubscribeRequestParams& p);
void from_json(const nlohmann::json& j, UnsubscribeRequestParams& p);

struct ResourceUpdatedNotificationParams {
    std::string uri;
};
void to_json(nlohmann::json& j, const ResourceUpdatedNotificationParams& p);
void from_json(const nlohmann::json& j, ResourceUpdatedNotificationParams& p);

inline constexpr std::string_view method_resources_list           = "resources/list";
inline constexpr std::string_view method_resources_templates_list = "resources/templates/list";
inline constexpr std::string_view method_resources_read           = "resources/read";
inline constexpr std::string_view method_resources_subscribe      = "resources/subscribe";
inline constexpr std::string_view method_resources_unsubscribe    = "resources/unsubscribe";
inline constexpr std::string_view method_notifications_resources_list_changed
    = "notifications/resources/list_changed";
inline constexpr std::string_view method_notifications_resources_updated
    = "notifications/resources/updated";

// --------------------------------------------------------------------------
// Prompts
// --------------------------------------------------------------------------

struct PromptArgument {
    std::string                name;
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::optional<bool>        required;
};
void to_json(nlohmann::json& j, const PromptArgument& a);
void from_json(const nlohmann::json& j, PromptArgument& a);

struct Prompt {
    std::string                       name;
    std::optional<std::string>        title;
    std::optional<std::string>        description;
    std::optional<std::vector<PromptArgument>> arguments;
};
void to_json(nlohmann::json& j, const Prompt& p);
void from_json(const nlohmann::json& j, Prompt& p);

struct PromptMessage {
    Role         role;
    ContentBlock content;
};
void to_json(nlohmann::json& j, const PromptMessage& m);
void from_json(const nlohmann::json& j, PromptMessage& m);

struct ListPromptsRequestParams {
    std::optional<std::string> cursor;
};
void to_json(nlohmann::json& j, const ListPromptsRequestParams& p);
void from_json(const nlohmann::json& j, ListPromptsRequestParams& p);

struct ListPromptsResult {
    std::vector<Prompt>        prompts;
    std::optional<std::string> next_cursor;
};
void to_json(nlohmann::json& j, const ListPromptsResult& r);
void from_json(const nlohmann::json& j, ListPromptsResult& r);

struct GetPromptRequestParams {
    std::string                                            name;
    std::optional<std::unordered_map<std::string, std::string>> arguments;
};
void to_json(nlohmann::json& j, const GetPromptRequestParams& p);
void from_json(const nlohmann::json& j, GetPromptRequestParams& p);

struct GetPromptResult {
    std::optional<std::string>  description;
    std::vector<PromptMessage>  messages;
};
void to_json(nlohmann::json& j, const GetPromptResult& r);
void from_json(const nlohmann::json& j, GetPromptResult& r);

inline constexpr std::string_view method_prompts_list = "prompts/list";
inline constexpr std::string_view method_prompts_get  = "prompts/get";
inline constexpr std::string_view method_notifications_prompts_list_changed
    = "notifications/prompts/list_changed";

// --------------------------------------------------------------------------
// Cancellation
// --------------------------------------------------------------------------

struct CancelledNotificationParams {
    std::optional<RequestId>   request_id;
    std::optional<std::string> reason;
};
void to_json(nlohmann::json& j, const CancelledNotificationParams& p);
void from_json(const nlohmann::json& j, CancelledNotificationParams& p);

inline constexpr std::string_view method_notifications_cancelled
    = "notifications/cancelled";

// --------------------------------------------------------------------------
// Progress
// --------------------------------------------------------------------------

/// Progress tokens are opaque values carried in `_meta.progressToken`
/// on the originating request. The spec permits string or number; we
/// normalise to a thin variant.
class ProgressToken {
public:
    ProgressToken() : value_(std::int64_t{0}) {}
    ProgressToken(std::string s)  : value_(std::move(s)) {}
    ProgressToken(const char* s)  : value_(std::string{s}) {}
    ProgressToken(std::int64_t i) : value_(i) {}
    ProgressToken(int i)          : value_(static_cast<std::int64_t>(i)) {}

    [[nodiscard]] bool is_string()  const noexcept { return value_.index() == 0; }
    [[nodiscard]] bool is_integer() const noexcept { return value_.index() == 1; }

    [[nodiscard]] const std::string& as_string() const { return std::get<0>(value_); }
    [[nodiscard]] std::int64_t       as_integer() const { return std::get<1>(value_); }
    [[nodiscard]] std::string        canonical() const;

    friend bool operator==(const ProgressToken&, const ProgressToken&) = default;

private:
    std::variant<std::string, std::int64_t> value_;
};
void to_json(nlohmann::json& j, const ProgressToken& t);
void from_json(const nlohmann::json& j, ProgressToken& t);

struct ProgressNotificationParams {
    ProgressToken              progress_token;
    double                     progress;
    std::optional<double>      total;
    std::optional<std::string> message;
};
void to_json(nlohmann::json& j, const ProgressNotificationParams& p);
void from_json(const nlohmann::json& j, ProgressNotificationParams& p);

inline constexpr std::string_view method_notifications_progress
    = "notifications/progress";

// --------------------------------------------------------------------------
// Logging (server → client log messages)
// --------------------------------------------------------------------------

/// Per-spec MCP log severity levels — this is the protocol-level
/// "logging" capability, distinct from this library's internal
/// LogLevel which writes to the host's stderr.
enum class LoggingLevel {
    debug, info, notice, warning, error, critical, alert, emergency,
};
[[nodiscard]] std::string_view to_string(LoggingLevel l) noexcept;
void to_json(nlohmann::json& j, const LoggingLevel& l);
void from_json(const nlohmann::json& j, LoggingLevel& l);

struct SetLevelRequestParams {
    LoggingLevel level;
};
void to_json(nlohmann::json& j, const SetLevelRequestParams& p);
void from_json(const nlohmann::json& j, SetLevelRequestParams& p);

struct LoggingMessageNotificationParams {
    LoggingLevel               level;
    std::optional<std::string> logger;  // optional name of the logger source
    nlohmann::json             data;    // free-form payload (string/object/...)
};
void to_json(nlohmann::json& j, const LoggingMessageNotificationParams& p);
void from_json(const nlohmann::json& j, LoggingMessageNotificationParams& p);

inline constexpr std::string_view method_logging_set_level = "logging/setLevel";
inline constexpr std::string_view method_notifications_message = "notifications/message";

// --------------------------------------------------------------------------
// Ping
// --------------------------------------------------------------------------

inline constexpr std::string_view method_ping = "ping";

}  // namespace mcp

// std::hash specialization so RequestId can key unordered containers.
namespace std {
template <>
struct hash<::mcp::RequestId> {
    [[nodiscard]] std::size_t operator()(const ::mcp::RequestId& id) const noexcept {
        return std::hash<std::string>{}(id.canonical());
    }
};
}  // namespace std
