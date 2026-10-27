#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aio {

struct ProcessResult {
    int exit_code;
    std::string output;  // stdout and stderr combined

    bool ok() const { return exit_code == 0; }
};

struct TerminalSize {
    int cols;
    int rows;
};

// Single entry point for every OS-dependent operation in the app.
// The implementation is selected at build time (AIO_PLATFORM in CMake);
// exactly one of platforms/{linux,macos,windows}.cpp is compiled in.
class Platform {
public:
    // Build-time platform identifier: "linux", "macos" or "windows".
    static const char* name();

    // Runs a program (looked up in PATH) without a shell and captures its output.
    static ProcessResult run_process(const std::vector<std::string>& args);
    // Hands the terminal over to a program. Replaces the current process where the
    // OS supports it; otherwise runs it attached and returns its exit code.
    static int exec_process(const std::vector<std::string>& args);
    // Runs a program attached to this terminal and, once it exits, puts the terminal
    // back how it was (modes, alternate screen, mouse tracking).
    static int run_in_terminal(const std::vector<std::string>& args);
    // Starts a program that outlives this process. Returns once it has detached.
    static void spawn_detached(const std::vector<std::string>& args);

    static std::string executable_path();
    static std::optional<std::string> env(const std::string& name);
    static void set_env(const std::string& name, const std::string& value);
    static void unset_env(const std::string& name);
    // IPv4 addresses of this machine, excluding 127.0.0.1.
    static std::vector<std::string> lan_addresses();
    // Short name of this machine, or empty if the OS has none.
    static std::string hostname();
    static std::string home_dir();
    // Per-user directory for this app's configuration.
    static std::string config_dir();
    // Per-user directory for runtime state (mailboxes, generated agent configs).
    static std::string state_dir();

    static std::string current_dir();
    static std::string absolute_path(const std::string& path);
    static bool is_directory(const std::string& path);
    static bool create_directories(const std::string& path);
    static std::optional<std::string> read_file(const std::string& path);
    static bool write_file(const std::string& path, const std::string& contents);
    static bool append_file(const std::string& path, const std::string& contents);
    static uint64_t file_size(const std::string& path);  // 0 if missing
    static std::string join_path(const std::string& base, const std::string& name);
    static std::string parent_path(const std::string& path);
    // Names of regular files directly inside `path`, sorted; empty if it does not exist.
    static std::vector<std::string> list_files(const std::string& path);
    // Names of subdirectories directly inside `path`, sorted.
    static std::vector<std::string> list_directories(const std::string& path);
    static bool remove_file(const std::string& path);
    // Removes a file or directory tree.
    static bool remove_all(const std::string& path);
    // Atomically replaces `to` with `from` when both are on the same filesystem.
    static bool rename_file(const std::string& from, const std::string& to);

    static std::optional<TerminalSize> terminal_size();
    // Stops Ctrl+C / Ctrl+Z on the controlling terminal from signalling this process.
    static void disable_terminal_signals();

    // Saved terminal state for begin_raw_input / end_raw_input. `bytes` is opaque.
    struct TerminalInput {
        bool active = false;
        alignas(8) unsigned char bytes[256]{};
    };
    // Passes keystrokes through as bytes, including Ctrl+C. `active` is false when there is no tty.
    static TerminalInput begin_raw_input();
    // Restores the mode from begin_raw_input and leaves the alternate screen.
    static void end_raw_input(TerminalInput& saved);

    // Quotes an argument for the shell tmux uses to run pane commands.
    static std::string shell_quote(const std::string& arg);
};

}  // namespace aio
