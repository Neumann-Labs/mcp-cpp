# mcp-cpp

A modern C++20 SDK for the [Model Context Protocol](https://modelcontextprotocol.io)
(MCP), targeting protocol revision **2025-11-25**.

> **Status:** in active development. The API is not yet stable.

## Why

LLM applications increasingly need a uniform way to talk to tool, resource, and
prompt providers. MCP is that protocol; this SDK aims to be a clean, fast,
spec-faithful C++ implementation suitable for embedding in editors, IDEs,
agents, native applications, and high-throughput servers.

Goals:

- **Spec-faithful.** Field names, method names, and behaviors match the official
  TypeScript schema verbatim.
- **Idiomatic C++20.** RAII, value semantics, `std::variant` for sum types,
  futures for async results, no inheritance hierarchies where composition will
  do.
- **Small surface, big leverage.** A focused public API; most of the work
  happens behind a few well-named types.
- **Production-ready.** Sanitizer-clean, thread-safe where it must be, no
  hidden allocations on hot paths.

## Building

```bash
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

### Useful build options

| Option                    | Default | Effect                                       |
| ------------------------- | ------- | -------------------------------------------- |
| `MCP_BUILD_TESTS`         | ON      | Build the GTest test suite.                  |
| `MCP_BUILD_EXAMPLES`      | ON      | Build the example servers/clients.           |
| `MCP_WARNINGS_AS_ERRORS`  | ON*     | Promote warnings to errors (top-level only). |
| `MCP_USE_SYSTEM_DEPS`     | OFF     | `find_package` instead of `FetchContent`.    |
| `MCP_ENABLE_ASAN`         | OFF     | `-fsanitize=address,undefined`.              |
| `MCP_ENABLE_TSAN`         | OFF     | `-fsanitize=thread`.                         |
| `MCP_ENABLE_COVERAGE`     | OFF     | `--coverage` for gcov/llvm-cov.              |

\* defaults to ON when this is the top-level project; OFF when consumed via `add_subdirectory`.

## License

Apache 2.0 — see [LICENSE](LICENSE).
