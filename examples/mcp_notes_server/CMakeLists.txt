# mcp-notes-server: a SQLite-backed scratchpad for Claude.
#
# Builds against system SQLite (find_package(SQLite3)). On Linux:
# `sudo apt install libsqlite3-dev`. On macOS: ships with Xcode CLT.

find_package(SQLite3 QUIET)
if(NOT SQLite3_FOUND)
    message(STATUS "examples/mcp_notes_server: SQLite3 not found; skipping. "
                   "Install libsqlite3-dev (Linux) or via brew (macOS) to build it.")
    return()
endif()

add_executable(mcp_notes_server main.cpp)
# Newer CMake exports SQLite3::SQLite3 (target style); older ones
# expose SQLite::SQLite3. Pick whichever the find_package gave us.
if(TARGET SQLite3::SQLite3)
    target_link_libraries(mcp_notes_server PRIVATE mcp::mcp SQLite3::SQLite3)
else()
    target_link_libraries(mcp_notes_server PRIVATE mcp::mcp SQLite::SQLite3)
endif()

target_compile_options(mcp_notes_server PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
        -Wall -Wextra -Wpedantic
    >
)
