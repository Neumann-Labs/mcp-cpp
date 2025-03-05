#include "mcp/types.hpp"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace mcp::types;
using json = nlohmann::json;

// Test fixture for types tests
class TypesTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Setup code if needed
  }

  void TearDown() override {
    // Teardown code if needed
  }
};

// Test ErrorCode serialization/deserialization
TEST_F(TypesTest, ErrorCodeSerialization) {
  const ErrorCode code = ErrorCode::InvalidParams;

  // Serialize to JSON
  json j = code;

  // Deserialize from JSON
  ErrorCode deserialized = j.get<ErrorCode>();

  // Check equality
  EXPECT_EQ(code, deserialized);
  EXPECT_EQ(static_cast<int>(ErrorCode::InvalidParams), j.get<int>());
}

// Test ErrorData serialization/deserialization
TEST_F(TypesTest, ErrorDataSerialization) {
  ErrorData error{.code = static_cast<int>(ErrorCode::InvalidParams),
                  .message = "Invalid parameters",
                  .data = json{{"param", "value"}}};

  // Serialize to JSON
  json j = error;

  // Check JSON structure
  EXPECT_EQ(j["code"], static_cast<int>(ErrorCode::InvalidParams));
  EXPECT_EQ(j["message"], "Invalid parameters");
  EXPECT_EQ(j["data"]["param"], "value");

  // Deserialize from JSON
  ErrorData deserialized = j.get<ErrorData>();

  // Check equality
  EXPECT_EQ(error.code, deserialized.code);
  EXPECT_EQ(error.message, deserialized.message);
  EXPECT_EQ(error.data["param"], deserialized.data["param"]);
}

// Test JSONRPCRequest serialization/deserialization
TEST_F(TypesTest, JSONRPCRequestSerialization) {
  JSONRPCRequest request{.jsonrpc = "2.0",
                         .id = 1,
                         .method = "test.method",
                         .params = json{{"param", "value"}}};

  // Serialize to JSON
  json j = request;

  // Check JSON structure
  EXPECT_EQ(j["jsonrpc"], "2.0");
  EXPECT_EQ(j["id"], 1);
  EXPECT_EQ(j["method"], "test.method");
  EXPECT_EQ(j["params"]["param"], "value");

  // Deserialize from JSON
  JSONRPCRequest deserialized = j.get<JSONRPCRequest>();

  // Check equality
  EXPECT_EQ(request.jsonrpc, deserialized.jsonrpc);
  EXPECT_EQ(std::get<int>(request.id), std::get<int>(deserialized.id));
  EXPECT_EQ(request.method, deserialized.method);
  EXPECT_EQ((*request.params)["param"], (*deserialized.params)["param"]);
}

// Test string ID in JSONRPCRequest
TEST_F(TypesTest, JSONRPCRequestStringId) {
  JSONRPCRequest request{.jsonrpc = "2.0",
                         .id = "test-id",
                         .method = "test.method",
                         .params = json{{"param", "value"}}};

  // Serialize to JSON
  json j = request;

  // Check JSON structure
  EXPECT_EQ(j["id"], "test-id");

  // Deserialize from JSON
  JSONRPCRequest deserialized = j.get<JSONRPCRequest>();

  // Check equality
  EXPECT_EQ(std::get<std::string>(request.id),
            std::get<std::string>(deserialized.id));
}

// Test JSONRPCResponse serialization/deserialization
TEST_F(TypesTest, JSONRPCResponseSerialization) {
  JSONRPCResponse response{
      .jsonrpc = "2.0", .id = 1, .result = json{{"result", "value"}}};

  // Serialize to JSON
  json j = response;

  // Check JSON structure
  EXPECT_EQ(j["jsonrpc"], "2.0");
  EXPECT_EQ(j["id"], 1);
  EXPECT_EQ(j["result"]["result"], "value");

  // Deserialize from JSON
  JSONRPCResponse deserialized = j.get<JSONRPCResponse>();

  // Check equality
  EXPECT_EQ(response.jsonrpc, deserialized.jsonrpc);
  EXPECT_EQ(std::get<int>(response.id), std::get<int>(deserialized.id));
  EXPECT_EQ(response.result["result"], deserialized.result["result"]);
}

