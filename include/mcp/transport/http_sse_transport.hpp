#ifndef MCP_TRANSPORT_HTTP_SSE_TRANSPORT_HPP_
#define MCP_TRANSPORT_HTTP_SSE_TRANSPORT_HPP_

#include "mcp/transport/transport.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// Forward declarations for libcurl handles
typedef void CURL;
typedef void CURLM;

// Forward include for proper curl_slist usage
#ifdef MCP_USE_LIBCURL
#include <curl/curl.h>
#else
struct curl_slist {
  char *data;
  struct curl_slist *next;
};
extern "C" {
curl_slist *curl_slist_append(curl_slist *list, const char *data);
}
#endif

// Helper function to copy a curl_slist (not provided by libcurl)
inline struct curl_slist *curl_slist_copy(struct curl_slist *list) {
  struct curl_slist *copy = nullptr;
  struct curl_slist *current = list;

  while (current) {
    copy = curl_slist_append(copy, current->data);
    current = current->next;
  }

  return copy;
}

namespace mcp {
namespace transport {

// Forward declaration for callback
class HttpSseTransport;

/**
 * @brief HTTP/SSE Transport implementation for MCP
 *
 * This transport uses HTTP for sending requests and SSE (Server-Sent Events)
 * for receiving responses and notifications.
 */
class HttpSseTransport : public Transport {

public:
  /**
   * @brief Configuration for HttpSseTransport
   */
  struct Config {
    /**
     * @brief HTTP endpoint URL for sending requests
     */
    std::string http_url;

    /**
     * @brief SSE endpoint URL for receiving responses and notifications
     */
    std::string sse_url;

    /**
     * @brief Connection timeout in milliseconds
     */
    std::chrono::milliseconds connect_timeout = std::chrono::seconds(10);

    /**
     * @brief Request timeout in milliseconds
     */
    std::chrono::milliseconds request_timeout = std::chrono::seconds(30);

    /**
     * @brief Reconnect delay in milliseconds
     */
    std::chrono::milliseconds reconnect_delay = std::chrono::seconds(1);

    /**
     * @brief Maximum reconnect attempts before giving up
     */
    int max_reconnect_attempts = 5;

    /**
     * @brief Maximum reconnection backoff in milliseconds
     */
    std::chrono::milliseconds max_reconnect_backoff = std::chrono::seconds(30);

    /**
     * @brief Enable compression for HTTP requests and responses
     */
    bool enable_compression = false;

    /**
     * @brief Authentication configuration
     */
    struct Auth {
      /**
       * @brief Authentication type
       */
      enum class Type {
        None,   ///< No authentication
        Basic,  ///< Basic authentication
        Bearer, ///< Bearer token authentication
        ApiKey, ///< API key authentication
        Custom  ///< Custom authentication (using headers)
      };

      /**
       * @brief Authentication type
       */
      Type type = Type::None;

      /**
       * @brief Username for Basic authentication
       */
      std::string username;

      /**
       * @brief Password for Basic authentication
       */
      std::string password;

      /**
       * @brief Token for Bearer authentication
       */
      std::string token;

      /**
       * @brief API key name for ApiKey authentication
       */
      std::string api_key_name;

      /**
       * @brief API key value for ApiKey authentication
       */
      std::string api_key_value;

      /**
       * @brief API key location (header or query)
       */
      enum class ApiKeyLocation {
        Header, ///< API key in header
        Query   ///< API key in query parameter
      };

      /**
       * @brief API key location
       */
      ApiKeyLocation api_key_location = ApiKeyLocation::Header;
    };

    /**
     * @brief Authentication configuration
     */
    Auth auth;

    /**
     * @brief Proxy configuration
     */
    struct Proxy {
      /**
       * @brief Proxy URL
       */
      std::string url;

      /**
       * @brief Proxy type
       */
      enum class Type {
        Http,   ///< HTTP proxy
        Https,  ///< HTTPS proxy
        Socks4, ///< SOCKS4 proxy
        Socks5  ///< SOCKS5 proxy
      };

