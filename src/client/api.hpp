#pragma once

#include <map>
#include <string>
#include <vector>

#include "adapters.hpp"
#include "agents.hpp"
#include "config.hpp"
#include "protocol.hpp"
#include "servers.hpp"

namespace aio {

// What the sidebar knows about the server. Filled by HTTP, never by tmux.
struct ControlState {
    std::vector<Agent> agents;
    std::map<std::string, size_t> unread_counts;
    std::string viewport;
    struct ServerRow {
        ServerInfo info;
        bool running = false;
        std::string error;
        size_t unread = 0;
    };
    std::vector<ServerRow> servers;
    Config config;
    AdapterSet adapters;
    int next_port = 47800;
    std::string error;  // set when the control server could not be reached
    struct Access {
        std::string token;
        int port = kControlPort;
        std::vector<std::string> addresses;
    };
    Access access;
    std::string host_id;
    std::string host_name;
};

// A machine registered with the relay. Empty id means this viewer has not chosen one.
struct JoinedHost {
    std::string id;
    std::string name;
};

// Where this process sends control requests. Environment overrides the defaults:
// AIO_HOST, AIO_PORT, AIO_TOKEN. Without those, the token is read from this machine.
struct ControlEndpoint {
    std::string host = kControlHost;
    int port = kControlPort;
    std::string token;
    // Set from AIO_TARGET when this viewer is driving one joined machine.
    std::string target;
    bool remote = false;
};

ControlEndpoint control_endpoint();
// True when the named server answers with this process's token.
bool control_reachable();

class Control {
public:
    ControlState state();
    // Joined machines, from the relay. Empty when this process is not a viewer.
    std::vector<JoinedHost> hosts();
    std::string create_agent(const std::string& name, const std::string& directory, const std::string& command,
                             const std::vector<std::string>& servers, bool manager);
    std::string delete_agent(const std::string& name);
    std::string show_agent(const std::string& name, bool focus);
    std::string stop_agent(const std::string& name);
    std::string create_server(const std::string& name, int port);
    std::string start_server(const std::string& name);
    std::string stop_server(const std::string& name);
    std::string delete_server(const std::string& name);
    std::string show_server(const std::string& name);
    std::string save_config(const Config& config);
    std::string zoom(bool on);
    void shutdown();
};

}  // namespace aio
