cmake_minimum_required(VERSION 3.20)

# Release version. The release workflow tags and names releases from this value.
project(ai-orchestrator VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

option(AIO_WERROR "Treat compiler warnings as errors" OFF)
option(AIO_BUILD_TESTS "Build unit tests" ON)

include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
FetchContent_Declare(ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI
  GIT_TAG v5.0.0
  GIT_SHALLOW TRUE
)
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib
  GIT_TAG v0.63.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(ftxui json httplib)

add_subdirectory(src/platform)

function(aio_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /utf-8 $<$<BOOL:${AIO_WERROR}>:/WX>)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic $<$<BOOL:${AIO_WERROR}>:-Werror>)
  endif()
endfunction()

# Linux server: agents, tmux, mailboxes, MCP. No UI.
add_library(aio_server STATIC
  src/server/tmux.cpp
  src/server/agents.cpp
  src/server/config.cpp
  src/server/mailbox.cpp
  src/server/mcp.cpp
  src/server/servers.cpp
  src/server/log.cpp
  src/server/adapters.cpp
  src/server/control.cpp
  src/server/terminal.cpp
  src/server/host.cpp
  src/server/relay.cpp
)
target_include_directories(aio_server PUBLIC src/server src/protocol)
target_compile_definitions(aio_server PUBLIC AIO_VERSION="${PROJECT_VERSION}")
target_link_libraries(aio_server PUBLIC aio_platform nlohmann_json::nlohmann_json httplib::httplib)
aio_set_warnings(aio_server)

# Local client. The sidebar talks to the control server over HTTP.
add_executable(aio
  src/client/main.cpp
  src/client/sidebar.cpp
  src/client/placeholder.cpp
  src/client/api.cpp
  src/client/attach.cpp
  src/client/join.cpp
)
target_link_libraries(aio PRIVATE aio_server ftxui::screen ftxui::dom ftxui::component)
aio_set_warnings(aio)
if(AIO_PLATFORM STREQUAL "windows" AND NOT MSVC)
  target_link_options(aio PRIVATE -static)
endif()

if(AIO_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
  enable_testing()
  add_subdirectory(tests)
endif()
