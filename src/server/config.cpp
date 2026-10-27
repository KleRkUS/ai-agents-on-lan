#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

#include "platform.hpp"

namespace aio {

namespace {

void parse_int(const std::string& value, int& out, IntRange range) {
    try {
        size_t used = 0;
        int v = std::stoi(value, &used);
        if (used == value.size()) out = std::clamp(v, range.min, range.max);
    } catch (...) {
    }
}

bool parse_bool(const std::string& value, bool fallback) {
    if (value == "true" || value == "1") return true;
    if (value == "false" || value == "0") return false;
    return fallback;
}

std::string range_error(const char* what, IntRange r) {
    return std::string(what) + " must be " + std::to_string(r.min) + "-" + std::to_string(r.max);
}

}  // namespace

std::string config_path() { return Platform::join_path(Platform::config_dir(), "config"); }

Config load_config(const std::string& path) {
    Config config;
    auto contents = Platform::read_file(path);
    if (!contents) return config;

    config.presets.clear();
    auto& m = config.messaging;
    std::istringstream in(*contents);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto key = line.substr(0, eq);
        auto value = line.substr(eq + 1);

        if (key == "default_command") config.default_command = value;
        else if (key == "key_detach") config.key_detach = value;
        else if (key == "key_kill") config.key_kill = value;
        else if (key == "messaging.enabled") m.enabled = parse_bool(value, m.enabled);
        else if (key == "messaging.auto_deliver") m.auto_deliver = parse_bool(value, m.auto_deliver);
        else if (key == "messaging.max_message_chars") parse_int(value, m.max_message_chars, kMessageCharsRange);
        else if (key == "messaging.max_messages_per_minute") parse_int(value, m.max_messages_per_minute, kPerMinuteRange);
        else if (key == "messaging.max_hops") parse_int(value, m.max_hops, kHopsRange);
        else if (key == "messaging.max_inbox") parse_int(value, m.max_inbox, kInboxRange);
        else if (key == "messaging.idle_seconds") parse_int(value, m.idle_seconds, kIdleRange);
        else if (key == "preset") {
            auto bar = value.find('|');
            if (bar != std::string::npos)
                config.presets.push_back({value.substr(0, bar), value.substr(bar + 1)});
        }
    }
    if (!validate_keys(config).empty()) {
        Config defaults;
        config.key_detach = defaults.key_detach;
        config.key_kill = defaults.key_kill;
    }
    return config;
}

bool save_config(const Config& config, const std::string& path) {
    if (!Platform::create_directories(Platform::parent_path(path))) return false;
    std::ostringstream out;
    out << "default_command=" << config.default_command << "\n"
        << "key_detach=" << config.key_detach << "\n"
        << "key_kill=" << config.key_kill << "\n";
    const auto& m = config.messaging;
    out << "messaging.enabled=" << (m.enabled ? "true" : "false") << "\n"
        << "messaging.auto_deliver=" << (m.auto_deliver ? "true" : "false") << "\n"
        << "messaging.max_message_chars=" << m.max_message_chars << "\n"
        << "messaging.max_messages_per_minute=" << m.max_messages_per_minute << "\n"
        << "messaging.max_hops=" << m.max_hops << "\n"
        << "messaging.max_inbox=" << m.max_inbox << "\n"
        << "messaging.idle_seconds=" << m.idle_seconds << "\n";
    for (const auto& p : config.presets) out << "preset=" << p.name << "|" << p.command << "\n";
    return Platform::write_file(path, out.str());
}

std::string validate_keys(const Config& config) {
    const auto& kill = config.key_kill;
    if (kill.size() != 1 || !std::isgraph(static_cast<unsigned char>(kill[0])))
        return "kill key must be a single character";
    if (std::strchr(kReservedKeys, kill[0]))
        return "'" + kill + "' is reserved (" + kReservedKeys + ")";

    // The detach key is active inside agent panes, so it must not be a plain character.
    const auto& detach = config.key_detach;
    bool modified = detach.size() > 2 && (detach.rfind("C-", 0) == 0 || detach.rfind("M-", 0) == 0);
    bool function_key = detach.size() > 1 && detach[0] == 'F' && std::isdigit(detach[1]);
    if ((!modified && !function_key) || detach.find(' ') != std::string::npos)
        return "back-to-list key must be like C-x, M-q or F12";
    return {};
}

std::string validate_preset(const Preset& preset) {
    if (preset.name.empty() || preset.command.empty()) return "preset needs a name and a command";
    if (preset.name.find('|') != std::string::npos) return "preset name cannot contain '|'";
    if ((preset.name + preset.command).find('\n') != std::string::npos)
        return "preset cannot contain line breaks";
    return {};
}

std::string validate_messaging(const MessagingConfig& m) {
    auto in = [](int v, IntRange r) { return v >= r.min && v <= r.max; };
    if (!in(m.max_message_chars, kMessageCharsRange)) return range_error("max message chars", kMessageCharsRange);
    if (!in(m.max_messages_per_minute, kPerMinuteRange)) return range_error("messages per minute", kPerMinuteRange);
    if (!in(m.max_hops, kHopsRange)) return range_error("max hops", kHopsRange);
    if (!in(m.max_inbox, kInboxRange)) return range_error("inbox limit", kInboxRange);
    if (!in(m.idle_seconds, kIdleRange)) return range_error("idle seconds", kIdleRange);
    return {};
}

std::string validate_agent_name(const std::string& name) {
    if (name.empty() || name.size() > 32) return "name must be 1-32 characters";
    if (name[0] == '.') return "name cannot start with '.'";
    for (char c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.')
            return "name may only use letters, digits, '-', '_' and '.'";
    return {};
}

}  // namespace aio
