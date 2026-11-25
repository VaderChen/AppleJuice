// 以同一份宿主 matcher 進行搜尋，客體提供者仍負責輸出索引、寫入副作用與回呼。
template<class Target, class Allocate, class Release>
service_callback::Task<std::uint64_t> write_text_result(IcuStatus& status, std::uint64_t dest,
        const UChar* text, std::int32_t length, Target target, Allocate allocate, Release release,
        std::int64_t index = 0) {
    const auto bytes = std::size_t(length) * sizeof(UChar);
    const auto aligned = (bytes + 3U) & ~std::size_t{3};
    const auto address = allocate(aligned + 4);
    if (!address) {
        if (U_SUCCESS(status.value)) status.value = U_MEMORY_ALLOCATION_ERROR;
        co_return dest;
    }
    struct Scratch {
        std::uint64_t address;
        Release& free;
        ~Scratch() { free(address); }
    } scratch{address, release};
    if (bytes) status.memory.write(GuestAddress{address}, icu_bytes(text, std::size_t(length)));
    if (!dest) {
        // 已產生的替換前綴也能在後續匹配錯誤時交回。配置結果另用狀態欄。
        const GuestAddress slot{address + aligned};
        status.memory.store(slot, 32, U_ZERO_ERROR);
        const auto result = co_await service_callback::call_value(target(icu_text::own_utf16_export),
            {address, static_cast<std::uint32_t>(length), slot.value});
        const auto error = static_cast<UErrorCode>(signed_argument(status.memory.load(slot, 32)));
        if (U_SUCCESS(status.value) && U_FAILURE(error)) status.value = error;
        if (result && index) {
            GuestCallback seek{target("utext_setNativeIndex"), {result, static_cast<std::uint64_t>(index)}};
            seek.wide_arguments = 2;
            (void)co_await service_callback::call_native(std::move(seek));
        }
        co_return result;
    }
    GuestCallback query{target("utext_nativeLength"), {dest}};
    query.wide_result = true;
    const auto limit = co_await service_callback::call_native(std::move(query));
    status.commit();
    GuestCallback put{target("utext_replace"), {dest, 0, limit, address, static_cast<std::uint32_t>(length), status.address.value}};
    put.wide_arguments = 6;
    (void)co_await service_callback::call_native(std::move(put));
    status.value = static_cast<UErrorCode>(signed_argument(status.memory.load(status.address, 32)));
    co_return dest;
}

template<class Target, class Allocate, class Release>
service_callback::Task<std::uint64_t> replace_text(Regex& object, IcuStatus& status, bool first,
        std::uint64_t source, std::uint64_t dest, Target target, Allocate allocate, Release release) {
    auto* native = object.native.get();
    if (!object.text && !object.projected) { status.value = U_REGEX_INVALID_STATE; co_return 0; }
    if (!source) { status.value = U_ILLEGAL_ARGUMENT_ERROR; co_return 0; }
    uregex_reset(native, 0, &status.value);
    if (U_FAILURE(status.value)) co_return 0;
    auto provider = object.guest_provider;
    icu::UnicodeString contents;
    icu_text::LocalText local;
    auto* buffer = dest ? nullptr : utext_openUnicodeString(&local.value, &contents, &status.value);
    if (U_FAILURE(status.value)) co_return 0;
    bool found = false;
    while (U_SUCCESS(status.value) && (co_await run_match(object, status.value, [&] {
        return uregex_findNext(native, &status.value);
    }))) {
        found = true;
        (void)co_await append_text(object, status, source, dest, target, allocate, release, buffer);
        if (first) break;
    }
    if (first && !found) {
        if (U_FAILURE(status.value)) co_return dest;
        co_return co_await subject_view(&object, status, target, allocate, release, dest, std::nullopt, 0, std::move(provider));
    }
    if (U_SUCCESS(status.value))
        (void)co_await append_text(object, status, 0, dest, target, allocate, release, buffer);
    if (dest) co_return dest;
    if (contents.isBogus()) { status.value = U_MEMORY_ALLOCATION_ERROR; co_return 0; }
    co_return co_await write_text_result(status, 0, contents.getBuffer(), contents.length(), target, allocate, release,
        utext_getNativeIndex(buffer));
}

template<class Target, class Allocate, class Release>
service_callback::Task<std::uint32_t> split_text(Regex& object, IcuStatus& status, std::uint64_t fields,
        std::int32_t capacity, Target target, Allocate allocate, Release release) {
    if (capacity < 1 || !fields) { status.value = U_ILLEGAL_ARGUMENT_ERROR; co_return 0; }
    if (!object.text && !object.projected) co_return 0;
    auto* native = object.native.get();
    uregex_reset(native, 0, &status.value);
    std::unique_ptr<UText, decltype(&utext_close)> input(uregex_getUText(native, nullptr, &status.value), utext_close);
    if (!input || U_FAILURE(status.value)) co_return 0;
    const auto limit = utext_nativeLength(input.get());
    if (!limit) co_return 0;
    const auto groups = uregex_groupCount(native, &status.value);
    const std::uint8_t bits = status.memory.execution_mode() == ExecutionMode::X86 ? 32 : 64;
    std::int32_t count{};
    auto emit = [&](std::int64_t start, std::int64_t end) -> service_callback::Task<bool> {
        const GuestAddress slot{fields + std::uint64_t(count) * (bits / 8U)};
        const auto dest = status.memory.load(slot, bits);
        if (!dest) status.memory.check(slot, bits / 8U, Access::Write);
        ++count;
        std::vector<UChar> units;
        std::int32_t length{};
        if (start != end) {
            auto probe = U_ZERO_ERROR;
            length = utext_extract(input.get(), start, end, nullptr, 0, &probe);
            if (U_FAILURE(probe) && probe != U_BUFFER_OVERFLOW_ERROR) { status.value = probe; co_return false; }
            units.resize(static_cast<std::size_t>(length));
            auto extracted = U_ZERO_ERROR;
            length = utext_extract(input.get(), start, end, units.data(), length, &extracted);
            // 以明確長度傳輸；緩衝區剛好填滿時不要求終止字元。
            if (U_FAILURE(extracted)) { status.value = extracted; co_return false; }
        }
        const auto result = co_await write_text_result(status, dest, units.data(), length, target, allocate, release);
        if (!dest) status.memory.store(slot, bits, result);
        co_return U_SUCCESS(status.value);
    };
    std::int64_t start{};
    while (count < capacity - 1) {
        const auto found = co_await run_match(object, status.value, [&] { return uregex_findNext(native, &status.value); });
        if (U_FAILURE(status.value) || !found) break;
        const auto delimiter = uregex_start64(native, 0, &status.value);
        const auto next = uregex_end64(native, 0, &status.value);
        if (!(co_await emit(start, delimiter))) co_return static_cast<std::uint32_t>(count);
        for (std::int32_t group = 1; group <= groups && count < capacity - 1; ++group) {
            const auto left = uregex_start64(native, group, &status.value);
            const auto right = uregex_end64(native, group, &status.value);
            if (!(co_await emit(left, right))) co_return static_cast<std::uint32_t>(count);
        }
        start = next;
        if (start == limit) break;
    }
    if (U_SUCCESS(status.value)) (void)co_await emit(start, limit);
    co_return static_cast<std::uint32_t>(count);
}
