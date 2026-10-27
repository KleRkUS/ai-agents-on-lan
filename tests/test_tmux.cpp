#include "check.hpp"
#include "tmux.hpp"

TEST(split_basic) {
    auto parts = aio::split("%1\tname\t/dir", '\t');
    CHECK_EQ(parts.size(), 3u);
    CHECK_EQ(parts[0], std::string("%1"));
    CHECK_EQ(parts[2], std::string("/dir"));
}

TEST(split_strips_carriage_returns) {
    auto lines = aio::split("a\r\nb\r\n", '\n');
    CHECK_EQ(lines.size(), 2u);
    CHECK_EQ(lines[0], std::string("a"));
    CHECK_EQ(lines[1], std::string("b"));
}

TEST(split_keeps_empty_fields) {
    auto parts = aio::split("%1\t\t/dir", '\t');
    CHECK_EQ(parts.size(), 3u);
    CHECK(parts[1].empty());
}
