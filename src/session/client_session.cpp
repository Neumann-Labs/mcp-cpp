#include "mcp/session/client_session.hpp"
#include "mcp/utils/error.hpp"
#include "mcp/utils/logging.hpp"
#include <algorithm>
#include <thread>

namespace mcp {

ClientSession::ClientSession(std::shared_ptr<transport::Transport> transport,
                             const ClientSessionConfig &config)
    : Session(std::move(transport)), config_(config),
      reconnect_info_({false, 0, config.initialReconnectDelay}) {

  // Set up notification handlers for resource updates and progress reports
  setupNotificationHandlers();
}

ClientSession::~ClientSession() {
  // Unsubscribe from all resources when the session is destroyed
  std::unordered_map<std::string, ResourceUpdateCallback> subscriptions;
  {
    std::lock_guard<std::mutex> lock(subscription_mutex_);
    subscriptions = resource_subscriptions_;
  }

  for (const auto &[uri, _] : subscriptions) {
    try {
      unsubscribeFromResource(uri).wait();
    } catch (const std::exception &e) {
      utils::logger::warn("Failed to unsubscribe from resource {}: {}", uri,
                          e.what());
    }
  }
}

void ClientSession::setupNotificationHandlers() {
  // Register handler for resource update notifications
  registerNotificationHandler(
      "resourceUpdate", [this](const types::JSONRPCNotification &notification) {
        handleResourceUpdate(notification);
      });

  // Register handler for progress update notifications
  registerNotificationHandler(
      "progressUpdate", [this](const types::JSONRPCNotification &notification) {
        handleProgressUpdate(notification);
      });
}

std::future<types::InitializeResult>
ClientSession::initialize(const types::ClientInfo &client_info,
                          std::chrono::milliseconds timeout) {

  // Store client info for reconnection
  {
    std::lock_guard<std::mutex> lock(client_info_mutex_);
    client_info_ = client_info;
  }

  // Prepare initialize parameters
  nlohmann::json params = {
      {"clientInfo",
       {{"name", client_info.name}, {"version", client_info.version}}}};

  // Add client capabilities if provided
  if (client_info.capabilities.has_value()) {
    nlohmann::json capabilities = nlohmann::json::object();

    // Add tool capabilities
    if (client_info.capabilities->tools.has_value()) {
      capabilities["tools"] = {
          {"supportsStreaming",
           client_info.capabilities->tools->supports_streaming}};
    }

    // Add resource capabilities
    if (client_info.capabilities->resources.has_value()) {
      capabilities["resources"] = {
          {"supportsSubscriptions",
           client_info.capabilities->resources->supports_subscriptions}};
    }

    // Add prompt capabilities
    if (client_info.capabilities->prompts.has_value()) {
      capabilities["prompts"] = {
          {"supportsStreaming",
           client_info.capabilities->prompts->supports_streaming}};
    }

    params["clientCapabilities"] = capabilities;
  }

  // Send the initialize request
  auto response_future = sendRequest("initialize", params, timeout);

  // Transform the result
  return std::async(std::launch::deferred, [response_future =
                                                std::move(response_future),
                                            this]() mutable {
    try {
      auto response = response_future.get();

      // Validate response if enabled
      validateResponse("initialize", response.result);

      // Parse the initialize result
      types::InitializeResult result;

      // Support both formats: either nested serverInfo or direct serverName
      // fields
      if (response.result.contains("serverInfo")) {
        // Format 1: Nested serverInfo object
        auto &server_info = response.result["serverInfo"];
        result.serverName = server_info["name"].get<std::string>();
        result.serverVersion = server_info["version"].get<std::string>();

        if (server_info.contains("instructions")) {
          result.instructions = server_info["instructions"].get<std::string>();
        }
      } else if (response.result.contains("serverName")) {
        // Format 2: Direct fields at the root level
        result.serverName = response.result["serverName"].get<std::string>();
        result.serverVersion =
            response.result["serverVersion"].get<std::string>();

        if (response.result.contains("instructions")) {
          result.instructions =
              response.result["instructions"].get<std::string>();
        }
      }

      // Update backward compatibility fields
      result.server_info.name = result.serverName;
      result.server_info.version = result.serverVersion;
      result.server_info.instructions = result.instructions;

      // Support both formats: either nested serverCapabilities or direct
      // capabilities fields
      if (response.result.contains("serverCapabilities")) {
        // Format 1: Nested serverCapabilities object
        auto capabilities = response.result["serverCapabilities"];
        types::ServerCapabilities server_capabilities;

        // Parse tool capabilities
        if (capabilities.contains("tools")) {
          auto &tools = capabilities["tools"];
          types::ToolCapabilities tool_capabilities;

          if (tools.contains("supportsStreaming")) {
            tool_capabilities.supports_streaming =
                tools["supportsStreaming"].get<bool>();
          }

          server_capabilities.tools = tool_capabilities;
        }

        // Parse resource capabilities
        if (capabilities.contains("resources")) {
          auto &resources = capabilities["resources"];
          types::ResourceCapabilities resource_capabilities;

          if (resources.contains("supportsSubscriptions")) {
            resource_capabilities.supports_subscriptions =
                resources["supportsSubscriptions"].get<bool>();
          }

          server_capabilities.resources = resource_capabilities;
        }

        // Parse prompt capabilities
        if (capabilities.contains("prompts")) {
          auto &prompts = capabilities["prompts"];
          types::PromptCapabilities prompt_capabilities;

          if (prompts.contains("supportsStreaming")) {
            prompt_capabilities.supports_streaming =
                prompts["supportsStreaming"].get<bool>();
          }

          server_capabilities.prompts = prompt_capabilities;
        }

        // Store server capabilities
        handleServerCapabilities(server_capabilities);
        result.capabilities = server_capabilities;

        // Update backward compatibility field
        result.server_capabilities = server_capabilities;
      } else if (response.result.contains("capabilities")) {
        // Format 2: Direct capabilities field at the root level
        auto capabilities = response.result["capabilities"];
        types::ServerCapabilities server_capabilities;

        // Parse tool capabilities
        if (capabilities.contains("tools")) {
          auto &tools = capabilities["tools"];
          types::ToolCapabilities tool_capabilities;

          if (tools.contains("supportsStreaming")) {
            tool_capabilities.supports_streaming =
                tools["supportsStreaming"].get<bool>();
          }

          server_capabilities.tools = tool_capabilities;
        }

        // Parse resource capabilities
        if (capabilities.contains("resources")) {
          auto &resources = capabilities["resources"];
          types::ResourceCapabilities resource_capabilities;

          if (resources.contains("supportsSubscriptions")) {
            resource_capabilities.supports_subscriptions =
                resources["supportsSubscriptions"].get<bool>();
          }

          server_capabilities.resources = resource_capabilities;
        }

        // Parse prompt capabilities
        if (capabilities.contains("prompts")) {
          auto &prompts = capabilities["prompts"];
          types::PromptCapabilities prompt_capabilities;

          if (prompts.contains("supportsStreaming")) {
            prompt_capabilities.supports_streaming =
                prompts["supportsStreaming"].get<bool>();
          }

          server_capabilities.prompts = prompt_capabilities;
        }

        // Store server capabilities
        handleServerCapabilities(server_capabilities);
        result.capabilities = server_capabilities;

        // Update backward compatibility field
        result.server_capabilities = server_capabilities;
      }

      // Clear caches on successful initialization
      clearCache();

      return result;
    } catch (const std::exception &e) {
      throw utils::ProtocolException(types::ErrorCode::ProtocolError,
                                     std::string("Initialize failed: ") +
                                         e.what());
    }
  });
}

std::future<types::ListToolsResult>
ClientSession::listTools(bool force_refresh,
                         std::chrono::milliseconds timeout) {

  // Check cache first if not forcing refresh
  if (!force_refresh) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    if (isCacheValid(tools_cache_)) {
      utils::logger::debug("Using cached tool listing");
      return std::async(std::launch::deferred,
                        [cache = tools_cache_->data]() { return cache; });
    }
  }

