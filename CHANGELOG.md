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
