#include "mcp/client.hpp"
#include "mcp/utils/logging.hpp"

namespace mcp {

Client::Client(const std::string &name, const std::string &version)
    : client_info_({name, version}) {}

Client::~Client() { disconnect(); }

Client &Client::setCapabilities(const types::ClientCapabilities &capabilities) {
  client_info_.capabilities = capabilities;
  return *this;
}

std::future<types::InitializeResult>
Client::initialize(std::shared_ptr<transport::Transport> transport,
                   std::chrono::milliseconds timeout) {

  // Create a new session
  session_ = std::make_shared<ClientSession>(transport);

  // Connect the transport
  session_->connect();

  // Initialize the session
  auto future = session_->initialize(client_info_, timeout);

  // Log the initialization
  utils::log(utils::LogLevel::Info,
             "Client initializing: " + client_info_.name + " v" +
                 client_info_.version);

  return future;
}

void Client::disconnect() {
  if (session_) {
    session_->disconnect();
    session_.reset();

    utils::log(utils::LogLevel::Info, "Client disconnected");
  }
}

bool Client::isConnected() const { return session_ && session_->isConnected(); }

std::future<types::ListToolsResult>
Client::listTools(std::chrono::milliseconds timeout) {

  if (!isConnected()) {
    std::promise<types::ListToolsResult> promise;
    promise.set_exception(std::make_exception_ptr(utils::TransportException(
        types::ErrorCode::TransportError, "Client not connected")));

    return promise.get_future();
  }

  return session_->listTools(timeout);
}

std::future<types::CallToolResult>
Client::callTool(const std::string &name, const nlohmann::json &params,
                 std::chrono::milliseconds timeout) {

  if (!isConnected()) {
    std::promise<types::CallToolResult> promise;
    promise.set_exception(std::make_exception_ptr(utils::TransportException(
        types::ErrorCode::TransportError, "Client not connected")));

    return promise.get_future();
  }

  return session_->callTool(name, params, timeout);
}

std::future<types::ListResourcesResult>
Client::listResources(std::chrono::milliseconds timeout) {

  if (!isConnected()) {
    std::promise<types::ListResourcesResult> promise;
    promise.set_exception(std::make_exception_ptr(utils::TransportException(
        types::ErrorCode::TransportError, "Client not connected")));

    return promise.get_future();
  }

  return session_->listResources(timeout);
}

std::future<types::ReadResourceResult>
Client::readResource(const std::string &uri,
                     std::chrono::milliseconds timeout) {

  if (!isConnected()) {
    std::promise<types::ReadResourceResult> promise;
    promise.set_exception(std::make_exception_ptr(utils::TransportException(
        types::ErrorCode::TransportError, "Client not connected")));

    return promise.get_future();
  }

  return session_->readResource(uri, timeout);
}

std::future<types::ListPromptsResult>
Client::listPrompts(std::chrono::milliseconds timeout) {

  if (!isConnected()) {
    std::promise<types::ListPromptsResult> promise;
    promise.set_exception(std::make_exception_ptr(utils::TransportException(
        types::ErrorCode::TransportError, "Client not connected")));

    return promise.get_future();
  }

  return session_->listPrompts(timeout);
}

std::future<types::GetPromptResult>
Client::getPrompt(const std::string &name, const nlohmann::json &args,
                  std::chrono::milliseconds timeout) {

  if (!isConnected()) {
    std::promise<types::GetPromptResult> promise;
    promise.set_exception(std::make_exception_ptr(utils::TransportException(
        types::ErrorCode::TransportError, "Client not connected")));

    return promise.get_future();
  }

  return session_->getPrompt(name, args, timeout);
}

} // namespace mcp