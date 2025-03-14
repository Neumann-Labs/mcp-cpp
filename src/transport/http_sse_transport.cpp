#include "mcp/transport/http_sse_transport.hpp"
#include "mcp/utils/error.hpp"
#include "mcp/utils/json_utils.hpp"
#include "mcp/utils/logging.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <curl/curl.h>
#include <future>
#include <random>
#include <regex>
#include <sstream>

namespace mcp {
namespace transport {

// Curl callback for writing received data from SSE - implemented as a static
// class member
size_t HttpSseTransport::sseWriteCallback(char *ptr, size_t size, size_t nmemb,
                                          void *userdata) {
  auto *transport = static_cast<HttpSseTransport *>(userdata);
  std::string data(ptr, size * nmemb);

  // Process the received data
  transport->processEvent(data);

  return size * nmemb;
}

// Curl callback for writing received data from HTTP requests
static size_t HttpWriteCallback(char *ptr, size_t size, size_t nmemb,
                                void *userdata) {
  auto *response = static_cast<std::string *>(userdata);
  response->append(ptr, size * nmemb);
  return size * nmemb;
}

HttpSseTransport::HttpSseTransport(const Config &config)
    : config_(config), connected_(false), connecting_(false), stopping_(false),
      sse_curl_(nullptr), multi_handle_(nullptr), http_headers_(nullptr),
      reconnect_attempts_(0) {

  // Initialize curl
  initCurl();
}

HttpSseTransport::~HttpSseTransport() {
  disconnect();
  cleanupCurl();
}

void HttpSseTransport::initCurl() {
  // Initialize global curl state (if needed)
  static std::once_flag curl_init_flag;
  std::call_once(curl_init_flag, []() { curl_global_init(CURL_GLOBAL_ALL); });

  // Create multi handle for parallel requests
  multi_handle_ = curl_multi_init();

  // Initialize HTTP headers
  http_headers_ = nullptr;
  http_headers_ =
      curl_slist_append(http_headers_, "Content-Type: application/json");
  http_headers_ = curl_slist_append(http_headers_, "Accept: application/json");

  // Add custom headers
  for (const auto &[name, value] : config_.headers) {
    std::string header = name + ": " + value;
    http_headers_ = curl_slist_append(http_headers_, header.c_str());
  }
}

void HttpSseTransport::applyAuthentication(CURL *curl) {
  if (!curl)
    return;

  switch (config_.auth.type) {
  case Config::Auth::Type::Basic:
    if (!config_.auth.username.empty() || !config_.auth.password.empty()) {
      curl_easy_setopt(curl, CURLOPT_USERNAME, config_.auth.username.c_str());
      curl_easy_setopt(curl, CURLOPT_PASSWORD, config_.auth.password.c_str());
      curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    }
    break;

  case Config::Auth::Type::Bearer:
    if (!config_.auth.token.empty()) {
      std::string auth_header = "Authorization: Bearer " + config_.auth.token;
      // Note: We use a temporary header here since it's specific to this
      // request
      struct curl_slist *headers = curl_slist_copy(http_headers_);
      headers = curl_slist_append(headers, auth_header.c_str());
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      // The headers will be freed in the request handling
    }
    break;

  case Config::Auth::Type::ApiKey:
    if (!config_.auth.api_key_name.empty() &&
        !config_.auth.api_key_value.empty()) {
      if (config_.auth.api_key_location ==
          Config::Auth::ApiKeyLocation::Header) {
        std::string api_key_header =
            config_.auth.api_key_name + ": " + config_.auth.api_key_value;
        struct curl_slist *headers = curl_slist_copy(http_headers_);
        headers = curl_slist_append(headers, api_key_header.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      } else { // Query parameter
        std::string url =
            curl_easy_escape(curl, config_.auth.api_key_name.c_str(), 0);
        std::string value =
            curl_easy_escape(curl, config_.auth.api_key_value.c_str(), 0);

        std::string current_url;
        char *url_ptr = nullptr;
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &url_ptr);
        if (url_ptr) {
          current_url = url_ptr;
        }

        // Append the query parameter
        current_url += (current_url.find('?') == std::string::npos) ? "?" : "&";
        current_url += url + "=" + value;

        curl_easy_setopt(curl, CURLOPT_URL, current_url.c_str());
      }
    }
    break;

  case Config::Auth::Type::Custom:
    // Custom authentication is handled via headers which are already applied
    break;

  case Config::Auth::Type::None:
  default:
    // No authentication
    break;
  }
}

void HttpSseTransport::applyProxySettings(CURL *curl) {
  if (!curl)
    return;

  if (!config_.proxy.url.empty()) {
    curl_easy_setopt(curl, CURLOPT_PROXY, config_.proxy.url.c_str());

    // Set proxy type
    switch (config_.proxy.type) {
    case Config::Proxy::Type::Http:
      curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);
      break;
    case Config::Proxy::Type::Https:
      curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_HTTPS);
      break;
    case Config::Proxy::Type::Socks4:
      curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS4);
      break;
    case Config::Proxy::Type::Socks5:
      curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5);
      break;
    }

    // Set proxy authentication if provided
    if (!config_.proxy.username.empty() || !config_.proxy.password.empty()) {
      curl_easy_setopt(curl, CURLOPT_PROXYUSERNAME,
                       config_.proxy.username.c_str());
      curl_easy_setopt(curl, CURLOPT_PROXYPASSWORD,
                       config_.proxy.password.c_str());
    }
  }
}

