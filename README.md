# mcp-cpp

A C++ SDK for the Model Context Protocol (MCP). The SDK provides a framework for creating MCP servers and clients in C++ following modern C++17 best practices.

## Features

- JSON-RPC 2.0 based messaging
- Multiple transport mechanisms (STDIO, HTTP/SSE planned)
- Support for MCP capabilities: Tools, Resources, and Prompts
- Thread-safe design
- Memory-safe implementation using RAII and smart pointers
- Comprehensive error handling

## Building the Library

The library uses CMake for building:

```bash
# Configure the build
cmake -B build

# Build the library
cmake --build build

# Run tests
cd build && ctest
```

## Examples

### Echo Server/Client

The repository includes a simple echo server and client example that demonstrates the basic usage of the library.

To run the echo server:

```bash
./build/examples/simple_server/echo_server
```

To run the echo client in a different terminal:

```bash
./build/examples/simple_client/echo_client
```

The client sends a test request and notification on startup and then allows you to enter messages that will be echoed back by the server.

## Development Status

The project is currently in development with the following components implemented:

- Core types and JSON serialization
- Error handling utilities
- Transport interface
- STDIO transport implementation
- Logging utilities

Upcoming features:

- Session management
- HTTP/SSE transport
- Server implementation
- Client implementation

## License

See the LICENSE file for details.