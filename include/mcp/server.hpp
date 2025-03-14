#ifndef MCP_SERVER_HPP_
#define MCP_SERVER_HPP_

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "mcp/session/server_session.hpp"
#include "mcp/transport/transport.hpp"
#include "mcp/types.hpp"

namespace mcp {

/**
 * @brief MCP Server implementation
 *
 * The Server class provides a high-level API for implementing an MCP server.
 * It manages server capabilities, tool/resource/prompt registration, and
 * request handling.
 */
class Server {
public:
  /**
   * @brief Construct a new Server
   *
   * @param name Server name
   * @param version Server version
   * @param instructions Optional instructions for the client
   */
  Server(const std::string &name, const std::string &version = "1.0.0",
         const std::string &instructions = "");

  /**
   * @brief Destroy the Server
   */
  ~Server();

  /**
   * @brief Set server capabilities
   *
   * @param capabilities The capabilities to set
   * @return Server& This instance for method chaining
   */
  Server &setCapabilities(const types::ServerCapabilities &capabilities);

  /**
   * @brief Register a tool with the server
   *
   * @param name Tool name
   * @param description Tool description
   * @param input_schema JSON schema for tool input
   * @param handler Function to call when the tool is invoked
   * @return Server& This instance for method chaining
   */
  Server &
  registerTool(const std::string &name, const std::string &description,
               const std::optional<nlohmann::json> &input_schema,
               std::function<nlohmann::json(const nlohmann::json &)> handler);

  /**
   * @brief Register a resource with the server
   *
   * @param uri Resource URI
   * @param name Resource name
   * @param description Resource description
   * @param handler Function to call when the resource is read
   * @return Server& This instance for method chaining
   */
  Server &registerResource(
      const std::string &uri, const std::string &name,
      const std::string &description,
      std::function<types::ReadResourceResult(const std::string &)> handler);

  /**
   * @brief Register a prompt with the server
   *
   * @param name Prompt name
   * @param description Prompt description
   * @param arguments Prompt arguments
   * @param handler Function to call when the prompt is requested
   * @return Server& This instance for method chaining
   */
  Server &
  registerPrompt(const std::string &name, const std::string &description,
                 const std::vector<types::PromptArgument> &arguments,
                 std::function<std::string(const nlohmann::json &)> handler);

  /**
   * @brief Run the server with the given transport
   *
   * @param transport The transport to use
   */
  void run(std::shared_ptr<transport::Transport> transport);

  /**
   * @brief Stop the server
   */
  void stop();

private:
  /**
   * @brief Server information
   */
  types::ServerInfo server_info_;

  /**
   * @brief Server capabilities
   */
  types::ServerCapabilities server_capabilities_;

  /**
   * @brief Server session
   */
  std::shared_ptr<ServerSession> session_;

  /**
   * @brief Registered tools
   */
  std::unordered_map<std::string, types::Tool> tools_;

  /**
   * @brief Registered resources
   */
  std::unordered_map<std::string, types::Resource> resources_;

  /**
   * @brief Registered prompts
   */
  std::unordered_map<std::string, types::Prompt> prompts_;

  /**
   * @brief Tool handlers
   */
  std::unordered_map<std::string,
                     std::function<nlohmann::json(const nlohmann::json &)>>
      tool_handlers_;

  /**
   * @brief Resource handlers
   */
  std::unordered_map<std::string, std::function<types::ReadResourceResult(
                                      const std::string &)>>
      resource_handlers_;

  /**
   * @brief Prompt handlers
   */
  std::unordered_map<std::string,
                     std::function<std::string(const nlohmann::json &)>>
      prompt_handlers_;

  /**
   * @brief Mutex for protecting the server state
   */
  std::mutex mutex_;
};

} // namespace mcp

#endif // MCP_SERVER_HPP_