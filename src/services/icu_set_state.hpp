#pragma once

#include <unicode/uset.h>
#include <cstdint>
#include <map>
#include <memory>

namespace x64arm {
struct CloseIcuSet {
    void operator()(USet* set) const {uset_close(set);}
};
using OwnedIcuSet=std::unique_ptr<USet,CloseIcuSet>;

struct IcuSetObject {
    OwnedIcuSet owned;
    const USet* borrowed{};
    std::uint64_t ranges{};
    const USet* get()const{return owned?owned.get():borrowed;}
};

struct IcuSetState {
    std::map<std::uint64_t,IcuSetObject> objects;
    std::map<const USet*,std::uint64_t> borrowed;
    const USet* get(std::uint64_t address)const{
        const auto found=objects.find(address);
        return found==objects.end()?nullptr:found->second.get();
    }
};
}
