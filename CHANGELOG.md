# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- C++20 SDK targeting MCP protocol revision **2025-11-25**.
- Wire types and JSON serialization for the full Phase 1–3 surface:
  initialize / capabilities, tools (list/call), resources (list, read,
  templates, subscribe/unsubscribe), prompts (list, get), sampling
  (`sampling/createMessage`), roots (`roots/list`), completion
  (`completion/complete`), cancellation (`notifications/cancelled`),
  progress (`notifications/progress`), protocol-level logging
  (`notifications/message` + `logging/setLevel`), and ping.
- `Transport` abstraction with a spec-correct POSIX `StdioTransport`.
  The read loop uses `poll(2)` plus a self-pipe wake-up so `close()`
  can interrupt a read in progress; the legacy implementation that
  this replaces deadlocked there.
- `Session` JSON-RPC dispatcher with request/response correlation,
  configurable per-request timeouts, and request-handler dispatch on a
  worker thread so handlers can themselves issue further requests
  (e.g. `server.sample()` from inside a tool handler) without
  deadlocking.
- High-level `Server` and `Client` façades composed on top of
  `Session`. The Server registers tools/resources/prompts and runs
  with a transport; the Client exposes typed convenience methods
  (`initialize`, `list_tools`, `call_tool`, `list_resources`,
  `read_resource`, `list_prompts`, `get_prompt`, `complete`, `ping`,
  etc.) that return `std::future<T>`.
- Cursor-based pagination on every list operation, controlled by
  `Server::set_page_size`.
- `examples/calculator_server` — a self-contained example MCP server.
- 122 unit/integration tests in 10 GTest binaries, including a
  subprocess test that spawns the example as a child process and
  drives it over real OS pipes.
- Clean build under `-Wall -Wextra -Wpedantic -Wshadow -Wconversion
  -Wsign-conversion -Werror`, plus `-fsanitize=address,undefined` and
  `-fsanitize=thread`.
- GitHub Actions CI: matrix of {Ubuntu, macOS} × {GCC, Clang} ×
  {Debug, RelWithDebInfo}, plus dedicated ASan/TSan jobs and an
  install-and-consume job that builds a downstream `find_package`
  user.
- **Streamable HTTP transport** (spec section "Streamable HTTP"):
  - `HttpClientTransport` — connects to any spec-compliant remote
    MCP endpoint. Handles application/json and text/event-stream
    responses, captures and round-trips the `Mcp-Session-Id`
    header, opens an SSE GET stream for server-initiated traffic
    (sampling, server log emissions, etc.).
  - `HttpServerHost` — multi-session host built on cpp-httplib.
    Mints session ids on `initialize`, routes POST + GET + DELETE
    on a single configurable endpoint path, validates the `Origin`
    header against a configurable allowlist (DNS-rebind defense),
    and exposes server-initiated traffic over an SSE GET stream.
  - cpp-httplib v0.18.5 wired via `FetchContent` and gated on the
    new `MCP_ENABLE_HTTP` CMake option (default ON).
  - `examples/http_calculator_server`: stdio calculator's twin,
    exposed over HTTP.
- **Elicitation** (2025-11-25): server-initiated `elicitation/create`
  requests for in-band JSON-Schema-described forms or out-of-band
  URL-mode flows, plus `notifications/elicitation/complete` for
  url-mode completion signals.
  - Wire types: `ElicitFormRequestParams`, `ElicitUrlRequestParams`,
    the tagged variant `ElicitRequestParams`, the `ElicitAction`
    enum (`accept`/`decline`/`cancel`), `ElicitResult`, and
    `ElicitationCompleteNotificationParams`.
  - API: `Server::elicit(...)`, `Server::notify_elicitation_complete(id)`,
    `Client::set_elicitation_handler(...)`,
    `Client::set_elicitation_complete_handler(...)`. Setting an
    elicitation handler auto-advertises the
    `elicitation: {form: {}, url: {}}` capability on the next
    `initialize()`.
- **Tasks** (2025-11-25): long-running tool calls. Opt in by passing
  a `task` augmentation in `tools/call`'s params; the receiver
  returns a `CreateTaskResult` envelope right away and runs the
  handler async, with the actual `CallToolResult` retrieved via
  `tasks/result`.
  - Methods: `tasks/get`, `tasks/result`, `tasks/list`, `tasks/cancel`;
    notification: `notifications/tasks/status`. State machine:
    `working`/`input_required` → terminal `completed`/`failed`/
    `cancelled`. Per spec, results carry an
    `_meta["io.modelcontextprotocol/related-task"]` envelope.
  - Server: `Server::enable_tasks(default_ttl_ms?)` opts the server in
    and advertises `tasks: { list:{}, cancel:{}, requests:{ tools:{
    call:{} }}}`. A private thread-safe `detail::TaskStore` owns the
    in-flight bookkeeping; the Server destructor blocks on
    `shutdown()` until every detached worker has returned, so
    long-running tasks never outlive their store.
  - Client: `call_tool_as_task(...)`, `task_get`, `task_result`,
    `task_list`, `task_cancel`, `set_task_status_handler(...)`.
- **OAuth 2.1 authorization** on the HTTP transport (RFC 6750
  bearer + RFC 9728 protected resource metadata):
  - Server: `HttpServerHost::Options::bearer_validator` gates every
    POST/GET/DELETE on `Authorization: Bearer <token>`. Missing /
    invalid token ⇒ 401 with `WWW-Authenticate: Bearer realm="...",
    resource_metadata="..."`. `Options::resource_metadata` /
    `resource_metadata_url` publish the discovery document at
    `/.well-known/oauth-protected-resource[/path]`.
  - Client: `HttpClientTransport::Options::access_token` injects the
    Authorization header on every outbound request. The
    `on_unauthorized(status, www_authenticate)` callback surfaces
    challenges so the application can drive its OAuth flow.
- 31 new tests (9 elicitation + 14 tasks + 8 OAuth), bringing the
  suite to 182 tests. Full matrix CI green on all three primitives.