void HttpSseTransport::applyCompressionSettings(CURL *curl) {
  if (!curl)
    return;

  if (config_.enable_compression) {
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip, deflate");
  }
}

void HttpSseTransport::cleanupCurl() {
  // Clean up handles
  if (sse_curl_) {
    curl_easy_cleanup(sse_curl_);
    sse_curl_ = nullptr;
  }

  if (multi_handle_) {
    curl_multi_cleanup(multi_handle_);
    multi_handle_ = nullptr;
  }

  if (http_headers_) {
    curl_slist_free_all(http_headers_);
    http_headers_ = nullptr;
  }
}

void HttpSseTransport::send(
    const types::JSONRPCMessage &message,
    std::function<void(const std::error_code &)> callback) {
  if (!isConnected()) {
    if (callback) {
      callback(std::make_error_code(std::errc::not_connected));
    }
    return;
  }

  // Serialize the message to JSON
  std::string json_message;

  try {
    json_message = utils::serialize_json(message);
  } catch (const std::exception &e) {
    utils::log(utils::LogLevel::Error,
               std::string("Failed to serialize message: ") + e.what());

    if (callback) {
      callback(std::make_error_code(std::errc::invalid_argument));
    }
    return;
  }

  // Add the message to the send queue
  {
    std::lock_guard<std::mutex> lock(send_queue_mutex_);
    send_queue_.push_back({json_message, std::move(callback)});
  }

  // Notify the sending thread
  send_queue_cv_.notify_one();
}

std::error_code HttpSseTransport::send(const types::JSONRPCMessage &message,
                                       std::chrono::milliseconds timeout) {
  if (!isConnected()) {
    return std::make_error_code(std::errc::not_connected);
  }

  // Serialize the message to JSON
  std::string json_message;

  try {
    json_message = utils::serialize_json(message);
  } catch (const std::exception &e) {
    utils::log(utils::LogLevel::Error,
               std::string("Failed to serialize message: ") + e.what());
    return std::make_error_code(std::errc::invalid_argument);
  }

  // Create a promise for the result
  std::promise<std::error_code> promise;
  auto future = promise.get_future();

  // Send the message asynchronously
  send(message,
       [&promise](const std::error_code &ec) { promise.set_value(ec); });

  // Wait for the result with timeout
  if (future.wait_for(timeout) == std::future_status::timeout) {
    return std::make_error_code(std::errc::timed_out);
  }

  return future.get();
}

