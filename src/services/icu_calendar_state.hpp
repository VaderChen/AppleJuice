#pragma once

#include <unicode/ucal.h>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace x64arm {
using OwnedIcuCalendar=std::unique_ptr<UCalendar,decltype(&ucal_close)>;

struct IcuCalendarObject {
    OwnedIcuCalendar owned{nullptr,ucal_close};
    const UCalendar* borrowed{};
    const UCalendar* get()const{return owned?owned.get():borrowed;}
};

struct IcuCalendarState {
    std::optional<std::u16string> default_zone;
    std::map<std::uint64_t,IcuCalendarObject> calendars;
};
}
