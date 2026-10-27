#include "relay.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "control.hpp"
#include "log.hpp"
#include "protocol.hpp"

namespace aio {
namespace {

struct Pending {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    int status = 504;
    std::string body;
};

struct Conn {
    std::mutex mu;
    httplib::ws::WebSocket* ws = nullptr;
    std::vector<uint64_t> inflight;
};

struct Slot {
    std::string name;
    uint64_t generation = 0;
    std::shared_ptr<Conn> conn;
};

// One viewer's terminal, bridged over a host's join socket.
struct Stream {
    std::mutex mu;
    std::condition_variable cv;
    std::shared_ptr<Conn> conn;
    bool opened = false;
    bool ok = false;
    bool closed = false;
    std::string error;
    std::string pending;
    std::string text;
    httplib::ws::WebSocket* viewer = nullptr;
};

std::mutex g_mu;
std::map<std::string, Slot> g_hosts;
std::map<uint64_t, std::shared_ptr<Pending>> g_pending;
std::map<uint64_t, std::shared_ptr<Stream>> g_streams;
std::atomic<uint64_t> g_generation{0};
std::atomic<uint64_t> g_next_call{1};
std::atomic<uint64_t> g_next_stream{1};
std::atomic<bool> g_running{false};

bool valid_id(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (unsigned char c : id)
        if (!std::isalnum(c) && c != '.' && c != '_' && c != '-') return false;
    return true;
}

std::string clean_name(const std::string& name) {
    std::string out;
    for (unsigned char c : name) {
        if (c < 32 || c == 127) continue;
        out.push_back(static_cast<char>(c));
        if (out.size() == 64) break;
    }
    return out.empty() ? "aio" : out;
}

bool from_loopback(const httplib::Request& req) {
    return req.remote_addr == "127.0.0.1" || req.remote_addr == "::1";
}

void reply_error(httplib::Response& res, int status, const std::string& error) {
    res.status = status;
    res.set_content(nlohmann::json{{"error", error}}.dump(), "application/json");
}

void complete_call(uint64_t id, int status, const std::string& body) {
    std::shared_ptr<Pending> pending;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_pending.find(id);
        if (it == g_pending.end()) return;
        pending = it->second;
    }
    {
        std::lock_guard<std::mutex> lock(pending->mu);
        if (pending->done) return;
        pending->status = status;
        pending->body = body;
        pending->done = true;
    }
    pending->cv.notify_one();
}

void fail_inflight(const std::shared_ptr<Conn>& conn) {
    std::vector<uint64_t> ids;
    {
        std::lock_guard<std::mutex> lock(conn->mu);
        conn->ws = nullptr;
        ids.swap(conn->inflight);
    }
    auto body = nlohmann::json{{"error", "agent host disconnected"}}.dump();
    for (auto id : ids) complete_call(id, 503, body);
}

// `host_id` empty means the only joined host. Returns false and fills `error`.
bool resolve_host(const std::string& request_path, std::string& host_id, std::string& forward_path,
                  std::string& error, int& status) {
    constexpr const char* kPrefix = "/v1/hosts/";
    if (request_path.rfind(kPrefix, 0) == 0) {
        auto rest = request_path.substr(std::char_traits<char>::length(kPrefix));
        auto slash = rest.find('/');
        if (slash == std::string::npos || slash == 0) {
            status = 404;
            error = "not found";
            return false;
        }
        host_id = rest.substr(0, slash);
        forward_path = "/v1" + rest.substr(slash);
        if (!valid_id(host_id)) {
            status = 404;
            error = "not found";
            return false;
        }
        return true;
    }
    forward_path = request_path;
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_hosts.empty()) {
        status = 503;
        error = "no agent host is joined";
        return false;
    }
    if (g_hosts.size() > 1) {
        status = 400;
        error = "more than one host is joined";
        return false;
    }
    host_id = g_hosts.begin()->first;
    return true;
}

bool layout_request(const std::string& method, const std::string& path) {
    if (method != "POST") return false;
    if (path == "/v1/shutdown" || path == "/v1/layout/zoom") return true;
    auto at = path.rfind("/show");
    if (at == std::string::npos || at + 5 != path.size()) return false;
    return path.rfind("/v1/agents/", 0) == 0 || path.rfind("/v1/servers/", 0) == 0;
}