void HttpSseTransport::setMessageCallback(
    std::function<void(types::JSONRPCMessage)> callback) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  message_callback_ = std::move(callback);
}

void HttpSseTransport::setErrorCallback(
    std::function<void(std::error_code)> callback) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  error_callback_ = std::move(callback);
}

void HttpSseTransport::setCloseCallback(std::function<void()> callback) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  close_callback_ = std::move(callback);
}

void HttpSseTransport::connect() {
  if (isConnected() || connecting_) {
    return;
  }

  connecting_ = true;
  stopping_ = false;
  reconnect_attempts_ = 0;

  // Start the sending thread
  http_thread_ = std::thread(&HttpSseTransport::httpSendThread, this);

  // Start the SSE connection
  auto ec = startSseConnection();
  if (ec) {
    utils::log(utils::LogLevel::Error,
               std::string("Failed to connect to SSE: ") + ec.message());

    // Notify the error
    std::function<void(std::error_code)> error_callback;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      error_callback = error_callback_;
    }

    if (error_callback) {
      error_callback(ec);
    }

    // Try to reconnect
    reconnectSse();
  }

  connected_ = true;
  connecting_ = false;
}

void HttpSseTransport::disconnect() {
  if (!isConnected() && !connecting_) {
    return;
  }

  stopping_ = true;
  connected_ = false;

  // Stop SSE connection
  stopSseConnection();

  // Notify the sending thread
  {
    std::lock_guard<std::mutex> lock(send_queue_mutex_);
    send_queue_cv_.notify_all();
  }

  // Wait for threads to complete
  if (http_thread_.joinable()) {
    http_thread_.join();
  }

  if (sse_thread_.joinable()) {
    sse_thread_.join();
  }

  // Notify close
  std::function<void()> close_callback;
  {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    close_callback = close_callback_;
  }

  if (close_callback) {
    close_callback();
  }
}

bool HttpSseTransport::isConnected() const { return connected_; }