// Test JSONRPCError serialization/deserialization
TEST_F(TypesTest, JSONRPCErrorSerialization) {
  JSONRPCError error{
      .jsonrpc = "2.0",
      .id = 1,
      .error = {.code = static_cast<int>(ErrorCode::InvalidParams),
                .message = "Invalid parameters",
                .data = json{{"param", "value"}}}};

  // Serialize to JSON
  json j = error;

  // Check JSON structure
  EXPECT_EQ(j["jsonrpc"], "2.0");
  EXPECT_EQ(j["id"], 1);
  EXPECT_EQ(j["error"]["code"], static_cast<int>(ErrorCode::InvalidParams));
  EXPECT_EQ(j["error"]["message"], "Invalid parameters");
  EXPECT_EQ(j["error"]["data"]["param"], "value");

  // Deserialize from JSON
  JSONRPCError deserialized = j.get<JSONRPCError>();

  // Check equality
  EXPECT_EQ(error.jsonrpc, deserialized.jsonrpc);
  EXPECT_EQ(std::get<int>(error.id), std::get<int>(deserialized.id));
  EXPECT_EQ(error.error.code, deserialized.error.code);
  EXPECT_EQ(error.error.message, deserialized.error.message);
  EXPECT_EQ(error.error.data["param"], deserialized.error.data["param"]);
}

// Test JSONRPCNotification serialization/deserialization
TEST_F(TypesTest, JSONRPCNotificationSerialization) {
  JSONRPCNotification notification{.jsonrpc = "2.0",
                                   .method = "test.notification",
                                   .params = json{{"param", "value"}}};

  // Serialize to JSON
  json j = notification;

  // Check JSON structure
  EXPECT_EQ(j["jsonrpc"], "2.0");
  EXPECT_EQ(j["method"], "test.notification");
  EXPECT_EQ(j["params"]["param"], "value");

  // Deserialize from JSON
  JSONRPCNotification deserialized = j.get<JSONRPCNotification>();

  // Check equality
  EXPECT_EQ(notification.jsonrpc, deserialized.jsonrpc);
  EXPECT_EQ(notification.method, deserialized.method);
  EXPECT_EQ((*notification.params)["param"], (*deserialized.params)["param"]);
}

// Test Server/Client capabilities
TEST_F(TypesTest, CapabilitiesSerialization) {
  ServerCapabilities server_capabilities{.supportsTools = true,
                                         .supportsResources = false,
                                         .supportsPrompts = true};

  ClientCapabilities client_capabilities{.supportsProgress = true,
                                         .supportsCancellation = false};

  // Serialize to JSON
  json j_server = server_capabilities;
  json j_client = client_capabilities;

  // Check JSON structure
  EXPECT_EQ(j_server["supportsTools"], true);
  EXPECT_EQ(j_server["supportsResources"], false);
  EXPECT_EQ(j_server["supportsPrompts"], true);

  EXPECT_EQ(j_client["supportsProgress"], true);
  EXPECT_EQ(j_client["supportsCancellation"], false);
}

// Test InitializeParams/Result
TEST_F(TypesTest, InitializeMessagesSerialization) {
  InitializeParams params{.clientName = "TestClient",
                          .clientVersion = "1.0.0",
                          .capabilities = {.supportsProgress = true,
                                           .supportsCancellation = false}};

  InitializeResult result{.serverName = "TestServer",
                          .serverVersion = "1.0.0",
                          .instructions = "Test instructions",
                          .capabilities = {.supportsTools = true,
                                           .supportsResources = false,
                                           .supportsPrompts = true}};

  // Serialize to JSON
  json j_params = params;
  json j_result = result;

  // Check JSON structure
  EXPECT_EQ(j_params["clientName"], "TestClient");
  EXPECT_EQ(j_params["clientVersion"], "1.0.0");
  EXPECT_EQ(j_params["capabilities"]["supportsProgress"], true);
  EXPECT_EQ(j_params["capabilities"]["supportsCancellation"], false);

  EXPECT_EQ(j_result["serverName"], "TestServer");
  EXPECT_EQ(j_result["serverVersion"], "1.0.0");
  EXPECT_EQ(j_result["instructions"], "Test instructions");
  EXPECT_EQ(j_result["capabilities"]["supportsTools"], true);
  EXPECT_EQ(j_result["capabilities"]["supportsResources"], false);
  EXPECT_EQ(j_result["capabilities"]["supportsPrompts"], true);
}

// Test Tool related structures
TEST_F(TypesTest, ToolStructuresSerialization) {
  Tool tool{.name = "test.tool",
            .description = "Test tool",
            .parameters = {{.name = "param1",
                            .description = "Parameter 1",
                            .schema = json{{"type", "string"}},
                            .required = true}},
            .returns = json{{"type", "object"}}};

  // Serialize to JSON
  json j = tool;

  // Check JSON structure
  EXPECT_EQ(j["name"], "test.tool");
  EXPECT_EQ(j["description"], "Test tool");
  EXPECT_EQ(j["parameters"][0]["name"], "param1");
  EXPECT_EQ(j["parameters"][0]["description"], "Parameter 1");
  EXPECT_EQ(j["parameters"][0]["schema"]["type"], "string");
  EXPECT_EQ(j["parameters"][0]["required"], true);
  EXPECT_EQ(j["returns"]["type"], "object");
}