bool forwardable(const std::string& path) {
    if (path.find("..") != std::string::npos) return false;
    return path.rfind("/v1/state", 0) == 0 || path.rfind("/v1/log", 0) == 0 || path.rfind("/v1/config", 0) == 0 ||
           path.rfind("/v1/agents", 0) == 0 || path.rfind("/v1/servers", 0) == 0;
}

void proxy_control(const httplib::Request& req, httplib::Response& res) {
    std::string host_id, path, error;
    int status = 400;
    if (!resolve_host(req.path, host_id, path, error, status)) return reply_error(res, status, error);
    if (req.method != "GET" && req.method != "POST" && req.method != "PUT" && req.method != "DELETE")
        return reply_error(res, 405, "method not allowed");
    if (layout_request(req.method, path)) {
        if (path == "/v1/shutdown") return reply_error(res, 403, "shutdown is only available on the agent machine");
        res.status = 200;
        res.set_content(nlohmann::json{{"error", ""}}.dump(), "application/json");
        return;
    }
    if (!forwardable(path)) return reply_error(res, 404, "not found");

    std::shared_ptr<Conn> conn;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_hosts.find(host_id);
        if (it == g_hosts.end() || !it->second.conn) {
            return reply_error(res, 404, "no host '" + host_id + "'");
        }
        conn = it->second.conn;
    }

    auto pending = std::make_shared<Pending>();
    auto id = ++g_next_call;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_pending[id] = pending;
    }
    auto frame = nlohmann::json{{"op", "call"}, {"id", id}, {"method", req.method}, {"path", path}, {"body", req.body}}.dump();
    bool sent = false;
    {
        std::lock_guard<std::mutex> lock(conn->mu);
        if (conn->ws) {
            conn->inflight.push_back(id);
            sent = conn->ws->send(frame);
        }
    }
    if (!sent) complete_call(id, 503, nlohmann::json{{"error", "agent host disconnected"}}.dump());
    int result_status = 504;
    std::string result_body = nlohmann::json{{"error", "agent host did not answer"}}.dump();
    {
        std::unique_lock<std::mutex> lock(pending->mu);
        if (sent) pending->cv.wait_for(lock, std::chrono::seconds(20), [&] { return pending->done; });
        if (pending->done) {
            result_status = pending->status;
            result_body = pending->body;
        } else {
            pending->done = true;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_pending.erase(id);
    }
    {
        std::lock_guard<std::mutex> lock(conn->mu);
        auto& ids = conn->inflight;
        ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
    }
    res.status = result_status;
    res.set_content(result_body.empty() ? "{}" : result_body, "application/json");
}

uint64_t json_id(const nlohmann::json& value) {
    if (!value.is_number() || value.is_number_float()) return 0;
    if (value.is_number_unsigned()) return value.get<uint64_t>();
    auto signed_id = value.get<int64_t>();
    return signed_id > 0 ? static_cast<uint64_t>(signed_id) : 0;
}

std::string pack_stream(uint64_t id, const std::string& payload) {
    std::string out(8 + payload.size(), '\0');
    for (int i = 0; i < 8; ++i)
        out[static_cast<size_t>(i)] = static_cast<char>((id >> ((7 - i) * 8)) & 0xff);
    if (!payload.empty()) out.replace(8, payload.size(), payload);
    return out;
}

uint64_t unpack_stream(const std::string& msg, std::string& payload) {
    if (msg.size() < 8) return 0;
    uint64_t id = 0;
    for (int i = 0; i < 8; ++i) id = (id << 8) | static_cast<unsigned char>(msg[static_cast<size_t>(i)]);
    payload.assign(msg, 8, std::string::npos);
    return id;
}

std::shared_ptr<Stream> find_stream(uint64_t id) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_streams.find(id);
    return it == g_streams.end() ? nullptr : it->second;
}

