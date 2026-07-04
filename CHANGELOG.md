# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed (post-0.1.0)

- **HTTP transport split into its own `mcp::http` target.** The core `mcp`
  library now depends only on `nlohmann_json`; the Streamable-HTTP transport
  (`HttpServerHost` / `HttpClientTransport`) and its `cpp-httplib → OpenSSL`
  dependency moved to a separate `mcp-http` / `mcp::http` target, still gated by
  the `MCP_ENABLE_HTTP` option (default ON). A stdio-only server linked against
  `mcp::mcp` now loads no OpenSSL/brotli/Security at all — smaller dependency
  surface, no OpenSSL CVE exposure, and ~halved cold start.
  - **Breaking (build only):** consumers of the HTTP transport must link
    `mcp::http` instead of `mcp::mcp`. Stdio-only consumers are unaffected.
- cpp-httplib's optional `brotli`/`zlib` auto-detection is now pinned **off**
  (OpenSSL stays on), so HTTP builds are deterministic and no longer break when a
  system `brotli` library is present without its headers.

### Performance (post-0.1.0)

- Request methods no longer spawn a `std::thread` per call. `Client::call_tool`
  et al. (and `Server::sample`/`list_roots`/`elicit`) previously wrapped each
  call in `std::async(std::launch::async)` solely to convert the result JSON to
  a typed value. That now runs as a typed continuation on the session read
  thread via the new `Session::send_request_for<T>()`, eliminating the per-call
  thread while preserving `std::future` semantics (including `.wait_for()`).
  Measured: in-process `tools/call` throughput **+~25%** (~24k → ~30k calls/sec)
  and p50 latency **−~21%** (~39 µs → ~31 µs) on an Apple Silicon dev machine.

### Fixed (post-0.1.0)

- `HttpServerHost` now honors `Options::port`. Previously `start()` always
  called `bind_to_any_port`, silently binding an OS-assigned port and ignoring
  a requested fixed port (surfaced by real-client interop testing — every
  consumer asking for a specific port got a random one). A bind failure on the
  requested port now throws instead of falling back.
