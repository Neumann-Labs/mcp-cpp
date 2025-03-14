#include "mcp/session/session.hpp"

#include <algorithm>
#include <random>
#include <sstream>
#include <thread>

namespace mcp {

Session::Session(std::shared_ptr<transport::Transport> transport,
                 const Config &config)
    : transport_(std::move(transport)), config_(config), request_id_counter_(0),
      connected_(false) {

  // Set up transport callbacks
  transport_->setMessageCallback([this](const types::JSONRPCMessage &message) {
    this->handleIncomingMessage(message);
  });

  transport_->setErrorCallback([this](const std::error_code &error) {
    this->handleTransportError(error);
  });

  transport_->setCloseCallback([this]() { this->handleTransportClosed(); });

  // Initialize session state
  {
    std::lock_guard<std::mutex> lock(session_state_mutex_);
    session_state_.session_id = generateRequestId();
    session_state_.creation_time = std::chrono::system_clock::now();
    session_state_.last_activity_time = session_state_.creation_time;
  }

  // Initialize rate limiter if needed
  if (config_.max_requests_per_second > 0) {
    rate_limiter_ = std::make_unique<RateLimiter>();
    rate_limiter_->window_start = std::chrono::steady_clock::now();
    rate_limiter_->request_count = 0;
  }

  // Initialize reconnection state
  reconnection_state_ = std::make_unique<ReconnectionState>();
  reconnection_state_->current_delay = config_.reconnect_delay;
}

Session::~Session() {
  // Ensure graceful disconnect
  disconnect(true);
}

void Session::setConfig(const Config &config) {
  config_ = config;

  // Update rate limiter if needed
  if (config_.max_requests_per_second > 0) {
    if (!rate_limiter_) {
      rate_limiter_ = std::make_unique<RateLimiter>();
      rate_limiter_->window_start = std::chrono::steady_clock::now();
      rate_limiter_->request_count = 0;
    }
  } else {
    rate_limiter_.reset();
  }
}

const Session::Config &Session::getConfig() const { return config_; }

Session::SessionState Session::getSessionState() const {
  std::lock_guard<std::mutex> lock(session_state_mutex_);

  // Update session data before returning
  SessionState state = session_state_;
  state.session_data = createResumptionData();

  return state;
}

bool Session::restoreSessionState(const SessionState &state) {
  if (!config_.enable_resumption) {
    logWithContext(utils::LogLevel::Warning, "Session resumption is disabled");
    return false;
  }

  // Check if session age is valid
  auto now = std::chrono::system_clock::now();
  auto session_age = std::chrono::duration_cast<std::chrono::seconds>(
      now - state.creation_time);

  if (session_age > config_.max_session_age) {
    logWithContext(utils::LogLevel::Warning,
                   "Session too old for resumption: " +
                       std::to_string(session_age.count()) + "s");
    return false;
  }

  // Update session state
  {
    std::lock_guard<std::mutex> lock(session_state_mutex_);
    session_state_ = state;
    session_state_.last_activity_time = now;
  }

  // Restore from session data
  return restoreFromResumptionData(state.session_data);
}

std::future<types::JSONRPCResponse>
Session::sendRequest(const std::string &method,
                     const std::optional<nlohmann::json> &params,
                     std::chrono::milliseconds timeout) {

  if (!transport_ || !isConnected()) {
    throw utils::TransportException(
        types::ErrorCode::TransportError,
        "Cannot send request: transport not connected");
  }

  // Create a new request ID
  std::string id = generateRequestId();

  // Create the request
  types::JSONRPCRequest request;
  request.jsonrpc = "2.0";
  request.id = id;
  request.method = method;
  request.params = params;

  // Create a promise for the response
  std::promise<types::JSONRPCResponse> promise;
  auto future = promise.get_future();

  // Store the promise with an expiry time
  {
    std::lock_guard<std::mutex> lock(pending_requests_mutex_);
    auto expiry = std::chrono::steady_clock::now() + timeout;
    pending_requests_[id] = {std::move(promise), expiry};
  }

  // Send the request
  std::error_code ec = transport_->send(request);
  if (ec) {
    // Remove the pending request
    std::lock_guard<std::mutex> lock(pending_requests_mutex_);
    auto it = pending_requests_.find(id);
    if (it != pending_requests_.end()) {
      it->second.promise.set_exception(
          std::make_exception_ptr(utils::TransportException(ec)));
      pending_requests_.erase(it);
    }
    throw utils::TransportException(ec);
  }

  // Clean up any expired requests
  cleanupExpiredRequests();

  return future;
}

void Session::sendNotification(const std::string &method,
                               const std::optional<nlohmann::json> &params) {

  if (!transport_ || !isConnected()) {
    throw utils::TransportException(
        types::ErrorCode::TransportError,
        "Cannot send notification: transport not connected");
  }

  // Create the notification
  types::JSONRPCNotification notification;
  notification.jsonrpc = "2.0";
  notification.method = method;
  notification.params = params;

  // Send the notification
  std::error_code ec = transport_->send(notification);
  if (ec) {
    throw utils::TransportException(ec);
  }
}

void Session::setRequestHandler(
    std::function<void(const types::JSONRPCRequest &,
                       std::function<void(const types::JSONRPCResponse &)>)>
        handler) {
  std::lock_guard<std::mutex> lock(handler_mutex_);
  request_handler_ = std::move(handler);
}

void Session::setNotificationHandler(
    std::function<void(const types::JSONRPCNotification &)> handler) {
  std::lock_guard<std::mutex> lock(handler_mutex_);
  notification_handler_ = std::move(handler);
}

void Session::connect() {
  if (transport_) {
    // Update last activity time
    {
      std::lock_guard<std::mutex> lock(session_state_mutex_);
      session_state_.last_activity_time = std::chrono::system_clock::now();
    }

    // Connect transport
    transport_->connect();
    connected_ = true;

    // Reset reconnection state
    {
      std::lock_guard<std::mutex> lock(reconnection_state_->mutex);
      reconnection_state_->attempt_count = 0;
      reconnection_state_->in_progress = false;
      reconnection_state_->current_delay = config_.reconnect_delay;
    }

    // Start the timeout checker thread
    startTimeoutChecker();

    logWithContext(utils::LogLevel::Info, "Session connected");
  }
}

void Session::disconnect(bool graceful) {
  if (!transport_ || !isConnected()) {
    return;
  }

  logWithContext(utils::LogLevel::Info,
                 graceful ? "Gracefully disconnecting session"
                          : "Forcefully disconnecting session");

  if (graceful) {
    // Wait for all pending requests to complete or timeout
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool has_pending_requests = true;

    while (has_pending_requests &&
           std::chrono::steady_clock::now() < deadline) {
      {
        std::lock_guard<std::mutex> lock(pending_requests_mutex_);
        has_pending_requests = !pending_requests_.empty();
      }

      if (has_pending_requests) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    }

    // Same for streaming requests
    bool has_streaming_requests = true;

    while (has_streaming_requests &&
           std::chrono::steady_clock::now() < deadline) {
      {
        std::lock_guard<std::mutex> lock(streaming_requests_mutex_);
        has_streaming_requests = !streaming_requests_.empty();
      }

      if (has_streaming_requests) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    }
  }

  // Disconnect transport
  transport_->disconnect();
  connected_ = false;

  // Fail all pending requests
  {
    std::lock_guard<std::mutex> lock(pending_requests_mutex_);
    for (auto &[id, pending_request] : pending_requests_) {
      pending_request.promise.set_exception(
          std::make_exception_ptr(utils::TransportException(
              types::ErrorCode::TransportError, "Session disconnected")));
    }
    pending_requests_.clear();
  }

  // Fail all streaming requests
  {
    std::lock_guard<std::mutex> lock(streaming_requests_mutex_);
    for (auto &[id, streaming_request] : streaming_requests_) {
      if (streaming_request.completion_callback) {
        streaming_request.completion_callback(
            std::make_error_code(std::errc::connection_aborted));
      }
    }
    streaming_requests_.clear();
  }
}

bool Session::isConnected() const {
  return connected_ && transport_ && transport_->isConnected();
}

bool Session::reconnect(bool resume) {
  if (isConnected()) {
    return true; // Already connected
  }

  logWithContext(utils::LogLevel::Info,
                 resume ? "Attempting to reconnect with session resumption"
                        : "Attempting to reconnect");

  // Set reconnection in progress
  {
    std::lock_guard<std::mutex> lock(reconnection_state_->mutex);
    if (reconnection_state_->in_progress) {
      logWithContext(utils::LogLevel::Warning,
                     "Reconnection already in progress");
      return false;
    }
    reconnection_state_->in_progress = true;
    reconnection_state_->attempt_count = 0;
  }

  // Attempt to reconnect
  bool reconnected = false;
  int max_attempts = config_.max_reconnect_attempts;

  for (int attempt = 1; attempt <= max_attempts; ++attempt) {
    {
      std::lock_guard<std::mutex> lock(reconnection_state_->mutex);
      reconnection_state_->attempt_count = attempt;
      reconnection_state_->last_attempt_time = std::chrono::steady_clock::now();
    }

    // Check if we should continue attempting
    if (!handleReconnectionAttempt(attempt, reconnection_state_->last_error)) {
      logWithContext(utils::LogLevel::Info,
                     "Reconnection aborted after attempt " +
                         std::to_string(attempt));
      break;
    }

    // Try to reconnect
    logWithContext(utils::LogLevel::Info, "Reconnection attempt " +
                                              std::to_string(attempt) + "/" +
                                              std::to_string(max_attempts));

    transport_->connect();

    if (transport_->isConnected()) {
      connected_ = true;
      reconnected = true;

      // Start the timeout checker thread
      startTimeoutChecker();

      // Reset reconnection state
      {
        std::lock_guard<std::mutex> lock(reconnection_state_->mutex);
        reconnection_state_->in_progress = false;
        reconnection_state_->current_delay = config_.reconnect_delay;
      }

      // Handle successful reconnection
      handleReconnected(resume);

      logWithContext(utils::LogLevel::Info, "Successfully reconnected");
      break;
    }

    // Calculate backoff delay with jitter for next attempt
    std::chrono::milliseconds delay;
    {
      std::lock_guard<std::mutex> lock(reconnection_state_->mutex);

      // Exponential backoff
      auto base_delay_ms = config_.reconnect_delay.count();
      auto max_delay_ms = config_.max_reconnect_backoff.count();

      double backoff = std::min(static_cast<double>(max_delay_ms),
                                base_delay_ms * std::pow(2.0, attempt - 1));

      // Add jitter (±20%)
      std::random_device rd;
      std::mt19937 gen(rd());
      std::uniform_real_distribution<> jitter(0.8, 1.2);

      auto delay_ms = static_cast<int>(backoff * jitter(gen));

      // Ensure delay is within bounds
      delay_ms = std::max(static_cast<int>(base_delay_ms),
                          std::min(delay_ms, static_cast<int>(max_delay_ms)));

      delay = std::chrono::milliseconds(delay_ms);
      reconnection_state_->current_delay = delay;
    }

    // Wait before next attempt
    std::this_thread::sleep_for(delay);
  }

  // Clear reconnection in progress flag if we failed
  if (!reconnected) {
    std::lock_guard<std::mutex> lock(reconnection_state_->mutex);
    reconnection_state_->in_progress = false;
    logWithContext(utils::LogLevel::Error,
                   "Failed to reconnect after " +
                       std::to_string(reconnection_state_->attempt_count) +
                       " attempts");
  }

  return reconnected;
}

void Session::handleIncomingMessage(const types::JSONRPCMessage &message) {
  // Update session activity time
  {
    std::lock_guard<std::mutex> lock(session_state_mutex_);
    session_state_.last_activity_time = std::chrono::system_clock::now();
  }

  // Handle different message types
  if (std::holds_alternative<types::JSONRPCResponse>(message)) {
    const auto &response = std::get<types::JSONRPCResponse>(message);

    // Check if this is a streaming response
    if (handleStreamingResponse(response)) {
      return; // Handled as streaming response
    }

    // Get the ID as a string
    std::string id;
    if (std::holds_alternative<std::string>(response.id)) {
      id = std::get<std::string>(response.id);
    } else if (std::holds_alternative<int>(response.id)) {
      id = std::to_string(std::get<int>(response.id));
    }

    // Find the corresponding request
    std::lock_guard<std::mutex> lock(pending_requests_mutex_);
    auto it = pending_requests_.find(id);
    if (it != pending_requests_.end()) {
      // Set the response for the promise
      it->second.promise.set_value(response);
      pending_requests_.erase(it);
    } else {
      logWithContext(utils::LogLevel::Warning,
                     "Received response for unknown request ID: " + id);
    }
  } else if (std::holds_alternative<types::JSONRPCError>(message)) {
    const auto &error = std::get<types::JSONRPCError>(message);

    // Get the ID as a string
    std::string id;
    if (std::holds_alternative<std::string>(error.id)) {
      id = std::get<std::string>(error.id);
    } else if (std::holds_alternative<int>(error.id)) {
      id = std::to_string(std::get<int>(error.id));
    }

    // First check if this is for a streaming request
    bool handled = false;
    {
      std::lock_guard<std::mutex> lock(streaming_requests_mutex_);
      auto it = streaming_requests_.find(id);
      if (it != streaming_requests_.end()) {
        // Get the completion callback before erasing
        auto completion_callback = it->second.completion_callback;
        streaming_requests_.erase(it);

        // Call the completion callback with an error
        if (completion_callback) {
          completion_callback(std::make_error_code(std::errc::protocol_error));
        }

        handled = true;
      }
    }

    if (!handled) {
      // Not a streaming request, check pending regular requests
      std::lock_guard<std::mutex> lock(pending_requests_mutex_);
      auto it = pending_requests_.find(id);
      if (it != pending_requests_.end()) {
        // Set the exception for the promise
        it->second.promise.set_exception(
            std::make_exception_ptr(utils::ProtocolException(error.error)));
        pending_requests_.erase(it);
      } else {
        logWithContext(utils::LogLevel::Warning,
                       "Received error for unknown request ID: " + id);
      }
    }
  } else if (std::holds_alternative<types::JSONRPCRequest>(message)) {
    const auto &request = std::get<types::JSONRPCRequest>(message);

    // Call the request handler
    std::function<void(const types::JSONRPCRequest &,
                       std::function<void(const types::JSONRPCResponse &)>)>
        handler;
    {
      std::lock_guard<std::mutex> lock(handler_mutex_);
      handler = request_handler_;
    }

    if (handler) {
      handler(request, [this](const types::JSONRPCResponse &response) {
        if (transport_ && isConnected()) {
          std::error_code ec = transport_->send(response);
          if (ec) {
            utils::log(utils::LogLevel::Error,
                       "Failed to send response: " + ec.message());
          }
        }
      });
    } else {
      // No handler, send method not found error
      types::JSONRPCError error;
      error.jsonrpc = "2.0";
      error.id = request.id;
      error.error.code = static_cast<int>(types::ErrorCode::MethodNotFound);
      error.error.message = "Method not found";

      if (transport_ && isConnected()) {
        std::error_code ec = transport_->send(error);
        if (ec) {
          utils::log(utils::LogLevel::Error,
                     "Failed to send error response: " + ec.message());
        }
      }
    }
  } else if (std::holds_alternative<types::JSONRPCNotification>(message)) {
    const auto &notification = std::get<types::JSONRPCNotification>(message);

    // Call the notification handler
    std::function<void(const types::JSONRPCNotification &)> handler;
    {
      std::lock_guard<std::mutex> lock(handler_mutex_);
      handler = notification_handler_;
    }

    if (handler) {
      handler(notification);
    }
  }
}

void Session::handleTransportError(const std::error_code &error) {
  utils::log(utils::LogLevel::Error, "Transport error: " + error.message());

  // If this is a fatal error, disconnect
  if (error.value() != 0) {
    disconnect();
  }
}

void Session::handleTransportClosed() {
  utils::log(utils::LogLevel::Info, "Transport closed");
  disconnect();
}

std::string Session::generateRequestId() {
  // Generate a unique ID using a combination of:
  // - Incrementing counter
  // - Random component
  // - timestamp

  static std::mt19937 rng(std::random_device{}());
  static std::uniform_int_distribution<> dist(0, 9999);

  uint64_t counter = request_id_counter_++;
  uint64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
  int random = dist(rng);

  std::stringstream ss;
  ss << std::hex << timestamp << "-" << counter << "-" << random;
  return ss.str();
}

void Session::cleanupExpiredRequests() {
  std::lock_guard<std::mutex> lock(pending_requests_mutex_);
  auto now = std::chrono::steady_clock::now();

  for (auto it = pending_requests_.begin(); it != pending_requests_.end();) {
    if (it->second.expiry < now) {
      try {
        it->second.promise.set_exception(std::make_exception_ptr(
            utils::TimeoutException("Request timed out")));
        utils::log(utils::LogLevel::Warning,
                   "Request " + it->first + " timed out");
      } catch (const std::exception &e) {
        utils::log(utils::LogLevel::Error,
                   "Error handling timeout: " + std::string(e.what()));
      }
      it = pending_requests_.erase(it);
    } else {
      ++it;
    }
  }
}

// Run this method periodically to clean up expired requests
void Session::startTimeoutChecker() {
  if (timeout_thread_.joinable()) {
    return; // Already running
  }

  // Start a thread that periodically checks for expired requests
  timeout_thread_ = std::thread([this]() {
    while (isConnected()) {
      cleanupExpiredRequests();
      cleanupExpiredStreamingRequests();
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });

  // Detach the thread so it can run independently
  timeout_thread_.detach();
}

std::error_code Session::applyRateLimit() {
  if (!rate_limiter_ || config_.max_requests_per_second <= 0) {
    return {}; // Rate limiting not enabled
  }

  std::unique_lock<std::mutex> lock(rate_limiter_->mutex);

  // Check the current window
  auto now = std::chrono::steady_clock::now();
  auto window_duration = std::chrono::seconds(1);

  if (now - rate_limiter_->window_start > window_duration) {
    // Start a new window
    rate_limiter_->window_start = now;
    rate_limiter_->request_count = 0;
  }

  // Check if we've exceeded the rate limit
  if (rate_limiter_->request_count >= config_.max_requests_per_second) {
    // Calculate time to wait until next window
    auto time_until_next_window =
        window_duration - (now - rate_limiter_->window_start);

    // Wait for the next window
    bool timed_out = !rate_limiter_->cv.wait_for(
        lock, time_until_next_window, [this, now]() {
          return (std::chrono::steady_clock::now() -
                      rate_limiter_->window_start >
                  std::chrono::seconds(1));
        });

    if (timed_out) {
      return std::make_error_code(std::errc::resource_unavailable_try_again);
    }

    // Reset the window
    rate_limiter_->window_start = std::chrono::steady_clock::now();
    rate_limiter_->request_count = 0;
  }

  // Increment the request count
  rate_limiter_->request_count++;

  return {};
}

void Session::sendStreamingRequest(
    const std::string &method, const std::optional<nlohmann::json> &params,
    std::function<void(const nlohmann::json &)> stream_callback,
    std::function<void(const std::error_code &)> completion_callback,
    std::function<void(float, const std::string &)> progress_callback,
    std::chrono::milliseconds timeout) {

  if (!transport_ || !isConnected()) {
    if (completion_callback) {
      completion_callback(std::make_error_code(std::errc::not_connected));
    }
    return;
  }

  // Apply rate limiting
  std::error_code rate_limit_ec = applyRateLimit();
  if (rate_limit_ec) {
    if (completion_callback) {
      completion_callback(rate_limit_ec);
    }
    return;
  }

  // Create a new request ID
  std::string id = generateRequestId();

  // Create the request
  types::JSONRPCRequest request;
  request.jsonrpc = "2.0";
  request.id = id;
  request.method = method;
  request.params = params;

  // Create meta parameters for streaming
  if (request.params) {
    // If params already has meta, extend it, otherwise create it
    nlohmann::json &request_params = request.params.value();

    if (!request_params.contains("meta")) {
      request_params["meta"] = nlohmann::json::object();
    }

    // Set streaming flag
    request_params["meta"]["stream"] = true;
  } else {
    // Create params with meta
    nlohmann::json request_params = nlohmann::json::object();
    request_params["meta"] = {{"stream", true}};
    request.params = request_params;
  }

  // Register the streaming request
  {
    std::lock_guard<std::mutex> lock(streaming_requests_mutex_);

    StreamingRequest req;
    req.id = id;
    req.stream_callback = stream_callback;
    req.completion_callback = completion_callback;
    req.progress_callback = progress_callback;
    req.expiry = std::chrono::steady_clock::now() + timeout;
    req.start_time = std::chrono::steady_clock::now();
    req.last_update_time = req.start_time;
    req.is_active = true;

    streaming_requests_[id] = req;

    // Call progress callback with initial state if provided
    if (progress_callback) {
      progress_callback(0.0f, "Starting");
    }
  }

  // Send the request
  std::error_code ec = transport_->send(request);
  if (ec) {
    // Remove the streaming request
    std::lock_guard<std::mutex> lock(streaming_requests_mutex_);
    auto it = streaming_requests_.find(id);
    if (it != streaming_requests_.end()) {
      if (it->second.completion_callback) {
        it->second.completion_callback(ec);
      }
      streaming_requests_.erase(it);
    }
  }
}

bool Session::cancelStreamingRequest(const std::string &request_id) {
  if (!isConnected()) {
    return false;
  }

  // Check if the request exists
  bool request_exists = false;
  {
    std::lock_guard<std::mutex> lock(streaming_requests_mutex_);
    request_exists =
        streaming_requests_.find(request_id) != streaming_requests_.end();
  }

  if (!request_exists) {
    return false;
  }

  // Create a cancel request
  types::JSONRPCRequest cancel_request;
  cancel_request.jsonrpc = "2.0";
  cancel_request.id = generateRequestId();
  cancel_request.method = "cancel";
  cancel_request.params = nlohmann::json::object();
  cancel_request.params.value()["id"] = request_id;

  // Send the cancel request
  std::error_code ec = transport_->send(cancel_request);
  if (ec) {
    logWithContext(utils::LogLevel::Error,
                   "Failed to send cancel request: " + ec.message());
    return false;
  }

  return true;
}

bool Session::isStreamingResponse(
    const types::JSONRPCResponse &response) const {
  // Check if this is a streaming response by looking for meta.progressToken or
  // meta.complete
  if (response.result.is_object() && response.result.contains("meta") &&
      response.result["meta"].is_object()) {

    const auto &meta = response.result["meta"];

    // Check for streaming token
    if (meta.contains("progressToken") && meta["progressToken"].is_string()) {
      return true;
    }

    // Check for completion flag
    if (meta.contains("complete") && meta["complete"].is_boolean()) {
      return true;
    }
  }

  return false;
}

bool Session::handleStreamingResponse(const types::JSONRPCResponse &response) {
  // Check if this is a streaming response
  if (!isStreamingResponse(response)) {
    return false;
  }

  // Extract the request ID
  std::string id;
  if (std::holds_alternative<std::string>(response.id)) {
    id = std::get<std::string>(response.id);
  } else if (std::holds_alternative<int>(response.id)) {
    id = std::to_string(std::get<int>(response.id));
  } else {
    return false; // No valid ID
  }

  // Find the streaming request
  std::lock_guard<std::mutex> lock(streaming_requests_mutex_);
  auto it = streaming_requests_.find(id);
  if (it == streaming_requests_.end()) {
    return false; // No matching streaming request
  }

  // Get the streaming request
  StreamingRequest &req = it->second;

  // Extract progress information if available
  if (response.result.is_object() && response.result.contains("meta") &&
      response.result["meta"].is_object()) {

    const auto &meta = response.result["meta"];

    // Check for completion
    bool is_complete = false;
    if (meta.contains("complete") && meta["complete"].is_boolean() &&
        meta["complete"].get<bool>()) {
      is_complete = true;
    }

    // Extract progress information
    if (meta.contains("progress") && meta["progress"].is_number()) {
      float progress = meta["progress"].get<float>();

      // Ensure progress is between 0 and 1
      progress = std::clamp(progress, 0.0f, 1.0f);

      // Extract status message if available
      std::string status;
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

      if (is_complete) {
        status = "Complete";
        progress = 1.0f;
      }

      // Update the request's progress
      req.current_progress = progress;
      req.current_status = status;
      req.last_update_time = std::chrono::steady_clock::now();

      // Call the progress callback if provided
      if (req.progress_callback) {
        req.progress_callback(progress, status);
      }
    }

    // Call the stream callback
    if (req.stream_callback) {
      req.stream_callback(response.result);
    }

    // If this is the final message, call the completion callback and remove the
    // request
    if (is_complete) {
      // Store the callback before erasing
      auto completion_callback = req.completion_callback;
      streaming_requests_.erase(it);

      // Call the completion callback
      if (completion_callback) {
        completion_callback({});
      }
    }
  }

  return true;
}

bool Session::updateStreamingProgress(const std::string &id, float progress,
                                      const std::string &status) {
  std::lock_guard<std::mutex> lock(streaming_requests_mutex_);

  auto it = streaming_requests_.find(id);
  if (it == streaming_requests_.end()) {
    return false; // Request not found
  }

  // Update the request's progress
  it->second.current_progress = std::clamp(progress, 0.0f, 1.0f);
  it->second.current_status = status;
  it->second.last_update_time = std::chrono::steady_clock::now();

  // Call the progress callback if provided
  if (it->second.progress_callback) {
    it->second.progress_callback(it->second.current_progress,
                                 it->second.current_status);
  }

  return true;
}

void Session::cleanupExpiredStreamingRequests() {
  std::lock_guard<std::mutex> lock(streaming_requests_mutex_);
  auto now = std::chrono::steady_clock::now();

  for (auto it = streaming_requests_.begin();
       it != streaming_requests_.end();) {
    if (it->second.expiry < now) {
      // Request has expired
      auto completion_callback = it->second.completion_callback;

      // Remove the request
      it = streaming_requests_.erase(it);

      // Call the completion callback with a timeout error
      if (completion_callback) {
        completion_callback(std::make_error_code(std::errc::timed_out));
      }
    } else {
      ++it;
    }
  }
}

bool Session::handleReconnectionAttempt(int attempt_number,
                                        const std::error_code &error) {
  // Default implementation always continues reconnection attempts
  return true;
}

void Session::handleReconnected(bool resumed) {
  // Default implementation does nothing
}

nlohmann::json Session::createResumptionData() const {
  // Default implementation returns an empty object
  return nlohmann::json::object();
}

bool Session::restoreFromResumptionData(const nlohmann::json &data) {
  // Default implementation always succeeds
  return true;
}

void Session::logWithContext(utils::LogLevel level,
                             const std::string &message) const {
  std::string session_id;
  {
    std::lock_guard<std::mutex> lock(session_state_mutex_);
    session_id = session_state_.session_id;
  }

  utils::log(level, "[Session " + session_id + "] " + message);
}

} // namespace mcp