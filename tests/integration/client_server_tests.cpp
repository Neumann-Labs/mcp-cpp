#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "mcp/client.hpp"
#include "mcp/server.hpp"
#include "mcp/transport/stdio_transport.hpp"
#include "mcp/types.hpp"
#include "mcp/utils/error.hpp"

#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

// End-to-end integration test for client/server communication
class IntegrationTest : public ::testing::Test {
protected:
  // Custom transport for in-memory communication
  class InMemoryTransport
      : public mcp::transport::Transport,
        public std::enable_shared_from_this<InMemoryTransport> {
  public:
    InMemoryTransport(std::shared_ptr<InMemoryTransport> peer = nullptr)
        : peer_(peer), connected_(false) {
      if (peer_) {
        peer_->peer_ = shared_from_this();
      }
    }

    // Transport interface
    void send(const mcp::types::JSONRPCMessage &message,
              std::function<void(const std::error_code &)> callback) override {
      if (!isConnected()) {
        if (callback) {
          callback(std::make_error_code(std::errc::not_connected));
        }
        return;
      }

      // Forward the message to peer
      std::lock_guard<std::mutex> lock(mutex_);
      if (peer_ && peer_->message_callback_) {
        peer_->message_callback_(message);
      }

      if (callback) {
        callback({});
      }
    }

    std::error_code send(const mcp::types::JSONRPCMessage &message,
                         std::chrono::milliseconds timeout =
                             std::chrono::milliseconds(5000)) override {
      if (!isConnected()) {
        return std::make_error_code(std::errc::not_connected);
      }

      // Forward the message to peer
      std::lock_guard<std::mutex> lock(mutex_);
      if (peer_ && peer_->message_callback_) {
        peer_->message_callback_(message);
      }

      return {};
    }

    void setMessageCallback(
        std::function<void(mcp::types::JSONRPCMessage)> callback) override {
      std::lock_guard<std::mutex> lock(mutex_);
      message_callback_ = callback;
    }

    void
    setErrorCallback(std::function<void(std::error_code)> callback) override {
      std::lock_guard<std::mutex> lock(mutex_);
      error_callback_ = callback;
    }

    void setCloseCallback(std::function<void()> callback) override {
      std::lock_guard<std::mutex> lock(mutex_);
      close_callback_ = callback;
    }

    void connect() override { connected_ = true; }

    void disconnect() override {
      connected_ = false;

      // Notify peer that we disconnected
      std::lock_guard<std::mutex> lock(mutex_);
      if (peer_ && peer_->close_callback_) {
        peer_->close_callback_();
      }

      // Notify our callbacks
      if (close_callback_) {
        close_callback_();
      }
    }

    bool isConnected() const override { return connected_; }

  private:
    std::shared_ptr<InMemoryTransport> peer_;
    std::atomic<bool> connected_;

    std::mutex mutex_;
    std::function<void(mcp::types::JSONRPCMessage)> message_callback_;
    std::function<void(std::error_code)> error_callback_;
    std::function<void()> close_callback_;
  };

  void SetUp() override {
    // Create client and server transports
    auto client_transport = std::make_shared<InMemoryTransport>();
    auto server_transport =
        std::make_shared<InMemoryTransport>(client_transport);

    // Create client and server
    client_ = std::make_shared<mcp::Client>("test-client", "1.0.0");
    server_ = std::make_shared<mcp::Server>("test-server", "1.0.0",
                                            "Test server instructions");

    // Set up client capabilities
    mcp::types::ClientCapabilities client_capabilities;
    mcp::types::ToolCapabilities tool_capabilities;
    tool_capabilities.supports_streaming = true;
    client_capabilities.tools = tool_capabilities;
    client_->setCapabilities(client_capabilities);

    // Set up server capabilities
    mcp::types::ServerCapabilities server_capabilities;
    mcp::types::ToolCapabilities server_tool_capabilities;
    server_tool_capabilities.supports_streaming = true;
    server_capabilities.tools = server_tool_capabilities;
    server_->setCapabilities(server_capabilities);

    // Register tools, resources, and prompts
    setupServerComponents();

    // Start the server
    server_->run(server_transport);

    // Initialize the client
    client_transport_ = client_transport;
    auto init_future = client_->initialize(client_transport);

    // Wait for initialization to complete
    init_future.wait();
  }

  void TearDown() override {
    if (client_->isConnected()) {
      client_->disconnect();
    }
    server_->stop();
  }

