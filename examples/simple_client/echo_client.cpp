#include "mcp/transport/stdio_transport.hpp"
#include "mcp/utils/error.hpp"
#include "mcp/utils/json_utils.hpp"
#include "mcp/utils/logging.hpp"
#include <atomic>
#include <cstdint>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <signal.h>
#include <string>

using namespace mcp;
using namespace mcp::transport;
using namespace mcp::types;

// Global flag for graceful shutdown
std::atomic<bool> g_running(true);

// Signal handler for graceful shutdown
void signal_handler(int signal) { g_running = false; }

// Simple request-response tracker
class RequestTracker {
public:
  // Send a request and set up a promise for the response
  std::future<JSONRPCResponse>
  sendRequest(Transport &transport, const std::string &method,
              const nlohmann::json &params = nullptr) {

    std::promise<JSONRPCResponse> promise;
    std::future<JSONRPCResponse> future = promise.get_future();

    JSONRPCRequest request;
    request.jsonrpc = "2.0";
    request.id = generateId();
    request.method = method;

    if (!params.is_null()) {
      request.params = params;
    }

    {
      std::lock_guard<std::mutex> lock(promises_mutex_);
      promises_[request.id] = std::move(promise);
    }

    transport.send(request, [this, id = request.id](const std::error_code &ec) {
      if (ec) {
        // If send fails, remove the promise and set an exception
        std::lock_guard<std::mutex> lock(promises_mutex_);
        auto it = promises_.find(id);
        if (it != promises_.end()) {
          try {
            it->second.set_exception(std::make_exception_ptr(
                TransportException("Failed to send request: " + ec.message())));
          } catch (const std::exception &) {
            // Promise might already be satisfied or broken
          }
          promises_.erase(it);
        }
      }
    });

    return future;
  }

  // Handle a response
  void handleResponse(const JSONRPCResponse &response) {
    std::lock_guard<std::mutex> lock(promises_mutex_);
    auto it = promises_.find(response.id);
    if (it != promises_.end()) {
      try {
        it->second.set_value(response);
      } catch (const std::exception &) {
        // Promise might already be satisfied or broken
      }
      promises_.erase(it);
    }
  }

  // Handle an error
  void handleError(const JSONRPCError &error) {
    std::lock_guard<std::mutex> lock(promises_mutex_);
    auto it = promises_.find(error.id);
    if (it != promises_.end()) {
      try {
        it->second.set_exception(std::make_exception_ptr(
            ProtocolException(error.error.message, error.error.data)));
      } catch (const std::exception &) {
        // Promise might already be satisfied or broken
      }
      promises_.erase(it);
    }
  }

private:
  // Generate a unique ID for requests
  std::variant<std::string, int> generateId() {
    return static_cast<int>(next_id_++);
  }

  std::mutex promises_mutex_;
  std::map<std::variant<std::string, int>, std::promise<JSONRPCResponse>>
      promises_;
  std::uint64_t next_id_ = 1;
};

int main(int argc, char **argv) {
  // Set up signal handling
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  // Configure logging
  logging::setLevel(logging::Level::Info);

  std::cout << "MCP Echo Client" << std::endl;
  std::cout << "Enter 'exit' to quit" << std::endl;

  // Create and configure transport
  StdioTransport transport;
  RequestTracker request_tracker;

  // Set up message callback
  transport.setMessageCallback([&request_tracker](JSONRPCMessage message) {
    try {
      if (std::holds_alternative<JSONRPCResponse>(message)) {
        auto response = std::get<JSONRPCResponse>(message);
        MCP_LOG_INFO("Received response");
        request_tracker.handleResponse(response);
      } else if (std::holds_alternative<JSONRPCError>(message)) {
        auto error = std::get<JSONRPCError>(message);
        MCP_LOG_ERROR("Received error: " + error.error.message);
        request_tracker.handleError(error);
      } else if (std::holds_alternative<JSONRPCNotification>(message)) {
        auto notification = std::get<JSONRPCNotification>(message);
        MCP_LOG_INFO("Received notification: " + notification.method);
      } else {
        MCP_LOG_WARNING("Received unexpected message type");
      }
    } catch (const std::exception &e) {
      MCP_LOG_ERROR("Error processing message: " + std::string(e.what()));
    }
  });

  // Set up error callback
  transport.setErrorCallback([](std::error_code ec) {
    MCP_LOG_ERROR("Transport error: " + ec.message());
  });

  // Set up close callback
  transport.setCloseCallback([]() {
    MCP_LOG_INFO("Transport closed");
    g_running = false;
  });

  // Connect the transport
  transport.connect();
  MCP_LOG_INFO("Echo client connected");

  // Send a test request
  try {
    nlohmann::json params = {{"name", "Echo Client"}, {"version", "1.0.0"}};

    auto future = request_tracker.sendRequest(transport, "echo.hello", params);

    // Wait for the response
    auto status = future.wait_for(std::chrono::seconds(5));
    if (status == std::future_status::ready) {
      try {
        JSONRPCResponse response = future.get();
        std::cout << "Received response: " << response.result.dump(2)
                  << std::endl;
      } catch (const std::exception &e) {
        std::cerr << "Error in response: " << e.what() << std::endl;
      }
    } else {
      std::cerr << "Timeout waiting for response" << std::endl;
    }

    // Send a notification
    JSONRPCNotification notification;
    notification.jsonrpc = "2.0";
    notification.method = "echo.event";
    notification.params = nlohmann::json{
        {"event", "connected"},
        {"timestamp",
         std::chrono::system_clock::now().time_since_epoch().count()}};

    transport.send(notification, [](const std::error_code &ec) {
      if (ec) {
        MCP_LOG_ERROR("Failed to send notification: " + ec.message());
      } else {
        MCP_LOG_INFO("Notification sent");
      }
    });

    // Main input loop
    std::string line;
    std::cout << "> ";
    while (g_running && std::getline(std::cin, line)) {
      if (line == "exit") {
        break;
      }

      try {
        // Send a request with the line as the message
        nlohmann::json req_params = {
            {"message", line},
            {"timestamp",
             std::chrono::system_clock::now().time_since_epoch().count()}};

        auto req_future =
            request_tracker.sendRequest(transport, "echo.message", req_params);

        // Wait for the response
        auto req_status = req_future.wait_for(std::chrono::seconds(5));
        if (req_status == std::future_status::ready) {
          try {
            JSONRPCResponse response = req_future.get();
            std::cout << "Echo response: " << response.result.dump(2)
                      << std::endl;
          } catch (const std::exception &e) {
            std::cerr << "Error in response: " << e.what() << std::endl;
          }
        } else {
          std::cerr << "Timeout waiting for response" << std::endl;
        }
      } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << std::endl;
      }

      std::cout << "> ";
    }
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << std::endl;
  }

  // Clean up
  transport.disconnect();
  MCP_LOG_INFO("Echo client shut down");

  return 0;
}