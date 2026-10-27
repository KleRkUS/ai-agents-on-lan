#include "ui.hpp"

#include <atomic>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "api.hpp"
#include "control.hpp"
#include "platform.hpp"
#include "protocol.hpp"

#ifndef _WIN32
#include <poll.h>
#include <unistd.h>
#endif

namespace aio {
namespace {

std::atomic<bool> g_resized{false};

void on_resize(int) { g_resized = true; }

#ifndef _WIN32

bool write_all(const std::string& bytes) {
    size_t off = 0;
    while (off < bytes.size()) {
        ssize_t n = ::write(STDOUT_FILENO, bytes.data() + off, bytes.size() - off);
        if (n < 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

void send_size(httplib::ws::WebSocketClient& ws) {
    auto size = Platform::terminal_size();
    if (!size) return;
    ws.send(nlohmann::json{{"cols", size->cols}, {"rows", size->rows}}.dump());
}

#endif

}  // namespace

int run_attach(const std::string& kind, const std::string& name) {
    if ((kind != "agent" && kind != "server") || name.empty() || name.find('/') != std::string::npos) {
        std::cerr << "usage: aio attach --agent <name> | --server <name>\n";
        return 1;
    }
#ifdef _WIN32
    std::cerr << "error: attach is not available in this build\n";
    return 1;
#else
    if (!control_endpoint().remote && !ensure_control_server()) return 1;
    auto endpoint = control_endpoint();
    if (endpoint.token.empty()) {
        std::cerr << "error: pass --token for " << endpoint.host << "\n";
        return 1;
    }
    auto path = "/v1/terminal/" + kind + "s/" + name;
    if (!endpoint.target.empty()) path = "/v1/hosts/" + endpoint.target + "/terminal/" + kind + "s/" + name;
    auto url = "ws://" + endpoint.host + ":" + std::to_string(endpoint.port) + path;
    httplib::ws::WebSocketClient ws(url, {{"Authorization", "Bearer " + endpoint.token}});
    if (!ws.connect()) {
        std::cerr << "error: could not attach to " << kind << " '" << name << "'\n";
        return 1;
    }

    auto raw = Platform::begin_raw_input();
    if (!raw.active) {
        ws.close();
        std::cerr << "error: attach needs a terminal\n";
        return 1;
    }
    struct Reset {
        Platform::TerminalInput* saved;
        ~Reset() { Platform::end_raw_input(*saved); }
    } reset{&raw};

    std::string error;
    {
        std::signal(SIGWINCH, on_resize);
        send_size(ws);
        std::atomic<bool> stop{false};
        std::thread keys([&] {
            while (!stop && ws.is_open()) {
                if (g_resized.exchange(false)) send_size(ws);
                pollfd pfd{};
                pfd.fd = STDIN_FILENO;
                pfd.events = POLLIN;
                if (::poll(&pfd, 1, 50) <= 0) continue;
                char buf[1024];
                ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
                if (n <= 0) {
                    stop = true;
                    ws.close();
                    break;
                }
                if (!ws.send(buf, static_cast<size_t>(n))) {
                    stop = true;
                    break;
                }
            }
        });

        while (!stop && ws.is_open()) {
            std::string msg;
            auto result = ws.read(msg);
            if (result == httplib::ws::Fail || result == httplib::ws::Timeout) break;
            if (result == httplib::ws::Text) {
                auto body = nlohmann::json::parse(msg, nullptr, false);
                if (body.is_object() && body.contains("error") && body["error"].is_string()) error = body["error"];
                break;
            }
            if (!write_all(msg)) break;
        }
        stop = true;
        ws.close();
        keys.join();
    }
    if (!error.empty()) {
        Platform::end_raw_input(raw);
        std::cerr << "error: " << error << "\n";
        return 1;
    }
    return 0;
#endif
}

}  // namespace aio