- POSTing to a terminated or unknown `Mcp-Session-Id` now returns **404**
  as the spec requires (the client's cue to re-initialize), instead of 400.
  A session-less non-initialize POST remains 400. GET already returned 404.

### Added (post-0.1.0)

- **Windows (MSVC) support for the core + HTTP transport.** The POSIX-only
  `StdioTransport` is compiled out on `_WIN32` (its header carries a matching
  `#error` guard); everything else — protocol, session, client/server, and the
  Streamable HTTP transport — builds and tests under MSVC. New `windows-latest`
  CI jobs (Debug + RelWithDebInfo) make this a supported configuration.

## [0.1.0] - 2026-04-29

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
  - API: `Server::elicit(params, timeout?)`,
    `Server::notify_elicitation_complete(id)`,
    `Server::set_elicitation_complete_handler(...)`,
    `Client::set_elicitation_handler(...)`,
    `Client::set_elicitation_complete_handler(...)`,
    `Client::notify_elicitation_complete(id)`. Either side can
    register a handler for the completion notification — the spec
    lets either side emit it. Setting an elicitation handler
    auto-advertises the `elicitation: {form: {}, url: {}}`
    capability on the next `initialize()`;
    `set_client_capabilities()` is a per-field override that lets
    you narrow `elicitation` to form-only without dropping the
    other handler-derived caps.
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
  - Server: `Server::enable_tasks(default_ttl_ms?, max_concurrent?)`
    opts the server in and advertises `tasks: { list:{}, cancel:{},
    requests:{ tools:{ call:{} }}}`. A private thread-safe
    `detail::TaskStore` owns the in-flight bookkeeping; the Server
    destructor blocks on `shutdown()` until every detached worker
    has returned, so long-running tasks never outlive their store.
    `max_concurrent` caps the number of in-flight task workers
    (DoS hardening); `0` is unlimited.
  - Client: `call_tool_as_task(...)`, `task_get`, `task_result`,
    `task_list`, `task_cancel`, `set_task_status_handler(...)`.
  - `tasks/result` returns a structured `data: { task: {...} }` on
    long-poll timeout so the caller can re-poll without first
    issuing a `tasks/get`.
- **OAuth 2.1 authorization** on the HTTP transport (RFC 6750
  bearer + RFC 9728 protected resource metadata):
  - Server: `HttpServerHost::Options::bearer_validator` returns a
    `BearerOutcome` (allow / invalid_token / insufficient_scope +
    required_scopes) and gates every POST/GET/DELETE on a
    well-formed `Authorization: Bearer` header. The Bearer scheme
    match is case-insensitive (RFC 7235 §2.1); empty / whitespace
    tokens are rejected before the validator is invoked; multiple
    `Authorization` headers ⇒ 400. `invalid_token` ⇒ 401 +
    `WWW-Authenticate: Bearer realm="…", error="invalid_token"
    [, resource_metadata="…"]`. `insufficient_scope` ⇒ 403 with
    the required scopes echoed in the challenge. `auth_realm` and
    `resource_metadata_url` are validated at `start()` to reject
    CRLF / quoting characters that would smuggle an HTTP header.
    `Options::resource_metadata` / `resource_metadata_url`
    publish the discovery document at
    `/.well-known/oauth-protected-resource[/path]` with
    `Access-Control-Allow-Origin: *` so browser-based MCP
    clients can run discovery cross-origin.
  - Client: `HttpClientTransport::Options::access_token` seeds the
    Authorization header. Use `set_access_token(...)` /
    `access_token()` (mutex-guarded) to rotate the token at
    runtime — typically from inside an `on_unauthorized` callback
    after refreshing. `on_unauthorized(status, www_authenticate)`
    fires on both POST and the long-lived GET stream when the
    server returns 401/403; the GET stream then exits the
    reconnect loop instead of busy-retrying.
- 51 new tests across the three primitives (15 elicitation +
  18 tasks + 17 OAuth), bringing the suite to 202 tests. Each
  primitive went through an adversarial review pass; legitimate
  findings closed in dedicated `fix(...)` commits before the
  release.
- **Spec-completeness pass against the full 2025-11-25 schema**:
  - New content variants `ToolUseContent` (`"tool_use"`) and
    `ToolResultContent` (`"tool_result"`) for agentic-sampling
    loops. ToolResultContent.content uses a separate
    `PlainContentBlock` variant (excluding tool_use / tool_result)
    to enforce the spec's no-nested-tool rule at compile time.
  - `CreateMessageRequestParams.tools` and `.tool_choice` for
    tool-enabled sampling. `ToolChoice` enum reshaped to
    `auto / any / none / tool` (matching the spec; was the
    pre-spec `auto / required / none`); the wire form is a bare
    string for the first three and `{type:"tool", name:"..."}`
    for the named-tool case.
  - `Tool.execution.taskSupport` (`forbidden / optional / required`)
    enforced server-side: required ⇒ a sync call is rejected;
    forbidden ⇒ a task-augmented call is rejected. New
    `Server::ToolMetadata` + `Server::tool(name, schema, handler,
    ToolMetadata{...})` overload for declaring per-tool policy.
  - Universal `_meta` envelope round-trips on every named result
    type (Initialize / ListTools / CallTool / ListResources /
    ReadResource / ListResourceTemplates / ListPrompts / GetPrompt /
    CreateMessage) and on Implementation, Tool, Resource,
    ResourceTemplate, ResourceLink, Prompt — important for
    forward compat with `io.modelcontextprotocol/...` reserved
    keys.
  - `Icon` mixin + optional `icons` field on Implementation, Tool,
    Resource, ResourceTemplate, ResourceLink, Prompt.
  - `UrlElicitationRequiredErrorData` — the structured `data`
    payload for JSON-RPC error code -32042 (the constant already
    existed). Carries the URL-mode elicitations the client must
    run before retrying.
  - Streamable HTTP transport: SSE event IDs are captured and
    sent back as `Last-Event-ID` on GET-stream reconnect so the
    server can replay missed events. Server-supplied SSE `retry:`
    field is honoured for the reconnect backoff (clamped to
    [50, 60000] ms). On 404 from the GET stream, the client
    clears its session id so the next outbound POST
    re-initializes. On graceful close, the client sends an HTTP
    `DELETE` to the MCP path so the server can free per-session
    state immediately.
  - HTTP server validates the inbound `MCP-Protocol-Version`
    header — present-but-unsupported values produce 400.
- **`examples/mcp_notes_server`** — a cool, usable MCP demo:
  SQLite + FTS5 backed scratchpad that gives Claude (or any MCP
  client) persistent, full-text-searchable memory across
  sessions. Tools: write / read / list / search / delete / info.
  Resource: `notes://all`. Globs and unconfirmed deletes route
  through MCP elicitation for human confirmation. Build is gated
  on system SQLite3 availability.

### Notes

- Final test count at 0.1.0: **220 tests** (218 active + 1
  conditional skip) — clean on Rel + TSan + ASan locally and on
  Linux/GCC via the worker1 parity check.
- Linux GCC 13 with `-Werror=missing-field-initializers` is the
  enforced strict reference build. Several rounds of
  designated-initializer fixups land throughout the changelog
  history above.
