#include "terminal.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

#include "agents.hpp"
#include "log.hpp"
#include "servers.hpp"
#include "tmux.hpp"

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace aio {
namespace {

std::atomic<bool> g_streams{true};

#ifndef _WIN32

struct Subscriber {
    std::mutex mu;
    std::string pending;
};

struct Pipe {
    std::string pane;
    std::string path;
    int fd = -1;
    std::atomic<bool> run{true};
    std::thread reader;
    std::mutex join_mu;
    std::mutex mu;
    std::vector<std::shared_ptr<Subscriber>> subs;
};

std::mutex g_pipes_mu;
std::map<std::string, std::shared_ptr<Pipe>> g_pipes;

std::string fifo_path(const std::string& pane) {
    std::string id;
    for (char c : pane) id += (c == '%' || c == '/') ? '_' : c;
    auto dir = Platform::join_path(Platform::state_dir(), "pipes");
    Platform::create_directories(dir);
    return Platform::join_path(dir, id + ".fifo");
}

void read_loop(const std::shared_ptr<Pipe>& pipe) {
    char buf[8192];
    while (pipe->run) {
        pollfd pfd{};
        pfd.fd = pipe->fd;
        pfd.events = POLLIN;
        int rc = ::poll(&pfd, 1, 200);
        if (!pipe->run) break;
        if (rc <= 0) continue;
        ssize_t n = ::read(pipe->fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            break;
        }
        if (n == 0) break;
        std::lock_guard<std::mutex> lock(pipe->mu);
        for (const auto& sub : pipe->subs) {
            std::lock_guard<std::mutex> sub_lock(sub->mu);
            sub->pending.append(buf, static_cast<size_t>(n));
            if (sub->pending.size() > 1024 * 1024)
                sub->pending.erase(0, sub->pending.size() - 1024 * 1024);
        }
    }
}

void halt(const std::shared_ptr<Pipe>& pipe) {
    std::lock_guard<std::mutex> lock(pipe->join_mu);
    pipe->run = false;
    if (pipe->reader.joinable()) pipe->reader.join();
    if (pipe->fd >= 0) {
        ::close(pipe->fd);
        pipe->fd = -1;
    }
    if (!pipe->path.empty()) {
        ::unlink(pipe->path.c_str());
        pipe->path.clear();
    }
    tmux({"pipe-pane", "-t", pipe->pane});
}

bool start_pipe(const std::shared_ptr<Pipe>& pipe) {
    pipe->path = fifo_path(pipe->pane);
    ::unlink(pipe->path.c_str());
    if (::mkfifo(pipe->path.c_str(), 0600) != 0) return false;
    // -O feeds the pane's output to the command's stdin. cat blocks until we open the fifo,
    // and tmux returns as soon as cat is spawned.
    auto cmd = "cat > " + Platform::shell_quote(pipe->path);
    if (!tmux({"pipe-pane", "-O", "-t", pipe->pane, cmd}).ok()) {
        ::unlink(pipe->path.c_str());
        pipe->path.clear();
        return false;
    }
    pipe->fd = ::open(pipe->path.c_str(), O_RDONLY | O_NONBLOCK);
    if (pipe->fd < 0) {
        tmux({"pipe-pane", "-t", pipe->pane});
        ::unlink(pipe->path.c_str());
        pipe->path.clear();
        return false;
    }
    pipe->reader = std::thread([pipe] { read_loop(pipe); });
    return true;
}

std::shared_ptr<Pipe> subscribe(const std::string& pane, const std::shared_ptr<Subscriber>& sub) {
    std::lock_guard<std::mutex> lock(g_pipes_mu);
    auto& pipe = g_pipes[pane];
    if (!pipe) {
        pipe = std::make_shared<Pipe>();
        pipe->pane = pane;
        if (!start_pipe(pipe)) {
            g_pipes.erase(pane);
            return nullptr;
        }
    }
    std::lock_guard<std::mutex> sub_lock(pipe->mu);
    pipe->subs.push_back(sub);
    return pipe;
}

void unsubscribe(const std::shared_ptr<Pipe>& pipe, const std::shared_ptr<Subscriber>& sub) {
    bool last = false;
    {
        std::lock_guard<std::mutex> lock(pipe->mu);
        auto& subs = pipe->subs;
        subs.erase(std::remove(subs.begin(), subs.end(), sub), subs.end());
        last = subs.empty();
    }
    if (!last) return;
    {
        std::lock_guard<std::mutex> lock(g_pipes_mu);
        auto it = g_pipes.find(pipe->pane);
        if (it != g_pipes.end() && it->second == pipe) g_pipes.erase(it);
    }
    halt(pipe);
}

std::string take_pending(const std::shared_ptr<Subscriber>& sub) {
    std::lock_guard<std::mutex> lock(sub->mu);
    std::string out;
    out.swap(sub->pending);
    return out;
}

#endif  // !_WIN32

std::string resolve_pane(const std::string& kind, const std::string& name) {
    if (kind == "agent") {
        for (const auto& agent : list_agents())
            if (agent.name == name) return agent.pane_id;
        return {};
    }
    if (kind == "server") return server_pane(name);
    return {};
}

std::string snapshot(const std::string& pane) {
    auto screen = tmux_out({"capture-pane", "-e", "-p", "-t", pane});
    int x = 0, y = 0;
    auto cursor = split(tmux_out({"display-message", "-p", "-t", pane, "#{cursor_x} #{cursor_y}"}), ' ');
    try {
        if (cursor.size() == 2) {
            x = std::stoi(cursor[0]);
            y = std::stoi(cursor[1]);
        }
    } catch (...) {
    }
    std::string out = "\033[0m\033[H\033[2J";
    out += screen;
    if (screen.empty() || screen.back() != '\n') out.push_back('\n');
    out += "\033[" + std::to_string(y + 1) + ";" + std::to_string(x + 1) + "H";
    return out;
}

void send_input(const std::string& pane, const std::string& bytes) {
    for (size_t i = 0; i < bytes.size(); i += 32) {
        std::vector<std::string> args = {"send-keys", "-t", pane, "-H"};
        size_t end = std::min(bytes.size(), i + 32);
        for (size_t j = i; j < end; ++j) {
            char hex[3];
            std::snprintf(hex, sizeof(hex), "%02x", static_cast<unsigned char>(bytes[j]));
            args.emplace_back(hex);
        }
        tmux(std::move(args));
    }
}

void resize_pane(const std::string& pane, int cols, int rows) {
    if (cols < 10 || rows < 3 || cols > 500 || rows > 200) return;
    auto window = tmux_out({"display-message", "-p", "-t", pane, "#{window_id}"});
    if (window.empty()) return;
    tmux({"set-option", "-w", "-t", window, "window-size", "manual"});
    tmux({"resize-window", "-t", window, "-x", std::to_string(cols), "-y", std::to_string(rows)});
}

void apply_text(const std::string& pane, const std::string& msg) {
    auto body = nlohmann::json::parse(msg, nullptr, false);
    if (!body.is_object() || !body.contains("cols") || !body.contains("rows") || !body["cols"].is_number_integer() ||
        !body["rows"].is_number_integer())
        return;
    resize_pane(pane, body["cols"], body["rows"]);
}

void send_error(httplib::ws::WebSocket& ws, const std::string& error) {
    ws.send(nlohmann::json{{"error", error}}.dump());
    ws.close();
}

}  // namespace

void start_terminal_streams() { g_streams = true; }

void stop_terminal_streams() {
    g_streams = false;
#ifndef _WIN32
    std::vector<std::shared_ptr<Pipe>> pipes;
    {
        std::lock_guard<std::mutex> lock(g_pipes_mu);
        for (auto& [_, pipe] : g_pipes) pipes.push_back(pipe);
        g_pipes.clear();
    }
    for (auto& pipe : pipes) halt(pipe);
#endif
}

void serve_terminal(const std::string& kind, const std::string& name, httplib::ws::WebSocket& ws) {
    auto pane = resolve_pane(kind, name);
    if (pane.empty()) return send_error(ws, "no " + kind + " named '" + name + "'");
#ifdef _WIN32
    return send_error(ws, "terminal streaming runs on the linux server");
#else
    auto sub = std::make_shared<Subscriber>();
    auto pipe = subscribe(pane, sub);
    if (!pipe) return send_error(ws, "could not read the terminal");
    log_event("terminal client attached to " + kind + " '" + name + "'");

    // One shot, long enough for the client's size frame. A short timeout here can
    // expire in the middle of a frame and drop the connection.
    ws.set_read_timeout(std::chrono::seconds(1));
    std::string first;
    auto opened = ws.read(first);
    ws.set_read_timeout(0, 0);
    if (opened == httplib::ws::Text) apply_text(pane, first);
    else if (opened == httplib::ws::Binary) send_input(pane, first);
    else if (opened == httplib::ws::Fail) return unsubscribe(pipe, sub);

    auto screen = snapshot(pane);
    if (!ws.send(screen.data(), screen.size())) return unsubscribe(pipe, sub);

    std::atomic<bool> go{true};
    std::thread flusher([&] {
        while (go && g_streams && ws.is_open()) {
            auto chunk = take_pending(sub);
            if (chunk.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (!ws.send(chunk.data(), chunk.size())) break;
        }
        if (!g_streams) ws.close();
    });

    while (g_streams && ws.is_open()) {
        std::string msg;
        auto result = ws.read(msg);
        if (result == httplib::ws::Fail || result == httplib::ws::Timeout) break;
        if (result == httplib::ws::Text) apply_text(pane, msg);
        else if (result == httplib::ws::Binary) send_input(pane, msg);
    }
    go = false;
    flusher.join();
    unsubscribe(pipe, sub);
#endif
}

}  // namespace aio