std::error_code HttpSseTransport::startSseConnection() {
  // Create a new curl handle for SSE
  if (sse_curl_) {
    curl_easy_cleanup(sse_curl_);
  }

  sse_curl_ = curl_easy_init();
  if (!sse_curl_) {
    return std::make_error_code(std::errc::resource_unavailable_try_again);
  }

  // Set up SSE connection
  curl_easy_setopt(sse_curl_, CURLOPT_URL, config_.sse_url.c_str());
  curl_easy_setopt(sse_curl_, CURLOPT_WRITEFUNCTION,
                   HttpSseTransport::sseWriteCallback);
  curl_easy_setopt(sse_curl_, CURLOPT_WRITEDATA, this);
  curl_easy_setopt(sse_curl_, CURLOPT_CONNECTTIMEOUT_MS,
                   config_.connect_timeout.count());
  curl_easy_setopt(sse_curl_, CURLOPT_TIMEOUT_MS, 0); // No timeout for SSE
  curl_easy_setopt(sse_curl_, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(sse_curl_, CURLOPT_MAXREDIRS, 5L);

  // Accept: text/event-stream for SSE
  struct curl_slist *sse_headers = curl_slist_copy(http_headers_);
  sse_headers = curl_slist_append(sse_headers, "Accept: text/event-stream");
  curl_easy_setopt(sse_curl_, CURLOPT_HTTPHEADER, sse_headers);

  // Apply authentication settings
  applyAuthentication(sse_curl_);

  // Apply proxy settings
  applyProxySettings(sse_curl_);

  // Apply compression settings
  applyCompressionSettings(sse_curl_);

  // Start the SSE thread
  sse_thread_ = std::thread(&HttpSseTransport::sseReceiveThread, this);

  // No error
  return {};
}

void HttpSseTransport::stopSseConnection() {
  // Signal the thread to stop
  stopping_ = true;

  // Wait for the thread to complete
  if (sse_thread_.joinable()) {
    sse_thread_.join();
  }
}

void HttpSseTransport::sseReceiveThread() {
  utils::log(utils::LogLevel::Info, "SSE thread started");

  while (!stopping_) {
    // Perform the request
    CURLcode res = curl_easy_perform(sse_curl_);

    // Check if we're stopping
    if (stopping_) {
      break;
    }

    // Handle errors
    if (res != CURLE_OK) {
      utils::log(utils::LogLevel::Error,
                 std::string("SSE connection failed: ") +
                     curl_easy_strerror(res));

      // Notify the error
      std::function<void(std::error_code)> error_callback;
      {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        error_callback = error_callback_;
      }

      if (error_callback) {
        error_callback(std::error_code(res, std::system_category()));
      }

      // Try to reconnect
      if (!reconnectSse()) {
        break;
      }
    }
  }

  utils::log(utils::LogLevel::Info, "SSE thread stopped");
}

bool HttpSseTransport::reconnectSse() {
  if (stopping_) {
    return false;
  }

  reconnect_attempts_++;

  if (reconnect_attempts_ > config_.max_reconnect_attempts) {
    utils::log(utils::LogLevel::Error, "Maximum reconnect attempts reached");

    // Notify the close
    std::function<void()> close_callback;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      close_callback = close_callback_;
    }

    if (close_callback) {
      close_callback();
    }

    connected_ = false;
    return false;
  }

  utils::log(utils::LogLevel::Info, "Reconnecting to SSE (attempt " +
                                        std::to_string(reconnect_attempts_) +
                                        ")");

  // Calculate backoff with jitter for exponential backoff
  const auto base_delay_ms = config_.reconnect_delay.count();
  const auto max_delay_ms = config_.max_reconnect_backoff.count();

  // Calculate exponential backoff value
  double backoff =
      std::min(static_cast<double>(max_delay_ms),
               base_delay_ms * std::pow(2.0, reconnect_attempts_ - 1));

  // Add jitter (±20%)
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_real_distribution<> jitter(0.8, 1.2);

  auto delay_ms = static_cast<int>(backoff * jitter(gen));

  // Ensure delay is within bounds
  delay_ms = std::max(static_cast<int>(base_delay_ms),
                      std::min(delay_ms, static_cast<int>(max_delay_ms)));

  utils::log(utils::LogLevel::Debug,
             "Reconnection backoff delay: " + std::to_string(delay_ms) + "ms");

  // Wait before reconnecting
  std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));

  // Try to reconnect
  auto ec = startSseConnection();
  if (ec) {
    utils::log(utils::LogLevel::Error,
               std::string("Failed to reconnect to SSE: ") + ec.message());

    // Notify the error
    std::function<void(std::error_code)> error_callback;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      error_callback = error_callback_;
    }

    if (error_callback) {
      error_callback(ec);
    }

    return false;
  }

  return true;
}

