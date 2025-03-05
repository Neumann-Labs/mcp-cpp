cmake_minimum_required(VERSION 3.14...3.25)
project(mcp-cpp 
    VERSION 0.1.0
    DESCRIPTION "C++ SDK for the Model Context Protocol (MCP)"
    LANGUAGES CXX
)

# Set C++17 as the standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Configuration options
option(MCP_BUILD_TESTS "Build test suite" ON)
option(MCP_BUILD_EXAMPLES "Build examples" ON)
option(MCP_BUILD_DOCS "Build documentation" OFF)
option(MCP_USE_LIBCURL "Use libcurl for HTTP transport" ON)
option(ENABLE_COVERAGE "Enable code coverage reporting" OFF)

# External dependencies
include(FetchContent)

# Option to use system-installed dependencies
option(MCP_USE_SYSTEM_DEPS "Use system-installed dependencies instead of fetching them" OFF)

# Add nlohmann/json for JSON handling
if(MCP_USE_SYSTEM_DEPS)
  find_package(nlohmann_json 3.10.0 REQUIRED)
else()
  FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.2
  )
  FetchContent_MakeAvailable(json)
endif()

# Add JSON Schema validator
FetchContent_Declare(
  json_schema
  GIT_REPOSITORY https://github.com/pboettch/json-schema-validator.git
  GIT_TAG 2.1.0
)
set(JSON_SCHEMA_VALIDATOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(JSON_SCHEMA_VALIDATOR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(json_schema)

# Add GoogleTest for testing if needed
if(MCP_BUILD_TESTS)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
  )
  # For Windows: Prevent overriding the parent project's compiler/linker settings
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

# Add libcurl if requested
if(MCP_USE_LIBCURL)
  find_package(CURL REQUIRED)
endif()

# Define library
add_library(mcp
  src/types.cpp
  src/client.cpp
  src/server.cpp
  src/transport/stdio_transport.cpp
  src/transport/http_sse_transport.cpp
  src/session/session.cpp
  src/session/client_session.cpp
  src/session/server_session.cpp
  src/utils/error.cpp
  src/utils/json_utils.cpp
  src/utils/logging.cpp
)

# Public include directories
target_include_directories(mcp
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Link dependencies
target_link_libraries(mcp
  PUBLIC
    nlohmann_json::nlohmann_json
    nlohmann_json_schema_validator
)

if(MCP_USE_LIBCURL)
  target_link_libraries(mcp PRIVATE CURL::libcurl)
  target_compile_definitions(mcp PRIVATE MCP_USE_LIBCURL)
endif()

# Build tests if requested
if(MCP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# Build examples if requested
if(MCP_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# Build documentation if requested
if(MCP_BUILD_DOCS)
  # Doxygen setup will go here
endif()

# Installation rules
include(GNUInstallDirs)

# Create a variable for the list of targets to install
set(MCP_INSTALL_TARGETS mcp)

# If we're using FetchContent for nlohmann_json, add the library to the export set
if(NOT MCP_USE_SYSTEM_DEPS)
  list(APPEND MCP_INSTALL_TARGETS nlohmann_json)
endif()

# Add json schema validator to the export set
list(APPEND MCP_INSTALL_TARGETS nlohmann_json_schema_validator)

# Install the targets
install(TARGETS ${MCP_INSTALL_TARGETS}
  EXPORT mcp-targets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Install header files
install(DIRECTORY include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Create and install Config files for find_package support
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/mcp-config-version.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

# Install the export set
install(EXPORT mcp-targets
  FILE mcp-targets.cmake
  NAMESPACE mcp::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mcp
)

configure_file(cmake/mcp-config.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/mcp-config.cmake" @ONLY)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/mcp-config.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/mcp-config-version.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mcp
)
