#ifndef MCP_SESSION_SESSION_HPP_
#define MCP_SESSION_SESSION_HPP_

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "mcp/transport/transport.hpp"
#include "mcp/types.hpp"
#include "mcp/utils/error.hpp"
#include "mcp/utils/logging.hpp"

namespace mcp {

/**
 * @brief Base Session class providing request/response correlation and message
 * routing
 *
 * The Session class is responsible for:
 * - Managing the communication channel via a Transport
 * - Correlating requests and responses
 * - Routing messages to appropriate handlers
 * - Managing timeouts for pending requests
 */
class Session {
public:
  /**
   * @brief Configuration options for Session
   */
  struct Config {
    /**
     * @brief Maximum consecutive reconnection attempts
     */
    int max_reconnect_attempts = 5;

    /**
     * @brief Delay between reconnection attempts
     */
    std::chrono::milliseconds reconnect_delay = std::chrono::seconds(1);

    /**
     * @brief Maximum backoff delay for reconnection attempts
     */
    std::chrono::milliseconds max_reconnect_backoff = std::chrono::seconds(30);

    /**
     * @brief Whether to automatically attempt reconnection on transport error
     */
    bool auto_reconnect = true;

    /**
     * @brief Maximum requests per second (rate limiting)
     * 0 means no rate limiting
     */
    int max_requests_per_second = 0;

    /**
     * @brief Default timeout for requests
     */
    std::chrono::milliseconds default_request_timeout =
        std::chrono::seconds(30);

    /**
     * @brief Enable session resumption
     */
    bool enable_resumption = true;

    /**
     * @brief Maximum age of session for resumption
     */
    std::chrono::seconds max_session_age = std::chrono::hours(24);
  };

  /**
   * @brief Session state information for resumption
   */
  struct SessionState {
    /**
     * @brief Session ID
     */
    std::string session_id;

    /**
     * @brief Session creation time
     */
    std::chrono::system_clock::time_point creation_time;

    /**
     * @brief Last activity time
     */
    std::chrono::system_clock::time_point last_activity_time;

    /**
     * @brief Session-specific data
     */
    nlohmann::json session_data;
  };

  /**
   * @brief Construct a new Session with the given transport
   *
   * @param transport The transport to use for communication
   * @param config Session configuration
   */
  Session(std::shared_ptr<transport::Transport> transport,
          const Config &config = Config());

  /**
   * @brief Destroy the Session object and close the transport
   */
  virtual ~Session();

  /**
   * @brief Send a request and wait for a response
   *
   * @param method The method name
   * @param params The request parameters (optional)
   * @param timeout Timeout for waiting for a response
   * @return std::future<types::JSONRPCResponse> A future that will contain the
   * response
   */
  std::future<types::JSONRPCResponse>
  sendRequest(const std::string &method,
              const std::optional<nlohmann::json> &params = std::nullopt,
              std::chrono::milliseconds timeout = std::chrono::seconds(30));

  /**
   * @brief Send a notification
   *
   * @param method The method name
   * @param params The notification parameters (optional)
   */
  void
  sendNotification(const std::string &method,
                   const std::optional<nlohmann::json> &params = std::nullopt);

  /**
   * @brief Set a handler for incoming requests
   *
   * @param handler The handler function
   */
  void setRequestHandler(
      std::function<void(const types::JSONRPCRequest &,
                         std::function<void(const types::JSONRPCResponse &)>)>
          handler);

  /**
   * @brief Set a handler for incoming notifications
   *
   * @param handler The handler function
   */
  void setNotificationHandler(
      std::function<void(const types::JSONRPCNotification &)> handler);

  /**
   * @brief Connect the session
   */
  virtual void connect();

  /**
   * @brief Disconnect the session
   *
   * @param graceful Whether to perform a graceful shutdown
   *                 (wait for pending requests to complete)
   */
  virtual void disconnect(bool graceful = true);