void HttpSseTransport::sendStream(
    const types::JSONRPCMessage &message,
    std::function<void(const types::JSONRPCMessage &)> stream_callback,
    std::function<void(const std::error_code &)> completion_callback,
    std::function<void(float, const std::string &)> progress_callback) {

  if (!isConnected()) {
    if (completion_callback) {
      completion_callback(std::make_error_code(std::errc::not_connected));
    }
    return;
  }

  // Get the request ID
  std::optional<std::variant<std::string, int>> id;

  if (std::holds_alternative<types::JSONRPCRequest>(message)) {
    const auto &request = std::get<types::JSONRPCRequest>(message);
    id = request.id;
  } else {
    // Not a request - can't stream
    utils::log(utils::LogLevel::Error, "Cannot stream non-request message");
    if (completion_callback) {
      completion_callback(std::make_error_code(std::errc::invalid_argument));
    }
    return;
  }

  if (!id) {
    // No ID - can't stream
    utils::log(utils::LogLevel::Error, "Cannot stream request without ID");
    if (completion_callback) {
      completion_callback(std::make_error_code(std::errc::invalid_argument));
    }
    return;
  }

  // Register the streaming request
  {
    std::lock_guard<std::mutex> lock(streaming_requests_mutex_);

    // Create the streaming request
    StreamingRequest req;
    req.id = *id;
    req.stream_callback = stream_callback;
    req.completion_callback = completion_callback;
    req.progress_callback = progress_callback;
    req.is_active = true;
    req.start_time = std::chrono::steady_clock::now();
    req.last_update_time = req.start_time;
    req.current_progress = 0.0f;
    req.current_status = "Starting";

    // Call the progress callback with initial status if provided
    if (progress_callback) {
      progress_callback(0.0f, "Starting");
    }

    // Store it in the appropriate map
    if (std::holds_alternative<std::string>(*id)) {
      streaming_requests_by_string_id_[std::get<std::string>(*id)] = req;
    } else {
      streaming_requests_by_int_id_[std::get<int>(*id)] = req;
    }
  }

  // Send the initial request
  send(message, [this, id](const std::error_code &ec) {
    if (ec) {
      // Failed to send request
      std::function<void(const std::error_code &)> completion_callback;

      {
        std::lock_guard<std::mutex> lock(streaming_requests_mutex_);

        // Get the completion callback and remove the request
        if (std::holds_alternative<std::string>(*id)) {
          auto it =
              streaming_requests_by_string_id_.find(std::get<std::string>(*id));
          if (it != streaming_requests_by_string_id_.end()) {
            completion_callback = it->second.completion_callback;
            streaming_requests_by_string_id_.erase(it);
          }
        } else {
          auto it = streaming_requests_by_int_id_.find(std::get<int>(*id));
          if (it != streaming_requests_by_int_id_.end()) {
            completion_callback = it->second.completion_callback;
            streaming_requests_by_int_id_.erase(it);
          }
        }
      }

      // Call the completion callback
      if (completion_callback) {
        completion_callback(ec);
      }
    } else {
      // Update progress to indicate request was sent
      updateStreamProgress(*id, 0.1f, "Request sent");
    }
  });
}

bool HttpSseTransport::updateStreamProgress(
    const std::variant<std::string, int> &id, float progress,
    const std::string &status) {

  std::lock_guard<std::mutex> lock(streaming_requests_mutex_);

  // Find the streaming request
  StreamingRequest *request = nullptr;

  if (std::holds_alternative<std::string>(id)) {
    auto it = streaming_requests_by_string_id_.find(std::get<std::string>(id));
    if (it != streaming_requests_by_string_id_.end()) {
      request = &it->second;
    }
  } else {
    auto it = streaming_requests_by_int_id_.find(std::get<int>(id));
    if (it != streaming_requests_by_int_id_.end()) {
      request = &it->second;
    }
  }

  if (!request) {
    return false;
  }

  // Update progress
  request->current_progress = std::clamp(progress, 0.0f, 1.0f);
  request->current_status = status;
  request->last_update_time = std::chrono::steady_clock::now();

  // Call the progress callback if provided
  if (request->progress_callback) {
    request->progress_callback(request->current_progress,
                               request->current_status);
  }

  return true;
}

std::error_code
HttpSseTransport::cancelStream(const std::variant<std::string, int> &id) {
  if (!isConnected()) {
    return std::make_error_code(std::errc::not_connected);
  }

  // Check if the stream exists
  bool stream_exists = false;

  {
    std::lock_guard<std::mutex> lock(streaming_requests_mutex_);

    if (std::holds_alternative<std::string>(id)) {
      stream_exists =
          streaming_requests_by_string_id_.count(std::get<std::string>(id)) > 0;
    } else {
      stream_exists =
          streaming_requests_by_int_id_.count(std::get<int>(id)) > 0;
    }
  }

  if (!stream_exists) {
    return std::make_error_code(std::errc::invalid_argument);
  }

  // Send a cancellation request
  return sendCancelRequest(id);
}

