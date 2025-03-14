#ifndef MCP_SESSION_CLIENT_SESSION_HPP_
#define MCP_SESSION_CLIENT_SESSION_HPP_

#include "mcp/session/session.hpp"
#include "mcp/types.hpp"
#include <chrono>
#include <functional>
#include <optional>
#include <unordered_map>

namespace mcp {

/**
 * @brief Configuration options for ClientSession
 */
struct ClientSessionConfig {
  /**
   * @brief Whether to enable caching for tool/resource/prompt listings
   */
  bool enableCaching = true;

  /**
   * @brief Cache TTL in seconds (0 = no expiration)
   */
  std::chrono::seconds cacheTTL =
      std::chrono::seconds(300); // 5 minutes default

  /**
   * @brief Whether to enable automatic reconnection
   */
  bool enableAutoReconnect = true;

  /**
   * @brief Maximum number of reconnection attempts (0 = unlimited)
   */
  int maxReconnectAttempts = 5;

  /**
   * @brief Initial reconnect delay
   */
  std::chrono::milliseconds initialReconnectDelay =
      std::chrono::milliseconds(100);

  /**
   * @brief Maximum reconnect delay (for exponential backoff)
   */
  std::chrono::milliseconds maxReconnectDelay = std::chrono::seconds(30);

  /**
   * @brief Whether to validate server responses against schema
   */
  bool validateResponses = true;
};

/**
 * @brief Progress information for long-running operations
 */
struct ProgressInfo {
  /**
   * @brief Current progress (0.0 - 1.0)
   */
  float progress = 0.0f;

  /**
   * @brief Optional message describing current progress
   */
  std::optional<std::string> message;

  /**
   * @brief Optional custom data
   */
  std::optional<nlohmann::json> data;
};

/**
 * @brief Callback type for progress reporting
 */
using ProgressCallback = std::function<void(const ProgressInfo &)>;

/**
 * @brief Subscription update callback type
 */
using ResourceUpdateCallback = std::function<void(
    const std::string &uri, const types::ReadResourceResult &)>;

/**
 * @brief ClientSession specialization for MCP client-side communication
 *
 * The ClientSession class is responsible for:
 * - Managing client-side initialization and negotiation
 * - Providing methods for MCP client capabilities
 * - Handling client-specific message routing
 * - Caching frequently requested data
 * - Managing resource subscriptions
 * - Handling automatic reconnection
 * - Providing progress reporting for long-running operations
 */
class ClientSession : public Session {
public:
  /**
   * @brief Construct a new ClientSession with the given transport
   *
   * @param transport The transport to use for communication
   * @param config Configuration options for the client session
   */
  explicit ClientSession(
      std::shared_ptr<transport::Transport> transport,
      const ClientSessionConfig &config = ClientSessionConfig());

  /**
   * @brief Destroy the ClientSession object
   */
  ~ClientSession() override;

  /**
   * @brief Initialize the MCP connection
   *
   * @param client_info Client information for initialization
   * @param timeout Timeout for initialization
   * @return std::future<types::InitializeResult> A future containing the
   * initialization result
   */
  std::future<types::InitializeResult>
  initialize(const types::ClientInfo &client_info,
             std::chrono::milliseconds timeout = std::chrono::seconds(30));

  /**
   * @brief Get a list of available tools
   *
   * @param force_refresh Force refresh from server instead of using cache
   * @param timeout Timeout for the request
   * @return std::future<types::ListToolsResult> A future containing the list of
   * tools
   */
  std::future<types::ListToolsResult>
  listTools(bool force_refresh = false,
            std::chrono::milliseconds timeout = std::chrono::seconds(30));

  /**
   * @brief Call a tool with the given parameters
   *
   * @param name Tool name
   * @param params Tool parameters
   * @param progress_callback Optional callback for progress updates
   * @param timeout Timeout for the request
   * @return std::future<types::CallToolResult> A future containing the tool
   * result
   */
  std::future<types::CallToolResult> callTool(
      const std::string &name, const nlohmann::json &params,
      const std::optional<ProgressCallback> &progress_callback = std::nullopt,
      std::chrono::milliseconds timeout = std::chrono::seconds(30));

  /**
   * @brief Get a list of available resources
   *
   * @param force_refresh Force refresh from server instead of using cache
   * @param timeout Timeout for the request
   * @return std::future<types::ListResourcesResult> A future containing the
   * list of resources
   */
  std::future<types::ListResourcesResult>
  listResources(bool force_refresh = false,
                std::chrono::milliseconds timeout = std::chrono::seconds(30));

  /**
   * @brief Read a resource by URI
   *
   * @param uri Resource URI
   * @param timeout Timeout for the request
   * @return std::future<types::ReadResourceResult> A future containing the
   * resource content
   */
  std::future<types::ReadResourceResult>
  readResource(const std::string &uri,
               std::chrono::milliseconds timeout = std::chrono::seconds(30));

  /**
   * @brief Subscribe to resource updates
   *
   * @param uri Resource URI to subscribe to
   * @param callback Callback to invoke when the resource is updated
   * @param timeout Timeout for the subscription request
   * @return std::future<bool> A future containing the subscription result
   */
  std::future<bool> subscribeToResource(
      const std::string &uri, ResourceUpdateCallback callback,
      std::chrono::milliseconds timeout = std::chrono::seconds(30));

