#include "mailbox.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <random>
#include <sstream>

#include "platform.hpp"

namespace aio {
namespace {

constexpr int64_t kMinuteMs = 60 * 1000;

std::string make_id(int64_t now_ms, const std::string& from) {
    static std::mt19937 rng{std::random_device{}()};
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%015lld-%08x", static_cast<long long>(now_ms),
                  static_cast<unsigned>(rng()));
    return std::string(buf) + "-" + from;
}

int64_t to_int64(const std::string& s, int64_t fallback = 0) {
    try {
        return std::stoll(s);
    } catch (...) {
        return fallback;
    }
}

}  // namespace

std::string serialize_message(const Message& m) {
    std::ostringstream out;
    std::string audience;
    for (const auto& r : m.recipients) audience += (audience.empty() ? "" : ",") + r;
    out << "id=" << m.id << "\n"
        << "from=" << m.from << "\n"
        << "to=" << m.to << "\n"
        << "recipients=" << audience << "\n"
        << "sent_at_ms=" << m.sent_at_ms << "\n"
        << "hops=" << m.hops << "\n"
        << "\n"
        << m.body;
    return out.str();
}

std::optional<Message> parse_message(const std::string& text) {
    auto split = text.find("\n\n");
    if (split == std::string::npos) return std::nullopt;

    Message m;
    std::istringstream header(text.substr(0, split));
    for (std::string line; std::getline(header, line);) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "id") m.id = value;
        else if (key == "from") m.from = value;
        else if (key == "to") m.to = value;
        else if (key == "recipients") {
            std::string name;
            for (char c : value + ",") {
                if (c == ',') {
                    if (!name.empty()) m.recipients.push_back(name);
                    name.clear();
                } else {
                    name += c;
                }
            }
        } else if (key == "sent_at_ms") m.sent_at_ms = to_int64(value);
        else if (key == "hops") m.hops = static_cast<int>(to_int64(value, 1));
    }
    m.body = text.substr(split + 2);
    if (m.id.empty() || m.from.empty()) return std::nullopt;
    return m;
}

Mailbox::Mailbox(std::string root, MessagingConfig limits, Clock clock)
    : root_(std::move(root)), limits_(limits), clock_(std::move(clock)) {}

int64_t Mailbox::system_clock() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string Mailbox::dir(const std::string& agent, const char* sub) const {
    auto base = Platform::join_path(root_, agent);
    return sub ? Platform::join_path(base, sub) : base;
}

int Mailbox::chain_hops(const std::string& agent) const {
    auto text = Platform::read_file(Platform::join_path(dir(agent), "chain"));
    if (!text) return 0;
    std::istringstream in(*text);
    int hops = 0;
    int64_t at = 0;
    in >> hops >> at;
    return clock_() - at < kChainWindowMs ? hops : 0;
}

std::vector<int64_t> Mailbox::recent_sends(const std::string& agent) const {
    std::vector<int64_t> sends;
    auto text = Platform::read_file(Platform::join_path(dir(agent), "sent"));
    if (!text) return sends;
    std::istringstream in(*text);
    int64_t now = clock_();
    for (int64_t t; in >> t;)
        if (now - t < kMinuteMs) sends.push_back(t);
    return sends;
}

std::string format_message(const std::string& you, const Message& m) {
    std::string others;
    auto audience = m.recipients.empty() ? std::vector<std::string>{m.to} : m.recipients;
    for (const auto& r : audience)
        if (r != you) others += (others.empty() ? "" : ", ") + r;

    std::ostringstream out;
    std::time_t t = static_cast<std::time_t>(m.sent_at_ms / 1000);
    char when[16];
    std::strftime(when, sizeof(when), "%H:%M:%S", std::localtime(&t));

    out << "From: " << m.from << "\n"
        << "To: you (" << you << ")\n";
    if (!others.empty()) out << "Also sent to: " << others << "\n";
    out << "At: " << when << "\n\n" << m.body;
    return out.str();
}

