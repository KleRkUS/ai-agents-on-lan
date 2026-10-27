#include "control.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <thread>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <httplib.h>

#include "adapters.hpp"
#include "agents.hpp"
#include "host.hpp"
#include "log.hpp"
#include "mailbox.hpp"
#include "platform.hpp"
#include "protocol.hpp"
#include "servers.hpp"
#include "terminal.hpp"
#include "tmux.hpp"

namespace aio {
namespace {

std::mutex g_mu;
std::atomic<bool> g_running{true};

nlohmann::json agent_json(const Agent& a, size_t unread) {
    return {{"name", a.name},
            {"dir", a.dir},
            {"command", a.command},
            {"pane_id", a.pane_id},
            {"dead", a.dead},
            {"exit_status", a.exit_status},
            {"manager", a.manager},
            {"servers", a.servers},
            {"unread", unread}};
}

std::map<std::string, size_t> unread_by_agent(const std::vector<Agent>& agents, const ServerRegistry& registry,
                                              const MessagingConfig& messaging,
                                              std::map<std::string, size_t>& by_server) {
    std::map<std::string, size_t> counts;
    by_server.clear();
    if (!messaging.enabled) return counts;
    for (const auto& a : agents) {
        size_t n = 0;
        for (const auto& s : a.servers) {
            auto mail = Mailbox(registry.mail_root(s), messaging).unread(a.name);
            by_server[s] += mail.size();
            n += mail.size();
        }
        counts[a.name] = n;
    }
    return counts;
}

// --- housekeeping: exits, briefs, and idle delivery. Runs only in this process. ---

std::map<std::string, bool> g_reported_dead;
struct Activity {
    size_t hash = 0;
    int64_t changed_at = 0;
};
std::map<std::string, Activity> g_activity;
std::map<std::string, std::string> g_notified;
std::map<std::string, bool> g_briefed;

void log_exits(const std::vector<Agent>& agents) {
    for (const auto& a : agents) {
        if (!a.dead || g_reported_dead[a.pane_id]) continue;
        g_reported_dead[a.pane_id] = true;
        std::vector<std::string> tail;
        for (const auto& line : split(capture_pane(a.pane_id), '\n'))
            if (line.find_first_not_of(' ') != std::string::npos) tail.push_back(line);
        if (tail.size() > 8) tail.erase(tail.begin(), tail.end() - 8);
        std::string output;
        for (const auto& line : tail) output += "\n    | " + line;
        log_event("agent '" + a.name + "' exited with status " + std::to_string(a.exit_status) +
                  "; last output:" + (output.empty() ? " (none)" : output));
    }
}

void deliver() {
    auto config = load_config();
    const auto& m = config.messaging;
    if (!m.enabled) return;
    auto agents = list_agents();
    ServerRegistry registry;
    int64_t now = Mailbox::system_clock();

    for (const auto& a : agents) {
        std::vector<Message> unread;
        for (const auto& s : a.servers) {
            auto mail = Mailbox(registry.mail_root(s), m).unread(a.name);
            unread.insert(unread.end(), mail.begin(), mail.end());
        }
        auto brief_path = Platform::join_path(agent_state_dir(), a.name + ".briefed");
        if (!g_briefed.count(a.name)) g_briefed[a.name] = Platform::read_file(brief_path).has_value();
        bool pending_brief = !a.servers.empty() && !g_briefed[a.name];
        bool pending_mail = false;
        if (!unread.empty() && m.auto_deliver) {
            std::sort(unread.begin(), unread.end(),
                      [](const Message& x, const Message& y) { return x.id < y.id; });
            pending_mail = g_notified[a.name] != unread.back().id;
        }
        if (!pending_brief && !pending_mail) continue;
        if (a.dead) continue;

        auto& act = g_activity[a.pane_id];
        auto hash = std::hash<std::string>{}(capture_pane(a.pane_id));
        if (hash != act.hash) {
            act = {hash, now};
            continue;
        }
        if (now - act.changed_at < m.idle_seconds * 1000LL) continue;

        auto one_line = [](std::string s) {
            for (char& c : s)
                if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            return s;
        };
        std::string text;
        if (pending_brief) {
            std::string servers, peers;
            for (const auto& s : a.servers) servers += (servers.empty() ? "" : ", ") + s;
            for (const auto& other : agents) {
                if (other.name == a.name) continue;
                bool shared = std::any_of(a.servers.begin(), a.servers.end(), [&](const std::string& s) {
                    return std::find(other.servers.begin(), other.servers.end(), s) != other.servers.end();
                });
                if (shared) peers += (peers.empty() ? "" : ", ") + other.name;
            }
            text = "[aio] You are " + a.name + " on server " + servers + ". Other agents: " +
                   (peers.empty() ? "none yet" : peers) +
                   ". To message them, call send_message once with \"to\" set to one name or a list of names; "
                   "every recipient sees you as the sender and sees the whole recipient list. Messages for you "
                   "are typed here only after you go idle, labeled From and To: you. Nothing is typed while you "
                   "are working, and you do not need to call read_messages unless a typed message was cut off." +
                   std::string(a.manager ? " You are a manager: stop_agent cancels another agent's current turn, "
                                           "kill_agent removes one, and create_agent starts a worker. You cannot "
                                           "target yourself or another manager."
                                         : "");
        }
        if (pending_mail) {
            if (!text.empty()) text += " ";
            text += "[aio] Delivered now because you are idle. Finish anything still in progress before acting. "
                    "These are for you (" +
                    a.name + ") only.";
            for (const auto& msg : unread) text += " " + one_line(format_message(a.name, msg));
        }
        if (!type_into_pane(a.pane_id, text)) continue;
        if (pending_brief) {
            g_briefed[a.name] = true;
            Platform::create_directories(agent_state_dir());
            Platform::write_file(brief_path, "1\n");
        }
        if (pending_mail) g_notified[a.name] = unread.back().id;
        act.changed_at = now;
    }
}

void startup_housekeeping() {
    ServerRegistry registry;
    auto agents = list_agents();
    for (const auto& info : registry.list()) start_server(info.name);
    for (const auto& traced : traced_agents()) {
        bool alive = std::any_of(agents.begin(), agents.end(), [&](const Agent& a) { return a.name == traced; });
        if (!alive) {
            log_event("removing leftover config of agent '" + traced + "'");
            remove_agent_traces(traced);
            registry.remove_member_everywhere(traced);
        }
    }
    bind_detach_key(load_config().key_detach);
}

void tick() {
    repair_layout();
    log_exits(list_agents());
    deliver();
}

nlohmann::json state_json() {
    repair_layout();
    auto config = load_config();
    ServerRegistry registry;
    auto agents = list_agents();
    std::map<std::string, size_t> by_server;
    auto unread = unread_by_agent(agents, registry, config.messaging, by_server);

    nlohmann::json agent_list = nlohmann::json::array();
    for (const auto& a : agents) agent_list.push_back(agent_json(a, unread[a.name]));

    nlohmann::json server_list = nlohmann::json::array();
    for (auto& info : registry.list()) {
        bool running = server_running(info.name);
        nlohmann::json members = nlohmann::json::array();
        for (const auto& [name, _] : info.members) members.push_back(name);
        server_list.push_back({{"name", info.name},
                               {"port", info.port},
                               {"color", info.color},
                               {"running", running},
                               {"error", running ? "" : registry.last_error(info.name)},
                               {"unread", by_server[info.name]},
                               {"members", members}});
    }

    auto adapters = load_adapters();
    nlohmann::json adapter_list = nlohmann::json::array();
    for (const auto& a : adapters.adapters) adapter_list.push_back({{"name", a.name}, {"match", a.match}});

    auto ident = host_identity();
    nlohmann::json addresses = nlohmann::json::array();
    for (const auto& ip : Platform::lan_addresses()) addresses.push_back(ip);
    return {{"viewport", viewport_target()},
            {"host", {{"id", ident.id}, {"name", ident.name}}},
            {"access", {{"token", access_token()}, {"port", kControlPort}, {"addresses", addresses}}},
            {"next_port", registry.next_free_port()},
            {"agents", agent_list},
            {"servers", server_list},
            {"config", config_to_json(config)},
            {"adapters", {{"dir", adapters.dir}, {"adapters", adapter_list}, {"errors", adapters.errors}}},
            {"log_path", log_path()}};
}

std::string body_error(const httplib::Request& req, nlohmann::json& out) {
    out = nlohmann::json::parse(req.body, nullptr, false);
    if (out.is_discarded() || !out.is_object()) return "expected a JSON object";
    return {};
}

void reply_error(httplib::Response& res, int status, const std::string& error) {
    res.status = status;
    res.set_content(nlohmann::json{{"error", error}}.dump(), "application/json");
}

void reply_ok(httplib::Response& res) {
    res.set_content(nlohmann::json{{"error", ""}}.dump(), "application/json");
}

const Agent* find_agent(const std::vector<Agent>& agents, const std::string& name) {
    for (const auto& a : agents)
        if (a.name == name) return &a;
    return nullptr;
}

void shutdown_sessions() {
    stop_terminal_streams();
    log_event("quit: stopping agents and servers");
    for (const auto& a : list_agents()) dismiss_agent(a);
    log_event("aio quit");
    tmux({"detach-client", "-s", std::string(kUiSession), "-a"});
    for (int i = 0; i < 50 && !tmux_out({"list-clients", "-t", kUiSession}).empty(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    tmux({"kill-server"});
}

}  // namespace

nlohmann::json config_to_json(const Config& config) {
    nlohmann::json presets = nlohmann::json::array();
    for (const auto& p : config.presets) presets.push_back({{"name", p.name}, {"command", p.command}});
    const auto& m = config.messaging;
    return {{"default_command", config.default_command},
            {"key_detach", config.key_detach},
            {"key_kill", config.key_kill},
            {"presets", presets},
            {"messaging",
             {{"enabled", m.enabled},
              {"auto_deliver", m.auto_deliver},
              {"max_message_chars", m.max_message_chars},
              {"max_messages_per_minute", m.max_messages_per_minute},
              {"max_hops", m.max_hops},
              {"max_inbox", m.max_inbox},
              {"idle_seconds", m.idle_seconds}}}};
}

std::string config_from_json(const nlohmann::json& j, Config& out) {
    if (!j.is_object()) return "config must be an object";
    Config cfg = out;
    if (j.contains("default_command") && j["default_command"].is_string())
        cfg.default_command = j["default_command"];
    if (j.contains("key_detach") && j["key_detach"].is_string()) cfg.key_detach = j["key_detach"];
    if (j.contains("key_kill") && j["key_kill"].is_string()) cfg.key_kill = j["key_kill"];
    if (j.contains("presets")) {
        if (!j["presets"].is_array()) return "presets must be a list";
        cfg.presets.clear();
        for (const auto& p : j["presets"]) {
            if (!p.is_object() || !p.contains("name") || !p["name"].is_string() || !p.contains("command") ||
                !p["command"].is_string())
                return "each preset needs a name and a command";
            Preset preset{p["name"], p["command"]};
            if (auto err = validate_preset(preset); !err.empty()) return err;
            cfg.presets.push_back(preset);
        }
    }
    if (j.contains("messaging")) {
        const auto& m = j["messaging"];
        if (!m.is_object()) return "messaging must be an object";
        if (m.contains("enabled") && m["enabled"].is_boolean()) cfg.messaging.enabled = m["enabled"];
        if (m.contains("auto_deliver") && m["auto_deliver"].is_boolean())
            cfg.messaging.auto_deliver = m["auto_deliver"];
        for (auto [key, dest] : {std::pair{"max_message_chars", &cfg.messaging.max_message_chars},
                                 {"max_messages_per_minute", &cfg.messaging.max_messages_per_minute},
                                 {"max_hops", &cfg.messaging.max_hops},
                                 {"max_inbox", &cfg.messaging.max_inbox},
                                 {"idle_seconds", &cfg.messaging.idle_seconds}}) {
            if (!m.contains(key)) continue;
            if (!m[key].is_number_integer()) return std::string(key) + " must be a number";
            *dest = m[key];
        }
        if (auto err = validate_messaging(cfg.messaging); !err.empty()) return err;
    }
    if (auto err = validate_keys(cfg); !err.empty()) return err;
    out = cfg;
    return {};
}

namespace {

std::string trim_copy(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    size_t start = 0;
    while (start < text.size() && (text[start] == ' ' || text[start] == '\n' || text[start] == '\r')) ++start;
    return text.substr(start);
}

std::string make_token() {
    std::random_device device;
    unsigned char bytes[32];
    for (unsigned char& byte : bytes) byte = static_cast<unsigned char>(device());
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(64, '0');
    for (int i = 0; i < 32; ++i) {
        out[i * 2] = kHex[bytes[i] >> 4];
        out[i * 2 + 1] = kHex[bytes[i] & 0xf];
    }
    return out;
}

bool bearer_matches(const std::string& header, const std::string& token) {
    constexpr const char* kPrefix = "Bearer ";
    constexpr size_t kPrefixLen = 7;
    if (token.empty() || header.size() != kPrefixLen + token.size()) return false;
    if (header.compare(0, kPrefixLen, kPrefix) != 0) return false;
    unsigned diff = 0;
    for (size_t i = 0; i < token.size(); ++i)
        diff |= static_cast<unsigned char>(header[kPrefixLen + i]) ^ static_cast<unsigned char>(token[i]);
    return diff == 0;
}

bool authorized(const httplib::Request& req) {
    return authorized_header(req.get_header_value("Authorization"));
}

bool from_loopback(const httplib::Request& req) {
    // The join process calls this API from the same machine. Those calls are a
    // remote user, so they must not move this machine's panes or shut it down.
    if (req.get_header_value("X-Aio-Via") == "relay") return false;
    return req.remote_addr == "127.0.0.1" || req.remote_addr == "::1";
}

}  // namespace

std::string access_token() {
    if (auto env = Platform::env("AIO_ACCESS_TOKEN")) {
        auto token = trim_copy(*env);
        if (!token.empty()) return token;
    }
    auto path = Platform::join_path(Platform::config_dir(), "access.token");
    if (auto existing = Platform::read_file(path)) {
        auto token = trim_copy(*existing);
        if (!token.empty()) return token;
    }
    auto token = make_token();
    Platform::create_directories(Platform::config_dir());
    if (!Platform::write_file(path, token + "\n")) return token;
#ifndef _WIN32
    ::chmod(path.c_str(), 0600);
#endif
    return token;
}

bool authorized_header(const std::string& authorization) {
    return bearer_matches(authorization, access_token());
}

int run_control(int port, const std::string& bind_host) {
    g_running = true;
    start_terminal_streams();
    {
        std::lock_guard<std::mutex> lock(g_mu);
        startup_housekeeping();
    }
    std::thread ticker([] {
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!g_running) break;
            std::lock_guard<std::mutex> lock(g_mu);
            tick();
        }
    });

