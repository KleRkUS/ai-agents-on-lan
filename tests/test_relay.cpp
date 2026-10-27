#include "check.hpp"

#include <atomic>
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
#include "host.hpp"
#include "relay.hpp"
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

TEST(host_identity_is_stable) {
    EnvGuard config_home("XDG_CONFIG_HOME");
    setenv("XDG_CONFIG_HOME", "/tmp/aio-host-cfg", 1);
    std::filesystem::remove_all("/tmp/aio-host-cfg");

    auto first = aio::host_identity();
    auto second = aio::host_identity();
    CHECK(!first.id.empty());
    CHECK_EQ(first.id, second.id);
    CHECK_EQ(first.name, second.name);
    CHECK(!first.name.empty());
}

// The relay records machines that hold a join socket open. It does not run agents.
TEST(relay_lists_joined_hosts) {
    EnvGuard token("AIO_ACCESS_TOKEN");
    EnvGuard config_home("XDG_CONFIG_HOME");
    EnvGuard state_home("XDG_STATE_HOME");
    constexpr const char* kToken = "test-token-relay";
    setenv("AIO_ACCESS_TOKEN", kToken, 1);
    setenv("XDG_CONFIG_HOME", "/tmp/aio-relay-cfg", 1);
    setenv("XDG_STATE_HOME", "/tmp/aio-relay-state", 1);
    std::filesystem::remove_all("/tmp/aio-relay-cfg");
    std::filesystem::remove_all("/tmp/aio-relay-state");

    constexpr int port = 47961;
    std::thread server([&] { aio::run_relay(port, "127.0.0.1"); });

    httplib::Client anon("127.0.0.1", port);
    anon.set_connection_timeout(0, 300000);
    anon.set_read_timeout(5, 0);
    bool up = false;
    for (int i = 0; i < 50 && !up; ++i) {
        if (auto res = anon.Get("/v1/hosts"); res && res->status == 401) up = true;
        else std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(up);

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(0, 300000);
    cli.set_read_timeout(5, 0);
    cli.set_default_headers({{"Authorization", std::string("Bearer ") + kToken}});

    if (up) {
        auto wrong = anon.Get("/v1/health");
        CHECK(wrong && wrong->status == 401);

        auto empty = cli.Get("/v1/hosts");
        CHECK(empty && empty->status == 200);
        auto empty_json = nlohmann::json::parse(empty->body, nullptr, false);
        CHECK(empty_json["hosts"].empty());

        httplib::ws::WebSocketClient ws("ws://127.0.0.1:47961/v1/join",
                                        {{"Authorization", std::string("Bearer ") + kToken}});
        CHECK(static_cast<bool>(ws.connect()));
        CHECK(ws.send(nlohmann::json{{"id", "host-a"}, {"name", "workstation"}}.dump()));
        ws.set_read_timeout(std::chrono::seconds(2));
        std::string ack;
        CHECK(ws.read(ack) == httplib::ws::Text);
        auto ack_json = nlohmann::json::parse(ack, nullptr, false);
        CHECK(ack_json.value("ok", false));

        bool listed = false;
        for (int i = 0; i < 20 && !listed; ++i) {
            auto hosts = cli.Get("/v1/hosts");
            if (hosts && hosts->status == 200) {
                auto j = nlohmann::json::parse(hosts->body, nullptr, false);
                if (j["hosts"].size() == 1 && j["hosts"][0].value("id", "") == "host-a" &&
                    j["hosts"][0].value("name", "") == "workstation")
                    listed = true;
            }
            if (!listed) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        CHECK(listed);

        ws.close();
        bool gone = false;
        for (int i = 0; i < 40 && !gone; ++i) {
            auto hosts = cli.Get("/v1/hosts");
            if (hosts && hosts->status == 200) {
                auto j = nlohmann::json::parse(hosts->body, nullptr, false);
                if (j["hosts"].empty()) gone = true;
            }
            if (!gone) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        CHECK(gone);
    }

    cli.set_read_timeout(2, 0);
    cli.Post("/v1/shutdown", "{}", "application/json");
    server.join();
}

// Control calls made to the relay run on the joined machine. Show does not.
TEST(relay_proxies_control_to_joined_host) {
    EnvGuard token("AIO_ACCESS_TOKEN");
    EnvGuard socket("AIO_TMUX_SOCKET");
    EnvGuard config_home("XDG_CONFIG_HOME");
    EnvGuard state_home("XDG_STATE_HOME");
    constexpr const char* kToken = "test-token-proxy";
    setenv("AIO_ACCESS_TOKEN", kToken, 1);
    setenv("AIO_TMUX_SOCKET", "aiotest-proxy", 1);
    setenv("XDG_CONFIG_HOME", "/tmp/aio-proxy-cfg", 1);
    setenv("XDG_STATE_HOME", "/tmp/aio-proxy-state", 1);
    std::filesystem::remove_all("/tmp/aio-proxy-cfg");
    std::filesystem::remove_all("/tmp/aio-proxy-state");
    if (!aio::tmux({"-V"}).ok()) return;
    aio::tmux({"kill-server"});

    constexpr int relay_port = 47971;
    constexpr int host_port = 47972;
    std::thread relay([&] { aio::run_relay(relay_port, "127.0.0.1"); });
    std::thread host([&] { aio::run_control(host_port, "127.0.0.1"); });

    httplib::Client via("127.0.0.1", relay_port);
    via.set_connection_timeout(0, 300000);
    via.set_read_timeout(10, 0);
    via.set_default_headers({{"Authorization", std::string("Bearer ") + kToken}});
    httplib::Client direct("127.0.0.1", host_port);
    direct.set_connection_timeout(0, 300000);
    direct.set_read_timeout(10, 0);
    direct.set_default_headers({{"Authorization", std::string("Bearer ") + kToken}});

    bool up = false;
    for (int i = 0; i < 50 && !up; ++i) {
        auto relay_up = via.Get("/v1/hosts");
        auto host_up = direct.Get("/v1/health");
        if (relay_up && relay_up->status == 200 && host_up && host_up->status == 200) up = true;
        else std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(up);

    std::thread bridge;
    httplib::ws::WebSocketClient ws("ws://127.0.0.1:47971/v1/join",
                                    {{"Authorization", std::string("Bearer ") + kToken}});
    if (up) {
        auto missing = via.Get("/v1/state");
        CHECK(missing && missing->status == 503);

        CHECK(static_cast<bool>(ws.connect()));
        CHECK(ws.send(nlohmann::json{{"id", "host-a"}, {"name", "workstation"}}.dump()));
        ws.set_read_timeout(std::chrono::seconds(2));
        std::string ack;
        CHECK(ws.read(ack) == httplib::ws::Text);
        bridge = std::thread([&] { aio::answer_relay_calls(ws, "127.0.0.1", host_port); });

        auto put = via.Put("/v1/config", nlohmann::json{{"key_kill", "x"}}.dump(), "application/json");
        CHECK(put && put->status == 200);
        auto state = via.Get("/v1/state");
        CHECK(state && state->status == 200);
        auto j = nlohmann::json::parse(state ? state->body : "", nullptr, false);
        CHECK_EQ(j["config"]["key_kill"].get<std::string>(), std::string("x"));
        CHECK(j["agents"].empty());

        auto body = nlohmann::json{{"name", "sleeper"}, {"directory", "/tmp"}, {"command", "sleep 30"}, {"manager", false}};
        auto created = via.Post("/v1/agents", body.dump(), "application/json");
        if (!created || created->status != 200)
            std::cerr << "create via relay: " << (created ? created->body : "no response") << "\n";
        CHECK(created && created->status == 200);

        auto listed = via.Get("/v1/hosts/host-a/state");
        CHECK(listed && listed->status == 200);
        auto listed_json = nlohmann::json::parse(listed ? listed->body : "", nullptr, false);
        CHECK_EQ(listed_json["agents"].size(), static_cast<size_t>(1));

        auto shown = via.Post("/v1/agents/sleeper/show", "{}", "application/json");
        CHECK(shown && shown->status == 200);
        auto on_host = direct.Get("/v1/state");
        CHECK(on_host && on_host->status == 200);
        auto host_json = nlohmann::json::parse(on_host ? on_host->body : "", nullptr, false);
        CHECK_EQ(host_json["viewport"].get<std::string>(), std::string(""));

        auto stopped = via.Post("/v1/agents/sleeper/stop", "{}", "application/json");
        CHECK(stopped && stopped->status == 200);
        auto removed = via.Delete("/v1/agents/sleeper");
        CHECK(removed && removed->status == 200);
    }

    ws.close();
    if (bridge.joinable()) bridge.join();
    direct.set_read_timeout(2, 0);
    via.set_read_timeout(2, 0);
    direct.Post("/v1/shutdown", "{}", "application/json");
    via.Post("/v1/shutdown", "{}", "application/json");
    host.join();
    relay.join();
    aio::tmux({"kill-server"});
}

// Terminal bytes travel relay -> joined host -> pane, and back.
TEST(relay_proxies_terminal) {
    EnvGuard token("AIO_ACCESS_TOKEN");
    EnvGuard socket("AIO_TMUX_SOCKET");
    EnvGuard config_home("XDG_CONFIG_HOME");
    EnvGuard state_home("XDG_STATE_HOME");
    constexpr const char* kToken = "test-token-rterm";
    setenv("AIO_ACCESS_TOKEN", kToken, 1);
    setenv("AIO_TMUX_SOCKET", "aiotest-rterm", 1);
    setenv("XDG_CONFIG_HOME", "/tmp/aio-rterm-cfg", 1);
    setenv("XDG_STATE_HOME", "/tmp/aio-rterm-state", 1);
    std::filesystem::remove_all("/tmp/aio-rterm-cfg");
    std::filesystem::remove_all("/tmp/aio-rterm-state");
    if (!aio::tmux({"-V"}).ok()) return;
    aio::tmux({"kill-server"});

    constexpr int relay_port = 47981;
    constexpr int host_port = 47982;
    std::thread relay([&] { aio::run_relay(relay_port, "127.0.0.1"); });
    std::thread host([&] { aio::run_control(host_port, "127.0.0.1"); });

    httplib::Client via("127.0.0.1", relay_port);
    via.set_connection_timeout(0, 300000);
    via.set_read_timeout(10, 0);
    via.set_default_headers({{"Authorization", std::string("Bearer ") + kToken}});
    httplib::Client direct("127.0.0.1", host_port);
    direct.set_connection_timeout(0, 300000);
    direct.set_read_timeout(10, 0);
    direct.set_default_headers({{"Authorization", std::string("Bearer ") + kToken}});

    bool up = false;
    for (int i = 0; i < 50 && !up; ++i) {
        auto relay_up = via.Get("/v1/health");
        auto host_up = direct.Get("/v1/health");
        if (relay_up && relay_up->status == 200 && host_up && host_up->status == 200) up = true;
        else std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(up);

    std::thread bridge;
    httplib::ws::WebSocketClient join("ws://127.0.0.1:47981/v1/join",
                                      {{"Authorization", std::string("Bearer ") + kToken}});
    httplib::ws::WebSocketClient view("ws://127.0.0.1:47981/v1/terminal/agents/sleeper",
                                      {{"Authorization", std::string("Bearer ") + kToken}});
    if (up) {
        CHECK(static_cast<bool>(join.connect()));
        CHECK(join.send(nlohmann::json{{"id", "host-a"}, {"name", "workstation"}}.dump()));
        join.set_read_timeout(std::chrono::seconds(2));
        std::string ack;
        CHECK(join.read(ack) == httplib::ws::Text);
        bridge = std::thread([&] { aio::answer_relay_calls(join, "127.0.0.1", host_port); });

        auto body = nlohmann::json{{"name", "sleeper"},
                                   {"directory", "/tmp"},
                                   {"command", "printf 'hello-attach\\n'; cat"},
                                   {"manager", false}};
        auto created = via.Post("/v1/agents", body.dump(), "application/json");
        if (!created || created->status != 200)
            std::cerr << "create via relay: " << (created ? created->body : "no response") << "\n";
        CHECK(created && created->status == 200);

        bool printed = false;
        for (int i = 0; i < 30 && !printed; ++i) {
            for (const auto& agent : aio::list_agents())
                if (agent.name == "sleeper" &&
                    aio::capture_pane(agent.pane_id).find("hello-attach") != std::string::npos)
                    printed = true;
            if (!printed) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        CHECK(printed);

        httplib::ws::WebSocketClient denied("ws://127.0.0.1:47981/v1/terminal/agents/sleeper");
        CHECK(!static_cast<bool>(denied.connect()));

        CHECK(static_cast<bool>(view.connect()));
        view.set_read_timeout(std::chrono::milliseconds(200));
        view.send(nlohmann::json{{"cols", 80}, {"rows", 24}}.dump());

        std::string got;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline && got.find("hello-attach") == std::string::npos) {
            std::string msg;
            auto result = view.read(msg);
            if (result == httplib::ws::Fail) break;
            if (result == httplib::ws::Timeout) continue;
            got += msg;
        }
        if (got.find("hello-attach") == std::string::npos) std::cerr << "terminal bytes: " << got << "\n";
        CHECK(got.find("hello-attach") != std::string::npos);

        const char typed[] = "xyz\n";
        CHECK(view.send(typed, sizeof(typed) - 1));
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline && got.find("xyz") == std::string::npos) {
            std::string msg;
            auto result = view.read(msg);
            if (result == httplib::ws::Fail) break;
            if (result == httplib::ws::Timeout) continue;
            got += msg;
        }
        if (got.find("xyz") == std::string::npos) std::cerr << "after input: " << got << "\n";
        CHECK(got.find("xyz") != std::string::npos);
        view.close();

        httplib::ws::WebSocketClient again("ws://127.0.0.1:47981/v1/hosts/host-a/terminal/agents/sleeper",
                                           {{"Authorization", std::string("Bearer ") + kToken}});
        CHECK(static_cast<bool>(again.connect()));
        again.set_read_timeout(std::chrono::milliseconds(200));
        again.send(nlohmann::json{{"cols", 80}, {"rows", 24}}.dump());
        std::string via_host;
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline && via_host.find("hello-attach") == std::string::npos) {
            std::string msg;
            auto result = again.read(msg);
            if (result == httplib::ws::Fail) break;
            if (result == httplib::ws::Timeout) continue;
            via_host += msg;
        }
        if (via_host.find("hello-attach") == std::string::npos) std::cerr << "prefixed terminal: " << via_host << "\n";
        CHECK(via_host.find("hello-attach") != std::string::npos);
        again.close();

        auto removed = via.Delete("/v1/agents/sleeper");
        CHECK(removed && removed->status == 200);
    }

    view.close();
    join.close();
    if (bridge.joinable()) bridge.join();
    direct.set_read_timeout(2, 0);
    via.set_read_timeout(2, 0);
    direct.Post("/v1/shutdown", "{}", "application/json");
    via.Post("/v1/shutdown", "{}", "application/json");
    host.join();
    relay.join();
    aio::tmux({"kill-server"});
}

// With two joined machines the short paths are refused. /v1/hosts/<id>/... picks one.
TEST(relay_selects_one_of_several_hosts) {
    EnvGuard token("AIO_ACCESS_TOKEN");
    EnvGuard socket("AIO_TMUX_SOCKET");
    EnvGuard config_home("XDG_CONFIG_HOME");
    EnvGuard state_home("XDG_STATE_HOME");
    constexpr const char* kToken = "test-token-pick";
    setenv("AIO_ACCESS_TOKEN", kToken, 1);
    setenv("AIO_TMUX_SOCKET", "aiotest-pick", 1);
    setenv("XDG_CONFIG_HOME", "/tmp/aio-pick-cfg", 1);
    setenv("XDG_STATE_HOME", "/tmp/aio-pick-state", 1);
    std::filesystem::remove_all("/tmp/aio-pick-cfg");
    std::filesystem::remove_all("/tmp/aio-pick-state");
    if (!aio::tmux({"-V"}).ok()) return;
    aio::tmux({"kill-server"});

    constexpr int relay_port = 47991;
    constexpr int host_port = 47992;
    std::thread relay([&] { aio::run_relay(relay_port, "127.0.0.1"); });
    std::thread host([&] { aio::run_control(host_port, "127.0.0.1"); });

    httplib::Client via("127.0.0.1", relay_port);
    via.set_connection_timeout(0, 300000);
    via.set_read_timeout(10, 0);
    via.set_default_headers({{"Authorization", std::string("Bearer ") + kToken}});
    httplib::Client direct("127.0.0.1", host_port);
    direct.set_connection_timeout(0, 300000);
    direct.set_read_timeout(10, 0);
    direct.set_default_headers({{"Authorization", std::string("Bearer ") + kToken}});

    bool up = false;
    for (int i = 0; i < 50 && !up; ++i) {
        auto relay_up = via.Get("/v1/health");
        auto host_up = direct.Get("/v1/health");
        if (relay_up && relay_up->status == 200 && host_up && host_up->status == 200) up = true;
        else std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(up);

    auto stub = [](httplib::ws::WebSocketClient& ws) {
        std::atomic<bool> alive{true};
        std::thread ping([&] {
            while (alive && ws.is_open()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                if (!alive || !ws.is_open()) break;
                if (!ws.send(nlohmann::json{{"op", "ping"}}.dump())) break;
            }
        });
        ws.set_read_timeout(0, 0);
        while (alive && ws.is_open()) {
            std::string msg;
            auto kind = ws.read(msg);
            if (kind == httplib::ws::Fail) break;
            if (kind != httplib::ws::Text) continue;
            auto call = nlohmann::json::parse(msg, nullptr, false);
            if (!call.is_object() || call.value("op", "") != "call") continue;
            auto body = nlohmann::json{{"host", {{"id", "host-b"}, {"name", "other"}}}}.dump();
            auto frame = nlohmann::json{{"op", "result"}, {"id", call["id"]}, {"status", 200}, {"body", body}}.dump();
            if (!ws.send(frame)) break;
        }
        alive = false;
        ping.join();
    };

    std::thread bridge_a;
    std::thread bridge_b;
    httplib::ws::WebSocketClient join_a("ws://127.0.0.1:47991/v1/join",
                                        {{"Authorization", std::string("Bearer ") + kToken}});
    httplib::ws::WebSocketClient join_b("ws://127.0.0.1:47991/v1/join",
                                        {{"Authorization", std::string("Bearer ") + kToken}});
    if (up) {
        CHECK(static_cast<bool>(join_a.connect()));
        CHECK(join_a.send(nlohmann::json{{"id", "host-a"}, {"name", "workstation"}}.dump()));
        join_a.set_read_timeout(std::chrono::seconds(2));
        std::string ack;
        CHECK(join_a.read(ack) == httplib::ws::Text);
        bridge_a = std::thread([&] { aio::answer_relay_calls(join_a, "127.0.0.1", host_port); });

        CHECK(static_cast<bool>(join_b.connect()));
        CHECK(join_b.send(nlohmann::json{{"id", "host-b"}, {"name", "other"}}.dump()));
        join_b.set_read_timeout(std::chrono::seconds(2));
        CHECK(join_b.read(ack) == httplib::ws::Text);
        bridge_b = std::thread([&] { stub(join_b); });

        auto plain = via.Get("/v1/state");
        CHECK(plain && plain->status == 400);
        auto plain_json = nlohmann::json::parse(plain ? plain->body : "", nullptr, false);
        CHECK_EQ(plain_json.value("error", ""), std::string("more than one host is joined"));

        auto listed = via.Get("/v1/hosts");
        CHECK(listed && listed->status == 200);
        auto listed_json = nlohmann::json::parse(listed ? listed->body : "", nullptr, false);
        CHECK_EQ(listed_json["hosts"].size(), static_cast<size_t>(2));

        auto first = via.Get("/v1/hosts/host-a/state");
        CHECK(first && first->status == 200);
        auto first_json = nlohmann::json::parse(first ? first->body : "", nullptr, false);
        CHECK(first_json.contains("agents"));

        auto second = via.Get("/v1/hosts/host-b/state");
        CHECK(second && second->status == 200);
        auto second_json = nlohmann::json::parse(second ? second->body : "", nullptr, false);
        CHECK_EQ(second_json["host"]["name"].get<std::string>(), std::string("other"));
    }

    join_a.close();
    join_b.close();
    if (bridge_a.joinable()) bridge_a.join();
    if (bridge_b.joinable()) bridge_b.join();
    direct.set_read_timeout(2, 0);
    via.set_read_timeout(2, 0);
    direct.Post("/v1/shutdown", "{}", "application/json");
    via.Post("/v1/shutdown", "{}", "application/json");
    host.join();
    relay.join();
    aio::tmux({"kill-server"});
}