void drop_streams(const std::shared_ptr<Conn>& conn) {
    std::vector<std::shared_ptr<Stream>> mine;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        for (auto it = g_streams.begin(); it != g_streams.end();) {
            if (it->second->conn == conn) {
                mine.push_back(it->second);
                it = g_streams.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto& stream : mine) {
        httplib::ws::WebSocket* viewer = nullptr;
        {
            std::lock_guard<std::mutex> lock(stream->mu);
            stream->closed = true;
            if (stream->error.empty()) stream->error = "agent host disconnected";
            stream->cv.notify_all();
            viewer = stream->viewer;
            stream->viewer = nullptr;
        }
        if (viewer) viewer->close();
    }
}

bool send_text(const std::shared_ptr<Conn>& conn, const std::string& text) {
    std::lock_guard<std::mutex> lock(conn->mu);
    return conn->ws && conn->ws->send(text);
}

bool send_binary(const std::shared_ptr<Conn>& conn, const std::string& bytes) {
    std::lock_guard<std::mutex> lock(conn->mu);
    return conn->ws && conn->ws->send(bytes.data(), bytes.size());
}

bool terminal_path(const std::string& path) {
    auto slash = path.rfind('/');
    if (slash == std::string::npos || slash + 1 >= path.size()) return false;
    auto name = path.substr(slash + 1);
    if (name.find('/') != std::string::npos) return false;
    return path == "/v1/terminal/agents/" + name || path == "/v1/terminal/servers/" + name;
}

void handle_join_frame(const std::string& msg) {
    auto body = nlohmann::json::parse(msg, nullptr, false);
    if (!body.is_object()) return;
    auto op = body.value("op", "");
    auto id = json_id(body.contains("id") ? body["id"] : nlohmann::json{});
    if (op == "result") {
        complete_call(id, body.value("status", 502), body.value("body", ""));
        return;
    }
    if (op != "opened" && op != "text" && op != "close") return;
    auto stream = find_stream(id);
    if (!stream) return;
    std::lock_guard<std::mutex> lock(stream->mu);
    if (op == "opened") {
        stream->opened = true;
        stream->ok = body.value("ok", false);
        stream->error = body.value("error", "");
    } else if (op == "text") {
        stream->text += body.value("body", "");
    } else {
        stream->closed = true;
    }
    stream->cv.notify_all();
}

void handle_join_binary(const std::string& msg) {
    std::string payload;
    auto id = unpack_stream(msg, payload);
    if (id == 0 || payload.empty()) return;
    auto stream = find_stream(id);
    if (!stream) return;
    std::lock_guard<std::mutex> lock(stream->mu);
    stream->pending += payload;
    stream->cv.notify_all();
}

void bridge_terminal(httplib::ws::WebSocket& ws, const std::string& host_id, const std::string& path) {
    std::shared_ptr<Conn> conn;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_hosts.find(host_id);
        if (it == g_hosts.end() || !it->second.conn) {
            ws.send(nlohmann::json{{"error", "no host '" + host_id + "'"}}.dump());
            return;
        }
        conn = it->second.conn;
    }
    if (!terminal_path(path)) {
        ws.send(nlohmann::json{{"error", "not found"}}.dump());
        return;
    }

    auto id = ++g_next_stream;
    auto stream = std::make_shared<Stream>();
    stream->conn = conn;
    stream->viewer = &ws;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_streams[id] = stream;
    }
    auto opened = nlohmann::json{{"op", "open"}, {"id", id}, {"path", path}}.dump();
    if (!send_text(conn, opened)) {
        std::lock_guard<std::mutex> lock(g_mu);
        g_streams.erase(id);
        ws.send(nlohmann::json{{"error", "agent host disconnected"}}.dump());
        return;
    }
    {
        std::unique_lock<std::mutex> lock(stream->mu);
        stream->cv.wait_for(lock, std::chrono::seconds(10), [&] { return stream->opened || stream->closed; });
        if (!stream->ok) {
            auto error = stream->error.empty() ? "could not attach" : stream->error;
            lock.unlock();
            std::lock_guard<std::mutex> map_lock(g_mu);
            g_streams.erase(id);
            ws.send(nlohmann::json{{"error", error}}.dump());
            return;
        }
    }

    std::atomic<bool> pumping{true};
    std::thread flusher([&] {
        while (pumping && ws.is_open()) {
            std::string binary;
            std::string text;
            bool closed = false;
            {
                std::unique_lock<std::mutex> lock(stream->mu);
                stream->cv.wait_for(lock, std::chrono::milliseconds(50), [&] {
                    return stream->closed || !stream->pending.empty() || !stream->text.empty();
                });
                text.swap(stream->text);
                binary.swap(stream->pending);
                closed = stream->closed;
            }
            if (!text.empty()) {
                ws.send(text);
                break;
            }
            if (!binary.empty() && !ws.send(binary.data(), binary.size())) break;
            if (closed) break;
        }
    });

    ws.set_read_timeout(0, 0);
    while (ws.is_open()) {
        std::string msg;
        auto kind = ws.read(msg);
        if (kind == httplib::ws::Fail || kind == httplib::ws::Timeout) break;
        bool sent = false;
        if (kind == httplib::ws::Text)
            sent = send_text(conn, nlohmann::json{{"op", "text"}, {"id", id}, {"body", msg}}.dump());
        else if (kind == httplib::ws::Binary)
            sent = send_binary(conn, pack_stream(id, msg));
        if (!sent) break;
    }

    pumping = false;
    {
        std::lock_guard<std::mutex> lock(stream->mu);
        stream->closed = true;
        stream->viewer = nullptr;
        stream->cv.notify_all();
    }
    send_text(conn, nlohmann::json{{"op", "close"}, {"id", id}}.dump());
    flusher.join();
    std::lock_guard<std::mutex> lock(g_mu);
    g_streams.erase(id);
}

}  // namespace

