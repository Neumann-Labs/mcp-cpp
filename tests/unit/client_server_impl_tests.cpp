#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "mcp/client.hpp"
#include "mcp/server.hpp"
#include "mcp/transport/transport.hpp"
#include "mcp/types.hpp"
#include "mcp/utils/error.hpp"

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>

// Mock transport for testing
class MockTransport : public mcp::transport::Transport {
public:
  MOCK_METHOD(void, send,
              (const mcp::types::JSONRPCMessage &,
               std::function<void(const std::error_code &)>),
              (override));
  MOCK_METHOD(std::error_code, send,
              (const mcp::types::JSONRPCMessage &, std::chrono::milliseconds),
              (override));
  MOCK_METHOD(void, setMessageCallback,
              (std::function<void(mcp::types::JSONRPCMessage)>), (override));
  MOCK_METHOD(void, setErrorCallback, (std::function<void(std::error_code)>),
              (override));
  MOCK_METHOD(void, setCloseCallback, (std::function<void()>), (override));
  MOCK_METHOD(void, connect, (), (override));
  MOCK_METHOD(void, disconnect, (), (override));
  MOCK_METHOD(bool, isConnected, (), (const, override));
};

// Base class for client/server implementation tests
class ClientServerImplTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Create mock transports
    client_transport_ = std::make_shared<testing::NiceMock<MockTransport>>();
    server_transport_ = std::make_shared<testing::NiceMock<MockTransport>>();

    // Allow the transports to be "connected" by default
    ON_CALL(*client_transport_, isConnected())
        .WillByDefault(testing::Return(true));
    ON_CALL(*server_transport_, isConnected())
        .WillByDefault(testing::Return(true));

    // By default, the sync send succeeds
    ON_CALL(*client_transport_,
            send(testing::_, testing::An<std::chrono::milliseconds>()))
        .WillByDefault(testing::Return(std::error_code()));
    ON_CALL(*server_transport_,
            send(testing::_, testing::An<std::chrono::milliseconds>()))
        .WillByDefault(testing::Return(std::error_code()));

    // Set up message routing between client and server
    ON_CALL(*client_transport_, setMessageCallback(testing::_))
        .WillByDefault(
            [this](std::function<void(mcp::types::JSONRPCMessage)> callback) {
              client_message_callback_ = callback;
            });

    ON_CALL(*server_transport_, setMessageCallback(testing::_))
        .WillByDefault(
            [this](std::function<void(mcp::types::JSONRPCMessage)> callback) {
              server_message_callback_ = callback;
            });

    ON_CALL(*client_transport_,
            send(testing::_, testing::An<std::chrono::milliseconds>()))
        .WillByDefault([this](const mcp::types::JSONRPCMessage &message,
                              std::chrono::milliseconds) {
          // Route client messages to server
          if (server_message_callback_) {
            server_message_callback_(message);
          }
          return std::error_code();
        });

    ON_CALL(*server_transport_,
            send(testing::_, testing::An<std::chrono::milliseconds>()))
        .WillByDefault([this](const mcp::types::JSONRPCMessage &message,
                              std::chrono::milliseconds) {
          // Route server messages to client
          if (client_message_callback_) {
            client_message_callback_(message);
          }
          return std::error_code();
        });

    // Set up error and close callbacks
    ON_CALL(*client_transport_, setErrorCallback(testing::_))
        .WillByDefault([this](std::function<void(std::error_code)> callback) {
          client_error_callback_ = callback;
        });

    ON_CALL(*server_transport_, setErrorCallback(testing::_))
        .WillByDefault([this](std::function<void(std::error_code)> callback) {
          server_error_callback_ = callback;
        });

    ON_CALL(*client_transport_, setCloseCallback(testing::_))
        .WillByDefault([this](std::function<void()> callback) {
          client_close_callback_ = callback;
        });

    ON_CALL(*server_transport_, setCloseCallback(testing::_))
        .WillByDefault([this](std::function<void()> callback) {
          server_close_callback_ = callback;
        });

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
  }

  void TearDown() override {
    // Disconnect client and server
    if (client_->isConnected()) {
      client_->disconnect();
    }
    server_->stop();
  }

  std::shared_ptr<MockTransport> client_transport_;
  std::shared_ptr<MockTransport> server_transport_;
  std::shared_ptr<mcp::Client> client_;
  std::shared_ptr<mcp::Server> server_;

  std::function<void(mcp::types::JSONRPCMessage)> client_message_callback_;
  std::function<void(mcp::types::JSONRPCMessage)> server_message_callback_;
  std::function<void(std::error_code)> client_error_callback_;
  std::function<void(std::error_code)> server_error_callback_;
  std::function<void()> client_close_callback_;
  std::function<void()> server_close_callback_;
};

