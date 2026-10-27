#include "log.hpp"

#include <ctime>

#include "platform.hpp"

namespace aio {

namespace {
constexpr uint64_t kMaxLogBytes = 1024 * 1024;
}

std::string log_path() { return Platform::join_path(Platform::state_dir(), "aio.log"); }

void log_event(const std::string& message) {
    auto path = log_path();
    Platform::create_directories(Platform::state_dir());
    if (Platform::file_size(path) > kMaxLogBytes) Platform::rename_file(path, path + ".1");

    std::time_t t = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    Platform::append_file(path, std::string(stamp) + "  " + message + "\n");
}

}  // namespace aio