std::error_code
HttpSseTransport::sendCancelRequest(const std::variant<std::string, int> &id) {
  // Create a cancellation request
  types::JSONRPCRequest cancel_request;
  cancel_request.jsonrpc = "2.0";
  cancel_request.method = "cancel";
  cancel_request.id = id;

  // Parameter object with request ID to cancel
  nlohmann::json params = nlohmann::json::object();

  if (std::holds_alternative<std::string>(id)) {
    params["id"] = std::get<std::string>(id);
  } else {
    params["id"] = std::get<int>(id);
  }

  cancel_request.params = params;

  // Create the message
  types::JSONRPCMessage message = cancel_request;

  // Send it synchronously
  return send(message, std::chrono::seconds(5));
}

std::pair<bool, std::optional<std::variant<std::string, int>>>
HttpSseTransport::isStreamingResponse(const types::JSONRPCMessage &message) {
  if (!std::holds_alternative<types::JSONRPCResponse>(message)) {
    return {false, std::nullopt};
  }

  const auto &response = std::get<types::JSONRPCResponse>(message);

  // Check for streaming response by looking for meta.progressToken
  if (response.result.is_object() && response.result.contains("meta") &&
      response.result["meta"].is_object() &&
      response.result["meta"].contains("progressToken") &&
      response.result["meta"]["progressToken"].is_string()) {

    return {true, response.id};
  }

  // Check for streaming complete by looking for meta.complete=true
  if (response.result.is_object() && response.result.contains("meta") &&
      response.result["meta"].is_object() &&
      response.result["meta"].contains("complete") &&
      response.result["meta"]["complete"].is_boolean() &&
      response.result["meta"]["complete"].get<bool>()) {

    return {true, response.id};
  }

  return {false, std::nullopt};
}