  /**
   * @brief Check if the session is connected
   *
   * @return true if connected, false otherwise
   */
  bool isConnected() const;

  /**
   * @brief Reconnect the session if it's disconnected
   *
   * @param resume Whether to attempt to resume the session
   * @return true if reconnected successfully, false otherwise
   */
  virtual bool reconnect(bool resume = true);

  /**
   * @brief Get the current session state for resumption
   *
   * @return SessionState The current session state
   */
  SessionState getSessionState() const;

  /**
   * @brief Restore a session from a previous state
   *
   * @param state The session state to restore
   * @return true if the session was restored successfully, false otherwise
   */
  virtual bool restoreSessionState(const SessionState &state);

  /**
   * @brief Set the session configuration
   *
   * @param config The new configuration
   */
  void setConfig(const Config &config);

  /**
   * @brief Get the current session configuration
   *
   * @return const Config& The current configuration
   */
  const Config &getConfig() const;

  /**
   * @brief Send a streaming request and receive streaming responses
   *
   * @param method The method name
   * @param params The request parameters (optional)
   * @param stream_callback Callback for each streamed response
   * @param completion_callback Callback when streaming is complete
   * @param progress_callback Optional callback for progress updates
   * @param timeout Timeout for the entire streaming operation
   */
  void sendStreamingRequest(
      const std::string &method, const std::optional<nlohmann::json> &params,
      std::function<void(const nlohmann::json &)> stream_callback,
      std::function<void(const std::error_code &)> completion_callback,
      std::function<void(float, const std::string &)> progress_callback =
          nullptr,
      std::chrono::milliseconds timeout = std::chrono::seconds(300));

  /**
   * @brief Cancel an ongoing streaming request
   *
   * @param request_id The ID of the request to cancel
   * @return true if the request was canceled, false otherwise
   */
  bool cancelStreamingRequest(const std::string &request_id);

protected:
  /**
   * @brief Handle an incoming message
   *
   * @param message The message to handle
   */
  virtual void handleIncomingMessage(const types::JSONRPCMessage &message);

  /**
   * @brief Handle a transport error
   *
   * @param error The error to handle
   */
  virtual void handleTransportError(const std::error_code &error);

  /**
   * @brief Handle a transport close
   */
  virtual void handleTransportClosed();

  /**
   * @brief Handle a reconnection attempt
   *
   * @param attempt_number The current attempt number (starting from 1)
   * @param error The error that caused the reconnection attempt
   * @return true if reconnection should continue, false to abort
   */
  virtual bool handleReconnectionAttempt(int attempt_number,
                                         const std::error_code &error);

  /**
   * @brief Handle a successful reconnection
   *
   * @param resumed Whether the session was resumed
   */
  virtual void handleReconnected(bool resumed);

  /**
   * @brief Handle a streaming response
   *
   * @param response The streaming response
   * @return true if handled as a streaming response, false otherwise
   */
  virtual bool handleStreamingResponse(const types::JSONRPCResponse &response);

  /**
   * @brief Create session resumption data
   *
   * @return nlohmann::json Session-specific data for resumption
   */
  virtual nlohmann::json createResumptionData() const;

  /**
   * @brief Restore session from resumption data
   *
   * @param data The session-specific resumption data
   * @return true if restored successfully, false otherwise
   */
  virtual bool restoreFromResumptionData(const nlohmann::json &data);

  /**
   * @brief Log a message with session context
   *
   * @param level The log level
   * @param message The message to log
   */
  void logWithContext(utils::LogLevel level, const std::string &message) const;

  /**
   * @brief The transport used for communication
   */
  std::shared_ptr<transport::Transport> transport_;

  /**
   * @brief Session configuration
   */
  Config config_;

  /**
   * @brief Session state
   */
  SessionState session_state_;

  /**
   * @brief Mutex to protect session state
   */
  mutable std::mutex session_state_mutex_;

