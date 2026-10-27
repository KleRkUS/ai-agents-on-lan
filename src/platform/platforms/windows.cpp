#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "platform.hpp"

namespace aio {
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string narrow(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0,
                                nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr,
                        nullptr);
    return out;
}

std::filesystem::path to_path(const std::string& utf8) { return std::filesystem::path(widen(utf8)); }
std::string from_path(const std::filesystem::path& p) { return narrow(p.wstring()); }

// Quotes one argument following the MSVC CommandLineToArgvW rules.
std::wstring quote_arg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (auto it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
        } else {
            out.append(backslashes, L'\\');
        }
        out.push_back(*it);
    }
    out.push_back(L'"');
    return out;
}

std::wstring command_line(const std::vector<std::string>& args) {
    std::wstring cmd;
    for (const auto& a : args) {
        if (!cmd.empty()) cmd.push_back(L' ');
        cmd += quote_arg(widen(a));
    }
    return cmd;
}

}  // namespace

const char* Platform::name() { return "windows"; }

ProcessResult Platform::run_process(const std::vector<std::string>& args) {
    if (args.empty()) return {-1, {}};

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read_end = nullptr, write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) return {-1, {}};
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = write_end;
    si.hStdError = write_end;

    PROCESS_INFORMATION pi{};
    auto cmd = command_line(args);
    BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                  nullptr, nullptr, &si, &pi);
    CloseHandle(write_end);
    if (!started) {
        CloseHandle(read_end);
        return {127, {}};
    }

    std::string out;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(read_end, buf, sizeof(buf), &n, nullptr) && n > 0) out.append(buf, n);
    CloseHandle(read_end);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return {static_cast<int>(code), out};
}

int Platform::run_in_terminal(const std::vector<std::string>& args) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD in_mode = 0, out_mode = 0;
    bool have_in = in != INVALID_HANDLE_VALUE && GetConsoleMode(in, &in_mode);
    bool have_out = out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &out_mode);

    int code = exec_process(args);

    // While the console still has the mode the client left, so these sequences are honored.
    const char reset[] = "\033[?1049l\033[?25h\033[?1000l\033[?1002l\033[?1003l\033[?1006l\033[?2004l\033[0m";
    DWORD written = 0;
    if (out != INVALID_HANDLE_VALUE) WriteFile(out, reset, sizeof(reset) - 1, &written, nullptr);
    if (have_in) SetConsoleMode(in, in_mode);
    if (have_out) SetConsoleMode(out, out_mode);
    return code;
}

void Platform::spawn_detached(const std::vector<std::string>& args) {
    if (args.empty()) return;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    auto cmd = command_line(args);
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                        nullptr, nullptr, &si, &pi))
        return;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

int Platform::exec_process(const std::vector<std::string>& args) {
    if (args.empty()) return 127;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    auto cmd = command_line(args);
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi))
        return 127;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(code);
}

std::string Platform::executable_path() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return "aio.exe";
        if (n < buf.size()) {
            buf.resize(n);
            return narrow(buf);
        }
        buf.resize(buf.size() * 2);
    }
}

void Platform::set_env(const std::string& name, const std::string& value) { _putenv_s(name.c_str(), value.c_str()); }

std::vector<std::string> Platform::lan_addresses() { return {}; }

std::string Platform::hostname() {
    wchar_t buf[256];
    DWORD n = 256;
    if (!GetComputerNameW(buf, &n)) return {};
    return narrow(std::wstring(buf, n));
}

std::optional<std::string> Platform::env(const std::string& name) {
    auto wname = widen(name);
    DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (n == 0) return std::nullopt;
    std::wstring value(n, L'\0');
    n = GetEnvironmentVariableW(wname.c_str(), value.data(), n);
    value.resize(n);
    return narrow(value);
}

void Platform::unset_env(const std::string& name) {
    SetEnvironmentVariableW(widen(name).c_str(), nullptr);
}

std::string Platform::home_dir() {
    if (auto profile = env("USERPROFILE")) return *profile;
    auto drive = env("HOMEDRIVE"), path = env("HOMEPATH");
    if (drive && path) return *drive + *path;
    return ".";
}

std::string Platform::config_dir() {
    auto appdata = env("APPDATA");
    return join_path(appdata ? *appdata : join_path(home_dir(), "AppData\\Roaming"), "aio");
}

std::string Platform::state_dir() {
    auto local = env("LOCALAPPDATA");
    return join_path(local ? *local : join_path(home_dir(), "AppData\\Local"), "aio");
}

std::string Platform::current_dir() {
    std::error_code ec;
    auto p = std::filesystem::current_path(ec);
    return ec ? std::string(".") : from_path(p);
}

