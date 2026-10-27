#include <filesystem>

#include "check.hpp"
#include "platform.hpp"

namespace {

std::string temp_dir(const std::string& name) {
    auto dir = std::filesystem::temp_directory_path() / ("aio-test-" + name);
    std::filesystem::remove_all(dir);
    return dir.string();
}

}  // namespace

TEST(platform_name_is_known) {
    std::string name = aio::Platform::name();
    CHECK(name == "linux" || name == "macos" || name == "windows");
}

TEST(platform_file_roundtrip) {
    auto dir = aio::Platform::join_path(temp_dir("files"), "nested/dir");
    CHECK(aio::Platform::create_directories(dir));
    CHECK(aio::Platform::is_directory(dir));

    auto file = aio::Platform::join_path(dir, "data.txt");
    CHECK(aio::Platform::write_file(file, "line1\nline2\n"));
    CHECK_EQ(aio::Platform::read_file(file).value_or(""), std::string("line1\nline2\n"));
    CHECK(!aio::Platform::read_file(file + ".missing").has_value());
    CHECK(!aio::Platform::is_directory(file));
}

TEST(platform_executable_path_exists) {
    CHECK(std::filesystem::exists(aio::Platform::executable_path()));
}

TEST(platform_run_process_captures_output) {
    auto r = aio::Platform::run_process({"sh", "-c", "echo hello; exit 3"});
    CHECK_EQ(r.exit_code, 3);
    CHECK_EQ(r.output, std::string("hello\n"));
}

TEST(platform_run_process_missing_program) {
    CHECK(!aio::Platform::run_process({"aio-definitely-not-a-program"}).ok());
}

TEST(platform_shell_quote_roundtrip) {
    std::string tricky = "it's a \"path\" with $HOME";
    auto r = aio::Platform::run_process({"sh", "-c", "printf %s " + aio::Platform::shell_quote(tricky)});
    CHECK_EQ(r.output, tricky);
}