bool HttpSseTransport::handleStreamingResponse(
    const types::JSONRPCMessage &message) {
  auto [is_streaming, id_opt] = isStreamingResponse(message);

  if (!is_streaming || !id_opt) {
    return false;
  }

  const auto &id = *id_opt;

  // Find the streaming request
  std::function<void(const types::JSONRPCMessage &)> stream_callback;
  std::function<void(const std::error_code &)> completion_callback;
  bool is_complete = false;
  float progress = 0.0f;
  std::string status;

  {
    std::lock_guard<std::mutex> lock(streaming_requests_mutex_);

    if (std::holds_alternative<std::string>(id)) {
      auto it =
          streaming_requests_by_string_id_.find(std::get<std::string>(id));
      if (it != streaming_requests_by_string_id_.end()) {
        stream_callback = it->second.stream_callback;

        // Check for response details
        if (std::holds_alternative<types::JSONRPCResponse>(message)) {
          const auto &response = std::get<types::JSONRPCResponse>(message);

          // Extract progress information if available
          if (response.result.is_object() && response.result.contains("meta") &&
              response.result["meta"].is_object()) {

            auto &meta = response.result["meta"];

            // Check for completion
            if (meta.contains("complete") && meta["complete"].is_boolean() &&
                meta["complete"].get<bool>()) {

              is_complete = true;
              progress = 1.0f;
              status = "Complete";

              completion_callback = it->second.completion_callback;
              streaming_requests_by_string_id_.erase(it);
            }
            // Check for progress indicator
            else if (meta.contains("progress") &&
                     meta["progress"].is_number()) {
              progress = meta["progress"].get<float>();

              // Ensure progress is between 0 and 1
              progress = std::clamp(progress, 0.0f, 1.0f);

              // Extract status message if available
              if (meta.contains("status") && meta["status"].is_string()) {
                status = meta["status"].get<std::string>();
              } else {
                // Default status message based on progress
                if (progress < 0.1f) {
                  status = "Starting";
                } else if (progress < 0.5f) {
                  status = "In progress";
                } else if (progress < 0.9f) {
                  status = "Almost done";
                } else {
                  status = "Finishing";
                }
              }

              // Update the request's progress
              it->second.current_progress = progress;
              it->second.current_status = status;
              it->second.last_update_time = std::chrono::steady_clock::now();

              // Call the progress callback if provided
              if (it->second.progress_callback) {
                it->second.progress_callback(progress, status);
              }
            }
          }
        }
      }
    } else {
      auto it = streaming_requests_by_int_id_.find(std::get<int>(id));
      if (it != streaming_requests_by_int_id_.end()) {
        stream_callback = it->second.stream_callback;

        // Check for response details
        if (std::holds_alternative<types::JSONRPCResponse>(message)) {
          const auto &response = std::get<types::JSONRPCResponse>(message);

          // Extract progress information if available
          if (response.result.is_object() && response.result.contains("meta") &&
              response.result["meta"].is_object()) {

            auto &meta = response.result["meta"];

            // Check for completion
            if (meta.contains("complete") && meta["complete"].is_boolean() &&
                meta["complete"].get<bool>()) {

              is_complete = true;
              progress = 1.0f;
              status = "Complete";

              completion_callback = it->second.completion_callback;
              streaming_requests_by_int_id_.erase(it);
            }
            // Check for progress indicator
            else if (meta.contains("progress") &&
                     meta["progress"].is_number()) {
              progress = meta["progress"].get<float>();

              // Ensure progress is between 0 and 1
              progress = std::clamp(progress, 0.0f, 1.0f);

              // Extract status message if available
              if (meta.contains("status") && meta["status"].is_string()) {
                status = meta["status"].get<std::string>();
              } else {
                // Default status message based on progress
                if (progress < 0.1f) {
                  status = "Starting";
                } else if (progress < 0.5f) {
                  status = "In progress";
                } else if (progress < 0.9f) {
                  status = "Almost done";
                } else {
                  status = "Finishing";
                }
              }

              // Update the request's progress
              it->second.current_progress = progress;
              it->second.current_status = status;
              it->second.last_update_time = std::chrono::steady_clock::now();

              // Call the progress callback if provided
              if (it->second.progress_callback) {
                it->second.progress_callback(progress, status);
              }
            }
          }
        }
      }
    }
  }

  // Call the stream callback with the message
  if (stream_callback) {
    stream_callback(message);
  }

  // If the stream is complete, call the completion callback
  if (is_complete && completion_callback) {
    // Update progress one last time if we have progress information
    if (!status.empty()) {
      updateStreamProgress(id, progress, status);
    }

    // Call the completion callback
    completion_callback({});
  }

  return true;
}

void HttpSseTransport::processEvent(const std::string &event_data) {
  // Append to buffer
  sse_buffer_ += event_data;

  // Process complete events
  size_t pos = 0;
  while ((pos = sse_buffer_.find("\n\n", pos)) != std::string::npos) {
    // Extract the event
    std::string event = sse_buffer_.substr(0, pos + 2);
    sse_buffer_.erase(0, pos + 2);
    pos = 0;

    // Parse the event
    std::string event_type;
    std::string data;

    std::istringstream iss(event);
    std::string line;
    while (std::getline(iss, line)) {
      // Remove carriage return if present
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }

      if (line.empty()) {
        continue;
      }

      if (line.compare(0, 6, "event:") == 0) {
        event_type = line.substr(6);
        // Trim leading whitespace
        event_type.erase(0, event_type.find_first_not_of(" \t"));
      } else if (line.compare(0, 5, "data:") == 0) {
        if (!data.empty()) {
          data += "\n";
        }
        data += line.substr(5);
        // Trim leading whitespace
        if (data.size() >= 1 && data[0] == ' ') {
          data.erase(0, 1);
        }
      }
    }

    // Process the event data
    if (!data.empty()) {
      // Default to message event
      if (event_type.empty()) {
        event_type = "message";
      }

      // Only process message events
      if (event_type == "message") {
        try {
          // Parse the JSON message
          auto message = utils::parse_json_message(data);

          // Check if this is a streaming response
          bool handled_as_streaming = handleStreamingResponse(message);

          if (!handled_as_streaming) {
            // Not a streaming response, use the normal callback
            std::function<void(types::JSONRPCMessage)> message_callback;
            {
              std::lock_guard<std::mutex> lock(callback_mutex_);
              message_callback = message_callback_;
            }

            if (message_callback) {
              message_callback(message);
            }
          }
        } catch (const std::exception &e) {
          utils::log(utils::LogLevel::Error,
                     std::string("Failed to parse event data: ") + e.what());
        }
      }
    }
  }
}

