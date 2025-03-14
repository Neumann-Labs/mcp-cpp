#ifndef MCP_TYPES_HPP_
#define MCP_TYPES_HPP_

#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace mcp {
namespace types {

/**
 * @brief Standard JSON-RPC 2.0 error codes and MCP-specific error codes
 */
enum class ErrorCode {
  // JSON-RPC 2.0 standard error codes
  ParseError = -32700,     ///< Invalid JSON was received
  InvalidRequest = -32600, ///< The JSON sent is not a valid Request object
  MethodNotFound = -32601, ///< The method does not exist / is not available
  InvalidParams = -32602,  ///< Invalid method parameter(s)
  InternalError = -32603,  ///< Internal JSON-RPC error

  // MCP-specific error codes
  ProtocolError = -32000,     ///< Protocol-related error
  TransportError = -32001,    ///< Transport-related error
  TimeoutError = -32002,      ///< Operation timed out
  AuthorizationError = -32003 ///< Authorization failed
};

/**
 * @brief Structure representing an error in JSON-RPC 2.0
 */
struct ErrorData {
  int code;            ///< Error code
  std::string message; ///< Error message
  nlohmann::json data; ///< Optional additional error data
};

/**
 * @brief Common metadata for request parameters
 */
struct RequestMeta {
  std::optional<std::string> progressToken; ///< Token for tracking progress
};

/**
 * @brief Base structure for request parameters
 */
struct RequestParams {
  std::optional<RequestMeta> meta; ///< Optional metadata
};

/**
 * @brief Common metadata for notification parameters
 */
struct NotificationMeta {
  // Additional metadata for notifications
};

/**
 * @brief Base structure for notification parameters
 */
struct NotificationParams {
  std::optional<NotificationMeta> meta; ///< Optional metadata
};

/**
 * @brief Base structure for result data
 */
struct Result {
  std::optional<nlohmann::json> meta; ///< Optional metadata
};

/**
 * @brief JSON-RPC 2.0 request message
 */
struct JSONRPCRequest {
  std::string jsonrpc = "2.0";          ///< JSON-RPC version (always "2.0")
  std::variant<std::string, int> id;    ///< Request identifier
  std::string method;                   ///< Method name
  std::optional<nlohmann::json> params; ///< Method parameters
};

/**
 * @brief JSON-RPC 2.0 notification message (request without id)
 */
struct JSONRPCNotification {
  std::string jsonrpc = "2.0";          ///< JSON-RPC version (always "2.0")
  std::string method;                   ///< Method name
  std::optional<nlohmann::json> params; ///< Method parameters
};

/**
 * @brief JSON-RPC 2.0 success response message
 */
struct JSONRPCResponse {
  std::string jsonrpc = "2.0";       ///< JSON-RPC version (always "2.0")
  std::variant<std::string, int> id; ///< Request identifier
  nlohmann::json result;             ///< Result data
};

/**
 * @brief JSON-RPC 2.0 error response message
 */
struct JSONRPCError {
  std::string jsonrpc = "2.0";       ///< JSON-RPC version (always "2.0")
  std::variant<std::string, int> id; ///< Request identifier
  ErrorData error;                   ///< Error data
};

/**
 * @brief Variant type that can hold any JSON-RPC 2.0 message
 */
using JSONRPCMessage = std::variant<JSONRPCRequest, JSONRPCNotification,
                                    JSONRPCResponse, JSONRPCError>;

/**
 * @brief Tool parameter schema description
 */
struct ToolParameter {
  std::string name;        ///< Parameter name
  std::string description; ///< Parameter description
  nlohmann::json schema;   ///< JSON Schema for the parameter
  bool required;           ///< Whether the parameter is required
};

/**
 * @brief Tool definition
 */
struct Tool {
  std::string name;                           ///< Tool name
  std::string description;                    ///< Tool description
  std::vector<ToolParameter> parameters;      ///< Tool parameters
  nlohmann::json returns;                     ///< JSON Schema for return value
  std::optional<nlohmann::json> input_schema; ///< JSON Schema for the input
};

/**
 * @brief Resource definition
 */
struct Resource {
  std::string uri;         ///< Resource URI
  std::string name;        ///< Resource name
  std::string description; ///< Resource description
  std::string contentType; ///< Resource content type (MIME type)
};

/**
 * @brief Prompt argument definition
 */
struct PromptArgument {
  std::string name;        ///< Argument name
  std::string description; ///< Argument description
  nlohmann::json schema;   ///< JSON Schema for the argument
  bool required;           ///< Whether the argument is required
};

/**
 * @brief Prompt definition
 */
struct Prompt {
  std::string name;                      ///< Prompt name
  std::string description;               ///< Prompt description
  std::vector<PromptArgument> arguments; ///< Prompt arguments
};

/**
 * @brief Tool capabilities
 */
struct ToolCapabilities {
  bool supports_streaming = false; ///< Tool supports streaming
};

/**
 * @brief Resource capabilities
 */
struct ResourceCapabilities {
  bool supports_subscriptions = false; ///< Resource supports subscriptions
};

/**
 * @brief Prompt capabilities
 */
struct PromptCapabilities {
  bool supports_streaming = false; ///< Prompt supports streaming
};

/**
 * @brief Server capabilities
 */
struct ServerCapabilities {
  bool supportsTools = false;                    ///< Server supports tool calls
  bool supportsResources = false;                ///< Server supports resources
  bool supportsPrompts = false;                  ///< Server supports prompts
  std::optional<ToolCapabilities> tools;         ///< Tool capabilities
  std::optional<ResourceCapabilities> resources; ///< Resource capabilities
  std::optional<PromptCapabilities> prompts;     ///< Prompt capabilities
};

/**
 * @brief Client capabilities
 */
struct ClientCapabilities {
  bool supportsProgress = false;         ///< Client supports progress reporting
  bool supportsCancellation = false;     ///< Client supports cancellation
  std::optional<ToolCapabilities> tools; ///< Tool capabilities
  std::optional<ResourceCapabilities> resources; ///< Resource capabilities
  std::optional<PromptCapabilities> prompts;     ///< Prompt capabilities
};

/**
 * @brief Server information
 */
struct ServerInfo {
  std::string name;                        ///< Server name
  std::string version;                     ///< Server version
  std::optional<std::string> instructions; ///< Optional usage instructions
  std::optional<ServerCapabilities> capabilities; ///< Server capabilities
};

/**
 * @brief Client information
 */
struct ClientInfo {
  std::string name;                               ///< Client name
  std::string version;                            ///< Client version
  std::optional<ClientCapabilities> capabilities; ///< Client capabilities
};

/**
 * @brief Initialize request parameters
 */
struct InitializeParams : RequestParams {
  std::string clientName;          ///< Client name
  std::string clientVersion;       ///< Client version
  ClientCapabilities capabilities; ///< Client capabilities
};

/**
 * @brief Initialize result
 */
struct InitializeResult : Result {
  std::string serverName;                  ///< Server name
  std::string serverVersion;               ///< Server version
  std::optional<std::string> instructions; ///< Optional usage instructions
  ServerCapabilities capabilities;         ///< Server capabilities

