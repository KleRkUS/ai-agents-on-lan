#pragma once

#include <string>

namespace aio {

// Appends a timestamped line to <state_dir>/aio.log (rotated to aio.log.1 past 1 MB).
void log_event(const std::string& message);

std::string log_path();

}  // namespace aio