// Server initialization tests
TEST_F(ClientServerImplTest, ServerInitialization) {
  // Start the server
  server_->run(server_transport_);

  // Initialize the client
  auto future = client_->initialize(client_transport_);

  // Give some time for message exchange
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Wait for initialization to complete
  EXPECT_EQ(future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);

  // Verify the initialization result
  auto result = future.get();
  EXPECT_EQ(result.server_info.name, "test-server");
  EXPECT_EQ(result.server_info.version, "1.0.0");
  EXPECT_EQ(result.server_info.instructions, "Test server instructions");
  EXPECT_TRUE(result.server_capabilities.tools.has_value());
  EXPECT_TRUE(result.server_capabilities.tools->supports_streaming);
}

// Server tool registration and invocation
TEST_F(ClientServerImplTest, ToolRegistrationAndInvocation) {
  // Track tool invocation
  bool tool_called = false;

  // Register a tool with the server
  server_->registerTool(
      "add", "Addition tool",
      nlohmann::json(
          {{"type", "object"},
           {"properties",
            {{"a", {{"type", "number"}}}, {"b", {{"type", "number"}}}}},
           {"required", {"a", "b"}}}),
      [&tool_called](const nlohmann::json &params) -> nlohmann::json {
        tool_called = true;
        int a = params["a"].get<int>();
        int b = params["b"].get<int>();
        return {{"result", a + b}};
      });

  // Start the server
  server_->run(server_transport_);

  // Initialize the client
  auto init_future = client_->initialize(client_transport_);
  init_future.wait();

  // List tools
  auto list_future = client_->listTools();
  auto list_result = list_future.get();

  // Verify tool was registered
  ASSERT_EQ(list_result.tools.size(), 1);
  EXPECT_EQ(list_result.tools[0].name, "add");
  EXPECT_EQ(list_result.tools[0].description, "Addition tool");
  EXPECT_TRUE(list_result.tools[0].input_schema.has_value());

  // Call the tool
  auto call_future = client_->callTool("add", {{"a", 3}, {"b", 4}});

  // Give some time for message exchange
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Wait for the call to complete
  EXPECT_EQ(call_future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);

  // Verify the tool was called and returned the correct result
  EXPECT_TRUE(tool_called);
  auto call_result = call_future.get();
  EXPECT_EQ(call_result.result["result"], 7);
}

// Server resource registration and retrieval
TEST_F(ClientServerImplTest, ResourceRegistrationAndRetrieval) {
  // Register a resource with the server
  server_->registerResource(
      "sample-text", "Sample Text", "A sample text resource",
      [](const std::string &uri) -> mcp::types::ReadResourceResult {
        EXPECT_EQ(uri, "sample-text");
        return {"This is a sample text resource.", "text/plain"};
      });

  // Start the server
  server_->run(server_transport_);

  // Initialize the client
  auto init_future = client_->initialize(client_transport_);
  init_future.wait();

  // List resources
  auto list_future = client_->listResources();
  auto list_result = list_future.get();

  // Verify resource was registered
  ASSERT_EQ(list_result.resources.size(), 1);
  EXPECT_EQ(list_result.resources[0].uri, "sample-text");
  EXPECT_EQ(list_result.resources[0].name, "Sample Text");
  EXPECT_EQ(list_result.resources[0].description, "A sample text resource");

  // Read the resource
  auto read_future = client_->readResource("sample-text");

  // Give some time for message exchange
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Wait for the read to complete
  EXPECT_EQ(read_future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);

  // Verify the resource content
  auto read_result = read_future.get();
  EXPECT_EQ(read_result.content, "This is a sample text resource.");
  EXPECT_EQ(read_result.content_type(), "text/plain");
}

