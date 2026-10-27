#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace aio {

// A user-created MCP server: a long-running `aio serve` process on localhost
// that its member agents connect to over HTTP to message each other.
struct ServerInfo {
    std::string name;
    int port = 0;
    std::map<std::string, std::string> members;  // agent name -> bearer token
    std::string color;                            // "#rrggbb", used to tint member agents in the UI

    std::string url() const;
    // Name of the MCP server entry inside agent CLIs, e.g. "aio-backend".
    std::string mcp_name() const { return "aio-" + name; }
    // Env var holding this server's token for agent CLIs that read it from the environment.
    std::string token_env() const;
};

inline constexpr int kFirstPort = 47800;

// Distinct, readable-on-dark colors assigned to servers in order; reused once exhausted.
inline constexpr const char* kServerPalette[] = {
    "#4fc3f7", "#ffb74d", "#ba68c8", "#81c784", "#f06292", "#fff176", "#4db6ac", "#e57373",
};

// Server names become CLI config keys and env var names: 1-24 of [a-z0-9-].
std::string validate_server_name(const std::string& name);

// Persists servers as <root>/<name>/server.json; mailboxes live in <root>/<name>/mail.
class ServerRegistry {
public:
    explicit ServerRegistry(std::string root = default_root());

    static std::string default_root();

    std::vector<ServerInfo> list() const;
    std::optional<ServerInfo> get(const std::string& name) const;
    // Returns an error message, or empty on success.
    std::string create(const std::string& name, int port);
    bool remove(const std::string& name);

    // Adds `agent` to the server, returning its new token (empty on failure).
    std::string add_member(const std::string& server, const std::string& agent);
    void remove_member_everywhere(const std::string& agent);

    std::string mail_root(const std::string& server) const;
    // Last startup error reported by `aio serve` (e.g. port in use); empty if none.
    std::string last_error(const std::string& server) const;
    void set_last_error(const std::string& server, const std::string& error) const;
    int next_free_port() const;

private:
    bool save(const ServerInfo& info) const;
    std::string dir(const std::string& name) const;

    std::string root_;
};

// --- process management (tmux) ---
bool server_running(const std::string& name);
std::string server_pane(const std::string& name);
// Starts `aio serve --name <name>` in a hidden tmux window. Returns an error or empty.
std::string start_server(const std::string& name);
void stop_server(const std::string& name);

}  // namespace aio
