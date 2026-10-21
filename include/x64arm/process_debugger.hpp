#pragma once
#include "x64arm/memory.hpp"

namespace x64arm::process_debugger {
struct Result {
    std::uint32_t error{};
    bool present{};
    std::uint64_t identity{};
    bool exited{};
};
Result query(std::uint32_t pid);
Result self(GuestMemory&,GuestAddress peb,bool refresh);
}