SendResult Mailbox::send(const std::string& from, const std::string& to, const std::string& body,
                         const std::vector<std::string>& members, const std::vector<std::string>& audience,
                         bool count_toward_limit) {
    if (!limits_.enabled) return {{}, "messaging is disabled in aio settings"};
    if (auto err = validate_agent_name(from); !err.empty()) return {{}, "invalid sender: " + err};
    if (auto err = validate_agent_name(to); !err.empty()) return {{}, "invalid recipient: " + err};
    if (from == to) return {{}, "cannot send a message to yourself"};
    if (std::find(members.begin(), members.end(), to) == members.end())
        return {{}, "no agent named '" + to + "' (use list_agents)"};
    if (body.empty()) return {{}, "message is empty"};
    if (static_cast<int>(body.size()) > limits_.max_message_chars)
        return {{}, "message is " + std::to_string(body.size()) + " chars; limit is " +
                        std::to_string(limits_.max_message_chars)};

    auto sends = recent_sends(from);
    if (count_toward_limit && static_cast<int>(sends.size()) >= limits_.max_messages_per_minute)
        return {{}, "rate limit reached (" + std::to_string(limits_.max_messages_per_minute) +
                        " messages per minute); try again later"};

    int hops = chain_hops(from) + 1;
    if (hops > limits_.max_hops)
        return {{}, "reply chain limit reached (" + std::to_string(limits_.max_hops) +
                        " hops); stop and report back to the user instead"};

    if (static_cast<int>(unread_count(to)) >= limits_.max_inbox)
        return {{}, "'" + to + "' has too many unread messages (" +
                        std::to_string(limits_.max_inbox) + ")"};

    auto inbox = dir(to, "inbox");
    if (!Platform::create_directories(inbox) || !Platform::create_directories(dir(from)))
        return {{}, "could not create mailbox directories"};

    int64_t now = clock_();
    Message m{make_id(now, from), from, to, audience.empty() ? std::vector<std::string>{to} : audience, body, now, hops};
    auto tmp = Platform::join_path(inbox, "." + m.id + ".tmp");
    if (!Platform::write_file(tmp, serialize_message(m)) ||
        !Platform::rename_file(tmp, Platform::join_path(inbox, m.id + ".msg"))) {
        Platform::remove_file(tmp);
        return {{}, "could not write message"};
    }

    if (count_toward_limit) {
        sends.push_back(now);
        std::ostringstream log;
        for (auto t : sends) log << t << "\n";
        Platform::write_file(Platform::join_path(dir(from), "sent"), log.str());
    }
    return {m.id, {}};
}

std::vector<Message> Mailbox::unread(const std::string& agent) const {
    std::vector<Message> messages;
    auto inbox = dir(agent, "inbox");
    for (const auto& name : Platform::list_files(inbox)) {
        if (name.empty() || name[0] == '.' || name.size() < 4 || name.substr(name.size() - 4) != ".msg")
            continue;
        auto text = Platform::read_file(Platform::join_path(inbox, name));
        if (!text) continue;
        if (auto m = parse_message(*text)) messages.push_back(std::move(*m));
    }
    return messages;
}

size_t Mailbox::unread_count(const std::string& agent) const { return unread(agent).size(); }

std::vector<Message> Mailbox::read(const std::string& agent) {
    auto messages = unread(agent);
    if (messages.empty()) return messages;

    auto inbox = dir(agent, "inbox");
    auto archive = dir(agent, "read");
    Platform::create_directories(archive);
    int hops = 0;
    for (const auto& m : messages) {
        auto file = m.id + ".msg";
        if (!Platform::rename_file(Platform::join_path(inbox, file), Platform::join_path(archive, file)))
            Platform::remove_file(Platform::join_path(inbox, file));
        hops = std::max(hops, m.hops);
    }
    Platform::write_file(Platform::join_path(dir(agent), "chain"),
                         std::to_string(hops) + " " + std::to_string(clock_()) + "\n");
    return messages;
}

}  // namespace aio