  void setupServerComponents() {
    // Register tools
    server_->registerTool("add", "Addition tool",
                          nlohmann::json({{"type", "object"},
                                          {"properties",
                                           {{"a", {{"type", "number"}}},
                                            {"b", {{"type", "number"}}}}},
                                          {"required", {"a", "b"}}}),
                          [](const nlohmann::json &params) -> nlohmann::json {
                            int a = params["a"].get<int>();
                            int b = params["b"].get<int>();
                            return {{"result", a + b}};
                          });

    server_->registerTool(
        "echo", "Echo tool",
        nlohmann::json({{"type", "object"},
                        {"properties", {{"message", {{"type", "string"}}}}},
                        {"required", {"message"}}}),
        [](const nlohmann::json &params) -> nlohmann::json {
          std::string message = params["message"].get<std::string>();
          return {{"result", message}};
        });

    // Register resources
    server_->registerResource(
        "sample-text", "Sample Text", "A sample text resource",
        [](const std::string &uri) -> mcp::types::ReadResourceResult {
          return {"This is a sample text resource.", "text/plain"};
        });

    server_->registerResource(
        "sample-json", "Sample JSON", "A sample JSON resource",
        [](const std::string &uri) -> mcp::types::ReadResourceResult {
          return {"{\"key\":\"value\",\"number\":42}", "application/json"};
        });

    // Register prompts
    std::vector<mcp::types::PromptArgument> greeting_args = {
        {"name", "User name", true}, {"language", "Greeting language", false}};

    server_->registerPrompt("greeting", "Greeting prompt", greeting_args,
                            [](const nlohmann::json &args) -> std::string {
                              std::string name =
                                  args["name"].get<std::string>();
                              std::string language =
                                  args.contains("language")
                                      ? args["language"].get<std::string>()
                                      : "en";

                              if (language == "es") {
                                return "�Hola, " + name + "!";
                              } else if (language == "fr") {
                                return "Bonjour, " + name + "!";
                              } else {
                                return "Hello, " + name + "!";
                              }
                            });
  }

  std::shared_ptr<mcp::Client> client_;
  std::shared_ptr<mcp::Server> server_;
  std::shared_ptr<mcp::transport::Transport> client_transport_;
};

// Test basic tool invocation
TEST_F(IntegrationTest, BasicToolInvocation) {
  // Call the add tool
  auto future = client_->callTool("add", {{"a", 5}, {"b", 7}});

  // Wait for the call to complete
  auto result = future.get();

  // Verify the result
  EXPECT_EQ(result.result["result"], 12);
}

// Test resource retrieval
TEST_F(IntegrationTest, ResourceRetrieval) {
  // Read resources
  auto text_future = client_->readResource("sample-text");
  auto json_future = client_->readResource("sample-json");

  // Wait for the reads to complete
  auto text_result = text_future.get();
  auto json_result = json_future.get();

  // Verify the results
  EXPECT_EQ(text_result.content, "This is a sample text resource.");
  EXPECT_EQ(text_result.content_type(), "text/plain");

  EXPECT_EQ(json_result.content, "{\"key\":\"value\",\"number\":42}");
  EXPECT_EQ(json_result.content_type(), "application/json");
}

// Test prompt execution
TEST_F(IntegrationTest, PromptExecution) {
  // Get prompts in different languages
  auto en_future = client_->getPrompt("greeting", {{"name", "John"}});
  auto es_future =
      client_->getPrompt("greeting", {{"name", "Juan"}, {"language", "es"}});
  auto fr_future =
      client_->getPrompt("greeting", {{"name", "Jean"}, {"language", "fr"}});

  // Wait for the gets to complete
  auto en_result = en_future.get();
  auto es_result = es_future.get();
  auto fr_result = fr_future.get();

  // Verify the results
  EXPECT_EQ(en_result.prompt, "Hello, John!");
  EXPECT_EQ(es_result.prompt, "�Hola, Juan!");
  EXPECT_EQ(fr_result.prompt, "Bonjour, Jean!");
}

// Test list operations
TEST_F(IntegrationTest, ListOperations) {
  // List tools
  auto tools_future = client_->listTools();
  auto tools_result = tools_future.get();

  // Verify tools list
  EXPECT_EQ(tools_result.tools.size(), 2);
  EXPECT_EQ(tools_result.tools[0].name, "add");
  EXPECT_EQ(tools_result.tools[1].name, "echo");

  // List resources
  auto resources_future = client_->listResources();
  auto resources_result = resources_future.get();

  // Verify resources list
  EXPECT_EQ(resources_result.resources.size(), 2);
  EXPECT_EQ(resources_result.resources[0].uri, "sample-text");
  EXPECT_EQ(resources_result.resources[1].uri, "sample-json");

  // List prompts
  auto prompts_future = client_->listPrompts();
  auto prompts_result = prompts_future.get();

  // Verify prompts list
  EXPECT_EQ(prompts_result.prompts.size(), 1);
  EXPECT_EQ(prompts_result.prompts[0].name, "greeting");
  EXPECT_EQ(prompts_result.prompts[0].arguments.size(), 2);
}

// Test error conditions
TEST_F(IntegrationTest, ErrorConditions) {
  // Call a non-existent tool
  auto tool_future = client_->callTool("non_existent_tool", {});

  // Wait for the call to complete
  EXPECT_THROW(tool_future.get(), mcp::utils::ProtocolException);

  // Missing required parameter
  auto prompt_future = client_->getPrompt("greeting", {});

  // Wait for the call to complete
  EXPECT_THROW(prompt_future.get(), mcp::utils::ProtocolException);

  // Invalid resource
  auto resource_future = client_->readResource("non_existent_resource");

  // Wait for the call to complete
  EXPECT_THROW(resource_future.get(), mcp::utils::ProtocolException);
}

// Test concurrent operations
TEST_F(IntegrationTest, ConcurrentOperations) {
  // Make multiple concurrent tool calls
  constexpr int NUM_CALLS = 5;
  std::vector<std::future<mcp::types::CallToolResult>> futures;

  for (int i = 0; i < NUM_CALLS; i++) {
    futures.push_back(client_->callTool("add", {{"a", i}, {"b", i * 2}}));
  }

  // Verify all calls complete successfully
  for (int i = 0; i < NUM_CALLS; i++) {
    auto result = futures[i].get();
    EXPECT_EQ(result.result["result"], i + i * 2);
  }
}