  // Send the listTools request
  auto response_future = sendRequest("listTools", std::nullopt, timeout);

  // Transform the result
  return std::async(
      std::launch::deferred,
      [response_future = std::move(response_future), this]() mutable {
        try {
          auto response = response_future.get();

          // Validate response if enabled
          validateResponse("listTools", response.result);

          // Parse the list tools result
          types::ListToolsResult result;

          if (response.result.contains("tools") &&
              response.result["tools"].is_array()) {
            for (const auto &tool_json : response.result["tools"]) {
              types::Tool tool;

              tool.name = tool_json["name"].get<std::string>();
              tool.description = tool_json["description"].get<std::string>();

              if (tool_json.contains("inputSchema")) {
                tool.input_schema = tool_json["inputSchema"];
              }

              result.tools.push_back(std::move(tool));
            }
          }

          // Update cache
          {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            tools_cache_ = CacheEntry<types::ListToolsResult>{
                result, calculateExpirationTime()};
            utils::logger::debug("Updated tool listing cache with {} tools",
                                 result.tools.size());
          }

          return result;
        } catch (const std::exception &e) {
          throw utils::ProtocolException(types::ErrorCode::ProtocolError,
                                         std::string("List tools failed: ") +
                                             e.what());
        }
      });
}

std::future<types::CallToolResult> ClientSession::callTool(
    const std::string &name, const nlohmann::json &params,
    const std::optional<ProgressCallback> &progress_callback,
    std::chrono::milliseconds timeout) {

  // Prepare callTool parameters
  nlohmann::json call_params = {{"name", name}, {"parameters", params}};

  // Add meta.progressToken if progress callback is provided
  if (progress_callback) {
    // Check if server supports progress reporting
    bool supports_progress = false;
    {
      std::lock_guard<std::mutex> lock(capabilities_mutex_);
      if (server_capabilities_.tools.has_value()) {
        supports_progress = server_capabilities_.tools->supports_streaming;
      }
    }

    if (supports_progress) {
      // Generate a unique progress token
      std::string progress_token =
          "progress_" +
          std::to_string(getCurrentTime().time_since_epoch().count());

      // Add meta.progressToken to the request
      call_params["meta"] = {{"progressToken", progress_token}};

      // Register the progress callback
      {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        progress_callbacks_[progress_token] = *progress_callback;

        // Set up a timer to remove the callback after the timeout
        std::thread([this, progress_token, timeout]() {
          std::this_thread::sleep_for(
              timeout + std::chrono::seconds(5)); // Add 5 seconds buffer

          std::lock_guard<std::mutex> lock(progress_mutex_);
          auto it = progress_callbacks_.find(progress_token);
          if (it != progress_callbacks_.end()) {
            utils::logger::debug(
                "Removing expired progress callback for token {}",
                progress_token);
            progress_callbacks_.erase(it);
          }
        }).detach();
      }

      utils::logger::debug(
          "Registered progress callback for tool {} with token {}", name,
          progress_token);
    } else {
      utils::logger::warn("Progress callback provided but server does not "
                          "support streaming for tools");
    }
  }

  // Send the callTool request
  auto response_future = sendRequest("callTool", call_params, timeout);

  // Transform the result
  return std::async(
      std::launch::deferred,
      [response_future = std::move(response_future), this]() mutable {
        try {
          auto response = response_future.get();

          // Validate response if enabled
          validateResponse("callTool", response.result);

          // Parse the call tool result
          types::CallToolResult result;

          if (response.result.contains("result")) {
            result.result = response.result["result"];
          }

          return result;
        } catch (const std::exception &e) {
          throw utils::ProtocolException(types::ErrorCode::ProtocolError,
                                         std::string("Call tool failed: ") +
                                             e.what());
        }
      });
}

