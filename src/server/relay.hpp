#pragma once

#include <string>

#include "protocol.hpp"

namespace httplib {
namespace ws {
class WebSocketClient;
}
}  // namespace httplib

namespace aio {

// Blocks, serving the relay on `bind_host`:`port`, until shutdown.
// Forwards control calls and live terminals to a joined host.
int run_relay(int port = kRelayPort, const std::string& bind_host = kRelayBind);

// Reads call frames from a joined socket and performs them on the local control
// port. Returns when the socket closes.
void answer_relay_calls(httplib::ws::WebSocketClient& ws, const std::string& control_host, int control_port);

}  // namespace aio
