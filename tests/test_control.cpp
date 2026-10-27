#include "check.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

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

// Private socket and directories so this never touches the user's tmux server or config.
TEST(control_api_serves_state_and_agents) {
    EnvGuard token("AIO_ACCESS_TOKEN");
    EnvGuard socket("AIO_TMUX_SOCKET");
    EnvGuard config_home("XDG_CONFIG_HOME");
    EnvGuard state_home("XDG_STATE_HOME");
    constexpr const char* kToken = "test-token-ctl";
    setenv("AIO_ACCESS_TOKEN", kToken, 1);
    setenv("AIO_TMUX_SOCKET", "aiotest-ctl", 1);
    setenv("XDG_CONFIG_HOME", "/tmp/aio-ctl-cfg", 1);
    setenv("XDG_STATE_HOME", "/tmp/aio-ctl-state", 1);
    std::filesystem::remove_all("/tmp/aio-ctl-cfg");
    std::filesystem::remove_all("/tmp/aio-ctl-state");
    if (!aio::tmux({"-V"}).ok()) return;
    aio::tmux({"kill-server"});

    constexpr int port = 47921;
    std::thread server([&] { aio::run_control(port, "127.0.0.1"); });

    httplib::Client anon("127.0.0.1", port);
    anon.set_connection_timeout(0, 300000);
    anon.set_read_timeout(5, 0);
    bool up = false;
    for (int i = 0; i < 50 && !up; ++i) {
        if (auto res = anon.Get("/v1/health"); res && res->status == 401) up = true;
        else std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(up);
    if (up) {
        auto state = anon.Get("/v1/state");
        CHECK(state && state->status == 401);
        httplib::Headers wrong_auth{{"Authorization", "Bearer nope"}};
        auto wrong = anon.Get("/v1/state", wrong_auth);
        CHECK(wrong && wrong->status == 401);
    }

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(0, 300000);
    cli.set_read_timeout(5, 0);
    cli.set_default_headers({{"Authorization", std::string("Bearer ") + kToken}});

    if (up) {
        auto state = cli.Get("/v1/state");
        CHECK(state && state->status == 200);
        auto j = nlohmann::json::parse(state->body, nullptr, false);
        CHECK(j.is_object());
        CHECK(j["agents"].empty());

        auto put = cli.Put("/v1/config", nlohmann::json{{"key_kill", "x"}}.dump(), "application/json");
        CHECK(put && put->status == 200);
        auto again = cli.Get("/v1/state");
        CHECK(again && again->status == 200);
        auto j2 = nlohmann::json::parse(again->body, nullptr, false);
        CHECK_EQ(j2["config"]["key_kill"].get<std::string>(), std::string("x"));

        auto bad = cli.Post("/v1/agents", "{\"name\":\"nope\"}", "application/json");
        CHECK(bad && bad->status == 400);

        auto body = nlohmann::json{{"name", "sleeper"},
                                   {"directory", "/tmp"},
                                   {"command", "sleep 30"},
                                   {"manager", false}};
        auto created = cli.Post("/v1/agents", body.dump(), "application/json");
        if (!created || created->status != 200)
            std::cerr << "create agent: " << (created ? created->body : "no response") << "\n";
        CHECK(created && created->status == 200);

        auto listed = cli.Get("/v1/state");
        auto j3 = nlohmann::json::parse(listed->body, nullptr, false);
        CHECK_EQ(j3["agents"].size(), static_cast<size_t>(1));
        CHECK_EQ(j3["agents"][0]["name"].get<std::string>(), std::string("sleeper"));

        auto stopped = cli.Post("/v1/agents/sleeper/stop", "{}", "application/json");
        CHECK(stopped && stopped->status == 200);
        auto mid = cli.Get("/v1/state");
        auto mid_json = nlohmann::json::parse(mid->body, nullptr, false);
        CHECK_EQ(mid_json["agents"].size(), static_cast<size_t>(1));
        CHECK(mid_json["agents"][0]["dead"].get<bool>());
        auto removed = cli.Delete("/v1/agents/sleeper");
        CHECK(removed && removed->status == 200);
        auto after = cli.Get("/v1/state");
        auto j4 = nlohmann::json::parse(after->body, nullptr, false);
        CHECK(j4["agents"].empty());
    }

    cli.set_read_timeout(2, 0);
    cli.Post("/v1/shutdown", "{}", "application/json");
    server.join();
    aio::tmux({"kill-server"});
}
