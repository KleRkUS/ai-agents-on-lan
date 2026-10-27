# Platform abstraction. Exactly one OS implementation is compiled, chosen by
# AIO_PLATFORM at configure time (defaults to the target system).

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  set(_aio_default_platform windows)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(_aio_default_platform macos)
else()
  set(_aio_default_platform linux)
endif()

set(AIO_PLATFORM "${_aio_default_platform}" CACHE STRING "Target platform: linux, macos or windows")
set_property(CACHE AIO_PLATFORM PROPERTY STRINGS linux macos windows)

if(NOT AIO_PLATFORM MATCHES "^(linux|macos|windows)$")
  message(FATAL_ERROR "AIO_PLATFORM must be linux, macos or windows (got '${AIO_PLATFORM}')")
endif()
message(STATUS "aio platform: ${AIO_PLATFORM}")

add_library(aio_platform STATIC platforms/${AIO_PLATFORM}.cpp)
target_include_directories(aio_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(aio_platform PUBLIC cxx_std_20)