void HttpSseTransport::httpSendThread() {
  utils::log(utils::LogLevel::Info, "HTTP thread started");

  while (!stopping_) {
    PendingRequest request;

    // Wait for a request
    {
      std::unique_lock<std::mutex> lock(send_queue_mutex_);

      send_queue_cv_.wait(
          lock, [this]() { return stopping_ || !send_queue_.empty(); });

      if (stopping_ && send_queue_.empty()) {
        break;
      }

      request = std::move(send_queue_.front());
      send_queue_.pop_front();
    }

    // Send the request
    auto ec = sendHttpRequest(request.message, std::move(request.callback));
    if (ec) {
      utils::log(utils::LogLevel::Error,
                 std::string("Failed to send HTTP request: ") + ec.message());
    }
  }

  utils::log(utils::LogLevel::Info, "HTTP thread stopped");
}

std::error_code HttpSseTransport::sendHttpRequest(
    const std::string &message,
    std::function<void(const std::error_code &)> callback) {

  // Create a new curl handle
  CURL *curl = curl_easy_init();
  if (!curl) {
    if (callback) {
      callback(std::make_error_code(std::errc::resource_unavailable_try_again));
    }
    return std::make_error_code(std::errc::resource_unavailable_try_again);
  }

  // Response data
  std::string response_data;

  // Set up the request
  curl_easy_setopt(curl, CURLOPT_URL, config_.http_url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, message.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, message.size());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, http_headers_);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                   config_.connect_timeout.count());
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, config_.request_timeout.count());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, HttpWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);

  // Apply authentication settings
  applyAuthentication(curl);

  // Apply proxy settings
  applyProxySettings(curl);

  // Apply compression settings
  applyCompressionSettings(curl);

  // Perform the request
  CURLcode res = curl_easy_perform(curl);

  // Check for errors
  if (res != CURLE_OK) {
    utils::log(utils::LogLevel::Error,
               std::string("HTTP request failed: ") + curl_easy_strerror(res));

    curl_easy_cleanup(curl);

    if (callback) {
      callback(std::error_code(res, std::system_category()));
    }

    return std::error_code(res, std::system_category());
  }

  // Check HTTP status code
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

  if (http_code < 200 || http_code >= 300) {
    utils::log(utils::LogLevel::Error, "HTTP request returned error code: " +
                                           std::to_string(http_code) +
                                           " Response: " + response_data);

    curl_easy_cleanup(curl);

    if (callback) {
      callback(std::make_error_code(std::errc::protocol_error));
    }

    return std::make_error_code(std::errc::protocol_error);
  }

  // Log the successful request (debug level)
  utils::log(utils::LogLevel::Debug,
             "HTTP request successful, response length: " +
                 std::to_string(response_data.size()));

  // Cleanup
  curl_easy_cleanup(curl);

  // Success
  if (callback) {
    callback({});
  }

  return {};
}

} // namespace transport
} // namespace mcp