  // For backwards compatibility with tests
  struct ServerInfo {
    std::string name;
    std::string version;
    std::optional<std::string> instructions;
  } server_info;

  // For backwards compatibility with tests
  ServerCapabilities server_capabilities;

  // Initialize backward compatibility fields in constructors
  InitializeResult() = default;

  InitializeResult(const InitializeResult &other)
      : Result(other), serverName(other.serverName),
        serverVersion(other.serverVersion), instructions(other.instructions),
        capabilities(other.capabilities),
        server_info{other.serverName, other.serverVersion, other.instructions},
        server_capabilities(other.capabilities) {}

  InitializeResult &operator=(const InitializeResult &other) {
    if (this != &other) {
      Result::operator=(other);
      serverName = other.serverName;
      serverVersion = other.serverVersion;
      instructions = other.instructions;
      capabilities = other.capabilities;
      server_info.name = other.serverName;
      server_info.version = other.serverVersion;
      server_info.instructions = other.instructions;
      server_capabilities = other.capabilities;
    }
    return *this;
  }
};

/**
 * @brief Parameters for listing tools
 */
struct ListToolsParams : RequestParams {
  // Empty for now, may add filtering options later
};

/**
 * @brief Result for listing tools
 */
struct ListToolsResult : Result {
  std::vector<Tool> tools; ///< Available tools
};

/**
 * @brief Parameters for calling a tool
 */
struct CallToolParams : RequestParams {
  std::string name;          ///< Tool name
  nlohmann::json parameters; ///< Tool parameters
};

/**
 * @brief Result from a tool call
 */
struct CallToolResult : Result {
  nlohmann::json result; ///< Tool result
};

/**
 * @brief Parameters for progress notification
 */
struct ProgressParams : NotificationParams {
  std::string token;                  ///< Progress token
  double progress;                    ///< Progress value (0.0 - 1.0)
  std::optional<std::string> message; ///< Optional progress message
};

/**
 * @brief Parameters for listing resources
 */
struct ListResourcesParams : RequestParams {
  // Empty for now, may add filtering options later
};

/**
 * @brief Result for listing resources
 */
struct ListResourcesResult : Result {
  std::vector<Resource> resources; ///< Available resources
};

/**
 * @brief Parameters for reading a resource
 */
struct ReadResourceParams : RequestParams {
  std::string uri; ///< Resource URI
};

/**
 * @brief Result from reading a resource
 */
struct ReadResourceResult : Result {
  std::string contentType; ///< Resource content type (MIME type)
  std::string content; ///< Resource content (may be encoded for binary data)

