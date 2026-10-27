#include "api.hpp"

#include <cctype>
#include <iostream>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "control.hpp"
#include "platform.hpp"
#include "protocol.hpp"

namespace aio {
namespace {

int port_from_env(int fallback) {
    auto text = Platform::env("AIO_PORT").value_or("");
    if (text.empty()) return fallback;
    try {
        size_t used = 0;
        int port = std::stoi(text, &used);
        if (used == text.size() && port > 0 && port < 65536) return port;
    } catch (...) {
    }
    return fallback;
}

bool valid_target(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (unsigned char c : id)
        if (!std::isalnum(c) && c != '.' && c != '_' && c != '-') return false;
    return true;
}

// One joined machine is addressable as /v1/state. Several need /v1/hosts/<id>/state.
std::string scoped(const std::string& path) {
    auto endpoint = control_endpoint();
    if (!endpoint.remote || endpoint.target.empty()) return path;
    if (path.rfind("/v1/", 0) != 0 || path.rfind("/v1/hosts/", 0) == 0) return path;
    return "/v1/hosts/" + endpoint.target + path.substr(3);
}

httplib::Client client() {
    auto endpoint = control_endpoint();
    httplib::Client cli(endpoint.host, endpoint.port);
    cli.set_connection_timeout(1, 0);
    cli.set_read_timeout(5, 0);
    if (!endpoint.token.empty())
        cli.set_default_headers({{"Authorization", "Bearer " + endpoint.token}});
    return cli;
}

std::string call(const char* method, const std::string& path, const std::string& body = {}) {
    auto cli = client();
    httplib::Result res;
    auto route = scoped(path);
    if (method == std::string("POST")) res = cli.Post(route, body, "application/json");
    else if (method == std::string("PUT")) res = cli.Put(route, body, "application/json");
    else if (method == std::string("DELETE")) res = cli.Delete(route);
    else res = cli.Get(route);
    if (!res) return "control server is not running";
    if (res->status >= 300) {
        auto j = nlohmann::json::parse(res->body, nullptr, false);
        if (j.is_object() && j.contains("error") && j["error"].is_string()) return j["error"];
        return "control server returned " + std::to_string(res->status);
    }
    return {};
}

}  // namespace

ControlState Control::state() {
    ControlState out;
    auto cli = client();
    auto res = cli.Get(scoped("/v1/state"));
    if (!res) {
        out.error = "control server is not running";
        return out;
    }
    if (res->status >= 300) {
        auto err = nlohmann::json::parse(res->body, nullptr, false);
        if (err.is_object() && err.contains("error") && err["error"].is_string() && !err["error"].get<std::string>().empty())
            out.error = err["error"];
        else
            out.error = "control server returned " + std::to_string(res->status);
        return out;
    }
    auto j = nlohmann::json::parse(res->body, nullptr, false);
    if (!j.is_object()) {
        out.error = "bad state from control server";
        return out;
    }
    out.viewport = j.value("viewport", "");
    out.next_port = j.value("next_port", out.next_port);
    for (const auto& a : j.value("agents", nlohmann::json::array())) {
        Agent agent;
        agent.name = a.value("name", "");
        agent.dir = a.value("dir", "");
        agent.command = a.value("command", "");
        agent.pane_id = a.value("pane_id", "");
        agent.dead = a.value("dead", false);
        agent.exit_status = a.value("exit_status", 0);
        agent.manager = a.value("manager", false);
        for (const auto& s : a.value("servers", nlohmann::json::array()))
            if (s.is_string()) agent.servers.push_back(s);
        out.unread_counts[agent.name] = a.value("unread", 0);
        out.agents.push_back(std::move(agent));
    }
    for (const auto& s : j.value("servers", nlohmann::json::array())) {
        ControlState::ServerRow row;
        row.info.name = s.value("name", "");
        row.info.port = s.value("port", 0);
        row.info.color = s.value("color", "");
        row.running = s.value("running", false);
        row.error = s.value("error", "");
        row.unread = s.value("unread", 0);
        for (const auto& m : s.value("members", nlohmann::json::array()))
            if (m.is_string()) row.info.members.emplace(m.get<std::string>(), "");
        out.servers.push_back(std::move(row));
    }
    if (j.contains("config")) {
        if (auto err = config_from_json(j["config"], out.config); !err.empty()) out.error = err;
    }
    if (j.contains("adapters") && j["adapters"].is_object()) {
        out.adapters.dir = j["adapters"].value("dir", "");
        for (const auto& a : j["adapters"].value("adapters", nlohmann::json::array())) {
            Adapter adapter;
            adapter.name = a.value("name", "");
            for (const auto& m : a.value("match", nlohmann::json::array()))
                if (m.is_string()) adapter.match.push_back(m);
            out.adapters.adapters.push_back(std::move(adapter));
        }
        for (const auto& e : j["adapters"].value("errors", nlohmann::json::array()))
            if (e.is_string()) out.adapters.errors.push_back(e);
    }
    if (j.contains("access") && j["access"].is_object()) {
        out.access.token = j["access"].value("token", "");
        out.access.port = j["access"].value("port", out.access.port);
        for (const auto& ip : j["access"].value("addresses", nlohmann::json::array()))
            if (ip.is_string()) out.access.addresses.push_back(ip);
    }
    if (j.contains("host") && j["host"].is_object()) {
        out.host_id = j["host"].value("id", "");
        out.host_name = j["host"].value("name", "");
    }
    return out;
}

std::vector<JoinedHost> Control::hosts() {
    std::vector<JoinedHost> out;
    if (!control_endpoint().remote) return out;
    auto res = client().Get("/v1/hosts");
    if (!res || res->status >= 300) return out;
    auto j = nlohmann::json::parse(res->body, nullptr, false);
    if (!j.is_object()) return out;
    for (const auto& row : j.value("hosts", nlohmann::json::array())) {
        JoinedHost host;
        host.id = row.value("id", "");
        host.name = row.value("name", "");
        if (!host.id.empty()) out.push_back(std::move(host));
    }
    return out;
}

ControlEndpoint control_endpoint() {
    ControlEndpoint endpoint;
    auto host = Platform::env("AIO_HOST").value_or("");
    if (!host.empty()) endpoint.host = host;
    endpoint.remote = !host.empty() && host != "127.0.0.1" && host != "localhost" && host != "::1";
    endpoint.port = port_from_env(endpoint.remote ? kRelayPort : kHostPort);
    auto token = Platform::env("AIO_TOKEN").value_or("");
    if (!token.empty()) endpoint.token = token;
    else if (!endpoint.remote) endpoint.token = access_token();
    auto target = Platform::env("AIO_TARGET").value_or("");
    if (endpoint.remote && valid_target(target)) endpoint.target = target;
    return endpoint;
}

bool control_reachable() {
    auto endpoint = control_endpoint();
    if (endpoint.token.empty()) {
        std::cerr << "error: pass --token for " << endpoint.host << "\n";
        return false;
    }
    httplib::Client cli(endpoint.host, endpoint.port);
    cli.set_connection_timeout(1, 0);
    cli.set_read_timeout(2, 0);
    httplib::Headers auth{{"Authorization", "Bearer " + endpoint.token}};
    auto res = cli.Get("/v1/health", auth);
    if (res && res->status == 200) return true;
    if (res && res->status == 401)
        std::cerr << "error: the server at " << endpoint.host << ":" << endpoint.port << " rejected the token\n";
    else
        std::cerr << "error: cannot reach aio at " << endpoint.host << ":" << endpoint.port << "\n";
    return false;
}

std::string Control::create_agent(const std::string& name, const std::string& directory, const std::string& command,
                                  const std::vector<std::string>& servers, bool manager) {
    nlohmann::json body = {{"name", name}, {"directory", directory}, {"command", command},
                           {"servers", servers}, {"manager", manager}};
    return call("POST", "/v1/agents", body.dump());
}

std::string Control::delete_agent(const std::string& name) { return call("DELETE", "/v1/agents/" + name); }

std::string Control::show_agent(const std::string& name, bool focus) {
    return call("POST", "/v1/agents/" + name + "/show", nlohmann::json{{"focus", focus}}.dump());
}

std::string Control::stop_agent(const std::string& name) { return call("POST", "/v1/agents/" + name + "/stop", "{}"); }

std::string Control::create_server(const std::string& name, int port) {
    return call("POST", "/v1/servers", nlohmann::json{{"name", name}, {"port", port}}.dump());
}

std::string Control::start_server(const std::string& name) { return call("POST", "/v1/servers/" + name + "/start", "{}"); }

std::string Control::stop_server(const std::string& name) { return call("POST", "/v1/servers/" + name + "/stop", "{}"); }

std::string Control::delete_server(const std::string& name) { return call("DELETE", "/v1/servers/" + name); }

std::string Control::show_server(const std::string& name) { return call("POST", "/v1/servers/" + name + "/show", "{}"); }

std::string Control::save_config(const Config& config) { return call("PUT", "/v1/config", config_to_json(config).dump()); }

std::string Control::zoom(bool on) {
    // Zooming changes this machine's split, never the server's.
    if (control_endpoint().remote) {
        set_sidebar_zoom(on);
        return {};
    }
    return call("POST", "/v1/layout/zoom", nlohmann::json{{"on", on}}.dump());
}

void Control::shutdown() {
    if (control_endpoint().remote) return;
    auto cli = client();
    cli.set_read_timeout(2, 0);
    cli.Post("/v1/shutdown", "{}", "application/json");
}

}  // namespace aio