std::future<types::ListResourcesResult>
ClientSession::listResources(bool force_refresh,
                             std::chrono::milliseconds timeout) {

  // Check cache first if not forcing refresh
  if (!force_refresh) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    if (isCacheValid(resources_cache_)) {
      utils::logger::debug("Using cached resource listing");
      return std::async(std::launch::deferred,
                        [cache = resources_cache_->data]() { return cache; });
    }
  }

  // Send the listResources request
  auto response_future = sendRequest("listResources", std::nullopt, timeout);

  // Transform the result
  return std::async(std::launch::deferred, [response_future =
                                                std::move(response_future),
                                            this]() mutable {
    try {
      auto response = response_future.get();

      // Validate response if enabled
      validateResponse("listResources", response.result);

      // Parse the list resources result
      types::ListResourcesResult result;

      if (response.result.contains("resources") &&
          response.result["resources"].is_array()) {
        for (const auto &resource_json : response.result["resources"]) {
          types::Resource resource;

          resource.uri = resource_json["uri"].get<std::string>();

          if (resource_json.contains("name")) {
            resource.name = resource_json["name"].get<std::string>();
          }

          if (resource_json.contains("description")) {
            resource.description =
                resource_json["description"].get<std::string>();
          }

          result.resources.push_back(std::move(resource));
        }
      }

      // Update cache
      {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        resources_cache_ = CacheEntry<types::ListResourcesResult>{
            result, calculateExpirationTime()};
        utils::logger::debug("Updated resource listing cache with {} resources",
                             result.resources.size());
      }

      return result;
    } catch (const std::exception &e) {
      throw utils::ProtocolException(types::ErrorCode::ProtocolError,
                                     std::string("List resources failed: ") +
                                         e.what());
    }
  });
}

