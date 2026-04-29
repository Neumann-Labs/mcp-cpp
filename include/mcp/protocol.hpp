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

/// 2025-11-25: optional icon descriptor used by Implementation,
/// Tool, Resource, Prompt, and ResourceTemplate. Mirrors the spec's
/// `Icon` shape — a URL/data pointer plus optional metadata.
struct Icon {
    /// `src` is either an absolute URL or a `data:` URL (RFC 2397).
    std::string                src;
    std::optional<std::string> mime_type;
    /// Whitespace-delimited list of WxH dimensions ("16x16 32x32"),
    /// matching HTML `<link rel=icon sizes>`.
    std::optional<std::string> sizes;
    /// "light" / "dark" / "monochrome" / app-defined.
    std::optional<std::string> theme;
};
void to_json(nlohmann::json& j, const Icon& c);
void from_json(const nlohmann::json& j, Icon& c);

struct Implementation {
    std::string                name;
    std::optional<std::string> title;
    std::string                version;
    std::optional<std::string> description;
    std::optional<std::string> website_url;
    /// 2025-11-25: optional list of icons describing this party.
    std::optional<std::vector<Icon>> icons;
    /// Top-level `_meta` envelope for protocol-level metadata.
    /// Round-trips opaquely; spec reserves keys with the
    /// `io.modelcontextprotocol/` prefix.
    std::optional<nlohmann::json>    meta;
};

void to_json(nlohmann::json& j, const Implementation& i);
void from_json(const nlohmann::json& j, Implementation& i);

// --------------------------------------------------------------------------
// Tasks: small capability/augmentation types (the primitive's own
// methods + Task struct live further down). Hoisted so capabilities
// and CallToolRequestParams below can hold std::optional<...> of them.
// --------------------------------------------------------------------------

struct TaskAugmentation {
    /// Milliseconds the task is retained after creation. Empty / null
    /// means unlimited (or implementation-defined retention).
    std::optional<std::int64_t> ttl;
};
void to_json(nlohmann::json& j, const TaskAugmentation& t);
void from_json(const nlohmann::json& j, TaskAugmentation& t);

struct TasksRequestsCapability {
    /// Per-namespace presence-only objects — e.g. `tools.call`,
    /// `sampling.createMessage`, `elicitation.create`. Held as raw
    /// json so a future spec extension (a new method) round-trips
    /// without an SDK release.
    std::optional<nlohmann::json> tools;
    std::optional<nlohmann::json> sampling;
    std::optional<nlohmann::json> elicitation;
};
void to_json(nlohmann::json& j, const TasksRequestsCapability& c);
void from_json(const nlohmann::json& j, TasksRequestsCapability& c);

struct TasksCapability {
    std::optional<nlohmann::json>          list;
    std::optional<nlohmann::json>          cancel;
    std::optional<TasksRequestsCapability> requests;
};
void to_json(nlohmann::json& j, const TasksCapability& c);
void from_json(const nlohmann::json& j, TasksCapability& c);

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
    /// 2025-11-25: client advertises which task-augmentable inbound
    /// methods it can handle (e.g. sampling/createMessage as a task).
    std::optional<TasksCapability>       tasks;
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
    /// 2025-11-25: which task-augmentable inbound methods the server
    /// can handle (e.g. tools/call as a task), plus list/cancel.
    std::optional<TasksCapability>     tasks;
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
    std::optional<nlohmann::json> meta;
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
    /// 2025-11-25 additions:
    std::optional<std::vector<Icon>> icons;
    std::optional<nlohmann::json>    meta;
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
    /// 2025-11-25 additions:
    std::optional<std::vector<Icon>> icons;
    std::optional<nlohmann::json>    meta;
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
    /// 2025-11-25 additions:
    std::optional<std::vector<Icon>> icons;
    std::optional<nlohmann::json>    meta;
};
void to_json(nlohmann::json& j, const ResourceLink& r);
void from_json(const nlohmann::json& j, ResourceLink& r);

