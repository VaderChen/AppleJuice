#pragma once
#include "icu_guest_abi.hpp"
#include "icu_text_projection.hpp"
#include "service_callback_task.hpp"

namespace x64arm::icu_text {
template<class Release>
service_callback::Task<SharedProjection> project_input(GuestMemory& memory, std::uint64_t target,
        std::uint64_t source, icu_abi::IcuStatus& status, Release release, std::uint64_t* retained = nullptr) {
    if (U_FAILURE(status.value)) co_return nullptr;
    if (!source) { status.value = U_ILLEGAL_ARGUMENT_ERROR; co_return nullptr; }
    const auto result = co_await service_callback::call_value(target, {source, status.address.value});
    struct SnapshotOwner {
        std::uint64_t address;
        Release& release;
        ~SnapshotOwner() { if (address) release(address); }
    } owner{result, release};
    status.value = static_cast<UErrorCode>(icu_abi::signed_argument(memory.load(status.address, 32)));
    if (!result && U_SUCCESS(status.value)) status.value = U_MEMORY_ALLOCATION_ERROR;
    if (U_FAILURE(status.value)) co_return nullptr;
    if (retained) *retained = memory.load(GuestAddress{result}, 64);
    try { co_return load_projection(memory, result + (retained ? sizeof(std::uint64_t) : 0), status.value); }
    catch (const std::bad_alloc&) { status.value = U_MEMORY_ALLOCATION_ERROR; co_return nullptr; }
}

struct LocalText {
    UText value = UTEXT_INITIALIZER;
    LocalText() = default;
    LocalText(const LocalText&) = delete;
    ~LocalText() { utext_close(&value); }
};
}
