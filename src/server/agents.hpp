#pragma once

#include <string>
#include <vector>

namespace aio {

// Main UI session: window "main" holds pane 0 (sidebar) and pane 1 (viewport).
inline constexpr const char* kUiSession = "aio";
// Hidden session where agent panes live while they are not displayed.
inline constexpr const char* kBgSession = "aio-bg";

struct Agent {
    std::string pane_id;  // tmux pane id, e.g. "%12"; stable for the pane's lifetime
    std::string name;
    std::string dir;
    std::string command;  // what is currently running in the pane
    std::vector<std::string> servers;  // aio MCP servers the agent was launched with
    bool dead = false;                 // process exited; pane kept to show its last output
    int exit_status = 0;
    bool manager = false;             // may stop, kill and create other agents
};

std::vector<Agent> list_agents();
std::string viewport_pane();  // pane id of the right-hand UI pane
// "agent:<name>", "server:<name>", or empty when the right pane is the placeholder.
std::string viewport_target();
// Replaces the right-hand pane with `aio attach` for this agent or server.
// `kind` is "agent" or "server". `focus` moves the keyboard there.
bool show_view(const std::string& kind, const std::string& name, bool focus);
// Returns an error message, or empty on success. The agent joins each server in `servers`.
// `manager` grants the control tools. `show` opens the agent in the right-hand pane.
std::string create_agent(const std::string& name, const std::string& dir, const std::string& command,
                         const std::vector<std::string>& servers, bool manager = false, bool show = true);

// Manager controls. Empty string on success, otherwise the reason it was refused.
// A manager cannot target itself or another manager. Stop sends Ctrl+C; the agent stays.
// Kill closes the pane and removes the config aio wrote for it.
std::string manager_stop(const std::string& actor, const std::string& target);
std::string manager_kill(const std::string& actor, const std::string& target);

// Closes the pane, removes generated config, and drops server memberships.
void dismiss_agent(const Agent& agent);
std::string capture_pane(const std::string& pane_id);
// Sends Ctrl+C to the named agent's pane. Empty string on success.
std::string interrupt_agent(const std::string& name);
// Types `text` into the pane's prompt and presses Enter.
bool type_into_pane(const std::string& pane_id, const std::string& text);
bool show_agent(const std::string& pane_id, bool focus);
bool kill_agent(const std::string& pane_id);

// Binds `key` (tmux syntax) to focus the sidebar while in the UI session; elsewhere the key
// passes through unchanged. Unbinds `previous` if given and different.
void bind_detach_key(const std::string& key, const std::string& previous = {});

// Zooms the sidebar to the full window so popups have room.
void set_sidebar_zoom(bool zoomed);

// Restores the two-pane layout if the displayed agent exited.
void repair_layout();

}  // namespace aio