std::future<types::ReadResourceResult>
ClientSession::readResource(const std::string &uri,
                            std::chrono::milliseconds timeout) {

  // Prepare readResource parameters
  nlohmann::json params = {{"uri", uri}};

  // Send the readResource request
  auto response_future = sendRequest("readResource", params, timeout);

  // Transform the result
  return std::async(std::launch::deferred, [response_future =
                                                std::move(response_future),
                                            this]() mutable {
    try {
      auto response = response_future.get();

      // Validate response if enabled
      validateResponse("readResource", response.result);

      // Parse the read resource result
      types::ReadResourceResult result;

      if (response.result.contains("content")) {
        result.content = response.result["content"];
      }

      if (response.result.contains("contentType")) {
        result.contentType = response.result["contentType"].get<std::string>();
      }

      return result;
    } catch (const std::exception &e) {
      throw utils::ProtocolException(types::ErrorCode::ProtocolError,
                                     std::string("Read resource failed: ") +
                                         e.what());
    }
  });
}

std::future<bool>
ClientSession::subscribeToResource(const std::string &uri,
                                   ResourceUpdateCallback callback,
                                   std::chrono::milliseconds timeout) {

  // Check if server supports resource subscriptions
  bool supports_subscriptions = false;
  {
    std::lock_guard<std::mutex> lock(capabilities_mutex_);
    if (server_capabilities_.resources.has_value()) {
      supports_subscriptions =
          server_capabilities_.resources->supports_subscriptions;
    }
  }

  if (!supports_subscriptions) {
    utils::logger::warn("Server does not support resource subscriptions");
    return std::async(std::launch::deferred, []() { return false; });
  }

  // Store the callback before sending the request
  {
    std::lock_guard<std::mutex> lock(subscription_mutex_);
    resource_subscriptions_[uri] = callback;
    utils::logger::debug("Registered callback for resource updates to {}", uri);
  }

  // Prepare subscription parameters
  nlohmann::json params = {{"uri", uri}};

  // Send the subscribeToResource request
  auto response_future = sendRequest("subscribeToResource", params, timeout);

  // Transform the result
  return std::async(std::launch::deferred, [response_future =
                                                std::move(response_future),
                                            uri, this]() mutable {
    try {
      auto response = response_future.get();

      // Validate response if enabled
      validateResponse("subscribeToResource", response.result);

      // Parse the subscription result (success flag)
      bool success = true;

      if (response.result.contains("success")) {
        success = response.result["success"].get<bool>();
      }

      if (!success) {
        // Remove the callback if subscription failed
        std::lock_guard<std::mutex> lock(subscription_mutex_);
        resource_subscriptions_.erase(uri);
        utils::logger::warn("Server rejected subscription to resource {}", uri);
      } else {
        utils::logger::info("Successfully subscribed to resource: {}", uri);
      }

      return success;
    } catch (const std::exception &e) {
      // Remove the callback if subscription failed
      {
        std::lock_guard<std::mutex> lock(subscription_mutex_);
        resource_subscriptions_.erase(uri);
      }

      throw utils::ProtocolException(
          types::ErrorCode::ProtocolError,
          std::string("Subscribe to resource failed: ") + e.what());
    }
  });
}