  /**
   * @brief Request rate limiter
   */
  struct RateLimiter {
    std::chrono::steady_clock::time_point window_start;
    int request_count;
    std::mutex mutex;
    std::condition_variable cv;
  };

  /**
   * @brief Rate limiter instance
   */
  std::unique_ptr<RateLimiter> rate_limiter_;

private:
  /**
   * @brief Structure to track a pending request
   */
  struct PendingRequest {
    std::promise<types::JSONRPCResponse> promise;
    std::chrono::steady_clock::time_point expiry;
  };

  /**
   * @brief Generate a new request ID
   *
   * @return std::string The new ID
   */
  std::string generateRequestId();

  /**
   * @brief Clean up expired requests
   */
  void cleanupExpiredRequests();

  /**
   * @brief Start a background thread to check for expired requests
   */
  void startTimeoutChecker();

  /**
   * @brief Map of request ID to pending request
   */
  std::unordered_map<std::string, PendingRequest> pending_requests_;

  /**
   * @brief Mutex to protect the pending_requests_ map
   */
  std::mutex pending_requests_mutex_;

  /**
   * @brief Handler for incoming requests
   */
  std::function<void(const types::JSONRPCRequest &,
                     std::function<void(const types::JSONRPCResponse &)>)>
      request_handler_;

  /**
   * @brief Handler for incoming notifications
   */
  std::function<void(const types::JSONRPCNotification &)> notification_handler_;

  /**
   * @brief Mutex to protect the handlers
   */
  std::mutex handler_mutex_;

  /**
   * @brief Counter for generating request IDs
   */
  std::atomic<uint64_t> request_id_counter_;

  /**
   * @brief Flag indicating if the session is connected
   */
  std::atomic<bool> connected_;

  /**
   * @brief Thread for checking expired requests
   */
  std::thread timeout_thread_;

  /**
   * @brief Structure to track a streaming request
   */
  struct StreamingRequest {
    std::string id;
    std::function<void(const nlohmann::json &)> stream_callback;
    std::function<void(const std::error_code &)> completion_callback;
    std::function<void(float, const std::string &)> progress_callback;
    std::chrono::steady_clock::time_point expiry;
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point last_update_time;
    float current_progress = 0.0f;
    std::string current_status = "Starting";
    bool is_active = true;
  };

  /**
   * @brief Map of request ID to streaming request
   */
  std::unordered_map<std::string, StreamingRequest> streaming_requests_;

  /**
   * @brief Mutex to protect the streaming_requests_ map
   */
  std::mutex streaming_requests_mutex_;

  /**
   * @brief Reconnection state
   */
  struct ReconnectionState {
    int attempt_count = 0;
    std::chrono::steady_clock::time_point last_attempt_time;
    std::chrono::milliseconds current_delay;
    std::error_code last_error;
    bool in_progress = false;
    std::mutex mutex;
  };

  /**
   * @brief Reconnection state
   */
  std::unique_ptr<ReconnectionState> reconnection_state_;

  /**
   * @brief Apply rate limiting before sending a request
   *
   * @return std::error_code Error code if rate limit exceeded
   */
  std::error_code applyRateLimit();

  /**
   * @brief Check if a response is a streaming response
   *
   * @param response The response to check
   * @return true if it's a streaming response, false otherwise
   */
  bool isStreamingResponse(const types::JSONRPCResponse &response) const;

  /**
   * @brief Update progress for a streaming request
   *
   * @param id The request ID
   * @param progress The progress value (0.0-1.0)
   * @param status The status message
   * @return true if updated successfully, false if request not found
   */
  bool updateStreamingProgress(const std::string &id, float progress,
                               const std::string &status);

  /**
   * @brief Clean up expired streaming requests
   */
  void cleanupExpiredStreamingRequests();
};

} // namespace mcp

#endif // MCP_SESSION_SESSION_HPP_