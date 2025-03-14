#include "mcp/session/server_session.hpp"
#include "mcp/utils/error.hpp"
#include "mcp/utils/logging.hpp"

namespace mcp {

void ServerSession::respondWithError(
    const types::JSONRPCError &error,
    std::function<void(const types::JSONRPCResponse &)> respond) {
  // Send an error response directly - don't convert to a response format
  // This code path actually shouldn't be used, but is here for compatibility
  types::JSONRPCResponse response;
  response.jsonrpc = "2.0";
  response.id = error.id;
  response.result =
      nlohmann::json::object(); // Empty result, since we're sending an error
  respond(response);
}

// Overload for when respond function is not provided (uses the stored respond
// function)
void ServerSession::respondWithError(const types::JSONRPCError &error) {
  // This method adds the respond function to the call
  if (request_respond_function_) {
    // Instead of calling the other method, directly send a proper JSON-RPC
    // error
    if (transport_ && isConnected()) {
      std::error_code ec = transport_->send(error);
      if (ec) {
        utils::log(utils::LogLevel::Error,
                   "Failed to send error response: " + ec.message());
      }
    } else {
      utils::log(utils::LogLevel::Error,
                 "Cannot send error: transport not connected");
    }
  } else {
    utils::log(utils::LogLevel::Error,
               "Cannot respond with error: no respond function available");
  }
}

ServerSession::ServerSession(std::shared_ptr<transport::Transport> transport,
                             const types::ServerInfo &server_info)
    : Session(std::move(transport)), server_info_(server_info),
      initialized_(false) {}

ServerSession::~ServerSession() = default;

ServerSession &ServerSession::setServerCapabilities(
    const types::ServerCapabilities &capabilities) {
  server_capabilities_ = capabilities;
  return *this;
}

ServerSession &ServerSession::registerTool(
    const types::Tool &tool,
    std::function<nlohmann::json(const nlohmann::json &)> handler) {

  std::lock_guard<std::mutex> lock(handlers_mutex_);

  if (tool_handlers_.find(tool.name) != tool_handlers_.end()) {
    utils::log(utils::LogLevel::Warning,
               "Tool already registered: " + tool.name);
  }

  tool_handlers_[tool.name] = ToolHandler{tool, std::move(handler)};
  return *this;
}

ServerSession &ServerSession::registerResource(
    const types::Resource &resource,
    std::function<types::ReadResourceResult(const std::string &)> handler) {

  std::lock_guard<std::mutex> lock(handlers_mutex_);

  if (resource_handlers_.find(resource.uri) != resource_handlers_.end()) {
    utils::log(utils::LogLevel::Warning,
               "Resource already registered: " + resource.uri);
  }

  resource_handlers_[resource.uri] =
      ResourceHandler{resource, std::move(handler)};
  return *this;
}

ServerSession &ServerSession::registerPrompt(
    const types::Prompt &prompt,
    std::function<std::string(const nlohmann::json &)> handler) {

  std::lock_guard<std::mutex> lock(handlers_mutex_);

  if (prompt_handlers_.find(prompt.name) != prompt_handlers_.end()) {
    utils::log(utils::LogLevel::Warning,
               "Prompt already registered: " + prompt.name);
  }

  prompt_handlers_[prompt.name] = PromptHandler{prompt, std::move(handler)};
  return *this;
}

void ServerSession::connect() {
  // Set up the request handler
  setRequestHandler(
      [this](const types::JSONRPCRequest &request,
             std::function<void(const types::JSONRPCResponse &)> respond) {
        // Store the current respond function for error handling
        request_respond_function_ = respond;

        // Route the request based on the method
        if (request.method == "initialize") {
          handleInitializeRequest(request, respond);
        } else if (!initialized_) {
          // If not initialized, reject all other requests
          types::JSONRPCError error;
          error.jsonrpc = "2.0";
          error.id = request.id;
          error.error.code = static_cast<int>(types::ErrorCode::InvalidRequest);
          error.error.message = "Session not initialized";
          respondWithError(error, respond);
        } else if (request.method == "listTools") {
          handleListToolsRequest(request, respond);
        } else if (request.method == "callTool") {
          handleCallToolRequest(request, respond);
        } else if (request.method == "listResources") {
          handleListResourcesRequest(request, respond);
        } else if (request.method == "readResource") {
          handleReadResourceRequest(request, respond);
        } else if (request.method == "listPrompts") {
          handleListPromptsRequest(request, respond);
        } else if (request.method == "getPrompt") {
          handleGetPromptRequest(request, respond);
        } else {
          // Method not found
          types::JSONRPCError error;
          error.jsonrpc = "2.0";
          error.id = request.id;
          error.error.code = static_cast<int>(types::ErrorCode::MethodNotFound);
          error.error.message = "Method not found: " + request.method;
          respondWithError(error, respond);
        }

        // Clear the respond function after handling is complete
        request_respond_function_ = nullptr;
      });

  // Connect the transport
  Session::connect();
}

void ServerSession::handleInitializeRequest(
    const types::JSONRPCRequest &request,
    std::function<void(const types::JSONRPCResponse &)> respond) {

  if (!request.params.has_value()) {
    // Missing parameters
    types::JSONRPCError error;
    error.jsonrpc = "2.0";
    error.id = request.id;
    error.error.code = static_cast<int>(types::ErrorCode::InvalidParams);
    error.error.message = "Missing parameters for initialize";
    respondWithError(error, respond);
    return;
  }

  // Parse client info
  types::ClientInfo client_info;

  try {
    const auto &params = request.params.value();

    if (params.contains("clientInfo")) {
      const auto &client_info_json = params["clientInfo"];

      if (client_info_json.contains("name")) {
        client_info.name = client_info_json["name"].get<std::string>();
      }

      if (client_info_json.contains("version")) {
        client_info.version = client_info_json["version"].get<std::string>();
      }
    }

    if (params.contains("clientCapabilities")) {
      const auto &capabilities = params["clientCapabilities"];
      types::ClientCapabilities client_capabilities;

      // Parse tool capabilities
      if (capabilities.contains("tools")) {
        const auto &tools = capabilities["tools"];
        types::ToolCapabilities tool_capabilities;

        if (tools.contains("supportsStreaming")) {
          tool_capabilities.supports_streaming =
              tools["supportsStreaming"].get<bool>();
        }

        client_capabilities.tools = tool_capabilities;
      }

      // Parse resource capabilities
      if (capabilities.contains("resources")) {
        const auto &resources = capabilities["resources"];
        types::ResourceCapabilities resource_capabilities;

        if (resources.contains("supportsSubscriptions")) {
          resource_capabilities.supports_subscriptions =
              resources["supportsSubscriptions"].get<bool>();
        }

        client_capabilities.resources = resource_capabilities;
      }

      // Parse prompt capabilities
      if (capabilities.contains("prompts")) {
        const auto &prompts = capabilities["prompts"];
        types::PromptCapabilities prompt_capabilities;

        if (prompts.contains("supportsStreaming")) {
          prompt_capabilities.supports_streaming =
              prompts["supportsStreaming"].get<bool>();
        }

        client_capabilities.prompts = prompt_capabilities;
      }

      client_info.capabilities = client_capabilities;
    }
  } catch (const std::exception &e) {
    // Error parsing client info
    types::JSONRPCError error;
    error.jsonrpc = "2.0";
    error.id = request.id;
    error.error.code = static_cast<int>(types::ErrorCode::InvalidParams);
    error.error.message = std::string("Error parsing client info: ") + e.what();
    respondWithError(error, respond);
    return;
  }

  // Log client info
  utils::log(utils::LogLevel::Info, "Client connected: " + client_info.name +
                                        " v" + client_info.version);

  // Prepare response
  types::JSONRPCResponse response;
  response.jsonrpc = "2.0";
  response.id = request.id;

  // Add server info
  response.result["serverInfo"] = {{"name", server_info_.name},
                                   {"version", server_info_.version}};

  if (server_info_.instructions.has_value()) {
    response.result["serverInfo"]["instructions"] = *server_info_.instructions;
  }

  // Add server capabilities
  nlohmann::json capabilities = nlohmann::json::object();

  // Add tool capabilities
  if (server_capabilities_.tools.has_value()) {
    capabilities["tools"] = {
        {"supportsStreaming", server_capabilities_.tools->supports_streaming}};
  }

  // Add resource capabilities
  if (server_capabilities_.resources.has_value()) {
    capabilities["resources"] = {
        {"supportsSubscriptions",
         server_capabilities_.resources->supports_subscriptions}};
  }

  // Add prompt capabilities
  if (server_capabilities_.prompts.has_value()) {
    capabilities["prompts"] = {
        {"supportsStreaming",
         server_capabilities_.prompts->supports_streaming}};
  }

  response.result["serverCapabilities"] = capabilities;

  // Mark as initialized
  initialized_ = true;

  // Send response
  respond(response);
}

void ServerSession::handleListToolsRequest(
    const types::JSONRPCRequest &request,
    std::function<void(const types::JSONRPCResponse &)> respond) {

  // Prepare response
  types::JSONRPCResponse response;
  response.jsonrpc = "2.0";
  response.id = request.id;

  // Add tools
  nlohmann::json tools = nlohmann::json::array();

  {
    std::lock_guard<std::mutex> lock(handlers_mutex_);

    for (const auto &[name, handler] : tool_handlers_) {
      nlohmann::json tool = {{"name", handler.tool.name},
                             {"description", handler.tool.description}};

      if (handler.tool.input_schema.has_value()) {
        tool["inputSchema"] = handler.tool.input_schema.value();
      }

      tools.push_back(tool);
    }
  }

  response.result["tools"] = tools;

  // Send response
  respond(response);
}

void ServerSession::handleCallToolRequest(
    const types::JSONRPCRequest &request,
    std::function<void(const types::JSONRPCResponse &)> respond) {

  if (!request.params.has_value()) {
    // Missing parameters
    types::JSONRPCError error;
    error.jsonrpc = "2.0";
    error.id = request.id;
    error.error.code = static_cast<int>(types::ErrorCode::InvalidParams);
    error.error.message = "Missing parameters for callTool";
    respondWithError(error, respond);
    return;
  }

  try {
    const auto &params = request.params.value();

    // Get tool name
    if (!params.contains("name") || !params["name"].is_string()) {
      // Missing or invalid tool name
      types::JSONRPCError error;
      error.jsonrpc = "2.0";
      error.id = request.id;
      error.error.code = static_cast<int>(types::ErrorCode::InvalidParams);
      error.error.message = "Missing or invalid tool name";
      respondWithError(error, respond);
      return;
    }

    std::string tool_name = params["name"].get<std::string>();

    // Get tool parameters
    nlohmann::json tool_params;
    if (params.contains("parameters")) {
      tool_params = params["parameters"];
    }

    // Find the tool handler
    std::function<nlohmann::json(const nlohmann::json &)> handler;

    {
      std::lock_guard<std::mutex> lock(handlers_mutex_);

      auto it = tool_handlers_.find(tool_name);
      if (it == tool_handlers_.end()) {
        // Tool not found
        types::JSONRPCError error;
        error.jsonrpc = "2.0";
        error.id = request.id;
        error.error.code = static_cast<int>(types::ErrorCode::InvalidParams);
        error.error.message = "Tool not found: " + tool_name;
        respondWithError(error, respond);
        return;
      }

      handler = it->second.handler;
    }

    // Call the tool handler
    try {
      nlohmann::json result = handler(tool_params);

      // Prepare response
      types::JSONRPCResponse response;
      response.jsonrpc = "2.0";
      response.id = request.id;
      response.result["result"] = result;

      // Send response
      respond(response);
    } catch (const std::exception &e) {
      // Error calling tool
      types::JSONRPCError error;
      error.jsonrpc = "2.0";
      error.id = request.id;
      error.error.code = static_cast<int>(types::ErrorCode::InternalError);
      error.error.message = std::string("Error calling tool: ") + e.what();
      respondWithError(error, respond);
    }
  } catch (const std::exception &e) {
    // Error parsing parameters
    types::JSONRPCError error;
    error.jsonrpc = "2.0";
    error.id = request.id;
    error.error.code = static_cast<int>(types::ErrorCode::InvalidParams);
    error.error.message = std::string("Error parsing parameters: ") + e.what();
    respondWithError(error, respond);
  }
}

void ServerSession::handleListResourcesRequest(
    const types::JSONRPCRequest &request,
    std::function<void(const types::JSONRPCResponse &)> respond) {

  // Prepare response
  types::JSONRPCResponse response;
  response.jsonrpc = "2.0";
  response.id = request.id;

  // Add resources
  nlohmann::json resources = nlohmann::json::array();

  {
    std::lock_guard<std::mutex> lock(handlers_mutex_);

    for (const auto &[uri, handler] : resource_handlers_) {
      nlohmann::json resource = {{"uri", handler.resource.uri}};

      if (!handler.resource.name.empty()) {
        resource["name"] = handler.resource.name;
      }

      if (!handler.resource.description.empty()) {
        resource["description"] = handler.resource.description;
      }

      resources.push_back(resource);
    }
  }

  response.result["resources"] = resources;

  // Send response
  respond(response);
}

void ServerSession::handleReadResourceRequest(
    const types::JSONRPCRequest &request,
    std::function<void(const types::JSONRPCResponse &)> respond) {

  if (!request.params.has_value()) {
    // Missing parameters
    types::JSONRPCError error;
    error.jsonrpc = "2.0";
    error.id = request.id;
    error.error.code = static_cast<int>(types::ErrorCode::InvalidParams);
    error.error.message = "Missing parameters for readResource";
    respondWithError(error, respond);
    return;
  }

  try {
    const auto &params = request.params.value();

    // Get resource URI
    if (!params.contains("uri") || !params["uri"].is_string()) {
      // Missing or invalid resource URI
      types::JSONRPCError error;
      error.jsonrpc = "2.0";
      error.id = request.id;
      error.error.code = static_cast<int>(types::ErrorCode::InvalidParams);
      error.error.message = "Missing or invalid resource URI";
      respondWithError(error, respond);
      return;
    }

    std::string uri = params["uri"].get<std::string>();

    // Find the resource handler
    std::function<types::ReadResourceResult(const std::string &)> handler;

    {
      std::lock_guard<std::mutex> lock(handlers_mutex_);

      auto it = resource_handlers_.find(uri);
      if (it == resource_handlers_.end()) {
        // Resource not found
        types::JSONRPCError error;
        error.jsonrpc = "2.0";
        error.id = request.id;
        error.error.code = static_cast<int>(types::ErrorCode::InvalidParams);
        error.error.message = "Resource not found: " + uri;
        respondWithError(error, respond);
        return;
      }

      handler = it->second.handler;
    }

    // Call the resource handler
    try {
      types::ReadResourceResult result = handler(uri);

      // Prepare response
      types::JSONRPCResponse response;
      response.jsonrpc = "2.0";
      response.id = request.id;
      response.result["content"] = result.content;

      // Use the contentType field directly
      if (!result.contentType.empty()) {
        response.result["contentType"] = result.contentType;
      }

      // Send response
      respond(response);
    } catch (const std::exception &e) {
      // Error reading resource
      types::JSONRPCError error;
      error.jsonrpc = "2.0";
      error.id = request.id;
      error.error.code = static_cast<int>(types::ErrorCode::InternalError);
      error.error.message = std::string("Error reading resource: ") + e.what();
      respondWithError(error, respond);
    }
  } catch (const std::exception &e) {
    // Error parsing parameters
    types::JSONRPCError error;
    error.jsonrpc = "2.0";
    error.id = request.id;
    error.error.code = static_cast<int>(types::ErrorCode::InvalidParams);
    error.error.message = std::string("Error parsing parameters: ") + e.what();
    respondWithError(error, respond);
  }
}

void ServerSession::handleListPromptsRequest(
    const types::JSONRPCRequest &request,
    std::function<void(const types::JSONRPCResponse &)> respond) {

  // Prepare response
  types::JSONRPCResponse response;
  response.jsonrpc = "2.0";
  response.id = request.id;

  // Add prompts
  nlohmann::json prompts = nlohmann::json::array();

  {
    std::lock_guard<std::mutex> lock(handlers_mutex_);

    for (const auto &[name, handler] : prompt_handlers_) {
      nlohmann::json prompt = {{"name", handler.prompt.name}};

      if (!handler.prompt.description.empty()) {
        prompt["description"] = handler.prompt.description;
      }

      if (!handler.prompt.arguments.empty()) {
        nlohmann::json arguments = nlohmann::json::array();

        for (const auto &arg : handler.prompt.arguments) {
          nlohmann::json argument = {{"name", arg.name}};

          if (!arg.description.empty()) {
            argument["description"] = arg.description;
          }

          if (arg.required) {
            argument["required"] = arg.required;
          }

          arguments.push_back(argument);
        }

        prompt["arguments"] = arguments;
      }

      prompts.push_back(prompt);
    }
  }

  response.result["prompts"] = prompts;

  // Send response
  respond(response);
}

void ServerSession::handleGetPromptRequest(
    const types::JSONRPCRequest &request,
    std::function<void(const types::JSONRPCResponse &)> respond) {

  if (!request.params.has_value()) {
    // Missing parameters
    types::JSONRPCError error;
    error.jsonrpc = "2.0";
    error.id = request.id;
    error.error.code = static_cast<int>(types::ErrorCode::InvalidParams);
    error.error.message = "Missing parameters for getPrompt";
    respondWithError(error, respond);
    return;
  }

  try {
    const auto &params = request.params.value();

    // Get prompt name
    if (!params.contains("name") || !params["name"].is_string()) {
      // Missing or invalid prompt name
      types::JSONRPCError error;
      error.jsonrpc = "2.0";
      error.id = request.id;
      error.error.code = static_cast<int>(types::ErrorCode::InvalidParams);
      error.error.message = "Missing or invalid prompt name";
      respondWithError(error, respond);
      return;
    }

    std::string name = params["name"].get<std::string>();

    // Get prompt arguments
    nlohmann::json args;
    if (params.contains("arguments")) {
      args = params["arguments"];
    }

    // Find the prompt handler
    std::function<std::string(const nlohmann::json &)> handler;
    types::Prompt prompt;

    {
      std::lock_guard<std::mutex> lock(handlers_mutex_);

      auto it = prompt_handlers_.find(name);
      if (it == prompt_handlers_.end()) {
        // Prompt not found
        types::JSONRPCError error;
        error.jsonrpc = "2.0";
        error.id = request.id;
        error.error.code = static_cast<int>(types::ErrorCode::InvalidParams);
        error.error.message = "Prompt not found: " + name;
        respondWithError(error, respond);
        return;
      }

      handler = it->second.handler;
      prompt = it->second.prompt;
    }

    // Validate required arguments
    for (const auto &arg : prompt.arguments) {
      if (arg.required &&
          (!args.contains(arg.name) || args[arg.name].is_null())) {
        // Missing required argument
        types::JSONRPCError error;
        error.jsonrpc = "2.0";
        error.id = request.id;
        error.error.code = static_cast<int>(types::ErrorCode::InvalidParams);
        error.error.message = "Missing required argument: " + arg.name;
        respondWithError(error, respond);
        return;
      }
    }

    // Call the prompt handler
    try {
      std::string result = handler(args);

      // Prepare response
      types::JSONRPCResponse response;
      response.jsonrpc = "2.0";
      response.id = request.id;
      response.result["prompt"] = result;

      // Send response
      respond(response);
    } catch (const std::exception &e) {
      // Error getting prompt
      types::JSONRPCError error;
      error.jsonrpc = "2.0";
      error.id = request.id;
      error.error.code = static_cast<int>(types::ErrorCode::InternalError);
      error.error.message = std::string("Error getting prompt: ") + e.what();
      respondWithError(error, respond);
    }
  } catch (const std::exception &e) {
    // Error parsing parameters
    types::JSONRPCError error;
    error.jsonrpc = "2.0";
    error.id = request.id;
    error.error.code = static_cast<int>(types::ErrorCode::InvalidParams);
    error.error.message = std::string("Error parsing parameters: ") + e.what();
    respondWithError(error, respond);
  }
}

} // namespace mcp
