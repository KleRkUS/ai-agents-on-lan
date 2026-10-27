#pragma once

#include <string>
#include <vector>

#include "platform.hpp"

namespace aio {

// aio runs its own tmux server (`tmux -L <socket>`) so it never shares sessions
// with the user's other tmux use. Overridable with AIO_TMUX_SOCKET.
std::string tmux_socket();

// Runs `tmux -L <socket> <args...>`.
ProcessResult tmux(std::vector<std::string> args);

// Like tmux(), but returns trimmed stdout (empty on failure).
std::string tmux_out(std::vector<std::string> args);

std::vector<std::string> split(const std::string& s, char sep);

}  // namespace aio
