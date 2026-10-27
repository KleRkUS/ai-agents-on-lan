#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

#include "agents.hpp"
#include "check.hpp"
#include "mcp.hpp"
#include "platform.hpp"
#include "tmux.hpp"

using json = nlohmann::json;

namespace {

aio::McpContext context() {
    return {"team", "alice", "/nonexistent", aio::MessagingConfig{}, {"alice", "bob"}};
}

std::string temp_root(const std::string& name) {
    auto dir = std::filesystem::temp_directory_path() / ("aio-test-mcp-" + name);
    std::filesystem::remove_all(dir);
    return dir.string();
}

std::string tool_text(const aio::HttpReply& reply) {
    return json::parse(reply.body)["result"]["content"][0]["text"].get<std::string>();
}

json call(const std::string& tool, json args = json::object()) {
    return {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
            {"params", {{"name", tool}, {"arguments", args}}}};
}

}  // namespace

TEST(mcp_initialize) {
    auto r = aio::handle_mcp_request(context(), {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
                                                 {"params", {{"protocolVersion", "2025-03-26"}}}});
    CHECK_EQ(r["id"].get<int>(), 1);
    CHECK_EQ(r["result"]["protocolVersion"].get<std::string>(), std::string("2025-03-26"));
    CHECK_EQ(r["result"]["serverInfo"]["name"].get<std::string>(), std::string("aio-team"));
    CHECK(r["result"]["capabilities"].contains("tools"));
}

