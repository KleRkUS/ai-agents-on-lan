#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "config.hpp"
#include "protocol.hpp"

namespace aio {

// Blocks, serving the agent-host API on `bind_host`:`port`, until shutdown.
// The default bind address is loopback. The relay is what other devices reach.
int run_control(int port, const std::string& bind_host = kControlBind);

// Starts `aio --control` if nothing is already answering on the default port.
// Prints the reason and returns false when it cannot.
bool ensure_control_server();

// The LAN credential. Created on first use and stored in the config directory.
// AIO_ACCESS_TOKEN overrides it (used by tests). On the agent host this guards
// the loopback API. On the relay it guards every connection from other devices.
std::string access_token();

// True when `authorization` is "Bearer " plus the current access token.
bool authorized_header(const std::string& authorization);

nlohmann::json config_to_json(const Config& config);
// Returns an error string, or empty and writes `out`.
std::string config_from_json(const nlohmann::json& j, Config& out);

}  // namespace aio
