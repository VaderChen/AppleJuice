#pragma once

#include <unicode/ubrk.h>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace x64arm {
namespace icu_break {
using OwnedBreak=std::unique_ptr<UBreakIterator,decltype(&ubrk_close)>;
using Rules=std::vector<std::uint32_t>;
struct BreakObject {
    std::shared_ptr<Rules> binary;
    std::shared_ptr<const void> text;
    OwnedBreak iterator{nullptr,ubrk_close};
    UBreakIterator* borrowed{};
    std::uint64_t case_owner{};

    UBreakIterator* get()const{return iterator?iterator.get():borrowed;}
};
}

struct IcuBreakState {
    std::map<std::uint64_t,icu_break::BreakObject> objects;
    icu_break::BreakObject* find(std::uint64_t handle){
        const auto it=objects.find(handle);
        return it==objects.end()?nullptr:&it->second;
    }
};
}
