# Shared setup for cross-compiling with `zig c++` from a Linux/macOS host.
# Included by the zig-<os>-<arch>.cmake toolchain files, which set ZIG_TARGET
# and CMAKE_SYSTEM_NAME first. Requires `zig` in PATH (or ZIG_EXECUTABLE).

find_program(ZIG_EXECUTABLE zig REQUIRED)

set(_zig_wrappers "${CMAKE_BINARY_DIR}/zig-wrappers")
file(MAKE_DIRECTORY "${_zig_wrappers}")

foreach(tool cc c++ ar ranlib)
  if(tool STREQUAL "cc" OR tool STREQUAL "c++")
    set(_args "${tool} -target ${ZIG_TARGET}")
  else()
    set(_args "${tool}")
  endif()
  file(WRITE "${_zig_wrappers}/zig-${tool}" "#!/bin/sh\nexec \"${ZIG_EXECUTABLE}\" ${_args} \"$@\"\n")
  file(CHMOD "${_zig_wrappers}/zig-${tool}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
endforeach()

set(CMAKE_C_COMPILER "${_zig_wrappers}/zig-cc")
set(CMAKE_CXX_COMPILER "${_zig_wrappers}/zig-c++")
set(CMAKE_AR "${_zig_wrappers}/zig-ar" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${_zig_wrappers}/zig-ranlib" CACHE FILEPATH "" FORCE)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