  // For backward compatibility with old code
  std::string content_type() const { return contentType; }

  // Constructor with default values
  ReadResourceResult() = default;

  // Constructor to initialize fields
  ReadResourceResult(const std::string &type, const std::string &cont)
      : contentType(type), content(cont) {}

  // Copy constructor
  ReadResourceResult(const ReadResourceResult &other)
      : Result(other), contentType(other.contentType), content(other.content) {}

  // Assignment operator
  ReadResourceResult &operator=(const ReadResourceResult &other) {
    if (this != &other) {
      Result::operator=(other);
      contentType = other.contentType;
      content = other.content;
    }
    return *this;
  }
};

/**
 * @brief Parameters for listing prompts
 */
struct ListPromptsParams : RequestParams {
  // Empty for now, may add filtering options later
};

/**
 * @brief Result for listing prompts
 */
struct ListPromptsResult : Result {
  std::vector<Prompt> prompts; ///< Available prompts
};

/**
 * @brief Parameters for getting a prompt
 */
struct GetPromptParams : RequestParams {
  std::string name;         ///< Prompt name
  nlohmann::json arguments; ///< Prompt arguments
};

/**
 * @brief Result from getting a prompt
 */
struct GetPromptResult : Result {
  std::string content; ///< Prompt content
  std::string prompt;  ///< Generated prompt text
};

/**
 * @brief Parameters for cancellation notification
 */
struct CancelParams : NotificationParams {
  std::variant<std::string, int> id; ///< ID of the request to cancel
};

/**
 * @brief Parameters for shutdown request
 */
struct ShutdownParams : RequestParams {
  // Empty for now
};

/**
 * @brief Result from shutdown request
 */
struct ShutdownResult : Result {
  // Empty for now
};

} // namespace types
} // namespace mcp