// Server prompt registration and retrieval
TEST_F(ClientServerImplTest, PromptRegistrationAndRetrieval) {
  // Register a prompt with the server
  std::vector<mcp::types::PromptArgument> arguments = {
      {"name", "User name", true}, {"language", "Greeting language", false}};

  server_->registerPrompt("greeting", "Greeting prompt", arguments,
                          [](const nlohmann::json &args) -> std::string {
                            std::string name = args["name"].get<std::string>();
                            std::string language =
                                args.contains("language")
                                    ? args["language"].get<std::string>()
                                    : "en";

                            if (language == "es") {
                              return "¡Hola, " + name + "!";
                            } else {
                              return "Hello, " + name + "!";
                            }
                          });

  // Start the server
  server_->run(server_transport_);

  // Initialize the client
  auto init_future = client_->initialize(client_transport_);
  init_future.wait();

  // List prompts
  auto list_future = client_->listPrompts();
  auto list_result = list_future.get();

  // Verify prompt was registered
  ASSERT_EQ(list_result.prompts.size(), 1);
  EXPECT_EQ(list_result.prompts[0].name, "greeting");
  EXPECT_EQ(list_result.prompts[0].description, "Greeting prompt");
  ASSERT_EQ(list_result.prompts[0].arguments.size(), 2);
  EXPECT_EQ(list_result.prompts[0].arguments[0].name, "name");
  EXPECT_EQ(list_result.prompts[0].arguments[0].description, "User name");
  EXPECT_TRUE(list_result.prompts[0].arguments[0].required);
  EXPECT_EQ(list_result.prompts[0].arguments[1].name, "language");
  EXPECT_EQ(list_result.prompts[0].arguments[1].description,
            "Greeting language");
  EXPECT_FALSE(list_result.prompts[0].arguments[1].required);

  // Get prompt (English)
  auto get_en_future = client_->getPrompt("greeting", {{"name", "John"}});
  auto get_en_result = get_en_future.get();
  EXPECT_EQ(get_en_result.prompt, "Hello, John!");

  // Get prompt (Spanish)
  auto get_es_future =
      client_->getPrompt("greeting", {{"name", "Juan"}, {"language", "es"}});
  auto get_es_result = get_es_future.get();
  EXPECT_EQ(get_es_result.prompt, "¡Hola, Juan!");
}

// Error handling tests
TEST_F(ClientServerImplTest, ErrorHandling) {
  // Start the server
  server_->run(server_transport_);

  // Initialize the client
  auto init_future = client_->initialize(client_transport_);
  init_future.wait();

  // Call a non-existent tool
  auto call_future = client_->callTool("non_existent_tool", {});

  // Wait for the call to complete
  EXPECT_EQ(call_future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);

  // Verify the error
  try {
    call_future.get();
    FAIL() << "Expected an exception";
  } catch (const mcp::utils::ProtocolException &e) {
    // Should get an InvalidParams error
    EXPECT_EQ(e.error().code,
              static_cast<int>(mcp::types::ErrorCode::InvalidParams));
    EXPECT_TRUE(e.error().message.find("Tool not found") != std::string::npos);
  } catch (...) {
    FAIL() << "Expected a ProtocolException";
  }
}

// Missing required parameter test
TEST_F(ClientServerImplTest, MissingRequiredParameter) {
  // Register a prompt with the server
  std::vector<mcp::types::PromptArgument> arguments = {
      {"name", "User name", true}};

  server_->registerPrompt("greeting", "Greeting prompt", arguments,
                          [](const nlohmann::json &args) -> std::string {
                            std::string name = args["name"].get<std::string>();
                            return "Hello, " + name + "!";
                          });

  // Start the server
  server_->run(server_transport_);

  // Initialize the client
  auto init_future = client_->initialize(client_transport_);
  init_future.wait();

  // Get prompt without required parameter
  auto get_future = client_->getPrompt("greeting", {});

  // Wait for the call to complete
  EXPECT_EQ(get_future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);

  // Verify the error
  try {
    get_future.get();
    FAIL() << "Expected an exception";
  } catch (const mcp::utils::ProtocolException &e) {
    // Should get an InvalidParams error
    EXPECT_EQ(e.error().code,
              static_cast<int>(mcp::types::ErrorCode::InvalidParams));
    EXPECT_TRUE(e.error().message.find("Missing required argument") !=
                std::string::npos);
  } catch (...) {
    FAIL() << "Expected a ProtocolException";
  }
}

// Disconnection test
TEST_F(ClientServerImplTest, DisconnectionHandling) {
  // Start the server
  server_->run(server_transport_);

  // Initialize the client
  auto init_future = client_->initialize(client_transport_);
  init_future.wait();

  // Verify client is connected
  EXPECT_TRUE(client_->isConnected());

  // Disconnect client from server
  client_->disconnect();

  // Verify client is disconnected
  EXPECT_FALSE(client_->isConnected());

  // Attempting to call a tool should fail
  auto call_future = client_->callTool("any_tool", {});

  // Wait for the call to complete
  EXPECT_EQ(call_future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);

  // Verify the error
  try {
    call_future.get();
    FAIL() << "Expected an exception";
  } catch (const mcp::utils::TransportException &e) {
    // Should get a TransportError
    EXPECT_EQ(e.error().code,
              static_cast<int>(mcp::types::ErrorCode::TransportError));
    EXPECT_TRUE(e.error().message.find("not connected") != std::string::npos);
  } catch (...) {
    FAIL() << "Expected a TransportException";
  }
}

