#pragma once

#include "x64arm/windows_abi.hpp"
#include <unicode/umsg.h>
#include <unicode/unistr.h>

namespace x64arm::icu_message {
class Arguments {
public:
    Arguments(const WindowsCall& call,const GuestMemory& memory,unsigned first,bool indirect=false);
    std::uint64_t take(bool wide,UErrorCode& status);
private:
    const WindowsCall& call_;
    const GuestMemory& memory_;
    std::uint64_t address_;
    unsigned slot_;
    bool x86_,indirect_;
};

icu::UnicodeString format(const UMessageFormat& message,Arguments& args,const GuestMemory& memory,UErrorCode& status);
void parse(const UMessageFormat& message,const icu::UnicodeString& text,Arguments& args,
           GuestMemory& memory,std::uint64_t count_address,UErrorCode& status);
}