  /**
   * @brief Unsubscribe from resource updates
   *
   * @param uri Resource URI to unsubscribe from
   * @param timeout Timeout for the unsubscription request
   * @return std::future<bool> A future containing the unsubscription result
   */
  std::future<bool> unsubscribeFromResource(
      const std::string &uri,
      std::chrono::milliseconds timeout = std::chrono::seconds(30));

  /**
   * @brief Get a list of available prompts
   *
   * @param force_refresh Force refresh from server instead of using cache
   * @param timeout Timeout for the request
   * @return std::future<types::ListPromptsResult> A future containing the list
   * of prompts
   */
  std::future<types::ListPromptsResult>
  listPrompts(bool force_refresh = false,
              std::chrono::milliseconds timeout = std::chrono::seconds(30));

  /**
   * @brief Get a prompt by name with the given arguments
   *
   * @param name Prompt name
   * @param args Prompt arguments
   * @param timeout Timeout for the request
   * @return std::future<types::GetPromptResult> A future containing the prompt
   * content
   */
  std::future<types::GetPromptResult>
  getPrompt(const std::string &name, const nlohmann::json &args,
            std::chrono::milliseconds timeout = std::chrono::seconds(30));

  /**
   * @brief Clear the cache for all listings
   */
  void clearCache();

  /**
   * @brief Set the client session configuration
   *
   * @param config New configuration options
   */
  void setConfig(const ClientSessionConfig &config);

  /**
   * @brief Get the current client session configuration
   *
   * @return The current configuration
   */
  ClientSessionConfig getConfig() const;

protected:
  /**
   * @brief Handle server capabilities from initialization
   *
   * @param capabilities Server capabilities
   */
  void handleServerCapabilities(const types::ServerCapabilities &capabilities);

  /**
   * @brief Handle transport disconnection
   *
   * @param error Optional error that caused disconnection
   */
  void handleTransportDisconnect(
      const std::optional<std::error_code> &error) override;

  /**
   * @brief Handle resource update notifications
   *
   * @param notification Resource update notification
   */
  void handleResourceUpdate(const types::JSONRPCNotification &notification);

  /**
   * @brief Handle progress update notifications
   *
   * @param notification Progress update notification
   */
  void handleProgressUpdate(const types::JSONRPCNotification &notification);

private:
  /**
   * @brief Server capabilities received during initialization
   */
  types::ServerCapabilities server_capabilities_;

  /**
   * @brief Mutex to protect server_capabilities_
   */
  std::mutex capabilities_mutex_;

  /**
   * @brief Client session configuration
   */
  ClientSessionConfig config_;

  /**
   * @brief Mutex to protect config_
   */
  std::mutex config_mutex_;

  /**
   * @brief Struct for cache entry with expiration time
   */
  template <typename T> struct CacheEntry {
    T data;
    std::chrono::steady_clock::time_point expiration;
  };

  /**
   * @brief Cache for tool listings
   */
  std::optional<CacheEntry<types::ListToolsResult>> tools_cache_;

  /**
   * @brief Cache for resource listings
   */
  std::optional<CacheEntry<types::ListResourcesResult>> resources_cache_;

  /**
   * @brief Cache for prompt listings
   */
  std::optional<CacheEntry<types::ListPromptsResult>> prompts_cache_;

  /**
   * @brief Mutex to protect cache entries
   */
  std::mutex cache_mutex_;

  /**
   * @brief Map of resource subscriptions by URI
   */
  std::unordered_map<std::string, ResourceUpdateCallback>
      resource_subscriptions_;

  /**
   * @brief Mutex to protect resource_subscriptions_
   */
  std::mutex subscription_mutex_;

  /**
   * @brief Map of progress callbacks by request ID
   */
  std::unordered_map<std::string, ProgressCallback> progress_callbacks_;

  /**
   * @brief Mutex to protect progress_callbacks_
   */
  std::mutex progress_mutex_;

  /**
   * @brief Reconnection information
   */
  struct ReconnectInfo {
    bool reconnecting = false;
    int attempts = 0;
    std::chrono::milliseconds current_delay;
  };

  /**
   * @brief Current reconnection state
   */
  ReconnectInfo reconnect_info_;

  /**
   * @brief Mutex to protect reconnect_info_
   */
  std::mutex reconnect_mutex_;

  /**
   * @brief Client info used for initialization (for reconnection)
   */
  std::optional<types::ClientInfo> client_info_;

  /**
   * @brief Mutex to protect client_info_
   */
  std::mutex client_info_mutex_;

  /**
   * @brief Attempt to reconnect to the server
   */
  void attemptReconnect();

  /**
   * @brief Check if a cache entry is valid
   *
   * @tparam T Cache entry type
   * @param entry Cache entry
   * @return true if valid, false if expired or missing
   */
  template <typename T>
  bool isCacheValid(const std::optional<CacheEntry<T>> &entry) const;

  /**
   * @brief Get current time for cache expiration calculation
   *
   * @return Current time point
   */
  std::chrono::steady_clock::time_point getCurrentTime() const;

  /**
   * @brief Calculate expiration time based on TTL
   *
   * @return Expiration time point
   */
  std::chrono::steady_clock::time_point calculateExpirationTime() const;

  /**
   * @brief Validate JSON response against schema
   *
   * @param method Method name
   * @param json JSON response to validate
   * @throws ProtocolException if validation fails
   */
  void validateResponse(const std::string &method, const nlohmann::json &json);

  /**
   * @brief Set up notification handlers for resource updates and progress
   */
  void setupNotificationHandlers();
};

} // namespace mcp

#endif // MCP_SESSION_CLIENT_SESSION_HPP_