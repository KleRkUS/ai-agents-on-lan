#pragma once

#include <string>

#include <httplib.h>

namespace aio {

// Streams one agent or server pane to a websocket client and applies its keystrokes.
// `kind` is "agent" or "server".
void serve_terminal(const std::string& kind, const std::string& name, httplib::ws::WebSocket& ws);

// Closes pane pipes so a shutdown does not leave readers blocked.
void stop_terminal_streams();
// Allows streaming again after a previous control process in this executable stopped it.
void start_terminal_streams();

}  // namespace aio