int run_relay(int port, const std::string& bind_host) {
    g_running = true;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_hosts.clear();
        g_pending.clear();
        g_streams.clear();
    }

    httplib::Server http;
    http.set_pre_request_handler([](const httplib::Request& req, httplib::Response& res) {
        if (authorized_header(req.get_header_value("Authorization")))
            return httplib::Server::HandlerResponse::Unhandled;
        res.status = 401;
        res.set_content(nlohmann::json{{"error", "unauthorized"}}.dump(), "application/json");
        return httplib::Server::HandlerResponse::Handled;
    });

    http.Get("/v1/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(nlohmann::json{{"ok", true}, {"role", "relay"}, {"protocol", kProtocolVersion}}.dump(),
                        "application/json");
    });

    http.Get("/v1/hosts", [](const httplib::Request&, httplib::Response& res) {
        std::vector<std::pair<std::string, std::string>> rows;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            for (const auto& [id, slot] : g_hosts) rows.emplace_back(slot.name, id);
        }
        std::sort(rows.begin(), rows.end());
        nlohmann::json hosts = nlohmann::json::array();
        for (const auto& [name, id] : rows) hosts.push_back({{"id", id}, {"name", name}});
        res.set_content(nlohmann::json{{"hosts", hosts}}.dump(), "application/json");
    });

    http.Post("/v1/shutdown", [&http](const httplib::Request& req, httplib::Response& res) {
        if (!from_loopback(req)) return reply_error(res, 403, "shutdown is only available on the relay machine");
        res.set_content(nlohmann::json{{"error", ""}}.dump(), "application/json");
        g_running = false;
        std::vector<std::shared_ptr<Pending>> pending;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            for (const auto& [id, item] : g_pending) pending.push_back(item);
        }
        auto body = nlohmann::json{{"error", "relay is stopping"}}.dump();
        for (const auto& item : pending) {
            std::lock_guard<std::mutex> lock(item->mu);
            if (item->done) continue;
            item->status = 503;
            item->body = body;
            item->done = true;
            item->cv.notify_one();
        }
        std::vector<std::shared_ptr<Stream>> streams;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            for (const auto& [id, stream] : g_streams) streams.push_back(stream);
            g_streams.clear();
        }
        for (const auto& stream : streams) {
            httplib::ws::WebSocket* viewer = nullptr;
            {
                std::lock_guard<std::mutex> lock(stream->mu);
                stream->closed = true;
                stream->cv.notify_all();
                viewer = stream->viewer;
                stream->viewer = nullptr;
            }
            if (viewer) viewer->close();
        }
        http.stop();
    });

    auto proxy = [](const httplib::Request& req, httplib::Response& res) { proxy_control(req, res); };
    http.Get(R"(/v1/.*)", proxy);
    http.Post(R"(/v1/.*)", proxy);
    http.Put(R"(/v1/.*)", proxy);
    http.Delete(R"(/v1/.*)", proxy);

    auto bridge = [](const httplib::Request& req, httplib::ws::WebSocket& ws) {
        std::string host_id;
        std::string path;
        if (req.matches.size() == 4) {
            host_id = req.matches[1];
            path = "/v1/terminal/" + req.matches[2].str() + "/" + req.matches[3].str();
        } else if (req.matches.size() == 3) {
            path = "/v1/terminal/" + req.matches[1].str() + "/" + req.matches[2].str();
            std::string forward, error;
            int status = 400;
            if (!resolve_host(path, host_id, forward, error, status)) {
                ws.send(nlohmann::json{{"error", error}}.dump());
                return;
            }
        } else {
            ws.send(nlohmann::json{{"error", "not found"}}.dump());
            return;
        }
        bridge_terminal(ws, host_id, path);
    };
    http.WebSocket(R"(/v1/terminal/(agents|servers)/([^/]+))", bridge);
    http.WebSocket(R"(/v1/hosts/([^/]+)/terminal/(agents|servers)/([^/]+))", bridge);

    http.WebSocket("/v1/join", [](const httplib::Request&, httplib::ws::WebSocket& ws) {
        ws.set_read_timeout(std::chrono::seconds(10));
        std::string first;
        if (ws.read(first) != httplib::ws::Text) return;
        auto body = nlohmann::json::parse(first, nullptr, false);
        auto id = body.is_object() ? body.value("id", "") : "";
        auto name = clean_name(body.is_object() ? body.value("name", "") : "");
        if (!valid_id(id)) {
            ws.send(nlohmann::json{{"ok", false}, {"error", "id must be 1-64 of [A-Za-z0-9._-]"}}.dump());
            return;
        }
        auto conn = std::make_shared<Conn>();
        {
            std::lock_guard<std::mutex> lock(conn->mu);
            conn->ws = &ws;
        }
        uint64_t generation = ++g_generation;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            g_hosts[id] = Slot{name, generation, conn};
        }
        log_event("host '" + name + "' joined (" + id + ")");
        if (!ws.send(nlohmann::json{{"ok", true}}.dump())) {
            {
                std::lock_guard<std::mutex> lock(conn->mu);
                conn->ws = nullptr;
            }
            fail_inflight(conn);
            std::lock_guard<std::mutex> lock(g_mu);
            auto it = g_hosts.find(id);
            if (it != g_hosts.end() && it->second.generation == generation) g_hosts.erase(it);
            return;
        }

        ws.set_read_timeout(std::chrono::seconds(5));
        while (g_running) {
            std::string msg;
            auto result = ws.read(msg);
            if (result == httplib::ws::Fail || result == httplib::ws::Timeout) break;
            if (result == httplib::ws::Text) handle_join_frame(msg);
            else if (result == httplib::ws::Binary) handle_join_binary(msg);
        }
        {
            std::lock_guard<std::mutex> lock(conn->mu);
            conn->ws = nullptr;
        }
        fail_inflight(conn);
        drop_streams(conn);
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_hosts.find(id);
        if (it != g_hosts.end() && it->second.generation == generation) {
            log_event("host '" + name + "' left (" + id + ")");
            g_hosts.erase(it);
        }
    });

    log_event("relay listening on " + bind_host + ":" + std::to_string(port));
    if (!http.bind_to_port(bind_host, port)) {
        std::cerr << "error: relay port " << port << " is already in use\n";
        log_event("relay port " + std::to_string(port) + " is unavailable");
        g_running = false;
        return 1;
    }
    http.listen_after_bind();
    g_running = false;
    return 0;
}

