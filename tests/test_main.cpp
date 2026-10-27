#include "check.hpp"

int main() {
    using namespace aio::test;
    for (const auto& c : registry()) {
        int before = failures();
        c.fn();
        std::cout << (failures() == before ? "PASS " : "FAIL ") << c.name << "\n";
    }
    std::cout << registry().size() << " tests, " << failures() << " failed checks\n";
    return failures() == 0 ? 0 : 1;
}
