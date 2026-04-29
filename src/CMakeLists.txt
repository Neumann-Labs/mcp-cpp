target_sources(mcp PRIVATE
    client.cpp
    log.cpp
    protocol.cpp
    server.cpp
    session.cpp
    stdio_transport.cpp
)

if(MCP_ENABLE_HTTP)
    target_sources(mcp PRIVATE http_client_transport.cpp)
endif()