TEST(mcp_notifications_get_no_response) {
    auto r = aio::handle_mcp_request(context(), {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
    CHECK(r.is_null());
}

TEST(mcp_lists_messaging_tools) {
    auto r = aio::handle_mcp_request(context(), {{"jsonrpc", "2.0"}, {"id", "x"}, {"method", "tools/list"}});
    std::vector<std::string> names;
    for (const auto& t : r["result"]["tools"]) names.push_back(t["name"].get<std::string>());
    CHECK_EQ(names.size(), 3u);
    for (const char* n : {"send_message", "read_messages", "list_agents"})
        CHECK(std::find(names.begin(), names.end(), n) != names.end());
}

TEST(manager_tools_are_hidden_from_workers) {
    auto ctx = context();
    ctx.manager = true;
    auto r = aio::handle_mcp_request(ctx, {{"jsonrpc", "2.0"}, {"id", "x"}, {"method", "tools/list"}});
    std::vector<std::string> names;
    for (const auto& t : r["result"]["tools"]) names.push_back(t["name"].get<std::string>());
    CHECK_EQ(names.size(), 6u);
    for (const char* n : {"stop_agent", "kill_agent", "create_agent"})
        CHECK(std::find(names.begin(), names.end(), n) != names.end());

    auto worker = aio::handle_mcp_request(context(), {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                                                      {"params", {{"name", "stop_agent"}, {"arguments", {{"name", "bob"}}}}}});
    CHECK(worker["result"]["isError"].get<bool>());
    CHECK(worker["result"]["content"][0]["text"].get<std::string>().find("only managers") != std::string::npos);

    auto self = aio::handle_mcp_request(ctx, {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                                              {"params", {{"name", "kill_agent"}, {"arguments", {{"name", "alice"}}}}}});
    auto self_text = self["result"]["content"][0]["text"].get<std::string>();
    CHECK(self_text.find("itself") != std::string::npos);
}

TEST(manager_controls_other_agents) {
    if (!aio::tmux({"-V"}).ok()) return;
    setenv("AIO_TMUX_SOCKET", "aiotest-mgr", 1);
    setenv("XDG_CONFIG_HOME", "/tmp/aiocfg-mgr", 1);
    setenv("XDG_STATE_HOME", "/tmp/aiostate-mgr", 1);
    aio::tmux({"kill-server"});

    CHECK(aio::create_agent("boss", "/tmp", "sleep 60", {}, true, false).empty());
    CHECK(aio::create_agent("worker", "/tmp", "sleep 60", {}, false, false).empty());
    CHECK(aio::create_agent("boss-2", "/tmp", "sleep 60", {}, true, false).empty());
    CHECK(aio::manager_stop("boss", "boss").find("itself") != std::string::npos);
    CHECK(aio::manager_kill("boss", "boss-2").find("another manager") != std::string::npos);
    CHECK(aio::manager_stop("worker", "boss").find("only managers") != std::string::npos);
    CHECK(aio::manager_stop("boss", "worker").empty());

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    bool dead = false;
    for (const auto& a : aio::list_agents())
        if (a.name == "worker") dead = a.dead;
    CHECK(dead);
    CHECK(aio::manager_kill("boss", "worker").empty());

    for (const auto& a : aio::list_agents()) aio::dismiss_agent(a);
    aio::tmux({"kill-server"});
}

TEST(mcp_unknown_method_is_error) {
    auto r = aio::handle_mcp_request(context(), {{"jsonrpc", "2.0"}, {"id", 7}, {"method", "nope"}});
    CHECK_EQ(r["error"]["code"].get<int>(), -32601);
}

TEST(mcp_http_requires_member_token) {
    aio::ServerRegistry registry(temp_root("auth"));
    CHECK(registry.create("team", 47999).empty());
    auto token = registry.add_member("team", "alice");
    CHECK(!token.empty());
    aio::MessagingConfig limits;
    auto body = call("list_agents").dump();

    CHECK_EQ(aio::handle_mcp_http(registry, "team", limits, "", body).status, 401);
    CHECK_EQ(aio::handle_mcp_http(registry, "team", limits, "Bearer wrong", body).status, 401);
    CHECK_EQ(aio::handle_mcp_http(registry, "team", limits, "Bearer " + token, body).status, 200);
    CHECK_EQ(aio::handle_mcp_http(registry, "team", limits, "Bearer " + token, "{bad json").status, 400);

    json note = {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};
    CHECK_EQ(aio::handle_mcp_http(registry, "team", limits, "Bearer " + token, note.dump()).status, 202);
}

TEST(mcp_http_messaging_between_members) {
    aio::ServerRegistry registry(temp_root("messaging"));
    CHECK(registry.create("team", 47998).empty());
    auto alice = "Bearer " + registry.add_member("team", "alice");
    auto bob = "Bearer " + registry.add_member("team", "bob");
    aio::MessagingConfig limits;

    auto listed = tool_text(aio::handle_mcp_http(registry, "team", limits, alice, call("list_agents").dump()));
    CHECK(listed.find("You are alice") != std::string::npos);
    CHECK(listed.find("bob") != std::string::npos);

    auto sent = aio::handle_mcp_http(registry, "team", limits, alice,
                                     call("send_message", {{"to", "bob"}, {"message", "ping"}}).dump());
    CHECK(tool_text(sent).find("sent") != std::string::npos);

    auto outsider = aio::handle_mcp_http(registry, "team", limits, alice,
                                         call("send_message", {{"to", "carol"}, {"message", "x"}}).dump());
    CHECK(json::parse(outsider.body)["result"]["isError"].get<bool>());

    auto read = tool_text(aio::handle_mcp_http(registry, "team", limits, bob, call("read_messages").dump()));
    CHECK(read.find("From: alice") != std::string::npos);
    CHECK(read.find("To: you (bob)") != std::string::npos);
    CHECK(read.find("ping") != std::string::npos);

    auto carol = "Bearer " + registry.add_member("team", "carol");
    auto fan = aio::handle_mcp_http(registry, "team", limits, alice,
                                    call("send_message", {{"to", json::array({"bob", "carol"})}, {"message", "plan"}}).dump());
    auto fan_text = tool_text(fan);
    CHECK(fan_text.find("bob") != std::string::npos);
    CHECK(fan_text.find("carol") != std::string::npos);

    auto bob_copy = tool_text(aio::handle_mcp_http(registry, "team", limits, bob, call("read_messages").dump()));
    CHECK(bob_copy.find("To: you (bob)") != std::string::npos);
    CHECK(bob_copy.find("Also sent to: carol") != std::string::npos);
    CHECK(bob_copy.find("plan") != std::string::npos);
    auto carol_copy = tool_text(aio::handle_mcp_http(registry, "team", limits, carol, call("read_messages").dump()));
    CHECK(carol_copy.find("To: you (carol)") != std::string::npos);
    CHECK(carol_copy.find("Also sent to: bob") != std::string::npos);
    CHECK(carol_copy.find("From: alice") != std::string::npos);
}
