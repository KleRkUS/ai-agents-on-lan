#include "servers.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <iterator>
#include <random>

#include <nlohmann/json.hpp>

#include "agents.hpp"
#include "platform.hpp"
#include "tmux.hpp"

namespace aio {

using json = nlohmann::json;

namespace {

inline constexpr const char* kServerSession = "aio-srv";

std::string make_token() {
    static std::random_device rd;
    static const char* hex = "0123456789abcdef";
    std::string token;
    for (int i = 0; i < 32; ++i) token += hex[rd() % 16];
    return token;
}

struct PaneState {
    std::string id;
    bool dead = false;
};

std::optional<PaneState> find_server_pane(const std::string& name) {
    auto out = tmux_out({"list-panes", "-a", "-F", "#{pane_id}\t#{@aio_server}\t#{pane_dead}"});
    for (const auto& line : split(out, '\n')) {
        auto f = split(line, '\t');
        if (f.size() == 3 && f[1] == name) return PaneState{f[0], f[2] == "1"};
    }
    return std::nullopt;
}

// First palette color no existing server uses; cycles when all are taken.
std::string pick_color(const std::vector<ServerInfo>& existing) {
    constexpr size_t n = std::size(kServerPalette);
    for (size_t i = 0; i < n; ++i) {
        bool used = std::any_of(existing.begin(), existing.end(),
                                [&](const ServerInfo& s) { return s.color == kServerPalette[i]; });
        if (!used) return kServerPalette[i];
    }
    return kServerPalette[existing.size() % n];
}

}  // namespace

std::string ServerInfo::url() const { return "http://127.0.0.1:" + std::to_string(port) + "/mcp"; }

std::string ServerInfo::token_env() const {
    std::string env = "AIO_TOKEN_";
    for (char c : name) env += c == '-' ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return env;
}

std::string validate_server_name(const std::string& name) {
    if (name.empty() || name.size() > 24) return "server name must be 1-24 characters";
    for (char c : name)
        if (!(std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '-'))
            return "server name may only use a-z, 0-9 and '-'";
    if (name.front() == '-' || name.back() == '-') return "server name cannot start or end with '-'";
    return {};
}

ServerRegistry::ServerRegistry(std::string root) : root_(std::move(root)) {}

std::string ServerRegistry::default_root() { return Platform::join_path(Platform::state_dir(), "servers"); }

std::string ServerRegistry::dir(const std::string& name) const { return Platform::join_path(root_, name); }

std::string ServerRegistry::mail_root(const std::string& server) const {
    return Platform::join_path(dir(server), "mail");
}

std::string ServerRegistry::last_error(const std::string& server) const {
    return Platform::read_file(Platform::join_path(dir(server), "last_error")).value_or("");
}

void ServerRegistry::set_last_error(const std::string& server, const std::string& error) const {
    auto path = Platform::join_path(dir(server), "last_error");
    if (error.empty()) Platform::remove_file(path);
    else Platform::write_file(path, error);
}

std::optional<ServerInfo> ServerRegistry::get(const std::string& name) const {
    if (!validate_server_name(name).empty()) return std::nullopt;
    auto text = Platform::read_file(Platform::join_path(dir(name), "server.json"));
    if (!text) return std::nullopt;
    try {
        auto j = json::parse(*text);
        ServerInfo info;
        info.name = name;
        info.port = j.at("port").get<int>();
        info.members = j.value("members", std::map<std::string, std::string>{});
        constexpr size_t n = std::size(kServerPalette);
        info.color = j.value("color", std::string(kServerPalette[std::hash<std::string>{}(name) % n]));
        return info;
    } catch (const json::exception&) {
        return std::nullopt;
    }
}

std::vector<ServerInfo> ServerRegistry::list() const {
    std::vector<ServerInfo> servers;
    for (const auto& name : Platform::list_directories(root_))
        if (auto info = get(name)) servers.push_back(std::move(*info));
    return servers;
}

bool ServerRegistry::save(const ServerInfo& info) const {
    auto d = dir(info.name);
    if (!Platform::create_directories(d)) return false;
    json j = {{"port", info.port}, {"members", info.members}, {"color", info.color}};
    auto path = Platform::join_path(d, "server.json");
    auto tmp = path + ".tmp";
    return Platform::write_file(tmp, j.dump(2)) && Platform::rename_file(tmp, path);
}

std::string ServerRegistry::create(const std::string& name, int port) {
    if (auto err = validate_server_name(name); !err.empty()) return err;
    if (port < 1024 || port > 65535) return "port must be 1024-65535";
    auto existing = list();
    for (const auto& s : existing) {
        if (s.name == name) return "a server named '" + name + "' already exists";
        if (s.port == port) return "port " + std::to_string(port) + " is used by '" + s.name + "'";
    }
    return save({name, port, {}, pick_color(existing)}) ? "" : "could not write server config";
}

bool ServerRegistry::remove(const std::string& name) {
    if (!validate_server_name(name).empty()) return false;
    return Platform::remove_all(dir(name));
}

std::string ServerRegistry::add_member(const std::string& server, const std::string& agent) {
    auto info = get(server);
    if (!info) return {};
    auto token = make_token();
    info->members[agent] = token;
    return save(*info) ? token : "";
}

void ServerRegistry::remove_member_everywhere(const std::string& agent) {
    for (auto info : list())
        if (info.members.erase(agent)) save(info);
}

int ServerRegistry::next_free_port() const {
    int port = kFirstPort;
    for (const auto& s : list()) port = std::max(port, s.port + 1);
    return port;
}

bool server_running(const std::string& name) {
    auto pane = find_server_pane(name);
    return pane && !pane->dead;
}

std::string server_pane(const std::string& name) {
    auto pane = find_server_pane(name);
    return pane ? pane->id : "";
}

std::string start_server(const std::string& name) {
    if (auto pane = find_server_pane(name)) {
        if (!pane->dead) return {};
        kill_agent(pane->id);
    }
    // Same as agents: the first session cannot set remain-on-exit until the server exists.
    const std::string command =
        Platform::shell_quote(Platform::executable_path()) + " serve --name " + name;
    bool fresh = !tmux({"has-session", "-t", kServerSession}).ok();
    if (!fresh) tmux({"set-option", "-g", "-w", "remain-on-exit", "on"});
    std::vector<std::string> args = fresh ? std::vector<std::string>{"new-session", "-d", "-s", kServerSession}
                                          : std::vector<std::string>{"new-window", "-d", "-t", std::string(kServerSession) + ":"};
    args.insert(args.end(), {"-P", "-F", "#{pane_id}"});
    if (!fresh) args.push_back(command);
    auto pane = tmux_out(args);
    if (pane.empty()) return "tmux failed to start the server";
    if (fresh) {
        tmux({"set-option", "-g", "-w", "remain-on-exit", "on"});
        tmux({"set-option", "-w", "-t", pane, "remain-on-exit", "on"});
        if (!tmux({"respawn-pane", "-k", "-t", pane, command}).ok()) {
            tmux({"kill-pane", "-t", pane});
            return "tmux failed to start the server";
        }
    }
    tmux({"set-option", "-p", "-t", pane, "@aio_server", name});
    return {};
}

void stop_server(const std::string& name) {
    if (auto pane = find_server_pane(name)) kill_agent(pane->id);
}

}  // namespace aio
