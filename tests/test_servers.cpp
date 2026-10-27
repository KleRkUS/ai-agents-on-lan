#include <filesystem>

#include "check.hpp"
#include "servers.hpp"

namespace {

aio::ServerRegistry registry(const std::string& name) {
    auto dir = std::filesystem::temp_directory_path() / ("aio-test-servers-" + name);
    std::filesystem::remove_all(dir);
    return aio::ServerRegistry(dir.string());
}

}  // namespace

TEST(servers_validate_names) {
    CHECK(aio::validate_server_name("backend-team").empty());
    CHECK(aio::validate_server_name("x1").empty());
    CHECK(!aio::validate_server_name("").empty());
    CHECK(!aio::validate_server_name("Upper").empty());
    CHECK(!aio::validate_server_name("dots.bad").empty());
    CHECK(!aio::validate_server_name("-edge").empty());
    CHECK(!aio::validate_server_name("../up").empty());
}

TEST(servers_create_list_remove) {
    auto reg = registry("crud");
    CHECK(reg.list().empty());
    CHECK_EQ(reg.next_free_port(), aio::kFirstPort);

    CHECK(reg.create("alpha", 47800).empty());
    CHECK(reg.create("beta", 47801).empty());
    CHECK(!reg.create("alpha", 47900).empty());   // duplicate name
    CHECK(!reg.create("gamma", 47801).empty());   // duplicate port
    CHECK(!reg.create("delta", 80).empty());      // privileged port
    CHECK_EQ(reg.list().size(), 2u);
    CHECK_EQ(reg.next_free_port(), 47802);

    CHECK(reg.remove("alpha"));
    CHECK_EQ(reg.list().size(), 1u);
    CHECK(!reg.get("alpha").has_value());
}

TEST(servers_membership) {
    auto reg = registry("members");
    CHECK(reg.create("alpha", 47800).empty());
    CHECK(reg.create("beta", 47801).empty());
    auto t1 = reg.add_member("alpha", "agent-1");
    auto t2 = reg.add_member("beta", "agent-1");
    CHECK(!t1.empty());
    CHECK(t1 != t2);
    CHECK(reg.add_member("missing", "agent-1").empty());
    CHECK_EQ(reg.get("alpha")->members.at("agent-1"), t1);

    reg.remove_member_everywhere("agent-1");
    CHECK(reg.get("alpha")->members.empty());
    CHECK(reg.get("beta")->members.empty());
}

TEST(servers_get_distinct_colors) {
    auto reg = registry("colors");
    CHECK(reg.create("alpha", 47800).empty());
    CHECK(reg.create("beta", 47801).empty());
    auto a = reg.get("alpha")->color, b = reg.get("beta")->color;
    CHECK_EQ(a, std::string(aio::kServerPalette[0]));
    CHECK_EQ(b, std::string(aio::kServerPalette[1]));

    reg.remove("alpha");
    CHECK(reg.create("gamma", 47802).empty());
    CHECK_EQ(reg.get("gamma")->color, std::string(aio::kServerPalette[0]));  // freed color reused

    reg.add_member("beta", "agent-1");
    CHECK_EQ(reg.get("beta")->color, b);  // saving members keeps the color
}

TEST(servers_info_helpers) {
    aio::ServerInfo s{"backend-team", 47805, {}, ""};
    CHECK_EQ(s.url(), std::string("http://127.0.0.1:47805/mcp"));
    CHECK_EQ(s.mcp_name(), std::string("aio-backend-team"));
    CHECK_EQ(s.token_env(), std::string("AIO_TOKEN_BACKEND_TEAM"));
}

TEST(servers_last_error) {
    auto reg = registry("errors");
    CHECK(reg.create("alpha", 47800).empty());
    CHECK(reg.last_error("alpha").empty());
    reg.set_last_error("alpha", "port 47800 is unavailable");
    CHECK_EQ(reg.last_error("alpha"), std::string("port 47800 is unavailable"));
    reg.set_last_error("alpha", "");
    CHECK(reg.last_error("alpha").empty());
}
