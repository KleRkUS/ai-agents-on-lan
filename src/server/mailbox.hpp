#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "config.hpp"

namespace aio {

struct Message {
    std::string id;
    std::string from;
    std::string to;
    std::vector<std::string> recipients;  // every agent this send was delivered to, including `to`
    std::string body;
    int64_t sent_at_ms = 0;
    int hops = 1;
};

struct SendResult {
    std::string id;     // set on success
    std::string error;  // set on failure

    bool ok() const { return error.empty(); }
};

// File-backed message store for one aio MCP server, also read by the sidebar
// for unread counts. Layout under `root`:
//   <agent>/inbox/<id>.msg   unread messages
//   <agent>/read/<id>.msg    messages already handed to the agent
//   <agent>/sent             send timestamps for rate limiting
//   <agent>/chain            hop count of the reply chain the agent is in
class Mailbox {
public:
    using Clock = std::function<int64_t()>;  // milliseconds since epoch

    // A reply chain resets after this long without reading a message.
    static constexpr int64_t kChainWindowMs = 10 * 60 * 1000;

    Mailbox(std::string root, MessagingConfig limits, Clock clock = system_clock);

    // `members` are the agents that currently exist; sends to anyone else fail.
    // `audience` is everyone this send is going to (recorded in the message so each
    // recipient sees the others). Empty means just `to`. When `count_toward_limit`
    // is false the send is not charged against the per-minute rate limit, so one
    // fan-out counts as a single send.
    SendResult send(const std::string& from, const std::string& to, const std::string& body,
                    const std::vector<std::string>& members, const std::vector<std::string>& audience = {},
                    bool count_toward_limit = true);
    // Returns unread messages oldest-first and moves them to the read archive.
    std::vector<Message> read(const std::string& agent);

    std::vector<Message> unread(const std::string& agent) const;
    size_t unread_count(const std::string& agent) const;

    static int64_t system_clock();

private:
    std::string dir(const std::string& agent, const char* sub = nullptr) const;
    int chain_hops(const std::string& agent) const;
    std::vector<int64_t> recent_sends(const std::string& agent) const;

    std::string root_;
    MessagingConfig limits_;
    Clock clock_;
};

std::string serialize_message(const Message& m);
std::optional<Message> parse_message(const std::string& text);

// One block an agent can act on without any other context: who sent it, that it is
// for `you`, and who else received the same send.
std::string format_message(const std::string& you, const Message& m);

}  // namespace aio
