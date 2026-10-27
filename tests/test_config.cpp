#include <filesystem>

#include "check.hpp"
#include "config.hpp"
#include "platform.hpp"

TEST(config_defaults_are_valid) {
    aio::Config config;
    CHECK(aio::validate_keys(config).empty());
    CHECK(!config.presets.empty());
}

TEST(config_rejects_bad_keys) {
    aio::Config config;
    config.key_kill = "j";
    CHECK(!aio::validate_keys(config).empty());

    config = {};
    config.key_kill = "dd";
    CHECK(!aio::validate_keys(config).empty());

    config = {};
    config.key_detach = "q";
    CHECK(!aio::validate_keys(config).empty());

    config = {};
    config.key_detach = "C-";
    CHECK(!aio::validate_keys(config).empty());
}

TEST(config_accepts_modifier_and_function_keys) {
    aio::Config config;
    for (const char* key : {"C-x", "M-q", "F12"}) {
        config.key_detach = key;
        CHECK(aio::validate_keys(config).empty());
    }
}

TEST(config_save_load_roundtrip) {
    auto dir = std::filesystem::temp_directory_path() / "aio-test-config";
    std::filesystem::remove_all(dir);
    auto path = (dir / "config").string();

    aio::Config config;
    config.default_command = "codex";
    config.key_detach = "M-q";
    config.key_kill = "x";
    config.presets = {{"one", "echo 1"}, {"two", "a | b"}};
    CHECK(aio::save_config(config, path));

    auto loaded = aio::load_config(path);
    CHECK_EQ(loaded.default_command, std::string("codex"));
    CHECK_EQ(loaded.key_detach, std::string("M-q"));
    CHECK_EQ(loaded.key_kill, std::string("x"));
    CHECK_EQ(loaded.presets.size(), 2u);
    if (loaded.presets.size() == 2) CHECK_EQ(loaded.presets[1].command, std::string("a | b"));
}

TEST(config_validates_presets) {
    CHECK(aio::validate_preset({"claude", "claude --resume"}).empty());
    CHECK(aio::validate_preset({"a | b", "cmd | grep x"}) != "");
    CHECK(aio::validate_preset({"", "cmd"}) != "");
    CHECK(aio::validate_preset({"name", ""}) != "");
}

TEST(config_messaging_roundtrip_and_validation) {
    auto dir = std::filesystem::temp_directory_path() / "aio-test-config-messaging";
    std::filesystem::remove_all(dir);
    auto path = (dir / "config").string();

    aio::Config config;
    config.messaging.enabled = false;
    config.messaging.max_hops = 9;
    config.messaging.idle_seconds = 7;
    CHECK(aio::save_config(config, path));
    auto loaded = aio::load_config(path);
    CHECK(!loaded.messaging.enabled);
    CHECK_EQ(loaded.messaging.max_hops, 9);
    CHECK_EQ(loaded.messaging.idle_seconds, 7);

    CHECK(aio::validate_messaging(aio::MessagingConfig{}).empty());
    aio::MessagingConfig bad;
    bad.max_hops = 0;
    CHECK(!aio::validate_messaging(bad).empty());

    CHECK(aio::Platform::write_file(path, "messaging.max_hops=99999\nmessaging.idle_seconds=abc\n"));
    loaded = aio::load_config(path);
    CHECK_EQ(loaded.messaging.max_hops, aio::kHopsRange.max);
    CHECK_EQ(loaded.messaging.idle_seconds, aio::MessagingConfig{}.idle_seconds);
}

TEST(config_validates_agent_names) {
    CHECK(aio::validate_agent_name("claude-1").empty());
    CHECK(aio::validate_agent_name("api_v2.worker").empty());
    CHECK(!aio::validate_agent_name("").empty());
    CHECK(!aio::validate_agent_name("../etc").empty());
    CHECK(!aio::validate_agent_name("has space").empty());
    CHECK(!aio::validate_agent_name(".hidden").empty());
    CHECK(!aio::validate_agent_name(std::string(33, 'a')).empty());
}

TEST(config_invalid_keys_fall_back_to_defaults) {
    auto dir = std::filesystem::temp_directory_path() / "aio-test-config-bad";
    std::filesystem::create_directories(dir);
    auto path = (dir / "config").string();
    CHECK(aio::Platform::write_file(path, "key_detach=q\nkey_kill=d\r\npreset=a|b\n"));

    auto loaded = aio::load_config(path);
    aio::Config defaults;
    CHECK_EQ(loaded.key_detach, defaults.key_detach);
    CHECK_EQ(loaded.presets.size(), 1u);
}
