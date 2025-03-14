#include "mcp/server.hpp"
#include "mcp/utils/logging.hpp"

namespace mcp {

Server::Server(const std::string &name, const std::string &version,
               const std::string &instructions)
    : server_info_({name, version, instructions}) {}

Server::~Server() { stop(); }

Server &Server::setCapabilities(const types::ServerCapabilities &capabilities) {
  std::lock_guard<std::mutex> lock(mutex_);

  server_capabilities_ = capabilities;

  if (session_) {
    session_->setServerCapabilities(capabilities);
  }

  return *this;
}

Server &Server::registerTool(
    const std::string &name, const std::string &description,
    const std::optional<nlohmann::json> &input_schema,
    std::function<nlohmann::json(const nlohmann::json &)> handler) {

  std::lock_guard<std::mutex> lock(mutex_);

  if (tools_.find(name) != tools_.end()) {
    utils::log(utils::LogLevel::Warning, "Tool already registered: " + name);
  }

  // Create and store the tool
  types::Tool tool;
  tool.name = name;
  tool.description = description;
  tool.input_schema = input_schema;

  tools_[name] = tool;
  tool_handlers_[name] = std::move(handler);

  // Register with session if running
  if (session_) {
    session_->registerTool(tool, tool_handlers_[name]);
  }

  return *this;
}

Server &Server::registerResource(
    const std::string &uri, const std::string &name,
    const std::string &description,
    std::function<types::ReadResourceResult(const std::string &)> handler) {

  std::lock_guard<std::mutex> lock(mutex_);

  if (resources_.find(uri) != resources_.end()) {
    utils::log(utils::LogLevel::Warning, "Resource already registered: " + uri);
  }

  // Create and store the resource
  types::Resource resource;
  resource.uri = uri;
  resource.name = name;
  resource.description = description;

  resources_[uri] = resource;
  resource_handlers_[uri] = std::move(handler);

  // Register with session if running
  if (session_) {
    session_->registerResource(resource, resource_handlers_[uri]);
  }

  return *this;
}

Server &Server::registerPrompt(
    const std::string &name, const std::string &description,
    const std::vector<types::PromptArgument> &arguments,
    std::function<std::string(const nlohmann::json &)> handler) {

  std::lock_guard<std::mutex> lock(mutex_);

  if (prompts_.find(name) != prompts_.end()) {
    utils::log(utils::LogLevel::Warning, "Prompt already registered: " + name);
  }

  // Create and store the prompt
  types::Prompt prompt;
  prompt.name = name;
  prompt.description = description;
  prompt.arguments = arguments;

  prompts_[name] = prompt;
  prompt_handlers_[name] = std::move(handler);

  // Register with session if running
  if (session_) {
    session_->registerPrompt(prompt, prompt_handlers_[name]);
  }

  return *this;
}

void Server::run(std::shared_ptr<transport::Transport> transport) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Create a new session
  session_ = std::make_shared<ServerSession>(transport, server_info_);

  // Set capabilities
  session_->setServerCapabilities(server_capabilities_);

  // Register all tools, resources, and prompts
  for (const auto &[name, tool] : tools_) {
    session_->registerTool(tool, tool_handlers_[name]);
  }

  for (const auto &[uri, resource] : resources_) {
    session_->registerResource(resource, resource_handlers_[uri]);
  }

  for (const auto &[name, prompt] : prompts_) {
    session_->registerPrompt(prompt, prompt_handlers_[name]);
  }

  // Start the session
  session_->connect();

  utils::log(utils::LogLevel::Info, "Server started: " + server_info_.name +
                                        " v" + server_info_.version);
}

void Server::stop() {
  std::lock_guard<std::mutex> lock(mutex_);

  if (session_) {
    session_->disconnect();
    session_.reset();

    utils::log(utils::LogLevel::Info, "Server stopped");
  }
}

} // namespace mcp