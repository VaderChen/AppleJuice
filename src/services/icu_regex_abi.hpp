#pragma once
#include "icu_guest_abi.hpp"
#include <unicode/uregex.h>
#include <tuple>

namespace x64arm::icu_regex {
// 直接由 ICU 宣告取得 Win32 的雙槽整數參數。客體指標仍只佔一槽。
template<class Result, class... Args>
constexpr WindowsServiceDescriptor descriptor(std::string_view name, Result (*)(Args...)) {
    std::uint32_t wide = 0;
    unsigned slot = 0;
    ((wide |= std::uint32_t(std::is_integral_v<Args> && sizeof(Args) == 8) << slot++), ...);
    return {"icu.dll", name, sizeof...(Args), wide, 0, 0, true};
}

template<class Result, class... Args, std::size_t... I>
void scalar(Result (*function)(Args...), URegularExpression* regex, const WindowsCall& call,
            GuestState& state, icu_abi::IcuStatus& status, std::index_sequence<I...>) {
    using Types = std::tuple<Args...>;
    const auto initial = status.value;
    auto run = [&] {
        return function(regex, static_cast<std::tuple_element_t<I + 1, Types>>(call(I + 1))..., &status.value);
    };
    if constexpr (std::is_void_v<Result>) {
        run();
        if (initial != status.value) status.commit();
        call.return_to(state, 0);
    } else {
        const auto value = static_cast<std::make_unsigned_t<Result>>(run());
        if (initial != status.value) status.commit();
        call.return_to(state, value, sizeof(Result) == 8);
    }
}

template<class Result, class... Args>
void scalar(Result (*function)(Args...), URegularExpression* regex, const WindowsCall& call,
            GuestState& state, icu_abi::IcuStatus& status) {
    scalar(function, regex, call, state, status, std::make_index_sequence<sizeof...(Args) - 2>{});
}
}
