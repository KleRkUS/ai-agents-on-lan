#include "host.hpp"

#include <cctype>
#include <random>

#include "platform.hpp"

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace aio {
namespace {

std::string trim_copy(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    size_t start = 0;
    while (start < text.size() && (text[start] == ' ' || text[start] == '\n' || text[start] == '\r')) ++start;
    return text.substr(start);
}

bool valid_id(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (unsigned char c : id)
        if (!std::isalnum(c) && c != '.' && c != '_' && c != '-') return false;
    return true;
}

std::string random_id() {
    std::random_device device;
    unsigned char bytes[16];
    for (unsigned char& byte : bytes) byte = static_cast<unsigned char>(device());
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(32, '0');
    for (int i = 0; i < 16; ++i) {
        out[i * 2] = kHex[bytes[i] >> 4];
        out[i * 2 + 1] = kHex[bytes[i] & 0xf];
    }
    return out;
}

std::string display_name() {
    std::string name;
    for (unsigned char c : Platform::hostname()) {
        if (c < 32 || c == 127) continue;
        name.push_back(static_cast<char>(c));
        if (name.size() == 64) break;
    }
    return name.empty() ? "aio" : name;
}

}  // namespace

HostIdentity host_identity() {
    HostIdentity ident;
    ident.name = display_name();
    auto path = Platform::join_path(Platform::config_dir(), "host.id");
    if (auto existing = Platform::read_file(path)) {
        auto id = trim_copy(*existing);
        if (valid_id(id)) {
            ident.id = id;
            return ident;
        }
    }
    ident.id = random_id();
    Platform::create_directories(Platform::config_dir());
    if (!Platform::write_file(path, ident.id + "\n")) return ident;
#ifndef _WIN32
    ::chmod(path.c_str(), 0600);
#endif
    return ident;
}

}  // namespace aio
