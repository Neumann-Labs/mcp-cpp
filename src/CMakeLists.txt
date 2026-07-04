target_sources(mcp PRIVATE
    client.cpp
    log.cpp
    protocol.cpp
    server.cpp
    session.cpp
    stdio_transport.cpp
)

# Optional HTTP transport, split into its own target so the TLS/compression
# deps (httplib -> OpenSSL/brotli/zlib) are linked only into binaries that
# actually use HTTP. Consumers link `mcp::http` (which brings in `mcp`).
if(MCP_ENABLE_HTTP)
    add_library(mcp-http
        http_client_transport.cpp
        http_server_host.cpp
    )
    add_library(mcp::http ALIAS mcp-http)
    # Export as `mcp::http` (not `mcp::mcp-http`) so installed consumers use the
    # same name as the in-tree alias. install(EXPORT) ignores ALIAS targets and
    # would otherwise namespace the raw target name `mcp-http`.
    set_target_properties(mcp-http PROPERTIES EXPORT_NAME http)

    target_link_libraries(mcp-http PUBLIC mcp PRIVATE httplib::httplib)
    # PUBLIC so the `#if !defined(MCP_ENABLE_HTTP)` guards in the HTTP headers
    # are satisfied for anyone linking mcp::http.
    target_compile_definitions(mcp-http PUBLIC MCP_ENABLE_HTTP=1)

    mcp_target_warnings(mcp-http)
endif()
