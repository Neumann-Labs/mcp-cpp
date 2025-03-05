#include "mcp/transport/stdio_transport.hpp"
#include "mcp/utils/json_utils.hpp"
#include "mcp/utils/logging.hpp"
#include <atomic>
#include <iostream>
#include <signal.h>
#include <string>

using namespace mcp;
using namespace mcp::transport;
using namespace mcp::types;

// Global flag for graceful shutdown
std::atomic<bool> g_running(true);

// Signal handler for graceful shutdown
void signal_handler(int signal) { g_running = false; }

int main(int argc, char **argv) {
  // Set up signal handling
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  // Configure logging
  logging::setLevel(logging::Level::Info);

  std::cout << "MCP Echo Server" << std::endl;
  std::cout << "Press Ctrl+C to exit" << std::endl;

  // Create and configure transport
  StdioTransport transport;

  // Set up message callback to echo messages back
  transport.setMessageCallback([&transport](JSONRPCMessage message) {
    try {
      MCP_LOG_INFO("Received message, echoing back");

      // Handle different message types
      if (std::holds_alternative<JSONRPCRequest>(message)) {
        // For requests, convert to response and echo back
        auto request = std::get<JSONRPCRequest>(message);

        // Create response with same ID
        JSONRPCResponse response;
        response.jsonrpc = "2.0";
        response.id = request.id;

        // Echo back the params as the result
        if (request.params) {
          response.result = *request.params;
        } else {
          response.result = nlohmann::json::object();
        }

        // Include method name in the result
        response.result["method"] = request.method;
        response.result["echo"] = true;

        // Send the response
        transport.send(response, [](const std::error_code &ec) {
          if (ec) {
            MCP_LOG_ERROR("Failed to send response: " + ec.message());
          }
        });
      } else if (std::holds_alternative<JSONRPCNotification>(message)) {
        // For notifications, just acknowledge with a custom notification
        auto notification = std::get<JSONRPCNotification>(message);

        JSONRPCNotification echo_notification;
        echo_notification.jsonrpc = "2.0";
        echo_notification.method = "echo." + notification.method;

        if (notification.params) {
          echo_notification.params = *notification.params;
        } else {
          echo_notification.params = nlohmann::json::object();
        }

        (*echo_notification.params)["echo"] = true;

        // Send the notification
        transport.send(echo_notification, [](const std::error_code &ec) {
          if (ec) {
            MCP_LOG_ERROR("Failed to send notification: " + ec.message());
          }
        });
      } else {
        // For responses and errors, log but don't echo
        MCP_LOG_INFO("Received response or error, not echoing");
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
  MCP_LOG_INFO("Echo server started, waiting for messages");

  // Main loop
  while (g_running) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // Clean up
  transport.disconnect();
  MCP_LOG_INFO("Echo server shut down");

  return 0;
}