#include <filesystem>

#include "adapters.hpp"
#include "check.hpp"
#include "platform.hpp"

using json = nlohmann::json;

namespace {

std::string temp_root(const std::string& name) {
    auto dir = std::filesystem::temp_directory_path() / ("aio-test-adapters-" + name);
    std::filesystem::remove_all(dir);
    return dir.string();
}

aio::AdapterSet builtins() { return aio::load_adapters(temp_root("builtins")); }

json read_json(const std::string& path) {
    return json::parse(aio::Platform::read_file(path).value_or("{}"), nullptr, false);
}

}  // namespace

TEST(adapters_seed_builtins_once) {
    auto dir = temp_root("seed");
    auto set = aio::load_adapters(dir);
    CHECK_EQ(set.adapters.size(), aio::builtin_adapters().size());
    CHECK(set.errors.empty());
    aio::Platform::remove_file(aio::Platform::join_path(dir, "codex.json"));
    auto again = aio::load_adapters(dir);  // a deleted built-in stays deleted
    CHECK_EQ(again.adapters.size(), aio::builtin_adapters().size() - 1);
}

TEST(adapters_report_bad_files) {
    auto dir = temp_root("bad");
    aio::Platform::create_directories(dir);
    aio::Platform::write_file(aio::Platform::join_path(dir, "broken.json"), "{ nope");
    aio::Platform::write_file(aio::Platform::join_path(dir, "typo.json"), R"({"match":["x"],"argz":[]})");
    aio::Platform::write_file(aio::Platform::join_path(dir, "escape.json"),
                              R"({"match":["x"],"project_file":{"path":"../x.json","content":{}}})");
    auto set = aio::load_adapters(dir);
    CHECK(set.adapters.empty());
    CHECK_EQ(set.errors.size(), 3u);
}

TEST(wire_claude_writes_config_file) {
    auto state = temp_root("wire-claude");
    aio::ServerInfo team{"team", 47801, {}, ""};
    aio::ServerInfo ops{"ops", 47802, {}, ""};
    auto w = aio::wire_messaging(builtins(), "alice", "claude --resume", "/tmp", {{team, "tok1"}, {ops, "tok2"}}, state);
    CHECK(w.wired);
    CHECK(w.error.empty());
    CHECK(w.command.rfind("claude --resume --mcp-config ", 0) == 0);
    CHECK(w.command.find("--allowedTools mcp__aio-team,mcp__aio-ops") != std::string::npos);

    auto config = read_json(aio::Platform::join_path(state, "alice.mcp.json"));
    CHECK_EQ(config["mcpServers"]["aio-team"]["url"].get<std::string>(), std::string("http://127.0.0.1:47801/mcp"));
    CHECK_EQ(config["mcpServers"]["aio-ops"]["headers"]["Authorization"].get<std::string>(), std::string("Bearer tok2"));
}

TEST(wire_codex_uses_env_tokens) {
    aio::ServerInfo team{"my-team", 47801, {}, ""};
    auto w = aio::wire_messaging(builtins(), "alice", "codex resume", "/tmp", {{team, "tok"}}, temp_root("wire-codex"));
    CHECK(w.wired);
    CHECK(w.command.rfind("codex -c ", 0) == 0);
    CHECK(w.command.find("mcp_servers.aio-my-team.url=") != std::string::npos);
    CHECK(w.command.find("AIO_TOKEN_MY_TEAM") != std::string::npos);
    CHECK(w.command.find("tok\"") == std::string::npos);  // token travels via env, not argv
    CHECK(w.command.size() > 7 && w.command.substr(w.command.size() - 7) == " resume");
    CHECK_EQ(w.env.size(), 1u);
    if (!w.env.empty()) {
        CHECK_EQ(w.env[0].first, std::string("AIO_TOKEN_MY_TEAM"));
        CHECK_EQ(w.env[0].second, std::string("tok"));
    }
}

