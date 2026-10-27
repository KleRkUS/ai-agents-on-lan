#include "agents.hpp"

#include <algorithm>

#include "adapters.hpp"
#include "config.hpp"
#include "log.hpp"
#include "mcp.hpp"
#include "platform.hpp"
#include "servers.hpp"
#include "tmux.hpp"

namespace aio {
namespace {

const std::string kMain = std::string(kUiSession) + ":main";
const std::string kViewport = kMain + ".1";

int pane_number(const std::string& id) {
    return id.size() > 1 ? std::stoi(id.substr(1)) : 0;
}

std::string find_placeholder() {
    auto out = tmux_out({"list-panes", "-a", "-F", "#{pane_id}\t#{@aio_placeholder}"});
    for (const auto& line : split(out, '\n')) {
        auto f = split(line, '\t');
        if (f.size() == 2 && f[1] == "1") return f[0];
    }
    return {};
}

}  // namespace

std::vector<Agent> list_agents() {
    std::vector<Agent> agents;
    auto out = tmux_out({"list-panes", "-a", "-F",
                         "#{pane_id}\t#{@aio_agent}\t#{@aio_dir}\t#{pane_current_command}\t"
                         "#{pane_dead}\t#{pane_dead_status}\t#{@aio_role}\t#{@aio_servers}"});
    for (const auto& line : split(out, '\n')) {
        auto f = split(line, '\t');
        if (f.size() < 6 || f[1].empty()) continue;
        Agent a{f[0], f[1], f[2], f[3], {}, f[4] == "1", 0, false};
        try {
            if (!f[5].empty()) a.exit_status = std::stoi(f[5]);
        } catch (...) {
        }
        if (f.size() > 6) a.manager = f[6] == "manager";
        if (f.size() > 7)
            for (const auto& s : split(f[7], ','))
                if (!s.empty()) a.servers.push_back(s);
        agents.push_back(std::move(a));
    }
    std::sort(agents.begin(), agents.end(), [](const Agent& a, const Agent& b) {
        return pane_number(a.pane_id) < pane_number(b.pane_id);
    });
    return agents;
}

std::string viewport_pane() {
    return tmux_out({"display-message", "-p", "-t", kViewport, "#{pane_id}"});
}

std::string viewport_target() {
    return tmux_out({"display-message", "-p", "-t", kViewport, "#{@aio_view}"});
}

bool show_view(const std::string& kind, const std::string& name, bool focus) {
    if ((kind != "agent" && kind != "server") || name.empty()) return false;
    repair_layout();
    if (tmux_out({"display-message", "-p", "-t", kViewport, "#{pane_id}"}).empty()) return false;

    auto target = kind + ":" + name;
    bool dead = tmux_out({"display-message", "-p", "-t", kViewport, "#{pane_dead}"}) == "1";
    bool same = tmux_out({"display-message", "-p", "-t", kViewport, "#{@aio_view}"}) == target;
    if (!same || dead) {
        // The right pane is only a window. The agent process stays in its own session;
        // this process copies that terminal over the websocket.
        auto cmd = Platform::shell_quote(Platform::executable_path()) + " attach --" + kind + " " +
                   Platform::shell_quote(name);
        std::vector<std::string> args = {"respawn-pane", "-k", "-t", kViewport};
        for (const char* key : {"AIO_HOST", "AIO_PORT", "AIO_TOKEN", "AIO_TARGET"}) {
            if (auto value = Platform::env(key); value && !value->empty()) {
                args.push_back("-e");
                args.push_back(std::string(key) + "=" + *value);
            }
        }
        args.push_back(cmd);
        if (!tmux(std::move(args)).ok()) return false;
        tmux({"set-option", "-p", "-t", kViewport, "@aio_view", target});
        tmux({"set-option", "-p", "-u", "-t", kViewport, "@aio_placeholder"});
    }
    if (focus) tmux({"select-pane", "-t", kViewport});
    return true;
}

std::string create_agent(const std::string& name, const std::string& dir, const std::string& command,
                         const std::vector<std::string>& servers, bool manager, bool show) {
    if (auto err = validate_agent_name(name); !err.empty()) return err;
    for (const auto& a : list_agents())
        if (a.name == name) return "an agent named '" + name + "' already exists";
    auto adapters = load_adapters();
    if (!servers.empty() && !find_adapter(adapters, command))
        return "no adapter for '" + command_word(command) + "' — add one in " + adapters.dir;

    ServerRegistry registry;
    std::vector<ServerMembership> memberships;
    for (const auto& s : servers) {
        auto info = registry.get(s);
        if (!info) return "server '" + s + "' no longer exists";
        auto token = registry.add_member(s, name);
        if (token.empty()) return "could not add agent to server '" + s + "'";
        memberships.push_back({*info, token});
    }
    auto wiring = wire_messaging(adapters, name, command, dir, memberships, agent_state_dir());
    if (!wiring.error.empty()) {
        remove_agent_traces(name);
        registry.remove_member_everywhere(name);
        log_event("agent '" + name + "': " + wiring.error);
        return wiring.error;
    }

    // tmux drops set-option when its server is not running, so the first session is opened on a
    // shell and the real command starts only after remain-on-exit is on. Otherwise a command
    // that exits immediately (or is interrupted) takes the pane with it.
    bool fresh = !tmux({"has-session", "-t", kBgSession}).ok();
    if (!fresh) tmux({"set-option", "-g", "-w", "remain-on-exit", "on"});

    std::vector<std::string> args = fresh ? std::vector<std::string>{"new-session", "-d", "-s", kBgSession}
                                          : std::vector<std::string>{"new-window", "-d", "-t", std::string(kBgSession) + ":"};
    args.insert(args.end(), {"-P", "-F", "#{pane_id}", "-c", dir, "-e", "AIO_AGENT=" + name});
    for (const auto& [key, value] : wiring.env) args.insert(args.end(), {"-e", key + "=" + value});
    if (!fresh && !wiring.command.empty()) args.push_back(wiring.command);

    auto pane = tmux_out(args);
    if (pane.empty()) {
        remove_agent_traces(name);
        registry.remove_member_everywhere(name);
        log_event("agent '" + name + "': tmux failed to start `" + wiring.command + "` in " + dir);
        return "tmux failed to start the agent";
    }
    if (fresh) {
        tmux({"set-option", "-g", "-w", "remain-on-exit", "on"});
        tmux({"set-option", "-w", "-t", pane, "remain-on-exit", "on"});
        std::vector<std::string> respawn = {"respawn-pane", "-k", "-t", pane, "-c", dir, "-e", "AIO_AGENT=" + name};
        for (const auto& [key, value] : wiring.env) respawn.insert(respawn.end(), {"-e", key + "=" + value});
        if (!wiring.command.empty()) respawn.push_back(wiring.command);
        if (!tmux(std::move(respawn)).ok()) {
            tmux({"kill-pane", "-t", pane});
            remove_agent_traces(name);
            registry.remove_member_everywhere(name);
            log_event("agent '" + name + "': tmux failed to start `" + wiring.command + "` in " + dir);
            return "tmux failed to start the agent";
        }
    }
    log_event("agent '" + name + "' started in " + dir + ": " + command + (manager ? " (manager)" : "") +
              (servers.empty() ? "" : " (servers: " + std::to_string(servers.size()) + ")"));
    if (wiring.wired) {
        std::string joined;
        for (const auto& s : servers) joined += (joined.empty() ? "" : ",") + s;
        tmux({"set-option", "-p", "-t", pane, "@aio_servers", joined});
    }
    tmux({"set-option", "-p", "-t", pane, "@aio_agent", name});
    tmux({"set-option", "-p", "-t", pane, "@aio_dir", dir});
    if (manager) tmux({"set-option", "-p", "-t", pane, "@aio_role", "manager"});
    if (show) show_agent(pane, false);
    return {};
}

namespace {

const Agent* find_named(const std::vector<Agent>& agents, const std::string& name) {
    for (const auto& a : agents)
        if (a.name == name) return &a;
    return nullptr;
}

// Shared refusal rules for stop and kill. `verb` is "stop" or "kill".
// `agents` must outlive the returned pointers.
std::string authorize(const std::vector<Agent>& agents, const std::string& actor, const std::string& target,
                      const char* verb, const Agent*& self, const Agent*& other) {
    if (actor == target) return std::string("a manager cannot ") + verb + " itself";
    self = find_named(agents, actor);
    other = find_named(agents, target);
    if (!self || !self->manager) return std::string("only managers can ") + verb + " agents";
    if (!other) return "no agent named '" + target + "'";
    if (other->manager) return std::string("a manager cannot ") + verb + " another manager";
    return {};
}

}  // namespace

std::string manager_stop(const std::string& actor, const std::string& target) {
    auto agents = list_agents();
    const Agent* self = nullptr;
    const Agent* other = nullptr;
    if (auto err = authorize(agents, actor, target, "stop", self, other); !err.empty()) return err;
    if (other->dead) return "'" + target + "' has already exited";
    if (!tmux({"send-keys", "-t", other->pane_id, "C-c"}).ok()) return "could not reach " + target;
    log_event("manager '" + actor + "' stopped '" + target + "'");
    return {};
}

std::string manager_kill(const std::string& actor, const std::string& target) {
    auto agents = list_agents();
    const Agent* self = nullptr;
    const Agent* other = nullptr;
    if (auto err = authorize(agents, actor, target, "kill", self, other); !err.empty()) return err;
    log_event("manager '" + actor + "' killed '" + target + "'");
    dismiss_agent(*other);
    return {};
}

void dismiss_agent(const Agent& agent) {
    kill_agent(agent.pane_id);
    remove_agent_traces(agent.name);
    ServerRegistry{}.remove_member_everywhere(agent.name);
}

std::string capture_pane(const std::string& pane_id) {
    return tmux_out({"capture-pane", "-p", "-t", pane_id});
}

std::string interrupt_agent(const std::string& name) {
    for (const auto& a : list_agents()) {
        if (a.name != name) continue;
        if (a.dead) return "'" + name + "' has already exited";
        if (!tmux({"send-keys", "-t", a.pane_id, "C-c"}).ok()) return "could not reach " + name;
        log_event("interrupted agent '" + name + "'");
        return {};
    }
    return "no agent named '" + name + "'";
}

bool type_into_pane(const std::string& pane_id, const std::string& text) {
    if (!tmux({"send-keys", "-t", pane_id, "-l", text}).ok()) return false;
    return tmux({"send-keys", "-t", pane_id, "Enter"}).ok();
}

bool show_agent(const std::string& pane_id, bool focus) {
    for (const auto& agent : list_agents())
        if (agent.pane_id == pane_id) return show_view("agent", agent.name, focus);
    return false;
}

bool kill_agent(const std::string& pane_id) {
    if (viewport_pane() == pane_id) {
        auto ph = find_placeholder();
        if (!ph.empty()) tmux({"swap-pane", "-d", "-s", ph, "-t", pane_id});
    }
    bool ok = tmux({"kill-pane", "-t", pane_id}).ok();
    repair_layout();
    return ok;
}

void bind_detach_key(const std::string& key, const std::string& previous) {
    if (!previous.empty() && previous != key) tmux({"unbind-key", "-n", previous});
    tmux({"bind-key", "-n", key, "if-shell", "-F",
          "#{==:#{session_name}," + std::string(kUiSession) + "}", "select-pane -t " + kMain + ".0",
          "send-keys " + key});
}

void set_sidebar_zoom(bool zoomed) {
    // Checked and toggled inside tmux in one command, so concurrent callers cannot race.
    auto toggle = "resize-pane -Z -t " + kMain + ".0";
    if (zoomed)
        tmux({"if-shell", "-F", "-t", kMain, "#{window_zoomed_flag}", "", toggle});
    else
        tmux({"if-shell", "-F", "-t", kMain, "#{window_zoomed_flag}", toggle});
}

void repair_layout() {
    auto count = tmux_out({"display-message", "-p", "-t", kMain, "#{window_panes}"});
    if (count != "1") return;

    auto ph = find_placeholder();
    if (!ph.empty()) {
        tmux({"join-pane", "-d", "-h", "-l", "75%", "-s", ph, "-t", kMain + ".0"});
        return;
    }
    auto pane = tmux_out({"split-window", "-d", "-h", "-l", "75%", "-t", kMain + ".0", "-P", "-F",
                          "#{pane_id}", Platform::shell_quote(Platform::executable_path()) + " --placeholder"});
    if (!pane.empty()) tmux({"set-option", "-p", "-t", pane, "@aio_placeholder", "1"});
}

}  // namespace aio
