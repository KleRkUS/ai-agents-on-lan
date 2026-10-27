#pragma once

#include <string>

namespace aio {

// Stable identity of this machine. `id` is created once and stored in the config
// directory. `name` is the OS hostname, for display.
struct HostIdentity {
    std::string id;
    std::string name;
};

HostIdentity host_identity();

}  // namespace aio
