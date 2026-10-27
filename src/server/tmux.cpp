#include "tmux.hpp"

#include <sstream>

namespace aio {

std::string tmux_socket() {
    auto socket = Platform::env("AIO_TMUX_SOCKET");
    return socket && !socket->empty() ? *socket : "aio";
}

ProcessResult tmux(std::vector<std::string> args) {
    args.insert(args.begin(), {"tmux", "-L", tmux_socket()});
    return Platform::run_process(args);
}

std::string tmux_out(std::vector<std::string> args) {
    auto r = tmux(std::move(args));
    if (!r.ok()) return {};
    while (!r.output.empty() &&
           (r.output.back() == '\n' || r.output.back() == '\r' || r.output.back() == ' '))
        r.output.pop_back();
    return r.output;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> parts;
    std::string part;
    std::istringstream ss(s);
    while (std::getline(ss, part, sep)) {
        if (!part.empty() && part.back() == '\r') part.pop_back();
        parts.push_back(part);
    }
    return parts;
}

}  // namespace aio
