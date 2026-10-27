#include <filesystem>

#include "check.hpp"
#include "mailbox.hpp"

namespace {

struct Fixture {
    std::string root;
    int64_t now = 1'000'000;
    aio::MessagingConfig limits;
    std::vector<std::string> agents{"alice", "bob", "carol"};

    explicit Fixture(const std::string& name) {
        auto dir = std::filesystem::temp_directory_path() / ("aio-test-mail-" + name);
        std::filesystem::remove_all(dir);
        root = dir.string();
    }
    aio::Mailbox box() {
        return aio::Mailbox(root, limits, [this] { return now; });
    }
};

}  // namespace

TEST(mailbox_send_and_read) {
    Fixture f("basic");
    auto box = f.box();
    CHECK(box.send("alice", "bob", "hello bob", f.agents).ok());
    f.now += 5;
    CHECK(box.send("carol", "bob", "hi from carol", f.agents).ok());
    CHECK_EQ(box.unread_count("bob"), 2u);

    auto messages = box.read("bob");
    CHECK_EQ(messages.size(), 2u);
    if (messages.size() == 2) {
        CHECK_EQ(messages[0].from, std::string("alice"));
        CHECK_EQ(messages[0].body, std::string("hello bob"));
        CHECK_EQ(messages[1].from, std::string("carol"));
    }
    CHECK_EQ(box.unread_count("bob"), 0u);
    CHECK(box.read("bob").empty());
}

TEST(mailbox_multiline_body_roundtrip) {
    Fixture f("multiline");
    auto box = f.box();
    std::string body = "line 1\n\nline 3\nkey=value\n";
    CHECK(box.send("alice", "bob", body, f.agents).ok());
    auto messages = box.read("bob");
    CHECK_EQ(messages.size(), 1u);
    if (!messages.empty()) CHECK_EQ(messages[0].body, body);
}

TEST(mailbox_rejects_bad_recipients) {
    Fixture f("recipients");
    auto box = f.box();
    CHECK(!box.send("alice", "alice", "me", f.agents).ok());
    CHECK(!box.send("alice", "dave", "who", f.agents).ok());
    CHECK(!box.send("../alice", "bob", "escape", f.agents).ok());
    CHECK(!box.send("alice", "bob", "", f.agents).ok());
}

TEST(mailbox_enforces_message_size) {
    Fixture f("size");
    f.limits.max_message_chars = 100;
    auto box = f.box();
    CHECK(box.send("alice", "bob", std::string(100, 'x'), f.agents).ok());
    CHECK(!box.send("alice", "bob", std::string(101, 'x'), f.agents).ok());
}

TEST(mailbox_enforces_rate_limit) {
    Fixture f("rate");
    f.limits.max_messages_per_minute = 3;
    auto box = f.box();
    for (int i = 0; i < 3; ++i) CHECK(box.send("alice", "bob", "m", f.agents).ok());
    CHECK(!box.send("alice", "bob", "m", f.agents).ok());
    CHECK(box.send("carol", "bob", "other sender is fine", f.agents).ok());

    f.now += 61'000;
    CHECK(box.send("alice", "bob", "after a minute", f.agents).ok());
}

TEST(mailbox_enforces_inbox_limit) {
    Fixture f("inbox");
    f.limits.max_inbox = 2;
    auto box = f.box();
    CHECK(box.send("alice", "bob", "1", f.agents).ok());
    CHECK(box.send("carol", "bob", "2", f.agents).ok());
    CHECK(!box.send("alice", "bob", "3", f.agents).ok());
    box.read("bob");
    CHECK(box.send("alice", "bob", "4", f.agents).ok());
}

TEST(mailbox_enforces_reply_chain_limit) {
    Fixture f("hops");
    f.limits.max_hops = 3;
    auto box = f.box();
    CHECK(box.send("alice", "bob", "1", f.agents).ok());   // hop 1
    box.read("bob");
    CHECK(box.send("bob", "alice", "2", f.agents).ok());   // hop 2
    box.read("alice");
    CHECK(box.send("alice", "bob", "3", f.agents).ok());   // hop 3
    box.read("bob");
    auto blocked = box.send("bob", "alice", "4", f.agents);
    CHECK(!blocked.ok());
    CHECK(blocked.error.find("chain") != std::string::npos);

    f.now += aio::Mailbox::kChainWindowMs;
    CHECK(box.send("bob", "alice", "fresh conversation", f.agents).ok());
}

TEST(mailbox_disabled) {
    Fixture f("disabled");
    f.limits.enabled = false;
    auto box = f.box();
    CHECK(!box.send("alice", "bob", "hi", f.agents).ok());
}

TEST(message_serialization_roundtrip) {
    aio::Message m{"id-1", "alice", "bob", {"bob", "carol"}, "body\nwith lines", 42, 3};
    auto parsed = aio::parse_message(aio::serialize_message(m));
    CHECK(parsed.has_value());
    if (parsed) {
        CHECK_EQ(parsed->id, m.id);
        CHECK_EQ(parsed->body, m.body);
        CHECK_EQ(parsed->sent_at_ms, m.sent_at_ms);
        CHECK_EQ(parsed->hops, 3);
        CHECK_EQ(parsed->recipients.size(), 2u);
    }
    auto shown = aio::format_message("bob", m);
    CHECK(shown.find("From: alice") != std::string::npos);
    CHECK(shown.find("To: you (bob)") != std::string::npos);
    CHECK(shown.find("Also sent to: carol") != std::string::npos);
    CHECK(!aio::parse_message("garbage").has_value());
}
