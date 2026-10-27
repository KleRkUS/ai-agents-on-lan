#include "ui.hpp"

#include <iostream>
#include <string>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "control.hpp"
#include "host.hpp"
#include "protocol.hpp"
#include "relay.hpp"

namespace aio {

int run_join(const std::string& relay_host, int port, const std::string& token) {
    if (relay_host.empty() || token.empty() || port <= 0) {
        std::cerr << "usage: aio --join ADDR --token TOKEN [--port N]\n";
        return 1;
    }
    if (!ensure_control_server()) return 1;

    auto ident = host_identity();
    std::string host = relay_host;
    if (host.find(':') != std::string::npos && host.front() != '[') host = "[" + host + "]";
    auto url = "ws://" + host + ":" + std::to_string(port) + "/v1/join";
    httplib::Headers headers{{"Authorization", "Bearer " + token}};
    httplib::ws::WebSocketClient ws(url, headers);
    ws.set_connection_timeout(3, 0);
    ws.set_read_timeout(5, 0);
    if (!ws.connect()) {
        std::cerr << "error: cannot reach relay at " << relay_host << ":" << port << "\n";
        return 1;
    }
    if (!ws.send(nlohmann::json{{"id", ident.id}, {"name", ident.name}}.dump())) {
        std::cerr << "error: relay did not accept the join\n";
        return 1;
    }
    std::string ack;
    auto got = ws.read(ack);
    auto body = got == httplib::ws::Text ? nlohmann::json::parse(ack, nullptr, false) : nlohmann::json{};
    if (!body.is_object() || !body.value("ok", false)) {
        auto error = body.is_object() ? body.value("error", std::string("relay rejected the join"))
                                      : std::string("relay rejected the join");
        std::cerr << "error: " << error << "\n";
        return 1;
    }
    std::cout << "joined " << relay_host << ":" << port << " as " << ident.name << "\n";
    answer_relay_calls(ws, kHostBind, kHostPort);
    std::cerr << "error: relay connection closed\n";
    return 1;
}

}  // namespace aio