std::future<bool>
ClientSession::unsubscribeFromResource(const std::string &uri,
                                       std::chrono::milliseconds timeout) {

  // Check if we have an active subscription to this resource
  bool has_subscription = false;
  {
    std::lock_guard<std::mutex> lock(subscription_mutex_);
    has_subscription =
        resource_subscriptions_.find(uri) != resource_subscriptions_.end();
  }

  if (!has_subscription) {
    utils::logger::warn("No active subscription to resource {}", uri);
    return std::async(std::launch::deferred, []() { return false; });
  }

  // Prepare unsubscription parameters
  nlohmann::json params = {{"uri", uri}};

  // Send the unsubscribeFromResource request
  auto response_future =
      sendRequest("unsubscribeFromResource", params, timeout);

  // Transform the result
  return std::async(std::launch::deferred, [response_future =
                                                std::move(response_future),
                                            uri, this]() mutable {
    try {
      auto response = response_future.get();

      // Validate response if enabled
      validateResponse("unsubscribeFromResource", response.result);

      // Parse the unsubscription result (success flag)
      bool success = true;

      if (response.result.contains("success")) {
        success = response.result["success"].get<bool>();
      }

      // Remove the callback regardless of success (server might have removed
      // the subscription)
      {
        std::lock_guard<std::mutex> lock(subscription_mutex_);
        resource_subscriptions_.erase(uri);
      }

      if (success) {
        utils::logger::info("Successfully unsubscribed from resource: {}", uri);
      } else {
        utils::logger::warn(
            "Server reported failure to unsubscribe from resource {}", uri);
      }

      return success;
    } catch (const std::exception &e) {
      // Remove the callback regardless of the error
      {
        std::lock_guard<std::mutex> lock(subscription_mutex_);
        resource_subscriptions_.erase(uri);
      }

      throw utils::ProtocolException(
          types::ErrorCode::ProtocolError,
          std::string("Unsubscribe from resource failed: ") + e.what());
    }
  });
}

std::future<types::ListPromptsResult>
ClientSession::listPrompts(bool force_refresh,
                           std::chrono::milliseconds timeout) {

  // Check cache first if not forcing refresh
  if (!force_refresh) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    if (isCacheValid(prompts_cache_)) {
      utils::logger::debug("Using cached prompt listing");
      return std::async(std::launch::deferred,
                        [cache = prompts_cache_->data]() { return cache; });
    }
  }

  // Send the listPrompts request
  auto response_future = sendRequest("listPrompts", std::nullopt, timeout);

  // Transform the result
  return std::async(std::launch::deferred, [response_future =
                                                std::move(response_future),
                                            this]() mutable {
    try {
      auto response = response_future.get();

      // Validate response if enabled
      validateResponse("listPrompts", response.result);

      // Parse the list prompts result
      types::ListPromptsResult result;

      if (response.result.contains("prompts") &&
          response.result["prompts"].is_array()) {
        for (const auto &prompt_json : response.result["prompts"]) {
          types::Prompt prompt;

          prompt.name = prompt_json["name"].get<std::string>();

          if (prompt_json.contains("description")) {
            prompt.description = prompt_json["description"].get<std::string>();
          }

          if (prompt_json.contains("arguments") &&
              prompt_json["arguments"].is_array()) {
            for (const auto &arg_json : prompt_json["arguments"]) {
              types::PromptArgument arg;

              arg.name = arg_json["name"].get<std::string>();

              if (arg_json.contains("description")) {
                arg.description = arg_json["description"].get<std::string>();
              }

              if (arg_json.contains("required")) {
                arg.required = arg_json["required"].get<bool>();
              }

              prompt.arguments.push_back(std::move(arg));
            }
          }

          result.prompts.push_back(std::move(prompt));
        }
      }

      // Update cache
      {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        prompts_cache_ = CacheEntry<types::ListPromptsResult>{
            result, calculateExpirationTime()};
        utils::logger::debug("Updated prompt listing cache with {} prompts",
                             result.prompts.size());
      }

      return result;
    } catch (const std::exception &e) {
      throw utils::ProtocolException(types::ErrorCode::ProtocolError,
                                     std::string("List prompts failed: ") +
                                         e.what());
    }
  });
}

