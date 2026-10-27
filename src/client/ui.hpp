#pragma once

#include <string>

namespace aio {

int run_sidebar();
int run_placeholder();
// Draws one agent or MCP server terminal from the control server's websocket.
int run_attach(const std::string& kind, const std::string& name);
// Holds a registration open with a relay. Agents keep running on this machine.
int run_join(const std::string& relay_host, int port, const std::string& token);

}  // namespace aio