// JSON serialization/deserialization functions
namespace nlohmann {

// ErrorCode enum serialization
template <> struct adl_serializer<mcp::types::ErrorCode> {
  static void to_json(json &j, const mcp::types::ErrorCode &code) {
    j = static_cast<int>(code);
  }

  static void from_json(const json &j, mcp::types::ErrorCode &code) {
    code = static_cast<mcp::types::ErrorCode>(j.get<int>());
  }
};

// ErrorData serialization
template <> struct adl_serializer<mcp::types::ErrorData> {
  static void to_json(json &j, const mcp::types::ErrorData &error) {
    j = json{{"code", error.code}, {"message", error.message}};
    if (!error.data.is_null()) {
      j["data"] = error.data;
    }
  }

  static void from_json(const json &j, mcp::types::ErrorData &error) {
    j.at("code").get_to(error.code);
    j.at("message").get_to(error.message);
    if (j.contains("data")) {
      error.data = j["data"];
    } else {
      error.data = nullptr;
    }
  }
};

// RequestMeta serialization
template <> struct adl_serializer<mcp::types::RequestMeta> {
  static void to_json(json &j, const mcp::types::RequestMeta &meta) {
    j = json::object();
    if (meta.progressToken) {
      j["progressToken"] = *meta.progressToken;
    }
  }

  static void from_json(const json &j, mcp::types::RequestMeta &meta) {
    if (j.contains("progressToken")) {
      meta.progressToken = j["progressToken"].get<std::string>();
    }
  }
};

// JSONRPCRequest serialization
template <> struct adl_serializer<mcp::types::JSONRPCRequest> {
  static void to_json(json &j, const mcp::types::JSONRPCRequest &request) {
    j = json::object();
    j["jsonrpc"] = request.jsonrpc;

    // Handle variant ID properly
    std::visit([&j](const auto &id) { j["id"] = id; }, request.id);

    j["method"] = request.method;
    if (request.params) {
      j["params"] = *request.params;
    }
  }

  static void from_json(const json &j, mcp::types::JSONRPCRequest &request) {
    j.at("jsonrpc").get_to(request.jsonrpc);

    // Handle variant id (string or int)
    if (j.at("id").is_string()) {
      request.id = j.at("id").get<std::string>();
    } else {
      request.id = j.at("id").get<int>();
    }

    j.at("method").get_to(request.method);
    if (j.contains("params")) {
      request.params = j["params"];
    }
  }
};

// JSONRPCResponse serialization
template <> struct adl_serializer<mcp::types::JSONRPCResponse> {
  static void to_json(json &j, const mcp::types::JSONRPCResponse &response) {
    j = json::object();
    j["jsonrpc"] = response.jsonrpc;

    // Handle variant ID properly
    std::visit([&j](const auto &id) { j["id"] = id; }, response.id);

    j["result"] = response.result;
  }

  static void from_json(const json &j, mcp::types::JSONRPCResponse &response) {
    j.at("jsonrpc").get_to(response.jsonrpc);

    // Handle variant id (string or int)
    if (j.at("id").is_string()) {
      response.id = j.at("id").get<std::string>();
    } else {
      response.id = j.at("id").get<int>();
    }

    j.at("result").get_to(response.result);
  }
};

// JSONRPCError serialization
template <> struct adl_serializer<mcp::types::JSONRPCError> {
  static void to_json(json &j, const mcp::types::JSONRPCError &error) {
    j = json::object();
    j["jsonrpc"] = error.jsonrpc;

    // Handle variant ID properly
    std::visit([&j](const auto &id) { j["id"] = id; }, error.id);

    j["error"] = error.error;
  }

  static void from_json(const json &j, mcp::types::JSONRPCError &error) {
    j.at("jsonrpc").get_to(error.jsonrpc);
    if (j.at("id").is_string()) {
      error.id = j.at("id").get<std::string>();
    } else {
      error.id = j.at("id").get<int>();
    }
    j.at("error").get_to(error.error);
  }
};

// JSONRPCNotification serialization
template <> struct adl_serializer<mcp::types::JSONRPCNotification> {
  static void to_json(json &j,
                      const mcp::types::JSONRPCNotification &notification) {
    j = json::object();
    j["jsonrpc"] = notification.jsonrpc;
    j["method"] = notification.method;
    if (notification.params) {
      j["params"] = *notification.params;
    }
  }

  static void from_json(const json &j,
                        mcp::types::JSONRPCNotification &notification) {
    j.at("jsonrpc").get_to(notification.jsonrpc);
    j.at("method").get_to(notification.method);
    if (j.contains("params")) {
      notification.params = j["params"];
    }
  }
};

// ServerCapabilities serialization
template <> struct adl_serializer<mcp::types::ServerCapabilities> {
  static void to_json(json &j,
                      const mcp::types::ServerCapabilities &capabilities) {
    j = json::object();
    j["supportsTools"] = capabilities.supportsTools;
    j["supportsResources"] = capabilities.supportsResources;
    j["supportsPrompts"] = capabilities.supportsPrompts;
  }

  static void from_json(const json &j,
                        mcp::types::ServerCapabilities &capabilities) {
    j.at("supportsTools").get_to(capabilities.supportsTools);
    j.at("supportsResources").get_to(capabilities.supportsResources);
    j.at("supportsPrompts").get_to(capabilities.supportsPrompts);
  }
};

// ClientCapabilities serialization
template <> struct adl_serializer<mcp::types::ClientCapabilities> {
  static void to_json(json &j,
                      const mcp::types::ClientCapabilities &capabilities) {
    j = json::object();
    j["supportsProgress"] = capabilities.supportsProgress;
    j["supportsCancellation"] = capabilities.supportsCancellation;
  }

  static void from_json(const json &j,
                        mcp::types::ClientCapabilities &capabilities) {
    j.at("supportsProgress").get_to(capabilities.supportsProgress);
    j.at("supportsCancellation").get_to(capabilities.supportsCancellation);
  }
};

// ToolParameter serialization
template <> struct adl_serializer<mcp::types::ToolParameter> {
  static void to_json(json &j, const mcp::types::ToolParameter &param) {
    j = json::object();
    j["name"] = param.name;
    j["description"] = param.description;
    j["schema"] = param.schema;
    j["required"] = param.required;
  }

  static void from_json(const json &j, mcp::types::ToolParameter &param) {
    j.at("name").get_to(param.name);
    j.at("description").get_to(param.description);
    j.at("schema").get_to(param.schema);
    j.at("required").get_to(param.required);
  }
};

// Tool serialization
template <> struct adl_serializer<mcp::types::Tool> {
  static void to_json(json &j, const mcp::types::Tool &tool) {
    j = json::object();
    j["name"] = tool.name;
    j["description"] = tool.description;

    // Convert parameters to JSON array
    json params_array = json::array();
    for (const auto &param : tool.parameters) {
      // Convert each parameter to JSON and add to array
      json param_json = json(param); // Use the ToolParameter serializer
      params_array.push_back(param_json);
    }
    j["parameters"] = params_array;

    j["returns"] = tool.returns;

    if (tool.input_schema) {
      j["inputSchema"] = *tool.input_schema;
    }
  }

  static void from_json(const json &j, mcp::types::Tool &tool) {
    j.at("name").get_to(tool.name);
    j.at("description").get_to(tool.description);

    // Handle parameters array with manual conversion
    tool.parameters.clear();
    for (const auto &param_json : j.at("parameters")) {
      mcp::types::ToolParameter param;
      adl_serializer<mcp::types::ToolParameter>::from_json(param_json, param);
      tool.parameters.push_back(param);
    }

    j.at("returns").get_to(tool.returns);

    if (j.contains("inputSchema")) {
      tool.input_schema = j["inputSchema"];
    }
  }
};

// InitializeParams serialization
template <> struct adl_serializer<mcp::types::InitializeParams> {
  static void to_json(json &j, const mcp::types::InitializeParams &params) {
    j = json::object();

    // First handle base class fields if any
    if (params.meta) {
      j["meta"] = *params.meta;
    }

    // Handle derived class fields
    j["clientName"] = params.clientName;
    j["clientVersion"] = params.clientVersion;
    j["capabilities"] = params.capabilities;
  }

  static void from_json(const json &j, mcp::types::InitializeParams &params) {
    // Handle base class fields if any
    if (j.contains("meta")) {
      params.meta = j["meta"].get<mcp::types::RequestMeta>();
    }

    // Handle derived class fields
    j.at("clientName").get_to(params.clientName);
    j.at("clientVersion").get_to(params.clientVersion);
    j.at("capabilities").get_to(params.capabilities);
  }
};

// InitializeResult serialization
template <> struct adl_serializer<mcp::types::InitializeResult> {
  static void to_json(json &j, const mcp::types::InitializeResult &result) {
    j = json::object();

    // First handle base class fields if any
    if (result.meta) {
      j["meta"] = *result.meta;
    }

    // Handle derived class fields
    j["serverName"] = result.serverName;
    j["serverVersion"] = result.serverVersion;
    if (result.instructions) {
      j["instructions"] = *result.instructions;
    }
    j["capabilities"] = result.capabilities;
  }

  static void from_json(const json &j, mcp::types::InitializeResult &result) {
    // Handle base class fields if any
    if (j.contains("meta")) {
      result.meta = j["meta"];
    }

    // Handle derived class fields
    j.at("serverName").get_to(result.serverName);
    j.at("serverVersion").get_to(result.serverVersion);
    if (j.contains("instructions")) {
      result.instructions = j["instructions"].get<std::string>();
    }
    j.at("capabilities").get_to(result.capabilities);

    // Update backward compatibility fields
    result.server_info.name = result.serverName;
    result.server_info.version = result.serverVersion;
    result.server_info.instructions = result.instructions;
    result.server_capabilities = result.capabilities;
  }
};

// ServerInfo serialization
template <> struct adl_serializer<mcp::types::ServerInfo> {
  static void to_json(json &j, const mcp::types::ServerInfo &info) {
    j = json::object();
    j["name"] = info.name;
    j["version"] = info.version;
    if (info.instructions) {
      j["instructions"] = *info.instructions;
    }
    if (info.capabilities) {
      j["capabilities"] = *info.capabilities;
    }
  }

  static void from_json(const json &j, mcp::types::ServerInfo &info) {
    j.at("name").get_to(info.name);
    j.at("version").get_to(info.version);
    if (j.contains("instructions")) {
      info.instructions = j["instructions"].get<std::string>();
    }
    if (j.contains("capabilities")) {
      info.capabilities =
          j["capabilities"].get<mcp::types::ServerCapabilities>();
    }
  }
};

// ClientInfo serialization
template <> struct adl_serializer<mcp::types::ClientInfo> {
  static void to_json(json &j, const mcp::types::ClientInfo &info) {
    j = json::object();
    j["name"] = info.name;
    j["version"] = info.version;
    if (info.capabilities) {
      j["capabilities"] = *info.capabilities;
    }
  }

  static void from_json(const json &j, mcp::types::ClientInfo &info) {
    j.at("name").get_to(info.name);
    j.at("version").get_to(info.version);
    if (j.contains("capabilities")) {
      info.capabilities =
          j["capabilities"].get<mcp::types::ClientCapabilities>();
    }
  }
};

// ToolCapabilities serialization
template <> struct adl_serializer<mcp::types::ToolCapabilities> {
  static void to_json(json &j,
                      const mcp::types::ToolCapabilities &capabilities) {
    j = json::object();
    j["supportsStreaming"] = capabilities.supports_streaming;
  }

  static void from_json(const json &j,
                        mcp::types::ToolCapabilities &capabilities) {
    j.at("supportsStreaming").get_to(capabilities.supports_streaming);
  }
};

// ResourceCapabilities serialization
template <> struct adl_serializer<mcp::types::ResourceCapabilities> {
  static void to_json(json &j,
                      const mcp::types::ResourceCapabilities &capabilities) {
    j = json::object();
    j["supportsSubscriptions"] = capabilities.supports_subscriptions;
  }

  static void from_json(const json &j,
                        mcp::types::ResourceCapabilities &capabilities) {
    j.at("supportsSubscriptions").get_to(capabilities.supports_subscriptions);
  }
};

// PromptCapabilities serialization
template <> struct adl_serializer<mcp::types::PromptCapabilities> {
  static void to_json(json &j,
                      const mcp::types::PromptCapabilities &capabilities) {
    j = json::object();
    j["supportsStreaming"] = capabilities.supports_streaming;
  }

  static void from_json(const json &j,
                        mcp::types::PromptCapabilities &capabilities) {
    j.at("supportsStreaming").get_to(capabilities.supports_streaming);
  }
};

// GetPromptResult serialization
template <> struct adl_serializer<mcp::types::GetPromptResult> {
  static void to_json(json &j, const mcp::types::GetPromptResult &result) {
    j = json::object();
    if (result.meta) {
      j["meta"] = *result.meta;
    }
    j["content"] = result.content;
    j["prompt"] = result.prompt;
  }

  static void from_json(const json &j, mcp::types::GetPromptResult &result) {
    if (j.contains("meta")) {
      result.meta = j["meta"];
    }
    j.at("content").get_to(result.content);
    j.at("prompt").get_to(result.prompt);
  }
};

// ReadResourceResult serialization
template <> struct adl_serializer<mcp::types::ReadResourceResult> {
  static void to_json(json &j, const mcp::types::ReadResourceResult &result) {
    j = json::object();
    if (result.meta) {
      j["meta"] = *result.meta;
    }
    j["contentType"] = result.contentType;
    j["content"] = result.content;
  }

  static void from_json(const json &j, mcp::types::ReadResourceResult &result) {
    if (j.contains("meta")) {
      result.meta = j["meta"];
    }

    // Support both contentType and content_type field names
    if (j.contains("contentType")) {
      j.at("contentType").get_to(result.contentType);
    } else if (j.contains("content_type")) {
      j.at("content_type").get_to(result.contentType);
    }

    j.at("content").get_to(result.content);
  }
};

} // namespace nlohmann

#endif // MCP_TYPES_HPP_