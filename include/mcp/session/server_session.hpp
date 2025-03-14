#ifndef MCP_SESSION_SERVER_SESSION_HPP_
#define MCP_SESSION_SERVER_SESSION_HPP_

#include "mcp/session/session.hpp"
#include "mcp/types.hpp"

#include <functional>
#include <string>
#include <unordered_map>

namespace mcp {

/**
 * @brief ServerSession specialization for MCP server-side communication
 *
 * The ServerSession class is responsible for:
 * - Managing server-side initialization and negotiation
 * - Registering and invoking tools, resources, and prompts
 * - Handling server-specific message routing
 */
class ServerSession : public Session {
public:
  /**
   * @brief Construct a new ServerSession with the given transport
   *
   * @param transport The transport to use for communication
   * @param server_info Server information to provide during initialization
   */
  ServerSession(std::shared_ptr<transport::Transport> transport,
                const types::ServerInfo &server_info);

  /**
   * @brief Destroy the ServerSession object
   */
  ~ServerSession() override;

  /**
   * @brief Set the server capabilities
   *
   * @param capabilities The server capabilities to set
   * @return ServerSession& This instance for method chaining
   */
  ServerSession &
  setServerCapabilities(const types::ServerCapabilities &capabilities);

  /**
   * @brief Register a tool with the server
   *
   * @param tool Tool information
   * @param handler Handler function to execute when the tool is called
   * @return ServerSession& This instance for method chaining
   */
  ServerSession &
  registerTool(const types::Tool &tool,
               std::function<nlohmann::json(const nlohmann::json &)> handler);

  /**
   * @brief Register a resource with the server
   *
   * @param resource Resource information
   * @param handler Handler function to execute when the resource is read
   * @return ServerSession& This instance for method chaining
   */
  ServerSession &registerResource(
      const types::Resource &resource,
      std::function<types::ReadResourceResult(const std::string &)> handler);

  /**
   * @brief Register a prompt with the server
   *
   * @param prompt Prompt information
   * @param handler Handler function to execute when the prompt is requested
   * @return ServerSession& This instance for method chaining
   */
  ServerSession &
  registerPrompt(const types::Prompt &prompt,
                 std::function<std::string(const nlohmann::json &)> handler);

  /**
   * @brief Connect the session and start handling requests
   */
  void connect() override;

protected:
  /**
   * @brief Handle an initialization request
   *
   * @param request The initialization request
   * @param respond Function to send a response
   */
  void handleInitializeRequest(
      const types::JSONRPCRequest &request,
      std::function<void(const types::JSONRPCResponse &)> respond);

  /**
   * @brief Handle a listTools request
   *
   * @param request The listTools request
   * @param respond Function to send a response
   */
  void handleListToolsRequest(
      const types::JSONRPCRequest &request,
      std::function<void(const types::JSONRPCResponse &)> respond);

  /**
   * @brief Handle a callTool request
   *
   * @param request The callTool request
   * @param respond Function to send a response
   */
  void handleCallToolRequest(
      const types::JSONRPCRequest &request,
      std::function<void(const types::JSONRPCResponse &)> respond);

  /**
   * @brief Handle a listResources request
   *
   * @param request The listResources request
   * @param respond Function to send a response
   */
  void handleListResourcesRequest(
      const types::JSONRPCRequest &request,
      std::function<void(const types::JSONRPCResponse &)> respond);

  /**
   * @brief Handle a readResource request
   *
   * @param request The readResource request
   * @param respond Function to send a response
   */
  void handleReadResourceRequest(
      const types::JSONRPCRequest &request,
      std::function<void(const types::JSONRPCResponse &)> respond);

  /**
   * @brief Handle a listPrompts request
   *
   * @param request The listPrompts request
   * @param respond Function to send a response
   */
  void handleListPromptsRequest(
      const types::JSONRPCRequest &request,
      std::function<void(const types::JSONRPCResponse &)> respond);

  /**
   * @brief Handle a getPrompt request
   *
   * @param request The getPrompt request
   * @param respond Function to send a response
   */
  void handleGetPromptRequest(
      const types::JSONRPCRequest &request,
      std::function<void(const types::JSONRPCResponse &)> respond);

private:
  /**
   * @brief Helper method to respond with an error
   *
   * @param error The error to respond with
   * @param respond Function to call with the response
   */
  void
  respondWithError(const types::JSONRPCError &error,
                   std::function<void(const types::JSONRPCResponse &)> respond);

  /**
   * @brief Helper method to respond with an error using stored respond function
   *
   * @param error The error to respond with
   */
  void respondWithError(const types::JSONRPCError &error);

  /**
   * @brief Handler information for a registered tool
   */
  struct ToolHandler {
    types::Tool tool;
    std::function<nlohmann::json(const nlohmann::json &)> handler;
  };

  /**
   * @brief Handler information for a registered resource
   */
  struct ResourceHandler {
    types::Resource resource;
    std::function<types::ReadResourceResult(const std::string &)> handler;
  };

  /**
   * @brief Handler information for a registered prompt
   */
  struct PromptHandler {
    types::Prompt prompt;
    std::function<std::string(const nlohmann::json &)> handler;
  };

  /**
   * @brief Server information
   */
  types::ServerInfo server_info_;

  /**
   * @brief Server capabilities
   */
  types::ServerCapabilities server_capabilities_;

  /**
   * @brief Map of tool name to tool handler
   */
  std::unordered_map<std::string, ToolHandler> tool_handlers_;

  /**
   * @brief Map of resource URI to resource handler
   */
  std::unordered_map<std::string, ResourceHandler> resource_handlers_;

  /**
   * @brief Map of prompt name to prompt handler
   */
  std::unordered_map<std::string, PromptHandler> prompt_handlers_;

  /**
   * @brief Mutex to protect the handlers
   */
  std::mutex handlers_mutex_;

  /**
   * @brief Flag indicating if the session has been initialized
   */
  std::atomic<bool> initialized_;

  /**
   * @brief Current request response function
   */
  std::function<void(const types::JSONRPCResponse &)> request_respond_function_;
};

} // namespace mcp

#endif // MCP_SESSION_SERVER_SESSION_HPP_