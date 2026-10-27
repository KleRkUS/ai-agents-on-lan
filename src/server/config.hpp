#pragma once

#include <string>
#include <vector>

namespace aio {

struct Preset {
    std::string name;
    std::string command;
};

// Safeguards for agent-to-agent messaging.
struct MessagingConfig {
    bool enabled = true;  // master switch: servers refuse sends and agents are not wired when off
    // Type a notice into an idle agent's prompt when it has unread messages.
    bool auto_deliver = true;
    int max_message_chars = 4000;
    int max_messages_per_minute = 10;  // per sender
    // Longest reply chain (A->B->A->...) before sends are refused until it cools down.
    int max_hops = 6;
    int max_inbox = 50;    // unread messages per recipient
    int idle_seconds = 3;  // screen unchanged this long counts as idle
};

struct IntRange {
    int min;
    int max;
};
inline constexpr IntRange kMessageCharsRange{100, 100000};
inline constexpr IntRange kPerMinuteRange{1, 600};
inline constexpr IntRange kHopsRange{1, 100};
inline constexpr IntRange kInboxRange{1, 1000};
inline constexpr IntRange kIdleRange{1, 60};

struct Config {
    std::string default_command = "claude";
    std::string key_detach = "C-x";  // tmux key syntax; bound globally, works inside agents
    std::string key_kill = "d";
    std::vector<Preset> presets = {
        {"claude", "claude"},
        {"claude resume", "claude --resume"},
        {"claude continue", "claude --continue"},
        {"cursor", "agent"},
        {"cursor resume", "agent resume"},
        {"codex", "codex"},
    };
    MessagingConfig messaging;
};

// Keys the sidebar uses for itself; shortcuts may not be bound to these.
inline constexpr const char* kReservedKeys = "jklnqs12";

std::string config_path();
Config load_config(const std::string& path = config_path());
bool save_config(const Config& config, const std::string& path = config_path());

// Returns an error message, or empty if the shortcuts are usable.
std::string validate_keys(const Config& config);
std::string validate_preset(const Preset& preset);
std::string validate_messaging(const MessagingConfig& messaging);

// Agent names double as mailbox directory names: 1-32 of [A-Za-z0-9._-], not starting with '.'.
std::string validate_agent_name(const std::string& name);

}  // namespace aio
