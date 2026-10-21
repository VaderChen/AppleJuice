#pragma once
#include "policy_lease.hpp"
#include <memory>
#include <string>

namespace x64arm {
// 僅供引擎自己的 mapping session 使用；名稱由 machine／虛擬使用者 SID 決定。
class SharedPolicy {
    struct Storage;
    std::unique_ptr<Storage> storage_;
    explicit SharedPolicy(std::unique_ptr<Storage>);
public:
    ~SharedPolicy();
    static std::shared_ptr<SharedPolicy> open(const std::string& session,const std::u16string& name);
    PolicyLease::Acquisition acquire(std::uint64_t thread,std::uint64_t now);
    std::uint32_t release(std::uint64_t thread,std::uint64_t generation,std::uint64_t now);
};
}