struct EmbeddedResource {
    ResourceContents           resource;
    std::optional<Annotations> annotations;
};
void to_json(nlohmann::json& j, const EmbeddedResource& r);
void from_json(const nlohmann::json& j, EmbeddedResource& r);

/// 2025-11-25: tool-use content block. Used in sampling messages to
/// represent the assistant emitting a tool call (i.e. an
/// agentic-loop step). Wire type discriminator: `"tool_use"`.
struct ToolUseContent {
    /// Identifier the assistant assigns to this call so the
    /// corresponding `tool_result` block can reference it.
    std::string                id;
    /// Name of the tool being invoked. Should match a registered
    /// `Tool.name` server-side.
    std::string                name;
    /// Arguments to the tool. JSON object matching the tool's
    /// `inputSchema`.
    nlohmann::json             input;
    std::optional<Annotations> annotations;
};
void to_json(nlohmann::json& j, const ToolUseContent& c);
void from_json(const nlohmann::json& j, ToolUseContent& c);

/// Plain (non-tool) content blocks — the subset legal as the
/// `content` of a ToolResultContent. The spec forbids nested
/// tool_use/tool_result, and modelling the constraint in the type
/// system catches malformed payloads at compile time.
using PlainContentBlock = std::variant<TextContent,
                                       ImageContent,
                                       AudioContent,
                                       ResourceLink,
                                       EmbeddedResource>;
void to_json(nlohmann::json& j, const PlainContentBlock& c);
void from_json(const nlohmann::json& j, PlainContentBlock& c);

/// 2025-11-25: tool-result content block. Carries the result of a
/// previous `tool_use` block, referenced by `toolUseId`. Wire type
/// discriminator: `"tool_result"`.
struct ToolResultContent {
    /// The `id` of the originating ToolUseContent.
    std::string                     tool_use_id;
    /// The tool's actual output. Restricted to plain content blocks
    /// per spec (no nested tool_* allowed).
    std::vector<PlainContentBlock>  content;
    /// True when the tool errored. The content array typically
    /// carries a TextContent describing the error in that case.
    std::optional<bool>             is_error;
    std::optional<Annotations>      annotations;
};
void to_json(nlohmann::json& j, const ToolResultContent& c);
void from_json(const nlohmann::json& j, ToolResultContent& c);

/// Content blocks used by tools/calls, prompts, and sampling. The
/// type discriminator on the wire is `"text" | "image" | "audio"
/// | "resource_link" | "resource" | "tool_use" | "tool_result"`.
/// The last two are 2025-11-25 additions for agentic sampling
/// loops.
using ContentBlock = std::variant<TextContent,
                                  ImageContent,
                                  AudioContent,
                                  ResourceLink,
                                  EmbeddedResource,
                                  ToolUseContent,
                                  ToolResultContent>;

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

/// 2025-11-25: per-tool execution policy. The receiver advertises
/// whether a tool supports being task-augmented at all, optionally,
/// or requires it.
enum class TaskSupport { forbidden, optional, required };
void to_json(nlohmann::json& j, TaskSupport t);
void from_json(const nlohmann::json& j, TaskSupport& t);

struct ToolExecution {
    /// "forbidden" / "optional" / "required". Default is
    /// "optional" when `execution` is absent.
    TaskSupport task_support = TaskSupport::optional;
};
void to_json(nlohmann::json& j, const ToolExecution& e);
void from_json(const nlohmann::json& j, ToolExecution& e);

struct Tool {
    std::string                    name;
    std::optional<std::string>     title;
    std::optional<std::string>     description;
    nlohmann::json                 input_schema;   // JSON Schema; type:"object"
    std::optional<nlohmann::json>  output_schema;  // optional JSON Schema
    std::optional<ToolAnnotations> annotations;
    /// 2025-11-25 additions:
    std::optional<std::vector<Icon>> icons;
    std::optional<ToolExecution>     execution;
    std::optional<nlohmann::json>    meta;
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
    std::optional<nlohmann::json> meta;
};
void to_json(nlohmann::json& j, const ListToolsResult& r);
void from_json(const nlohmann::json& j, ListToolsResult& r);

