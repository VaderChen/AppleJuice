#pragma once

#include <unicode/ufieldpositer.h>
#include <unicode/uformattable.h>
#include <unicode/unum.h>
#include <array>
#include <cstdint>
#include <map>
#include <memory>

namespace x64arm {
using OwnedIcuNumber=std::unique_ptr<UNumberFormat,decltype(&unum_close)>;
using OwnedIcuFormattable=std::unique_ptr<UFormattable,decltype(&ufmt_close)>;
using OwnedIcuFieldIterator=std::unique_ptr<UFieldPositionIterator,decltype(&ufieldpositer_close)>;

struct IcuNumberObject {
    OwnedIcuNumber owned{nullptr,unum_close};
    const UNumberFormat* borrowed{};
    const UNumberFormat* get()const{return owned?owned.get():borrowed;}
};

struct IcuFormattableObject {
    OwnedIcuFormattable owned{nullptr,ufmt_close};
    const UFormattable* borrowed{};
    std::uint64_t owner{};
    std::array<std::uint64_t,2> text{};
    std::array<std::size_t,2> capacities{};
    std::map<const void*,std::uint64_t> objects;
    std::map<const UFormattable*,std::uint64_t> children;
    const UFormattable* get()const{return owned?owned.get():borrowed;}
};

struct IcuNumberState {
    std::map<std::uint64_t,IcuNumberObject> formats;
    std::map<std::uint64_t,IcuFormattableObject> values;
    // 值為客體 heap 上的欄位資料；游標只保存在客體 view。
    std::map<std::uint64_t,std::uint64_t> positions;
};
}
