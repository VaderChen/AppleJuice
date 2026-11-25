#pragma once

#include "icu_guest_abi.hpp"
#include "native_callback_job.hpp"
#include "service_callback_task.hpp"
#include <unicode/ustring.h>
#include <unicode/utext.h>

namespace x64arm::icu_text {
// ICU 寫入時保留提供者自己的索引與回傳差值，客體記憶體只在服務續行中存取。
class WritableDestination {
    struct Request {
        bool length_query;
        std::int64_t start{}, limit{};
        const UChar* units{};
        std::int32_t length{};
        UErrorCode* status{};
        std::int64_t* result{};
    };
    NativeCallbackJob<Request, 0>* job_{};
    UText value_ = UTEXT_INITIALIZER;

    static WritableDestination& owner(const UText* text) {
        return *static_cast<WritableDestination*>(const_cast<void*>(text->context));
    }
    static std::int64_t U_CALLCONV native_length(UText* text) {
        std::int64_t length{};
        owner(text).job_->call({true, 0, 0, nullptr, 0, nullptr, &length});
        return length;
    }
    static std::int32_t U_CALLCONV replace(UText* text, std::int64_t start, std::int64_t limit,
                                         const UChar* units, std::int32_t length, UErrorCode* status) {
        if (length < -1 || (!units && length != 0)) {
            *status = U_ILLEGAL_ARGUMENT_ERROR;
            return 0;
        }
        std::int64_t delta{};
        if (!owner(text).job_->call({false, start, limit, units, length, status, &delta}))
            *status = U_INTERNAL_PROGRAM_ERROR;
        return static_cast<std::int32_t>(delta);
    }

public:
    WritableDestination() {
        static const UTextFuncs funcs{sizeof(UTextFuncs),0,0,0,nullptr,native_length,nullptr,nullptr,
                                     replace,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr};
        value_.pFuncs = &funcs;
        value_.context = this;
        value_.providerProperties = 1 << UTEXT_PROVIDER_WRITABLE;
    }
    template<class Work, class Allocate, class Release, class Sync>
    service_callback::Task<bool> write(GuestMemory& memory, std::uint64_t destination,
            std::uint64_t length_fn, std::uint64_t replace_fn, icu_abi::IcuStatus& status,
            Work work, Allocate allocate, Release release, Sync sync) {
        NativeCallbackJob<Request, 0> job;
        job_ = &job;
        job.start([&] { work(&value_); });
        while (const auto request = job.wait()) {
            (void)co_await sync(false, false);
            if (request->length_query) {
                GuestCallback length{length_fn, {destination}};
                length.wide_result = true;
                *request->result = std::bit_cast<std::int64_t>(co_await service_callback::call_native(std::move(length)));
            } else {
                const auto count = request->length == -1 ? std::size_t(u_strlen(request->units)) + 1 :
                                                           static_cast<std::size_t>(request->length);
                const auto size = count * sizeof(UChar);
                const auto address = request->units ? allocate(std::max<std::size_t>(size, sizeof(UChar))) : 0;
                if (request->units && !address) {
                    *request->status = U_MEMORY_ALLOCATION_ERROR;
                } else {
                    struct Pending {
                        std::uint64_t address;
                        Release& free;
                        ~Pending() { if (address) free(address); }
                    } scratch{address, release};
                    if (size) memory.write(GuestAddress{address}, icu_abi::icu_bytes(request->units, count));
                    memory.store(status.address, 32, static_cast<std::uint32_t>(*request->status));
                    GuestCallback put{replace_fn, {destination, static_cast<std::uint64_t>(request->start),
                        static_cast<std::uint64_t>(request->limit), address, static_cast<std::uint32_t>(request->length),
                        status.address.value}};
                    put.wide_arguments = 6;
                    *request->result = icu_abi::signed_argument(co_await service_callback::call_native(std::move(put)));
                    *request->status = static_cast<UErrorCode>(icu_abi::signed_argument(memory.load(status.address, 32)));
                }
            }
            (void)co_await sync(true, !request->length_query);
            job.reply(1);
        }
        co_return U_SUCCESS(status.value);
    }
};
}