namespace {

struct LocalReply {
    int status = 502;
    std::string body;
};

LocalReply local_control(const std::string& control_host, int control_port, const std::string& method,
                         const std::string& path, const std::string& body) {
    httplib::Client cli(control_host, control_port);
    cli.set_connection_timeout(1, 0);
    cli.set_read_timeout(20, 0);
    httplib::Headers headers{{"Authorization", "Bearer " + access_token()}, {"X-Aio-Via", "relay"}};
    httplib::Result res;
    if (method == "GET") res = cli.Get(path, headers);
    else if (method == "POST") res = cli.Post(path, headers, body, "application/json");
    else if (method == "PUT") res = cli.Put(path, headers, body, "application/json");
    else if (method == "DELETE") res = cli.Delete(path, headers);
    else return {400, nlohmann::json{{"error", "method not allowed"}}.dump()};
    if (!res) return {502, nlohmann::json{{"error", "agent host is not running"}}.dump()};
    return {res->status, res->body};
}

}  // namespace

void answer_relay_calls(httplib::ws::WebSocketClient& ws, const std::string& control_host, int control_port) {
    struct Pipe {
        std::shared_ptr<httplib::ws::WebSocketClient> client;
        std::atomic<bool> go{true};
        std::thread pump;
    };
    std::mutex pipes_mu;
    std::map<uint64_t, std::shared_ptr<Pipe>> pipes;

    auto stop_pipe = [&](const std::shared_ptr<Pipe>& pipe) {
        if (!pipe) return;
        pipe->go = false;
        if (pipe->client) pipe->client->close();
    };

    std::atomic<bool> alive{true};
    std::thread ping([&] {
        while (alive && ws.is_open()) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (!alive || !ws.is_open()) break;
            if (!ws.send(nlohmann::json{{"op", "ping"}}.dump())) break;
        }
    });

    auto open_terminal = [&](uint64_t id, const std::string& path) {
        if (!terminal_path(path)) {
            ws.send(nlohmann::json{{"op", "opened"}, {"id", id}, {"ok", false}, {"error", "not found"}}.dump());
            return;
        }
        std::string host = control_host;
        if (host.find(':') != std::string::npos && host.front() != '[') host = "[" + host + "]";
        auto url = "ws://" + host + ":" + std::to_string(control_port) + path;
        httplib::Headers headers{{"Authorization", "Bearer " + access_token()}};
        auto client = std::make_shared<httplib::ws::WebSocketClient>(url, headers);
        client->set_connection_timeout(3, 0);
        client->set_read_timeout(0, 0);
        if (!client->connect()) {
            ws.send(nlohmann::json{{"op", "opened"},
                                   {"id", id},
                                   {"ok", false},
                                   {"error", "could not attach"}}
                        .dump());
            return;
        }
        auto pipe = std::make_shared<Pipe>();
        pipe->client = client;
        {
            std::lock_guard<std::mutex> lock(pipes_mu);
            pipes[id] = pipe;
        }
        if (!ws.send(nlohmann::json{{"op", "opened"}, {"id", id}, {"ok", true}}.dump())) {
            stop_pipe(pipe);
            return;
        }
        pipe->pump = std::thread([pipe, &ws, &alive, id] {
            while (pipe->go && alive && pipe->client->is_open()) {
                std::string msg;
                auto kind = pipe->client->read(msg);
                if (!pipe->go || kind == httplib::ws::Fail) break;
                if (kind == httplib::ws::Text) {
                    if (!ws.send(nlohmann::json{{"op", "text"}, {"id", id}, {"body", msg}}.dump())) break;
                } else if (kind == httplib::ws::Binary) {
                    auto framed = pack_stream(id, msg);
                    if (!ws.send(framed.data(), framed.size())) break;
                }
            }
            pipe->go = false;
            ws.send(nlohmann::json{{"op", "close"}, {"id", id}}.dump());
        });
    };

    auto pipe_for = [&](uint64_t id) {
        std::lock_guard<std::mutex> lock(pipes_mu);
        auto it = pipes.find(id);
        return it == pipes.end() ? nullptr : it->second;
    };

    ws.set_read_timeout(0, 0);
    while (alive && ws.is_open()) {
        std::string msg;
        auto kind = ws.read(msg);
        if (kind == httplib::ws::Fail) break;
        if (kind == httplib::ws::Binary) {
            std::string payload;
            auto id = unpack_stream(msg, payload);
            auto pipe = pipe_for(id);
            if (pipe && pipe->client && !payload.empty()) pipe->client->send(payload.data(), payload.size());
            continue;
        }
        if (kind != httplib::ws::Text) continue;
        auto call = nlohmann::json::parse(msg, nullptr, false);
        if (!call.is_object()) continue;
        auto op = call.value("op", "");
        auto id = json_id(call.contains("id") ? call["id"] : nlohmann::json{});
        if (op == "call") {
            auto reply = local_control(control_host, control_port, call.value("method", ""), call.value("path", ""),
                                       call.value("body", ""));
            auto frame = nlohmann::json{{"op", "result"}, {"id", id}, {"status", reply.status}, {"body", reply.body}}.dump();
            if (!ws.send(frame)) break;
        } else if (op == "open") {
            open_terminal(id, call.value("path", ""));
        } else if (op == "text") {
            auto pipe = pipe_for(id);
            if (pipe && pipe->client) pipe->client->send(call.value("body", ""));
        } else if (op == "close") {
            stop_pipe(pipe_for(id));
        }
    }
    alive = false;
    std::vector<std::shared_ptr<Pipe>> all;
    {
        std::lock_guard<std::mutex> lock(pipes_mu);
        for (const auto& [id, pipe] : pipes) all.push_back(pipe);
        pipes.clear();
    }
    for (const auto& pipe : all) stop_pipe(pipe);
    for (const auto& pipe : all)
        if (pipe->pump.joinable()) pipe->pump.join();
    ping.join();
}

}  // namespace aio
