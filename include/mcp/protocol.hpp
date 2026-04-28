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

/// Content blocks used by tools/calls and prompts. The `ResourceLink` and
/// `EmbeddedResource` variants are added in Phase 2 alongside resources.
using ContentBlock = std::variant<TextContent, ImageContent, AudioContent>;

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