      /**
       * @brief Proxy type
       */
      Type type = Type::Http;

      /**
       * @brief Proxy username
       */
      std::string username;

      /**
       * @brief Proxy password
       */
      std::string password;
    };

    /**
     * @brief Proxy configuration
     */
    Proxy proxy;

    /**
     * @brief Optional HTTP headers to send with requests
     */
    std::unordered_map<std::string, std::string> headers;
  };

  /**
   * @brief Construct a new HttpSseTransport with the given configuration
   *
   * @param config Transport configuration
   */
  explicit HttpSseTransport(const Config &config);

  /**
   * @brief Destroy the HttpSseTransport and clean up resources
   */
  ~HttpSseTransport() override;

  // Transport interface

  /**
   * @brief Send a message asynchronously
   *
   * @param message The message to send
   * @param callback Callback to be called when the send completes
   */
  void send(const types::JSONRPCMessage &message,
            std::function<void(const std::error_code &)> callback) override;

  /**
   * @brief Send a message synchronously
   *
   * @param message The message to send
   * @param timeout Timeout for the send operation
   * @return std::error_code Error code if the send failed
   */
  std::error_code send(const types::JSONRPCMessage &message,
                       std::chrono::milliseconds timeout =
                           std::chrono::milliseconds(5000)) override;

  /**
   * @brief Send a streaming request asynchronously
   *
   * @param message The initial message to send
   * @param stream_callback Callback to be called for each streamed response
   * @param completion_callback Callback to be called when the stream completes
   * @param progress_callback Optional callback for progress updates (progress
   * value 0.0-1.0, status message)
   */
  void
  sendStream(const types::JSONRPCMessage &message,
             std::function<void(const types::JSONRPCMessage &)> stream_callback,
             std::function<void(const std::error_code &)> completion_callback,
             std::function<void(float, const std::string &)> progress_callback =
                 nullptr);

  /**
   * @brief Cancel an ongoing streaming request
   *
   * @param id The ID of the request to cancel
   * @return std::error_code Error code if the cancellation failed
   */
  std::error_code cancelStream(const std::variant<std::string, int> &id);

  /**
   * @brief Set the callback for incoming messages
   *
   * @param callback The callback to set
   */
  void setMessageCallback(
      std::function<void(types::JSONRPCMessage)> callback) override;

  /**
   * @brief Set the callback for transport errors
   *
   * @param callback The callback to set
   */
  void setErrorCallback(std::function<void(std::error_code)> callback) override;

  /**
   * @brief Set the callback for transport closure
   *
   * @param callback The callback to set
   */
  void setCloseCallback(std::function<void()> callback) override;

  /**
   * @brief Connect the transport
   */
  void connect() override;

  /**
   * @brief Disconnect the transport
   */
  void disconnect() override;

  /**
   * @brief Check if the transport is connected
   *
   * @return true if connected, false otherwise
   */
  bool isConnected() const override;

  /**
   * @brief Process an SSE event
   *
   * @param event_data The event data
   */
  void processEvent(const std::string &event_data);

  /**
   * @brief Update progress information for a streaming request
   *
   * @param id The ID of the streaming request
   * @param progress The progress value (0.0 to 1.0)
   * @param status The status message
   * @return true if progress was updated, false if the request was not found
   */
  bool updateStreamProgress(const std::variant<std::string, int> &id,
                            float progress, const std::string &status);

protected:
  // Callbacks - moved to protected for testing
  std::mutex callback_mutex_;
  std::function<void(types::JSONRPCMessage)> message_callback_;
  std::function<void(std::error_code)> error_callback_;
  std::function<void()> close_callback_;

private:
  /**
   * @brief Static callback function for curl to write SSE data
   *
   * @param ptr Pointer to the received data
   * @param size Size of each data element
   * @param nmemb Number of data elements
   * @param userdata User data pointer (pointer to HttpSseTransport instance)
   * @return size_t Number of bytes processed
   */
  static size_t sseWriteCallback(char *ptr, size_t size, size_t nmemb,
                                 void *userdata);

  /**
   * @brief Initialize libcurl
   */
  void initCurl();

