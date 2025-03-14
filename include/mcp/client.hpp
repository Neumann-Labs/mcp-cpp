#ifndef MCP_CLIENT_HPP_
#define MCP_CLIENT_HPP_

#include <future>
#include <memory>
#include <string>

#include "mcp/session/client_session.hpp"
#include "mcp/transport/transport.hpp"
#include "mcp/types.hpp"

namespace mcp {

/**
 * @brief MCP Client implementation
 *
 * The Client class provides a high-level API for implementing an MCP client.
 * It manages client capabilities, MCP session initialization, and provides
 * methods for interacting with server-provided tools, resources, and prompts.
 */
class Client {
public:
  /**
   * @brief Construct a new Client
   *
   * @param name Client name
   * @param version Client version
   */
  Client(const std::string &name, const std::string &version = "1.0.0");

  /**
   * @brief Destroy the Client
   */
  ~Client();

  /**
   * @brief Set client capabilities
   *
   * @param capabilities The capabilities to set
   * @return Client& This instance for method chaining
   */
  Client &setCapabilities(const types::ClientCapabilities &capabilities);

  /**
   * @brief Initialize the MCP connection
   *
   * @param transport The transport to use
   * @param timeout Timeout for initialization
   * @return std::future<types::InitializeResult> A future containing the
   * initialization result
   */
  std::future<types::InitializeResult>
  initialize(std::shared_ptr<transport::Transport> transport,
             std::chrono::milliseconds timeout = std::chrono::seconds(30));

  /**
   * @brief Disconnect the client
   */
  void disconnect();

  /**
   * @brief Check if the client is connected
   *
   * @return true if connected, false otherwise
   */
  bool isConnected() const;

  /**
   * @brief Get a list of available tools
   *
   * @param timeout Timeout for the request
   * @return std::future<types::ListToolsResult> A future containing the list of
   * tools
   */
  std::future<types::ListToolsResult>
  listTools(std::chrono::milliseconds timeout = std::chrono::seconds(30));

  /**
   * @brief Call a tool with the given parameters
   *
   * @param name Tool name
   * @param params Tool parameters
   * @param timeout Timeout for the request
   * @return std::future<types::CallToolResult> A future containing the tool
   * result
   */
  std::future<types::CallToolResult>
  callTool(const std::string &name, const nlohmann::json &params,
           std::chrono::milliseconds timeout = std::chrono::seconds(30));

  /**
   * @brief Get a list of available resources
   *
   * @param timeout Timeout for the request
   * @return std::future<types::ListResourcesResult> A future containing the
   * list of resources
   */
  std::future<types::ListResourcesResult>
  listResources(std::chrono::milliseconds timeout = std::chrono::seconds(30));

  /**
   * @brief Read a resource by URI
   *
   * @param uri Resource URI
   * @param timeout Timeout for the request
   * @return std::future<types::ReadResourceResult> A future containing the
   * resource content
   */
  std::future<types::ReadResourceResult>
  readResource(const std::string &uri,
               std::chrono::milliseconds timeout = std::chrono::seconds(30));

  /**
   * @brief Get a list of available prompts
   *
   * @param timeout Timeout for the request
   * @return std::future<types::ListPromptsResult> A future containing the list
   * of prompts
   */
  std::future<types::ListPromptsResult>
  listPrompts(std::chrono::milliseconds timeout = std::chrono::seconds(30));

  /**
   * @brief Get a prompt by name with the given arguments
   *
   * @param name Prompt name
   * @param args Prompt arguments
   * @param timeout Timeout for the request
   * @return std::future<types::GetPromptResult> A future containing the prompt
   * content
   */
  std::future<types::GetPromptResult>
  getPrompt(const std::string &name, const nlohmann::json &args,
            std::chrono::milliseconds timeout = std::chrono::seconds(30));

private:
  /**
   * @brief Client information
   */
  types::ClientInfo client_info_;

  /**
   * @brief Client session
   */
  std::shared_ptr<ClientSession> session_;
};

} // namespace mcp

#endif // MCP_CLIENT_HPP_