struct CallToolRequestParams {
    std::string                       name;
    std::optional<nlohmann::json>     arguments;  // JSON object
    /// When set, the request is task-augmented: the receiver should
    /// return a CreateTaskResult envelope and run the call async. The
    /// final CallToolResult is fetched via `tasks/result`.
    std::optional<TaskAugmentation>   task;
};
void to_json(nlohmann::json& j, const CallToolRequestParams& p);
void from_json(const nlohmann::json& j, CallToolRequestParams& p);

struct CallToolResult {
    std::vector<ContentBlock>     content;
    std::optional<nlohmann::json> structured_content;  // matches outputSchema
    std::optional<bool>           is_error;
    std::optional<nlohmann::json> meta;
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
    std::optional<nlohmann::json> meta;
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
    std::optional<nlohmann::json> meta;
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
    std::optional<nlohmann::json> meta;
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
    /// 2025-11-25 additions:
    std::optional<std::vector<Icon>> icons;
    std::optional<nlohmann::json>    meta;
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
    std::optional<nlohmann::json> meta;
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
    std::optional<nlohmann::json> meta;
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

// --------------------------------------------------------------------------
// Sampling (sampling/createMessage) — server-initiated LLM calls
// --------------------------------------------------------------------------

struct ModelHint {
    std::optional<std::string> name;
};
void to_json(nlohmann::json& j, const ModelHint& h);
void from_json(const nlohmann::json& j, ModelHint& h);

struct ModelPreferences {
    std::optional<std::vector<ModelHint>> hints;
    std::optional<double> cost_priority;
    std::optional<double> speed_priority;
    std::optional<double> intelligence_priority;
};
void to_json(nlohmann::json& j, const ModelPreferences& p);
void from_json(const nlohmann::json& j, ModelPreferences& p);

enum class IncludeContext { none, this_server, all_servers };
[[nodiscard]] std::string_view to_string(IncludeContext c) noexcept;
void to_json(nlohmann::json& j, const IncludeContext& c);
void from_json(const nlohmann::json& j, IncludeContext& c);

/// Sampling tool-choice mode (2025-11-25). The spec lets the
/// requester force / forbid / pin tool use:
///   - "auto"   — model decides whether to call tools (default)
///   - "any"    — model MUST call at least one tool
///   - "none"   — model MUST NOT call tools
///   - "tool"   — model MUST call the specific tool named
enum class ToolChoiceMode { auto_, any, none, tool };
[[nodiscard]] std::string_view to_string(ToolChoiceMode m) noexcept;
void to_json(nlohmann::json& j, const ToolChoiceMode& m);
void from_json(const nlohmann::json& j, ToolChoiceMode& m);

/// Tool choice. The wire shape is either a bare string ("auto" |
/// "any" | "none") or an object `{ type: "tool", name: "..." }`
/// for the named-tool case. We model it as a struct with both
/// fields optional; serialise emits the object form when
/// `mode == tool` AND `name` is present.
struct ToolChoice {
    std::optional<ToolChoiceMode> mode;
    /// Required when mode == ToolChoiceMode::tool.
    std::optional<std::string>    name;
};
void to_json(nlohmann::json& j, const ToolChoice& c);
void from_json(const nlohmann::json& j, ToolChoice& c);

/// A message in a sampling exchange. Phase 3 supports text/image/audio
/// content; tool_use and tool_result variants land in Phase 4 alongside
/// the rest of the agentic-loop primitives.
///
/// The spec's `content` field accepts either a single block or an array
/// of blocks; we always model it as a vector internally so multi-block
/// messages don't get silently truncated. On the wire we emit an array
/// when there's more than one block and a single-block object otherwise
/// (matching what nearly every existing implementation produces).
struct SamplingMessage {
    Role                      role;
    std::vector<ContentBlock> content;
};
void to_json(nlohmann::json& j, const SamplingMessage& m);
void from_json(const nlohmann::json& j, SamplingMessage& m);

struct CreateMessageRequestParams {
    std::vector<SamplingMessage>      messages;
    std::optional<ModelPreferences>   model_preferences;
    std::optional<std::string>        system_prompt;
    std::optional<IncludeContext>     include_context;
    std::optional<double>             temperature;
    std::int64_t                      max_tokens{};   // required by spec
    std::optional<std::vector<std::string>> stop_sequences;
    std::optional<nlohmann::json>     metadata;
    /// 2025-11-25: tools the assistant may call during this
    /// sampling. Server side is expected to translate to the
    /// underlying LLM's tool-calling shape; `Tool::input_schema`
    /// is the JSON Schema the model is told to fit.
    std::optional<std::vector<Tool>>  tools;
    /// 2025-11-25: how to constrain tool calling.
    std::optional<ToolChoice>         tool_choice;
};
void to_json(nlohmann::json& j, const CreateMessageRequestParams& p);
void from_json(const nlohmann::json& j, CreateMessageRequestParams& p);

struct CreateMessageResult {
    Role                         role;
    std::vector<ContentBlock>    content;
    std::string                  model;
    std::optional<std::string>   stop_reason;
    /// Top-level _meta envelope for protocol-level metadata.
    std::optional<nlohmann::json> meta;
};
void to_json(nlohmann::json& j, const CreateMessageResult& r);
void from_json(const nlohmann::json& j, CreateMessageResult& r);

inline constexpr std::string_view method_sampling_create_message
    = "sampling/createMessage";

// --------------------------------------------------------------------------
// Roots (client → server: filesystem/URI scopes)
// --------------------------------------------------------------------------

struct Root {
    std::string                uri;
    std::optional<std::string> name;
};
void to_json(nlohmann::json& j, const Root& r);
void from_json(const nlohmann::json& j, Root& r);

struct ListRootsResult {
    std::vector<Root> roots;
};
void to_json(nlohmann::json& j, const ListRootsResult& r);
void from_json(const nlohmann::json& j, ListRootsResult& r);

inline constexpr std::string_view method_roots_list = "roots/list";
inline constexpr std::string_view method_notifications_roots_list_changed
    = "notifications/roots/list_changed";

// --------------------------------------------------------------------------
// Completion (autocomplete)
// --------------------------------------------------------------------------

struct ResourceTemplateReference {
    std::string uri_template;
};
void to_json(nlohmann::json& j, const ResourceTemplateReference& r);
void from_json(const nlohmann::json& j, ResourceTemplateReference& r);

struct PromptReference {
    std::string                name;
    std::optional<std::string> title;
};
void to_json(nlohmann::json& j, const PromptReference& r);
void from_json(const nlohmann::json& j, PromptReference& r);

using CompletionReference = std::variant<ResourceTemplateReference, PromptReference>;
void to_json(nlohmann::json& j, const CompletionReference& r);
void from_json(const nlohmann::json& j, CompletionReference& r);

struct CompleteArgument {
    std::string name;
    std::string value;
};
void to_json(nlohmann::json& j, const CompleteArgument& a);
void from_json(const nlohmann::json& j, CompleteArgument& a);

struct CompleteRequestParams {
    CompletionReference                     reference;
    CompleteArgument                        argument;
    std::optional<std::unordered_map<std::string, std::string>> context_arguments;
};
void to_json(nlohmann::json& j, const CompleteRequestParams& p);
void from_json(const nlohmann::json& j, CompleteRequestParams& p);

struct CompletionValues {
    std::vector<std::string>    values;
    std::optional<std::int64_t> total;
    std::optional<bool>         has_more;
};
void to_json(nlohmann::json& j, const CompletionValues& v);
void from_json(const nlohmann::json& j, CompletionValues& v);

struct CompleteResult {
    CompletionValues completion;
};
void to_json(nlohmann::json& j, const CompleteResult& r);
void from_json(const nlohmann::json& j, CompleteResult& r);

inline constexpr std::string_view method_completion_complete = "completion/complete";

// --------------------------------------------------------------------------
// Elicitation (server → client: ask the user for input)
// --------------------------------------------------------------------------
//
// Two modes: form (an in-band JSON-Schema-described form rendered by the
// client) and url (an out-of-band browser navigation, with an optional
// completion notification).
//
// The wire shape is a tagged-by-`mode` object: omitted/`form` means form
// mode, `url` means URL mode. We model it as a variant so callers don't
// have to inspect a discriminator field by hand.

struct ElicitFormRequestParams {
    /// Plain-text rationale for the request, shown to the user.
    std::string    message;
    /// JSON Schema describing the expected response. Per spec, restricted
    /// to a flat object of primitive properties; we don't enforce that
    /// here so applications can negotiate richer shapes when both sides
    /// agree, but the basic SDK sticks to the spec.
    nlohmann::json requested_schema;
};
void to_json(nlohmann::json& j, const ElicitFormRequestParams& p);
void from_json(const nlohmann::json& j, ElicitFormRequestParams& p);

struct ElicitUrlRequestParams {
    std::string message;
    /// HTTPS URL the user is asked to visit out-of-band.
    std::string url;
    /// Identifier the server uses to correlate later
    /// `notifications/elicitation/complete` notifications.
    std::string elicitation_id;
};
void to_json(nlohmann::json& j, const ElicitUrlRequestParams& p);
void from_json(const nlohmann::json& j, ElicitUrlRequestParams& p);

using ElicitRequestParams =
    std::variant<ElicitFormRequestParams, ElicitUrlRequestParams>;
void to_json(nlohmann::json& j, const ElicitRequestParams& p);
void from_json(const nlohmann::json& j, ElicitRequestParams& p);

enum class ElicitAction { accept, decline, cancel };
void to_json(nlohmann::json& j, ElicitAction a);
void from_json(const nlohmann::json& j, ElicitAction& a);

struct ElicitResult {
    ElicitAction                  action;
    /// Form-mode only: the user's submission, conforming to the
    /// server-supplied schema. Absent for decline/cancel and for URL
    /// mode.
    std::optional<nlohmann::json> content;
};
void to_json(nlohmann::json& j, const ElicitResult& r);
void from_json(const nlohmann::json& j, ElicitResult& r);

/// URL-mode out-of-band completion notification. Emitted by the server
/// or relayed by the client once the out-of-band flow finishes — but
/// the spec leaves precise emitter conventions open. The MCP SDK
/// accepts the notification on either side.
struct ElicitationCompleteNotificationParams {
    std::string elicitation_id;
};
void to_json(nlohmann::json& j, const ElicitationCompleteNotificationParams& p);
void from_json(const nlohmann::json& j, ElicitationCompleteNotificationParams& p);

inline constexpr std::string_view method_elicitation_create
    = "elicitation/create";
inline constexpr std::string_view method_notifications_elicitation_complete
    = "notifications/elicitation/complete";

/// 2025-11-25: structured `data` for the JSON-RPC error code
/// `error_code::url_elicitation_required` (-32042). Sent by the
/// server when an in-flight request can't proceed without the
/// user finishing one or more URL-mode elicitation flows. The
/// client MAY drive the URL flows out-of-band, then retry the
/// original request.
struct UrlElicitationRequiredErrorData {
    /// One or more URL-mode elicitations the client must run.
    /// Each element is the params shape of an `elicitation/create`
    /// request — message + url + elicitationId.
    std::vector<ElicitUrlRequestParams> elicitations;
};
void to_json(nlohmann::json& j, const UrlElicitationRequiredErrorData& d);
void from_json(const nlohmann::json& j, UrlElicitationRequiredErrorData& d);

// --------------------------------------------------------------------------
// Tasks (long-running operations)
// --------------------------------------------------------------------------
//
// A task primitive lets a request that would otherwise be synchronous
// (`tools/call`, `sampling/createMessage`, `elicitation/create`) be
// promoted to an async, polled operation. The requester opts in by
// attaching a `task` field to the request params; the receiver returns
// a `CreateTaskResult { task }` envelope right away, and the actual
// result is fetched later via `tasks/result`. Status transitions can
// be polled via `tasks/get` or pushed via `notifications/tasks/status`.

enum class TaskStatus {
    working,
    input_required,
    completed,
    failed,
    cancelled,
};
void to_json(nlohmann::json& j, TaskStatus s);
void from_json(const nlohmann::json& j, TaskStatus& s);

struct Task {
    /// Receiver-generated, unique. The string need not be a UUID; we
    /// emit a 128-bit hex token by default.
    std::string                taskId;
    TaskStatus                 status;
    /// Optional human-readable progress description.
    std::optional<std::string> status_message;
    /// ISO 8601 UTC timestamps.
    std::string                created_at;
    std::string                last_updated_at;
    /// Milliseconds the task is retained after creation. Absent / null
    /// means unlimited (or implementation-defined retention).
    std::optional<std::int64_t> ttl;
    /// Suggested polling interval in milliseconds; advisory.
    std::optional<std::int64_t> poll_interval;
};
void to_json(nlohmann::json& j, const Task& t);
void from_json(const nlohmann::json& j, Task& t);

/// Returned in place of the underlying request's normal result when
/// the request was task-augmented.
struct CreateTaskResult {
    Task task;
};
void to_json(nlohmann::json& j, const CreateTaskResult& r);
void from_json(const nlohmann::json& j, CreateTaskResult& r);

struct GetTaskRequestParams {
    std::string task_id;
};
void to_json(nlohmann::json& j, const GetTaskRequestParams& p);
void from_json(const nlohmann::json& j, GetTaskRequestParams& p);

struct GetTaskResultRequestParams {
    std::string task_id;
};
void to_json(nlohmann::json& j, const GetTaskResultRequestParams& p);
void from_json(const nlohmann::json& j, GetTaskResultRequestParams& p);

struct ListTasksRequestParams {
    std::optional<std::string> cursor;
};
void to_json(nlohmann::json& j, const ListTasksRequestParams& p);
void from_json(const nlohmann::json& j, ListTasksRequestParams& p);

struct ListTasksResult {
    std::vector<Task>          tasks;
    std::optional<std::string> next_cursor;
};
void to_json(nlohmann::json& j, const ListTasksResult& r);
void from_json(const nlohmann::json& j, ListTasksResult& r);

struct CancelTaskRequestParams {
    std::string task_id;
};
void to_json(nlohmann::json& j, const CancelTaskRequestParams& p);
void from_json(const nlohmann::json& j, CancelTaskRequestParams& p);

struct TaskStatusNotificationParams {
    /// A point-in-time projection of the underlying Task. Same shape
    /// as Task, no extra fields.
    Task task;
};
void to_json(nlohmann::json& j, const TaskStatusNotificationParams& p);
void from_json(const nlohmann::json& j, TaskStatusNotificationParams& p);

inline constexpr std::string_view method_tasks_get    = "tasks/get";
inline constexpr std::string_view method_tasks_result = "tasks/result";
inline constexpr std::string_view method_tasks_list   = "tasks/list";
inline constexpr std::string_view method_tasks_cancel = "tasks/cancel";
inline constexpr std::string_view method_notifications_tasks_status
    = "notifications/tasks/status";

/// Meta key the spec mandates on `tasks/result` payloads to bind the
/// result back to its task. Server-side tooling sets it; clients can
/// optionally check it.
inline constexpr std::string_view tasks_related_task_meta_key
    = "io.modelcontextprotocol/related-task";

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