std::string Platform::absolute_path(const std::string& path) {
    std::error_code ec;
    auto p = std::filesystem::absolute(to_path(path), ec);
    return ec ? path : from_path(p.lexically_normal());
}

bool Platform::is_directory(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_directory(to_path(path), ec);
}

bool Platform::create_directories(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(to_path(path), ec);
    return is_directory(path);
}

std::optional<std::string> Platform::read_file(const std::string& path) {
    std::ifstream in(to_path(path), std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool Platform::write_file(const std::string& path, const std::string& contents) {
    std::ofstream out(to_path(path), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << contents;
    return static_cast<bool>(out);
}

bool Platform::append_file(const std::string& path, const std::string& contents) {
    std::ofstream out(to_path(path), std::ios::binary | std::ios::app);
    if (!out) return false;
    out << contents;
    return static_cast<bool>(out);
}

uint64_t Platform::file_size(const std::string& path) {
    std::error_code ec;
    auto size = std::filesystem::file_size(to_path(path), ec);
    return ec ? 0 : static_cast<uint64_t>(size);
}

std::string Platform::join_path(const std::string& base, const std::string& name) {
    return from_path(to_path(base) / to_path(name));
}

std::string Platform::parent_path(const std::string& path) {
    return from_path(to_path(path).parent_path());
}

std::vector<std::string> Platform::list_files(const std::string& path) {
    std::vector<std::string> names;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(to_path(path), ec), end; !ec && it != end;
         it.increment(ec))
        if (it->is_regular_file(ec)) names.push_back(from_path(it->path().filename()));
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> Platform::list_directories(const std::string& path) {
    std::vector<std::string> names;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(to_path(path), ec), end; !ec && it != end;
         it.increment(ec))
        if (it->is_directory(ec)) names.push_back(from_path(it->path().filename()));
    std::sort(names.begin(), names.end());
    return names;
}

bool Platform::remove_all(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove_all(to_path(path), ec);
    return !ec;
}

bool Platform::remove_file(const std::string& path) {
    std::error_code ec;
    return std::filesystem::remove(to_path(path), ec);
}

bool Platform::rename_file(const std::string& from, const std::string& to) {
    return MoveFileExW(widen(from).c_str(), widen(to).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

std::optional<TerminalSize> Platform::terminal_size() {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) return std::nullopt;
    return TerminalSize{info.srWindow.Right - info.srWindow.Left + 1,
                        info.srWindow.Bottom - info.srWindow.Top + 1};
}

// tmux on Windows (MSYS2/Cygwin) runs pane commands through a POSIX sh.
void Platform::disable_terminal_signals() {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (in != INVALID_HANDLE_VALUE && GetConsoleMode(in, &mode))
        SetConsoleMode(in, mode & ~static_cast<DWORD>(ENABLE_PROCESSED_INPUT));
}

Platform::TerminalInput Platform::begin_raw_input() {
    TerminalInput in;
    HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD in_mode = 0, out_mode = 0;
    bool have_in = hin != INVALID_HANDLE_VALUE && GetConsoleMode(hin, &in_mode);
    bool have_out = hout != INVALID_HANDLE_VALUE && GetConsoleMode(hout, &out_mode);
    if (!have_in) return in;
    struct Store {
        DWORD in_mode;
        DWORD out_mode;
        bool have_in;
        bool have_out;
    };
    static_assert(sizeof(Store) <= sizeof(in.bytes));
    Store store{in_mode, out_mode, have_in, have_out};
    std::memcpy(in.bytes, &store, sizeof(store));
    DWORD raw = in_mode;
    raw &= ~static_cast<DWORD>(ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
    raw |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    if (!SetConsoleMode(hin, raw)) return in;
    if (have_out) SetConsoleMode(hout, out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    in.active = true;
    return in;
}

void Platform::end_raw_input(TerminalInput& saved) {
    if (!saved.active) return;
    struct Store {
        DWORD in_mode;
        DWORD out_mode;
        bool have_in;
        bool have_out;
    };
    Store store{};
    std::memcpy(&store, saved.bytes, sizeof(store));
    HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (store.have_in) SetConsoleMode(hin, store.in_mode);
    if (store.have_out) SetConsoleMode(hout, store.out_mode);
    const char reset[] = "\033[?1049l\033[?25h\033[?1000l\033[?1002l\033[?1003l\033[?1006l\033[?2004l\033[0m";
    DWORD written = 0;
    WriteFile(hout, reset, sizeof(reset) - 1, &written, nullptr);
    saved.active = false;
}

std::string Platform::shell_quote(const std::string& arg) {
    std::string out = "'";
    for (char c : arg) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

}  // namespace aio
