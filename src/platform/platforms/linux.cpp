#include <climits>

#include "posix_common.hpp"

namespace aio {

const char* Platform::name() { return "linux"; }

ProcessResult Platform::run_process(const std::vector<std::string>& args) { return posix::run_process(args); }
int Platform::exec_process(const std::vector<std::string>& args) { return posix::exec_process(args); }
int Platform::run_in_terminal(const std::vector<std::string>& args) { return posix::run_in_terminal(args); }
void Platform::spawn_detached(const std::vector<std::string>& args) { posix::spawn_detached(args); }

std::string Platform::executable_path() {
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    return n > 0 ? std::string(buf, n) : std::string("aio");
}

std::optional<std::string> Platform::env(const std::string& name) { return posix::env(name); }
void Platform::set_env(const std::string& name, const std::string& value) { posix::set_env(name, value); }
void Platform::unset_env(const std::string& name) { posix::unset_env(name); }
std::vector<std::string> Platform::lan_addresses() { return posix::lan_addresses(); }
std::string Platform::hostname() { return posix::hostname(); }
std::string Platform::home_dir() { return posix::home_dir(); }

std::string Platform::config_dir() {
    auto xdg = posix::env("XDG_CONFIG_HOME");
    std::string base = xdg && !xdg->empty() ? *xdg : posix::join_path(home_dir(), ".config");
    return posix::join_path(base, "aio");
}

std::string Platform::state_dir() {
    auto xdg = posix::env("XDG_STATE_HOME");
    std::string base = xdg && !xdg->empty() ? *xdg : posix::join_path(home_dir(), ".local/state");
    return posix::join_path(base, "aio");
}

std::string Platform::current_dir() { return posix::current_dir(); }
std::string Platform::absolute_path(const std::string& path) { return posix::absolute_path(path); }
bool Platform::is_directory(const std::string& path) { return posix::is_directory(path); }
bool Platform::create_directories(const std::string& path) { return posix::create_directories(path); }
std::optional<std::string> Platform::read_file(const std::string& path) { return posix::read_file(path); }
bool Platform::write_file(const std::string& path, const std::string& contents) {
    return posix::write_file(path, contents);
}
bool Platform::append_file(const std::string& path, const std::string& contents) {
    return posix::append_file(path, contents);
}
uint64_t Platform::file_size(const std::string& path) { return posix::file_size(path); }
std::string Platform::join_path(const std::string& base, const std::string& name) {
    return posix::join_path(base, name);
}
std::string Platform::parent_path(const std::string& path) { return posix::parent_path(path); }
std::vector<std::string> Platform::list_files(const std::string& path) { return posix::list_files(path); }
bool Platform::remove_file(const std::string& path) { return posix::remove_file(path); }
std::vector<std::string> Platform::list_directories(const std::string& path) { return posix::list_directories(path); }
bool Platform::remove_all(const std::string& path) { return posix::remove_all(path); }
bool Platform::rename_file(const std::string& from, const std::string& to) { return posix::rename_file(from, to); }

std::optional<TerminalSize> Platform::terminal_size() { return posix::terminal_size(); }
void Platform::disable_terminal_signals() { posix::disable_terminal_signals(); }
Platform::TerminalInput Platform::begin_raw_input() { return posix::begin_raw_input(); }
void Platform::end_raw_input(TerminalInput& saved) { posix::end_raw_input(saved); }
std::string Platform::shell_quote(const std::string& arg) { return posix::shell_quote(arg); }

}  // namespace aio