std::future<types::GetPromptResult>
ClientSession::getPrompt(const std::string &name, const nlohmann::json &args,
                         std::chrono::milliseconds timeout) {

  // Prepare getPrompt parameters
  nlohmann::json params = {{"name", name}, {"arguments", args}};

  // Send the getPrompt request
  auto response_future = sendRequest("getPrompt", params, timeout);

  // Transform the result
  return std::async(
      std::launch::deferred,
      [response_future = std::move(response_future), this]() mutable {
        try {
          auto response = response_future.get();

          // Validate response if enabled
          validateResponse("getPrompt", response.result);

          // Parse the get prompt result
          types::GetPromptResult result;

          if (response.result.contains("prompt")) {
            result.prompt = response.result["prompt"].get<std::string>();
          }

          return result;
        } catch (const std::exception &e) {
          throw utils::ProtocolException(types::ErrorCode::ProtocolError,
                                         std::string("Get prompt failed: ") +
                                             e.what());
        }
      });
}

void ClientSession::handleServerCapabilities(
    const types::ServerCapabilities &capabilities) {
  std::lock_guard<std::mutex> lock(capabilities_mutex_);
  server_capabilities_ = capabilities;
}

void ClientSession::clearCache() {
  std::lock_guard<std::mutex> lock(cache_mutex_);
  tools_cache_.reset();
  resources_cache_.reset();
  prompts_cache_.reset();

  utils::logger::debug("Cleared all client session caches");
}

void ClientSession::setConfig(const ClientSessionConfig &config) {
  std::lock_guard<std::mutex> lock(config_mutex_);

  // Store old caching setting to check if it changed
  bool old_caching_enabled = config_.enableCaching;

  // Update config
  config_ = config;

  // Reset reconnection parameters if they changed
  {
    std::lock_guard<std::mutex> reconnect_lock(reconnect_mutex_);
    reconnect_info_.current_delay = config_.initialReconnectDelay;
  }

  // Clear cache if caching was enabled and is now disabled
  if (old_caching_enabled && !config_.enableCaching) {
    clearCache();
  }

  utils::logger::debug("Updated client session configuration");
}

ClientSessionConfig ClientSession::getConfig() const {
  std::lock_guard<std::mutex> lock(config_mutex_);
  return config_;
}

std::chrono::steady_clock::time_point ClientSession::getCurrentTime() const {
  return std::chrono::steady_clock::now();
}

std::chrono::steady_clock::time_point
ClientSession::calculateExpirationTime() const {
  std::lock_guard<std::mutex> lock(config_mutex_);

  // If TTL is 0, never expire
  if (config_.cacheTTL.count() == 0) {
    // Use the maximum possible time point as "never expire"
    return std::chrono::steady_clock::time_point::max();
  }

  return getCurrentTime() + config_.cacheTTL;
}

template <typename T>
bool ClientSession::isCacheValid(
    const std::optional<CacheEntry<T>> &entry) const {
  std::lock_guard<std::mutex> lock(config_mutex_);

  // If caching is disabled, always invalid
  if (!config_.enableCaching) {
    return false;
  }

  // If entry is missing, it's invalid
  if (!entry.has_value()) {
    return false;
  }

  // Check if entry has expired
  return getCurrentTime() < entry->expiration;
}

void ClientSession::validateResponse(const std::string &method,
                                     const nlohmann::json &json) {
  std::lock_guard<std::mutex> lock(config_mutex_);

  // Skip validation if disabled
  if (!config_.validateResponses) {
    return;
  }

  // TODO: Implement actual JSON schema validation based on method
  // This would typically use a JSON schema validation library

  // For now, just do basic validation
  if (method == "initialize") {
    if (!json.contains("serverInfo") && !json.contains("serverName")) {
      throw utils::ProtocolException(
          types::ErrorCode::InvalidResponse,
          "Initialize response missing required server information");
    }
  } else if (method == "listTools") {
    if (!json.contains("tools") || !json["tools"].is_array()) {
      throw utils::ProtocolException(
          types::ErrorCode::InvalidResponse,
          "listTools response missing or invalid 'tools' array");
    }
  } else if (method == "callTool") {
    if (!json.contains("result")) {
      throw utils::ProtocolException(
          types::ErrorCode::InvalidResponse,
          "callTool response missing 'result' field");
    }
  } else if (method == "listResources") {
    if (!json.contains("resources") || !json["resources"].is_array()) {
      throw utils::ProtocolException(
          types::ErrorCode::InvalidResponse,
          "listResources response missing or invalid 'resources' array");
    }
  } else if (method == "readResource") {
    if (!json.contains("content")) {
      throw utils::ProtocolException(
          types::ErrorCode::InvalidResponse,
          "readResource response missing 'content' field");
    }
  } else if (method == "listPrompts") {
    if (!json.contains("prompts") || !json["prompts"].is_array()) {
      throw utils::ProtocolException(
          types::ErrorCode::InvalidResponse,
          "listPrompts response missing or invalid 'prompts' array");
    }
  } else if (method == "getPrompt") {
    if (!json.contains("prompt")) {
      throw utils::ProtocolException(
          types::ErrorCode::InvalidResponse,
          "getPrompt response missing 'prompt' field");
    }
  } else if (method == "subscribeToResource") {
    // No specific fields to validate for subscribe
  } else if (method == "unsubscribeFromResource") {
    // No specific fields to validate for unsubscribe
  }
}