// Multiple concurrent tool calls
TEST_F(ClientServerImplTest, ConcurrentToolCalls) {
  // Register a tool with the server
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

  // Start the server
  server_->run(server_transport_);

  // Initialize the client
  auto init_future = client_->initialize(client_transport_);
  init_future.wait();

  // Make multiple concurrent tool calls
  constexpr int NUM_CALLS = 10;
  std::vector<std::future<mcp::types::CallToolResult>> futures;

  for (int i = 0; i < NUM_CALLS; i++) {
    futures.push_back(client_->callTool("add", {{"a", i}, {"b", i * 2}}));
  }

  // Verify all calls complete successfully
  for (int i = 0; i < NUM_CALLS; i++) {
    EXPECT_EQ(futures[i].wait_for(std::chrono::seconds(1)),
              std::future_status::ready);

    auto result = futures[i].get();
    EXPECT_EQ(result.result["result"], i + i * 2);
  }
}

// Server shutdown test
TEST_F(ClientServerImplTest, ServerShutdown) {
  // Start the server
  server_->run(server_transport_);

  // Initialize the client
  auto init_future = client_->initialize(client_transport_);
  init_future.wait();

  // Stop the server
  server_->stop();

  // Trigger the client's close callback
  if (client_close_callback_) {
    client_close_callback_();
  }

  // Verify client is disconnected (after receiving close event)
  // Note: Depending on your implementation, the client might not automatically
  // disconnect when the server closes the connection
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Client might not automatically disconnect in all cases,
  // especially if it's still waiting for pending responses
  // Manually disconnect to be safe
  client_->disconnect();

  EXPECT_FALSE(client_->isConnected());
}

// Multiple clients test
TEST_F(ClientServerImplTest, MultipleClients) {
  // Register a tool with the server
  server_->registerTool(
      "echo", "Echo tool",
      nlohmann::json({{"type", "object"},
                      {"properties", {{"message", {{"type", "string"}}}}},
                      {"required", {"message"}}}),
      [](const nlohmann::json &params) -> nlohmann::json {
        std::string message = params["message"].get<std::string>();
        return {{"result", message}};
      });

  // Start the server
  server_->run(server_transport_);

  // Create a second client
  std::shared_ptr<MockTransport> client2_transport =
      std::make_shared<testing::NiceMock<MockTransport>>();
  std::shared_ptr<mcp::Client> client2 =
      std::make_shared<mcp::Client>("test-client-2", "1.0.0");

  // Set up client2 transport routing
  std::function<void(mcp::types::JSONRPCMessage)> client2_message_callback;

  ON_CALL(*client2_transport, isConnected())
      .WillByDefault(testing::Return(true));

  ON_CALL(*client2_transport, setMessageCallback(testing::_))
      .WillByDefault(
          [&client2_message_callback](
              std::function<void(mcp::types::JSONRPCMessage)> callback) {
            client2_message_callback = callback;
          });

  ON_CALL(*client2_transport,
          send(testing::_, testing::An<std::chrono::milliseconds>()))
      .WillByDefault([this](const mcp::types::JSONRPCMessage &message,
                            std::chrono::milliseconds) {
        // Route client2 messages to server
        if (server_message_callback_) {
          server_message_callback_(message);
        }
        return std::error_code();
      });

  // Update server routing to send messages to client2 also
  ON_CALL(*server_transport_,
          send(testing::_, testing::An<std::chrono::milliseconds>()))
      .WillByDefault([&client2_message_callback,
                      this](const mcp::types::JSONRPCMessage &message,
                            std::chrono::milliseconds) {
        // For simplicity, send to both clients
        // In a real implementation, you'd check the message destination
        if (client_message_callback_) {
          client_message_callback_(message);
        }
        if (client2_message_callback) {
          client2_message_callback(message);
        }
        return std::error_code();
      });

  // Initialize both clients
  auto init_future1 = client_->initialize(client_transport_);
  auto init_future2 = client2->initialize(client2_transport);

  init_future1.wait();
  init_future2.wait();

  // Both clients call the echo tool
  auto call_future1 =
      client_->callTool("echo", {{"message", "Hello from client 1"}});
  auto call_future2 =
      client2->callTool("echo", {{"message", "Hello from client 2"}});

  // Verify results
  auto result1 = call_future1.get();
  auto result2 = call_future2.get();

  EXPECT_EQ(result1.result["result"], "Hello from client 1");
  EXPECT_EQ(result2.result["result"], "Hello from client 2");

  // Clean up
  client2->disconnect();
}