    httplib::Server http;
    http.set_pre_request_handler([](const httplib::Request& req, httplib::Response& res) {
        if (authorized(req)) return httplib::Server::HandlerResponse::Unhandled;
        res.status = 401;
        res.set_content(nlohmann::json{{"error", "unauthorized"}}.dump(), "application/json");
        return httplib::Server::HandlerResponse::Handled;
    });

    http.Get("/v1/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(nlohmann::json{{"ok", true}, {"protocol", kProtocolVersion}}.dump(), "application/json");
    });
    http.Get("/v1/state", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(g_mu);
        res.set_content(state_json().dump(), "application/json");
    });
    http.Get("/v1/log", [](const httplib::Request&, httplib::Response& res) {
        auto text = Platform::read_file(log_path()).value_or("");
        auto cut = text.size() > 8000 ? text.size() - 8000 : 0;
        res.set_content(nlohmann::json{{"path", log_path()}, {"text", text.substr(cut)}}.dump(), "application/json");
    });
    http.Put("/v1/config", [](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json body;
        if (auto err = body_error(req, body); !err.empty()) return reply_error(res, 400, err);
        std::lock_guard<std::mutex> lock(g_mu);
        Config cfg = load_config();
        auto previous = cfg.key_detach;
        if (auto err = config_from_json(body, cfg); !err.empty()) return reply_error(res, 400, err);
        if (!save_config(cfg)) return reply_error(res, 500, "could not write " + config_path());
        bind_detach_key(cfg.key_detach, previous);
        reply_ok(res);
    });
    http.Post("/v1/agents", [](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json body;
        if (auto err = body_error(req, body); !err.empty()) return reply_error(res, 400, err);
        if (!body.contains("name") || !body["name"].is_string() || !body.contains("directory") ||
            !body["directory"].is_string() || !body.contains("command") || !body["command"].is_string())
            return reply_error(res, 400, "name, directory and command are required");
        auto name = body["name"].get<std::string>();
        auto directory = body["directory"].get<std::string>();
        auto command = body["command"].get<std::string>();
        std::vector<std::string> servers;
        if (body.contains("servers") && body["servers"].is_array())
            for (const auto& s : body["servers"])
                if (s.is_string()) servers.push_back(s);
        bool manager = body.contains("manager") && body["manager"].is_boolean() && body["manager"].get<bool>();
        bool show = from_loopback(req);
        std::lock_guard<std::mutex> lock(g_mu);
        if (auto err = create_agent(name, directory, command, servers, manager, show); !err.empty())
            return reply_error(res, 400, err);
        log_event("agent '" + name + "' created from the client");
        reply_ok(res);
    });
    http.Delete(R"(/v1/agents/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        auto name = req.matches[1].str();
        std::lock_guard<std::mutex> lock(g_mu);
        auto agents = list_agents();
        auto agent = find_agent(agents, name);
        if (!agent) return reply_error(res, 404, "no agent named '" + name + "'");
        log_event("agent '" + name + "' removed by user");
        dismiss_agent(*agent);
        reply_ok(res);
    });
    http.Post(R"(/v1/agents/([^/]+)/show)", [](const httplib::Request& req, httplib::Response& res) {
        auto name = req.matches[1].str();
        bool focus = true;
        if (!req.body.empty()) {
            auto body = nlohmann::json::parse(req.body, nullptr, false);
            if (body.is_object() && body.contains("focus") && body["focus"].is_boolean()) focus = body["focus"];
        }
        std::lock_guard<std::mutex> lock(g_mu);
        auto agents = list_agents();
        auto agent = find_agent(agents, name);
        if (!agent) return reply_error(res, 404, "no agent named '" + name + "'");
        // A client on another device draws the terminal itself. Only the machine
        // running the server has the local sidebar split to update.
        if (from_loopback(req) && !show_view("agent", name, focus))
            return reply_error(res, 500, "could not show " + name);
        reply_ok(res);
    });
    http.Post(R"(/v1/agents/([^/]+)/stop)", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(g_mu);
        if (auto err = interrupt_agent(req.matches[1].str()); !err.empty()) return reply_error(res, 400, err);
        reply_ok(res);
    });
    http.Post("/v1/servers", [](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json body;
        if (auto err = body_error(req, body); !err.empty()) return reply_error(res, 400, err);
        if (!body.contains("name") || !body["name"].is_string() || !body.contains("port") ||
            !body["port"].is_number_integer())
            return reply_error(res, 400, "name and port are required");
        auto name = body["name"].get<std::string>();
        int port = body["port"].get<int>();
        std::lock_guard<std::mutex> lock(g_mu);
        ServerRegistry registry;
        if (auto err = registry.create(name, port); !err.empty()) return reply_error(res, 400, err);
        if (auto err = start_server(name); !err.empty()) return reply_error(res, 500, err);
        reply_ok(res);
    });
    http.Post(R"(/v1/servers/([^/]+)/start)", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(g_mu);
        if (auto err = start_server(req.matches[1].str()); !err.empty()) return reply_error(res, 400, err);
        reply_ok(res);
    });
    http.Post(R"(/v1/servers/([^/]+)/stop)", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(g_mu);
        stop_server(req.matches[1].str());
        reply_ok(res);
    });
    http.Post(R"(/v1/servers/([^/]+)/show)", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(g_mu);
        auto pane = server_pane(req.matches[1].str());
        if (pane.empty()) return reply_error(res, 404, "server is not running");
        if (from_loopback(req) && !show_view("server", req.matches[1].str(), false))
            return reply_error(res, 500, "could not show server logs");
        reply_ok(res);
    });
    http.Delete(R"(/v1/servers/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        auto name = req.matches[1].str();
        std::lock_guard<std::mutex> lock(g_mu);
        stop_server(name);
        ServerRegistry registry;
        if (!registry.remove(name)) return reply_error(res, 404, "no server named '" + name + "'");
        reply_ok(res);
    });
    http.Post("/v1/layout/zoom", [](const httplib::Request& req, httplib::Response& res) {
        auto body = nlohmann::json::parse(req.body, nullptr, false);
        if (!body.is_object() || !body.contains("on") || !body["on"].is_boolean())
            return reply_error(res, 400, "expected {\"on\": true|false}");
        std::lock_guard<std::mutex> lock(g_mu);
        if (from_loopback(req)) set_sidebar_zoom(body["on"]);
        reply_ok(res);
    });
    http.WebSocket(R"(/v1/terminal/agents/([^/]+))", [](const httplib::Request& req, httplib::ws::WebSocket& ws) {
        serve_terminal("agent", req.matches[1].str(), ws);
    });
    http.WebSocket(R"(/v1/terminal/servers/([^/]+))", [](const httplib::Request& req, httplib::ws::WebSocket& ws) {
        serve_terminal("server", req.matches[1].str(), ws);
    });
    http.Post("/v1/shutdown", [&http](const httplib::Request& req, httplib::Response& res) {
        if (!from_loopback(req)) return reply_error(res, 403, "shutdown is only available on the server machine");
        {
            std::lock_guard<std::mutex> lock(g_mu);
            shutdown_sessions();
        }
        reply_ok(res);
        g_running = false;
        http.stop();
    });

    log_event("control server listening on " + bind_host + ":" + std::to_string(port));
    if (!http.bind_to_port(bind_host, port)) {
        log_event("control port " + std::to_string(port) + " is unavailable");
        g_running = false;
        ticker.join();
        return 1;
    }
    http.listen_after_bind();
    g_running = false;
    ticker.join();
    return 0;
}

bool ensure_control_server() {
    auto token = access_token();
    httplib::Client cli(kControlHost, kControlPort);
    cli.set_connection_timeout(0, 200000);
    cli.set_read_timeout(1, 0);
    httplib::Headers auth{{"Authorization", "Bearer " + token}};

    // A server from before tokens answers state with no credential. Don't talk to it.
    if (auto bare = cli.Get("/v1/state"); bare && bare->status == 200) {
        std::cerr << "error: an older control server is still running on port " << kControlPort
                  << ". Stop it and run aio again.\n";
        return false;
    }
    if (auto res = cli.Get("/v1/health", auth); res && res->status == 200) return true;

    Platform::spawn_detached({Platform::executable_path(), "--control"});
    for (int i = 0; i < 50; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (auto res = cli.Get("/v1/health", auth); res && res->status == 200) return true;
    }
    std::cerr << "error: agent host did not start on " << kHostBind << ":" << kHostPort << "\n";
    return false;
}

}  // namespace aio