TEST(wire_cursor_merges_project_file) {
    auto work = temp_root("wire-cursor");
    auto file = aio::Platform::join_path(aio::Platform::join_path(work, ".cursor"), "mcp.json");
    aio::Platform::create_directories(aio::Platform::parent_path(file));
    aio::Platform::write_file(file, R"({"mcpServers":{"mine":{"command":"x"}},"other":1})");
    aio::ServerInfo team{"team", 47801, {}, ""};
    auto w = aio::wire_messaging(builtins(), "bob", "agent", work, {{team, "tok"}}, temp_root("wire-cursor-state"));
    CHECK(w.wired);
    CHECK_EQ(w.command, std::string("agent --approve-mcps"));
    auto config = read_json(file);
    CHECK_EQ(config["other"].get<int>(), 1);
    CHECK(config["mcpServers"].contains("mine"));
    CHECK_EQ(config["mcpServers"]["aio-team"]["headers"]["Authorization"].get<std::string>(), std::string("Bearer tok"));
}

TEST(traces_removed_on_kill) {
    auto set = builtins();
    auto state = temp_root("traces-state");
    aio::ServerInfo team{"team", 47801, {}, ""};
    aio::ServerInfo ops{"ops", 47802, {}, ""};

    // claude: generated config file disappears
    aio::wire_messaging(set, "alice", "claude", "/tmp", {{team, "t"}}, state);
    auto claude_file = aio::Platform::join_path(state, "alice.mcp.json");
    CHECK(aio::Platform::read_file(claude_file).has_value());
    aio::remove_agent_traces("alice", state);
    CHECK(!aio::Platform::read_file(claude_file).has_value());
    CHECK(aio::traced_agents(state).empty());

    // cursor in a fresh project: file and .cursor dir vanish once the last agent goes
    auto work = temp_root("traces-fresh");
    aio::Platform::create_directories(work);
    auto file = aio::Platform::join_path(aio::Platform::join_path(work, ".cursor"), "mcp.json");
    aio::wire_messaging(set, "bob", "agent", work, {{team, "t1"}}, state);
    aio::wire_messaging(set, "carol", "agent", work, {{team, "t2"}, {ops, "t3"}}, state);
    aio::remove_agent_traces("carol", state);
    auto doc = read_json(file);
    CHECK(doc["mcpServers"].contains("aio-team"));  // bob still uses it
    CHECK(!doc["mcpServers"].contains("aio-ops"));
    aio::remove_agent_traces("bob", state);
    CHECK(!aio::Platform::read_file(file).has_value());
    CHECK(!aio::Platform::is_directory(aio::Platform::parent_path(file)));

    // existing user file: only aio's entries are removed
    auto user = temp_root("traces-user");
    auto user_file = aio::Platform::join_path(aio::Platform::join_path(user, ".cursor"), "mcp.json");
    aio::Platform::create_directories(aio::Platform::parent_path(user_file));
    aio::Platform::write_file(user_file, R"({"mcpServers":{"mine":{"command":"x"}}})");
    aio::wire_messaging(set, "dave", "agent", user, {{team, "t"}}, state);
    aio::remove_agent_traces("dave", state);
    CHECK_EQ(read_json(user_file), json::parse(R"({"mcpServers":{"mine":{"command":"x"}}})"));
}

TEST(wire_custom_adapter_and_unknown_cli) {
    auto dir = temp_root("custom");
    aio::Platform::create_directories(dir);
    aio::Platform::write_file(aio::Platform::join_path(dir, "tool.json"),
                              R"({"match":["tool"],"args_per_server":["--mcp","{url}"],"env":{"WHO":"{agent}"}})");
    auto set = aio::load_adapters(dir);
    aio::ServerInfo team{"team", 47801, {}, ""};
    auto w = aio::wire_messaging(set, "eve", "/opt/bin/tool run", "/tmp", {{team, "t"}}, temp_root("custom-state"));
    CHECK(w.wired);
    CHECK_EQ(w.command, std::string("/opt/bin/tool run --mcp http://127.0.0.1:47801/mcp"));
    CHECK_EQ(w.env.size(), 1u);

    auto other = aio::wire_messaging(set, "eve", "aider --model x", "/tmp", {{team, "t"}}, temp_root("custom-state"));
    CHECK(!other.wired);
    CHECK(!other.error.empty());
    CHECK(!aio::wire_messaging(set, "eve", "tool", "/tmp", {}, temp_root("custom-state")).wired);
}