void ClientSession::handleTransportDisconnect(
    const std::optional<std::error_code> &error) {
  utils::logger::info("Transport disconnected: {}", error.has_value()
                                                        ? error->message()
                                                        : "No error provided");

  std::lock_guard<std::mutex> lock(config_mutex_);

  // If auto-reconnect is disabled, just call the base implementation
  if (!config_.enableAutoReconnect) {
    Session::handleTransportDisconnect(error);
    return;
  }

  // Attempt to reconnect
  attemptReconnect();
}

void ClientSession::attemptReconnect() {
  std::lock_guard<std::mutex> lock(reconnect_mutex_);

  // If already reconnecting, don't start another attempt
  if (reconnect_info_.reconnecting) {
    return;
  }

  reconnect_info_.reconnecting = true;

  // Check if we've exceeded the maximum attempts
  {
    std::lock_guard<std::mutex> config_lock(config_mutex_);
    if (config_.maxReconnectAttempts > 0 &&
        reconnect_info_.attempts >= config_.maxReconnectAttempts) {
      utils::logger::error("Maximum reconnection attempts ({}) exceeded",
                           config_.maxReconnectAttempts);

      // Reset reconnection state for next time
      reconnect_info_.reconnecting = false;
      reconnect_info_.attempts = 0;
      reconnect_info_.current_delay = config_.initialReconnectDelay;

      // Call the base implementation to handle permanent disconnection
      Session::handleTransportDisconnect(
          std::make_error_code(std::errc::connection_aborted));
      return;
    }
  }

  utils::logger::info("Attempting to reconnect (attempt {})...",
                      reconnect_info_.attempts + 1);

  // Launch reconnection in a separate thread
  std::thread([this]() {
    // First, sleep for the current delay
    {
      std::chrono::milliseconds delay;
      {
        std::lock_guard<std::mutex> lock(reconnect_mutex_);
        delay = reconnect_info_.current_delay;
      }

      utils::logger::debug("Waiting {} ms before reconnection attempt",
                           delay.count());
      std::this_thread::sleep_for(delay);
    }

    // Get the transport and client info
    std::shared_ptr<transport::Transport> transport = getTransport();
    std::optional<types::ClientInfo> client_info;
    {
      std::lock_guard<std::mutex> lock(client_info_mutex_);
      client_info = client_info_;
    }

    // Check if we have client info to reconnect with
    if (!client_info.has_value()) {
      utils::logger::error(
          "Cannot reconnect: missing client info from previous initialization");

      // Reset reconnection state
      {
        std::lock_guard<std::mutex> lock(reconnect_mutex_);
        reconnect_info_.reconnecting = false;
      }

      // Call the base implementation to handle permanent disconnection
      Session::handleTransportDisconnect(
          std::make_error_code(std::errc::not_connected));
      return;
    }

    try {
      // Reconnect transport
      if (!transport->isConnected()) {
        transport->connect();
      }

      // Reinitialize session
      auto init_result = initialize(*client_info).get();

      utils::logger::info("Successfully reconnected to server: {}",
                          init_result.serverName);

      // Reset reconnection state on success
      {
        std::lock_guard<std::mutex> lock(reconnect_mutex_);
        reconnect_info_.reconnecting = false;
        reconnect_info_.attempts = 0;
        reconnect_info_.current_delay = getConfig().initialReconnectDelay;
      }

      // Resubscribe to resources
      std::unordered_map<std::string, ResourceUpdateCallback> subscriptions;
      {
        std::lock_guard<std::mutex> lock(subscription_mutex_);
        subscriptions = resource_subscriptions_;
      }

      for (const auto &[uri, callback] : subscriptions) {
        try {
          utils::logger::debug("Resubscribing to resource: {}", uri);
          subscribeToResource(uri, callback).wait();
        } catch (const std::exception &e) {
          utils::logger::warn("Failed to resubscribe to resource {}: {}", uri,
                              e.what());
        }
      }

    } catch (const std::exception &e) {
      utils::logger::error("Reconnection attempt failed: {}", e.what());

      // Update reconnection state for next attempt
      {
        std::lock_guard<std::mutex> lock(reconnect_mutex_);
        reconnect_info_.attempts++;

        // Update delay with exponential backoff
        std::lock_guard<std::mutex> config_lock(config_mutex_);
        reconnect_info_.current_delay = std::min(
            reconnect_info_.current_delay * 2, config_.maxReconnectDelay);

        reconnect_info_.reconnecting = false;
      }

      // Try again
      attemptReconnect();
    }
  }).detach();
}