  /**
   * @brief Cleanup libcurl
   */
  void cleanupCurl();

  /**
   * @brief Start the SSE connection
   *
   * @return std::error_code Error code if the connection failed
   */
  std::error_code startSseConnection();

  /**
   * @brief Stop the SSE connection
   */
  void stopSseConnection();

  /**
   * @brief SSE event receiving thread
   */
  void sseReceiveThread();

  /**
   * @brief HTTP request sending thread
   */
  void httpSendThread();

  /**
   * @brief Send a single message via HTTP
   *
   * @param message The message to send
   * @param callback Callback to be called when the send completes
   * @return std::error_code Error code if the send failed
   */
  std::error_code
  sendHttpRequest(const std::string &message,
                  std::function<void(const std::error_code &)> callback);

  /**
   * @brief Send a cancellation request for a streaming request
   *
   * @param id The ID of the request to cancel
   * @return std::error_code Error code if the cancellation failed
   */
  std::error_code sendCancelRequest(const std::variant<std::string, int> &id);

  /**
   * @brief Handle a completed HTTP request
   *
   * @param easy_handle The CURL easy handle
   * @param result The result code
   */
  void handleCompletedRequest(CURL *easy_handle, int result);

  /**
   * @brief Check if a message is a streaming response
   *
   * @param message The message to check
   * @return std::pair<bool, std::optional<std::variant<std::string, int>>>
   *         A pair containing:
   *         - A boolean indicating if this is a streaming response
   *         - The ID of the request if it is a streaming response
   */
  std::pair<bool, std::optional<std::variant<std::string, int>>>
  isStreamingResponse(const types::JSONRPCMessage &message);

  /**
   * @brief Handle a streaming response
   *
   * @param message The streaming response to handle
   * @return true if handled as a streaming response, false otherwise
   */
  bool handleStreamingResponse(const types::JSONRPCMessage &message);

  /**
   * @brief Apply authentication to the request
   *
   * @param curl The curl handle to apply authentication to
   */
  void applyAuthentication(CURL *curl);

  /**
   * @brief Apply proxy settings to the request
   *
   * @param curl The curl handle to apply proxy settings to
   */
  void applyProxySettings(CURL *curl);

  /**
   * @brief Apply compression settings to the request
   *
   * @param curl The curl handle to apply compression settings to
   */
  void applyCompressionSettings(CURL *curl);

  /**
   * @brief Reconnect the SSE connection with exponential backoff
   *
   * @return true if reconnected successfully, false otherwise
   */
  bool reconnectSse();

  // Configuration
  Config config_;

  // Connection state
  std::atomic<bool> connected_;
  std::atomic<bool> connecting_;
  std::atomic<bool> stopping_;

  // libcurl handles
  CURL *sse_curl_;
  CURLM *multi_handle_;
  struct curl_slist *http_headers_;

  // SSE connection
  std::thread sse_thread_;
  std::string sse_buffer_;
  std::atomic<int> reconnect_attempts_;

  // HTTP sending
  std::thread http_thread_;
  std::mutex send_queue_mutex_;
  std::condition_variable send_queue_cv_;

  struct PendingRequest {
    std::string message;
    std::function<void(const std::error_code &)> callback;
  };

  std::deque<PendingRequest> send_queue_;

  // Streaming support
  struct StreamingRequest {
    std::variant<std::string, int> id;
    std::function<void(const types::JSONRPCMessage &)> stream_callback;
    std::function<void(const std::error_code &)> completion_callback;

    // Progress tracking
    std::function<void(float, const std::string &)> progress_callback;
    float current_progress = 0.0f;
    std::string current_status;

    bool is_active;
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point last_update_time;
  };

  std::mutex streaming_requests_mutex_;
  std::unordered_map<std::string, StreamingRequest>
      streaming_requests_by_string_id_;
  std::unordered_map<int, StreamingRequest> streaming_requests_by_int_id_;
};

} // namespace transport
} // namespace mcp

#endif // MCP_TRANSPORT_HTTP_SSE_TRANSPORT_HPP_