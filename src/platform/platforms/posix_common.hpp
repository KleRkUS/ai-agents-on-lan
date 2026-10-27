#pragma once

// POSIX implementations shared by the linux and macos platform modules.
// Included only from those translation units.

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "platform.hpp"

namespace aio::posix {

inline ProcessResult run_process(const std::vector<std::string>& args) {
    if (args.empty()) return {-1, {}};
    int pipefd[2];
    if (pipe(pipefd) != 0) return {-1, {}};

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return {-1, {}};
    }
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        std::vector<char*> argv;
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    close(pipefd[1]);
    std::string out;
    std::array<char, 4096> buf{};
    ssize_t n;
    while ((n = read(pipefd[0], buf.data(), buf.size())) > 0) out.append(buf.data(), n);
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    return {WIFEXITED(status) ? WEXITSTATUS(status) : -1, out};
}

inline int run_in_terminal(const std::vector<std::string>& args) {
    if (args.empty()) return 127;
    termios saved{};
    bool have_tty = ::tcgetattr(STDIN_FILENO, &saved) == 0;

    pid_t pid = fork();
    if (pid < 0) return 127;
    if (pid == 0) {
        std::vector<char*> argv;
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);

    if (have_tty) {
        ::tcsetattr(STDIN_FILENO, TCSANOW, &saved);
        // A client that died without detaching leaves the alternate screen, a hidden
        // cursor and mouse tracking on, which makes the shell look frozen.
        const char reset[] = "\033[?1049l\033[?25h\033[?1000l\033[?1002l\033[?1003l\033[?1006l\033[?2004l\033[0m";
        if (::write(STDOUT_FILENO, reset, sizeof(reset) - 1) < 0) { /* best effort */ }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

inline void spawn_detached(const std::vector<std::string>& args) {
    if (args.empty()) return;
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        ::setsid();
        if (fork() != 0) _exit(0);
        int fd = ::open("/dev/null", O_RDWR);
        if (fd >= 0) {
            ::dup2(fd, STDIN_FILENO);
            ::dup2(fd, STDOUT_FILENO);
            ::dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO) ::close(fd);
        }
        std::vector<char*> argv;
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        ::execvp(argv[0], argv.data());
        _exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
}

inline int exec_process(const std::vector<std::string>& args) {
    if (args.empty()) return 127;
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    return 127;
}

inline std::optional<std::string> env(const std::string& name) {
    const char* v = std::getenv(name.c_str());
    if (!v) return std::nullopt;
    return std::string(v);
}

inline std::string home_dir() { return env("HOME").value_or("."); }

inline std::string current_dir() {
    std::error_code ec;
    auto p = std::filesystem::current_path(ec);
    return ec ? std::string(".") : p.string();
}

inline std::string absolute_path(const std::string& path) {
    std::error_code ec;
    auto p = std::filesystem::absolute(path, ec);
    return ec ? path : p.lexically_normal().string();
}

inline bool is_directory(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec);
}

inline bool create_directories(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return is_directory(path);
}

inline std::optional<std::string> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

inline bool write_file(const std::string& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << contents;
    return static_cast<bool>(out);
}

inline bool append_file(const std::string& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) return false;
    out << contents;
    return static_cast<bool>(out);
}

inline uint64_t file_size(const std::string& path) {
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    return ec ? 0 : static_cast<uint64_t>(size);
}

inline void disable_terminal_signals() {
    // The UI library may re-enable ISIG (e.g. on resize), so also ignore the signals themselves.
    for (int sig : {SIGINT, SIGQUIT, SIGTSTP}) std::signal(sig, SIG_IGN);
    termios t{};
    if (::tcgetattr(STDIN_FILENO, &t) != 0) return;
    t.c_lflag &= ~static_cast<tcflag_t>(ISIG);
    ::tcsetattr(STDIN_FILENO, TCSANOW, &t);
}

inline Platform::TerminalInput begin_raw_input() {
    Platform::TerminalInput in;
    termios saved{};
    if (::tcgetattr(STDIN_FILENO, &saved) != 0) return in;
    static_assert(sizeof(saved) <= sizeof(in.bytes));
    std::memcpy(in.bytes, &saved, sizeof(saved));
    for (int sig : {SIGINT, SIGQUIT, SIGTSTP}) std::signal(sig, SIG_IGN);
    termios raw = saved;
    raw.c_iflag &= ~static_cast<tcflag_t>(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_cflag |= static_cast<tcflag_t>(CS8);
    raw.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (::tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return in;
    in.active = true;
    return in;
}

inline void end_raw_input(Platform::TerminalInput& saved) {
    if (!saved.active) return;
    termios mode{};
    std::memcpy(&mode, saved.bytes, sizeof(mode));
    ::tcsetattr(STDIN_FILENO, TCSANOW, &mode);
    for (int sig : {SIGINT, SIGQUIT, SIGTSTP}) std::signal(sig, SIG_DFL);
    const char reset[] = "\033[?1049l\033[?25h\033[?1000l\033[?1002l\033[?1003l\033[?1006l\033[?2004l\033[0m";
    if (::write(STDOUT_FILENO, reset, sizeof(reset) - 1) < 0) { /* best effort */ }
    saved.active = false;
}

inline void set_env(const std::string& name, const std::string& value) {
    ::setenv(name.c_str(), value.c_str(), 1);
}

inline void unset_env(const std::string& name) { ::unsetenv(name.c_str()); }

inline std::string hostname() {
    char buf[256] = {};
    if (::gethostname(buf, sizeof(buf) - 1) != 0) return {};
    return buf;
}

inline std::vector<std::string> lan_addresses() {
    std::vector<std::string> out;
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) return out;
    for (auto* ifa = list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        char buf[INET_ADDRSTRLEN];
        auto* addr = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
        if (!::inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf))) continue;
        if (std::string(buf) == "127.0.0.1") continue;
        out.emplace_back(buf);
    }
    ::freeifaddrs(list);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

inline std::string join_path(const std::string& base, const std::string& name) {
    return (std::filesystem::path(base) / name).string();
}

inline std::string parent_path(const std::string& path) {
    return std::filesystem::path(path).parent_path().string();
}

inline std::vector<std::string> list_files(const std::string& path) {
    std::vector<std::string> names;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) names.push_back(it->path().filename().string());
    std::sort(names.begin(), names.end());
    return names;
}

inline std::vector<std::string> list_directories(const std::string& path) {
    std::vector<std::string> names;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec)) names.push_back(it->path().filename().string());
    std::sort(names.begin(), names.end());
    return names;
}

inline bool remove_all(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    return !ec;
}

inline bool remove_file(const std::string& path) {
    std::error_code ec;
    return std::filesystem::remove(path, ec);
}

inline bool rename_file(const std::string& from, const std::string& to) {
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    return !ec;
}

inline std::optional<TerminalSize> terminal_size() {
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0) return std::nullopt;
    return TerminalSize{ws.ws_col, ws.ws_row};
}

inline std::string shell_quote(const std::string& arg) {
    std::string out = "'";
    for (char c : arg) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

}  // namespace aio::posix
