#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "agents.hpp"
#include "api.hpp"
#include "control.hpp"
#include "mcp.hpp"
#include "platform.hpp"
#include "protocol.hpp"
#include "relay.hpp"
#include "tmux.hpp"
#include "ui.hpp"

namespace {

std::string sidebar_command() {
    return aio::Platform::shell_quote(aio::Platform::executable_path()) + " --sidebar";
}

void session_env_args(std::vector<std::string>& args) {
    for (const char* key : {"AIO_HOST", "AIO_PORT", "AIO_TOKEN", "AIO_TARGET"}) {
        if (auto value = aio::Platform::env(key); value && !value->empty()) {
            args.push_back("-e");
            args.push_back(std::string(key) + "=" + *value);
        }
    }
}

// Panes started later inherit this. Empty values are removed so a previous
// remote session does not keep pointing at another machine.
void publish_session_env() {
    for (const char* key : {"AIO_HOST", "AIO_PORT", "AIO_TOKEN", "AIO_TARGET"}) {
        if (auto value = aio::Platform::env(key); value && !value->empty())
            aio::tmux({"set-environment", "-t", aio::kUiSession, key, *value});
        else
            aio::tmux({"set-environment", "-u", "-t", aio::kUiSession, key});
    }
}

// The session outlives the sidebar process (remain-on-exit), so restart it if it ended.
// A remote launch always restarts it, so the pane picks up the host and token.
void revive_sidebar(bool always) {
    const std::string pane = std::string(aio::kUiSession) + ":main.0";
    if (always || aio::tmux_out({"display-message", "-p", "-t", pane, "#{pane_dead}"}) == "1")
        aio::tmux({"respawn-pane", "-k", "-t", pane, sidebar_command()});
    aio::set_sidebar_zoom(false);  // a sidebar that died with a popup open leaves the window zoomed
    aio::repair_layout();
    aio::tmux({"select-pane", "-t", pane});
}

bool create_ui_session() {
    auto size = aio::Platform::terminal_size().value_or(aio::TerminalSize{200, 50});

    using aio::tmux;
    const std::string s = aio::kUiSession;
    auto sidebar_cmd = sidebar_command();
    std::vector<std::string> args = {"new-session", "-d", "-s", s, "-n", "main", "-x", std::to_string(size.cols),
                                     "-y", std::to_string(size.rows)};
    session_env_args(args);
    args.push_back(sidebar_cmd);
    if (!tmux(std::move(args)).ok()) return false;

    // Keep exited panes (agents, servers) visible with their last output instead of vanishing.
    tmux({"set-option", "-g", "-w", "remain-on-exit", "on"});
    tmux({"set-option", "-t", s, "status", "off"});
    tmux({"set-option", "-t", s, "mouse", "on"});
    tmux({"set-option", "-w", "-t", s + ":main", "pane-border-lines", "heavy"});
    tmux({"set-option", "-w", "-t", s + ":main", "pane-active-border-style", "fg=magenta"});
    aio::repair_layout();
    tmux({"select-pane", "-t", s + ":main.0"});
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && (std::strcmp(argv[1], "--version") == 0 || std::strcmp(argv[1], "-v") == 0)) {
        std::cout << "aio " << AIO_VERSION << " (" << aio::Platform::name() << ")\n";
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "serve") == 0) {
        std::string server;
        for (int i = 2; i + 1 < argc; ++i)
            if (std::strcmp(argv[i], "--name") == 0) server = argv[i + 1];
        return aio::run_server(server);
    }
    if (argc > 1 && std::strcmp(argv[1], "--sidebar") == 0) return aio::run_sidebar();
    if (argc > 1 && std::strcmp(argv[1], "--placeholder") == 0) return aio::run_placeholder();
    if (argc > 1 && std::strcmp(argv[1], "--control") == 0) return aio::run_control(aio::kHostPort);
    if (argc > 1 && std::strcmp(argv[1], "--relay") == 0) return aio::run_relay();
    if (argc > 1 && std::strcmp(argv[1], "--join") == 0) {
        std::string relay, token;
        int port = aio::kRelayPort;
        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "--token") == 0 && i + 1 < argc) {
                token = argv[++i];
            } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
                const char* text = argv[++i];
                size_t used = 0;
                int value = 0;
                try {
                    value = std::stoi(text, &used);
                } catch (...) {
                    used = 0;
                }
                if (used != std::strlen(text) || value <= 0 || value >= 65536) {
                    std::cerr << "error: --port must be a number from 1 to 65535\n";
                    return 1;
                }
                port = value;
            } else if (argv[i][0] != '-' && relay.empty()) {
                relay = argv[i];
            } else {
                std::cerr << "usage: aio --join ADDR --token TOKEN [--port N]\n";
                return 1;
            }
        }
        if (relay.empty() || token.empty()) {
            std::cerr << "usage: aio --join ADDR --token TOKEN [--port N]\n";
            return 1;
        }
        return aio::run_join(relay, port, token);
    }
    if (argc > 1 && std::strcmp(argv[1], "attach") == 0) {
        std::string kind, name;
        for (int i = 2; i + 1 < argc; ++i) {
            if (std::strcmp(argv[i], "--agent") == 0) {
                kind = "agent";
                name = argv[++i];
            } else if (std::strcmp(argv[i], "--server") == 0) {
                kind = "server";
                name = argv[++i];
            }
        }
        if (name.empty()) {
            std::cerr << "usage: aio attach --agent <name> | --server <name>\n";
            return 1;
        }
        return aio::run_attach(kind, name);
    }

    for (int i = 1; i < argc; ++i) {
        auto needs = [&](const char* flag) {
            if (std::strcmp(argv[i], flag) != 0) return false;
            if (i + 1 >= argc) std::cerr << "error: " << flag << " needs a value\n";
            return true;
        };
        if (needs("--host")) {
            if (i + 1 >= argc) return 1;
            aio::Platform::set_env("AIO_HOST", argv[++i]);
        } else if (needs("--token")) {
            if (i + 1 >= argc) return 1;
            aio::Platform::set_env("AIO_TOKEN", argv[++i]);
        } else if (needs("--port")) {
            if (i + 1 >= argc) return 1;
            const char* port = argv[++i];
            size_t used = 0;
            int value = 0;
            try {
                value = std::stoi(port, &used);
            } catch (...) {
                used = 0;
            }
            if (used != std::strlen(port) || value <= 0 || value >= 65536) {
                std::cerr << "error: --port must be a number from 1 to 65535\n";
                return 1;
            }
            aio::Platform::set_env("AIO_PORT", port);
        } else {
            std::cerr << "usage: aio [--host ADDR --token TOKEN [--port N]]\n"
                      << "       aio --relay\n"
                      << "       aio --join ADDR --token TOKEN [--port N]\n";
            return 1;
        }
    }

    if (!aio::tmux({"-V"}).ok()) {
        std::cerr << "error: tmux not found in PATH\n";
        return 1;
    }
    if (aio::control_endpoint().remote) {
        if (!aio::control_reachable()) return 1;
    } else if (!aio::ensure_control_server()) {
        return 1;
    }
    bool remote = aio::control_endpoint().remote;
    if (aio::tmux({"has-session", "-t", aio::kUiSession}).ok()) {
        publish_session_env();
        revive_sidebar(remote);
    } else if (!create_ui_session()) {
        std::cerr << "error: failed to create tmux session\n";
        return 1;
    }

    // Inside a client of aio's own server we can switch; from any other tmux we must
    // attach as a nested client, which tmux only allows with $TMUX cleared.
    auto inside = aio::Platform::env("TMUX").value_or("");
    auto socket_path = inside.substr(0, inside.find(','));
    bool ours = !socket_path.empty() &&
                aio::Platform::join_path(aio::Platform::parent_path(socket_path), aio::tmux_socket()) == socket_path;
    if (!inside.empty() && !ours) aio::Platform::unset_env("TMUX");
    auto args = std::vector<std::string>{"tmux", "-L", aio::tmux_socket(), "attach-session", "-t", aio::kUiSession};
    if (ours) {
        args[3] = "switch-client";
        return aio::Platform::exec_process(args);
    }
    // Not exec: once the client exits we put the terminal back. A server killed while
    // a client is attached leaves the alternate screen and raw keyboard mode behind.
    return aio::Platform::run_in_terminal(args);
}