void ClientSession::handleResourceUpdate(
    const types::JSONRPCNotification &notification) {
  if (!notification.params.has_value()) {
    utils::logger::warn(
        "Received resourceUpdate notification without parameters");
    return;
  }

  try {
    const auto &params = notification.params.value();

    // Extract URI and content from the notification
    if (!params.contains("uri") || !params.contains("content")) {
      utils::logger::warn(
          "Invalid resourceUpdate notification: missing uri or content");
      return;
    }

    std::string uri = params["uri"].get<std::string>();

    // Create resource result from the notification
    types::ReadResourceResult result;
    result.content = params["content"];

    if (params.contains("contentType")) {
      result.contentType = params["contentType"].get<std::string>();
    }

    // Find and call the appropriate callback
    ResourceUpdateCallback callback;
    {
      std::lock_guard<std::mutex> lock(subscription_mutex_);
      auto it = resource_subscriptions_.find(uri);
      if (it == resource_subscriptions_.end()) {
        utils::logger::warn(
            "Received resourceUpdate for uri {} with no active subscription",
            uri);
        return;
      }
      callback = it->second;
    }

    // Call the callback with the updated resource
    if (callback) {
      callback(uri, result);
    }

  } catch (const std::exception &e) {
    utils::logger::error("Error handling resourceUpdate notification: {}",
                         e.what());
  }
}

void ClientSession::handleProgressUpdate(
    const types::JSONRPCNotification &notification) {
  if (!notification.params.has_value()) {
    utils::logger::warn(
        "Received progressUpdate notification without parameters");
    return;
  }

  try {
    const auto &params = notification.params.value();

    // Extract requestId and progress info from the notification
    if (!params.contains("requestId") || !params.contains("progress")) {
      utils::logger::warn(
          "Invalid progressUpdate notification: missing requestId or progress");
      return;
    }

    std::string requestId = params["requestId"].get<std::string>();
    float progress = params["progress"].get<float>();

    // Create progress info from the notification
    ProgressInfo progressInfo;
    progressInfo.progress = progress;

    if (params.contains("message")) {
      progressInfo.message = params["message"].get<std::string>();
    }

    if (params.contains("data")) {
      progressInfo.data = params["data"];
    }

    // Find and call the appropriate callback
    ProgressCallback callback;
    {
      std::lock_guard<std::mutex> lock(progress_mutex_);
      auto it = progress_callbacks_.find(requestId);
      if (it == progress_callbacks_.end()) {
        utils::logger::debug("Received progressUpdate for requestId {} with no "
                             "registered callback",
                             requestId);
        return;
      }
      callback = it->second;
    }

    // Call the callback with the progress info
    if (callback) {
      callback(progressInfo);
    }

  } catch (const std::exception &e) {
    utils::logger::error("Error handling progressUpdate notification: {}",
                         e.what());
  }
}

} // namespace mcp