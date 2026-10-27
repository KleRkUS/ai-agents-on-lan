#include "check.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "agents.hpp"
#include "control.hpp"
#include "tmux.hpp"

namespace {

struct EnvGuard {
    const char* key;
    std::optional<std::string> saved;
    explicit EnvGuard(const char* key) : key(key) {
        if (auto v = std::getenv(key)) saved = v;
    }
    ~EnvGuard() {
        if (saved) setenv(key, saved->c_str(), 1);
        else unsetenv(key);
    }
};

}  // namespace

// The terminal websocket copies pane output and delivers keystrokes. Private socket only.
TEST(terminal_websocket_streams_output_and_input) {
    EnvGuard token("AIO_ACCESS_TOKEN");
    EnvGuard socket("AIO_TMUX_SOCKET");
    EnvGuard config_home("XDG_CONFIG_HOME");
    EnvGuard state_home("XDG_STATE_HOME");
    constexpr const char* kToken = "test-token-term";
    setenv("AIO_ACCESS_TOKEN", kToken, 1);
    setenv("AIO_TMUX_SOCKET", "aiotest-term", 1);
    setenv("XDG_CONFIG_HOME", "/tmp/aio-term-cfg", 1);
    setenv("XDG_STATE_HOME", "/tmp/aio-term-state", 1);
    std::filesystem::remove_all("/tmp/aio-term-cfg");
    std::filesystem::remove_all("/tmp/aio-term-state");
    if (!aio::tmux({"-V"}).ok()) return;
    aio::tmux({"kill-server"});

    constexpr int port = 47931;
    std::thread server([&] { aio::run_control(port, "127.0.0.1"); });

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(0, 300000);
    cli.set_read_timeout(5, 0);
    bool up = false;
    for (int i = 0; i < 50 && !up; ++i) {
        if (auto res = cli.Get("/v1/health"); res && res->status == 401) up = true;
        else std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(up);
    cli.set_default_headers({{"Authorization", std::string("Bearer ") + kToken}});

    if (up) {
        auto body = nlohmann::json{{"name", "sleeper"},
                                   {"directory", "/tmp"},
                                   {"command", "printf 'hello-attach\\n'; cat"},
                                   {"manager", false}};
        auto created = cli.Post("/v1/agents", body.dump(), "application/json");
        if (!created || created->status != 200)
            std::cerr << "create agent: " << (created ? created->body : "no response") << "\n";
        CHECK(created && created->status == 200);

        bool printed = false;
        for (int i = 0; i < 30 && !printed; ++i) {
            for (const auto& agent : aio::list_agents())
                if (agent.name == "sleeper" && aio::capture_pane(agent.pane_id).find("hello-attach") != std::string::npos)
                    printed = true;
            if (!printed) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        CHECK(printed);

        httplib::ws::WebSocketClient denied("ws://127.0.0.1:47931/v1/terminal/agents/sleeper");
        CHECK(!static_cast<bool>(denied.connect()));

        httplib::ws::WebSocketClient ws("ws://127.0.0.1:47931/v1/terminal/agents/sleeper",
                                        {{"Authorization", std::string("Bearer ") + kToken}});
        CHECK(static_cast<bool>(ws.connect()));
        ws.set_read_timeout(std::chrono::milliseconds(200));
        ws.send(nlohmann::json{{"cols", 80}, {"rows", 24}}.dump());

        std::string got;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline && got.find("hello-attach") == std::string::npos) {
            std::string msg;
            auto result = ws.read(msg);
            if (result == httplib::ws::Fail) break;
            if (result == httplib::ws::Timeout) continue;
            got += msg;
        }
        if (got.find("hello-attach") == std::string::npos) std::cerr << "terminal bytes: " << got << "\n";
        CHECK(got.find("hello-attach") != std::string::npos);

        const char typed[] = "xyz\n";
        CHECK(ws.send(typed, sizeof(typed) - 1));
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline && got.find("xyz") == std::string::npos) {
            std::string msg;
            auto result = ws.read(msg);
            if (result == httplib::ws::Fail) break;
            if (result == httplib::ws::Timeout) continue;
            got += msg;
        }
        if (got.find("xyz") == std::string::npos) std::cerr << "after input: " << got << "\n";
        CHECK(got.find("xyz") != std::string::npos);
        ws.close();

        auto removed = cli.Delete("/v1/agents/sleeper");
        CHECK(removed && removed->status == 200);
    }

    cli.set_read_timeout(2, 0);
    cli.Post("/v1/shutdown", "{}", "application/json");
    server.join();
    aio::tmux({"kill-server"});
}
