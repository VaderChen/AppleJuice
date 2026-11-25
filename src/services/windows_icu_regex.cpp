#include "x64arm/windows_services.hpp"
#include "icu_regex_abi.hpp"
#include "icu_regex_callbacks.hpp"
#include "icu_text_input_task.hpp"
#include "icu_text_output_task.hpp"
#include "icu_text_guest.hpp"
#include <unicode/ustring.h>
#include <unicode/unistr.h>

namespace x64arm {
namespace {
using namespace icu_abi;
struct GuestPattern {
    std::uint64_t view{};
};

struct Regex {
    std::shared_ptr<GuestPattern> pattern;
    std::unique_ptr<IcuTextInput<UChar>> text;
    icu_text::SharedProjection projected;
    std::shared_ptr<std::uint64_t> guest_provider;
    std::unique_ptr<URegularExpression, decltype(&uregex_close)> native{nullptr, uregex_close};
    icu_regex::Handlers handlers;
    bool matching{};
    std::uint64_t guest_input{}, pattern_copy{}, input_copy{};
};

struct MatchingScope {
    explicit MatchingScope(Regex& object) : flag(object.matching), previous(std::exchange(flag, true)) {}
    ~MatchingScope() { flag = previous; }
    bool& flag;
    bool previous;
};

// 部分錯誤仍會寫出前綴。保留原有內容，只呼叫一次會改變 matcher 狀態的 ICU 操作。
class Output {
public:
    Output(GuestMemory& memory, std::uint64_t address, std::int32_t capacity)
        : memory_(memory), address_(address) {
        if (address && capacity > 0) {
            const auto size = std::size_t(capacity) * sizeof(UChar);
            memory.check(GuestAddress{address}, size, Access::Write);
            data_.resize(static_cast<std::size_t>(capacity));
            memory.read(GuestAddress{address}, {reinterpret_cast<std::uint8_t*>(data_.data()), size});
            original_ = data_;
        }
    }
    UChar* data() { return !address_ ? nullptr : data_.empty() ? &empty_ : data_.data(); }
    void commit() {
        for (std::size_t i = 0; i < data_.size();) {
            if (data_[i] == original_[i]) { ++i; continue; }
            const auto first = i++;
            while (i < data_.size() && data_[i] != original_[i]) ++i;
            memory_.write(GuestAddress{address_ + first * sizeof(UChar)}, icu_bytes(data_.data() + first, i - first));
        }
    }
private:
    GuestMemory& memory_;
    std::uint64_t address_;
    std::vector<UChar> data_, original_;
    UChar empty_{};
};

// 暫存配置與續行同壽；客體關閉或複製 matcher 時仍可安全完成目前呼叫。
template<class Target, class Allocate, class Release>
service_callback::Task<std::uint64_t> pattern_view(Regex& object, IcuStatus& status, bool x86,
        Target target, Allocate allocate, Release release) {
    auto* view = uregex_patternUText(object.native.get(), &status.value);
    if (!view || U_FAILURE(status.value)) co_return 0;
    if (object.pattern->view) co_return object.pattern->view;
    std::int32_t length{};
    const auto* units = uregex_pattern(object.native.get(), &length, &status.value);
    if (!units || U_FAILURE(status.value)) co_return 0;
    if (length < 0) length = u_strlen(units);
    const auto index = utext_getNativeIndex(view);
    // 客體 UText 使用 UTEXT_INITIALIZER 的公開配置，後面緊接它所參照的 UTF-16。
    const auto header = x86 ? 112U : 144U;
    const auto address = allocate(header + (std::size_t(length) + 1) * sizeof(UChar));
    if (!address) { status.value = U_MEMORY_ALLOCATION_ERROR; co_return 0; }
    struct Pending {
        std::uint64_t address;
        Release& free;
        ~Pending() { if (address) free(address); }
    } pending{address, release};
    auto& memory = status.memory;
    memory.fill(GuestAddress{address}, 0, header);
    memory.store(GuestAddress{address}, 32, UTEXT_MAGIC);
    memory.store(GuestAddress{address + 12}, 32, header);
    memory.write(GuestAddress{address + header}, icu_bytes(units, std::size_t(length) + 1));
    status.commit();
    GuestCallback open{target("utext_openUChars"),
        {address, address + header, static_cast<std::uint32_t>(length), status.address.value}};
    open.wide_arguments = 4;
    const auto opened = co_await service_callback::call_native(std::move(open));
    status.value = static_cast<UErrorCode>(signed_argument(memory.load(status.address, 32)));
    if (!opened && U_SUCCESS(status.value)) status.value = U_MEMORY_ALLOCATION_ERROR;
    if (U_FAILURE(status.value)) co_return 0;
    if (index) {
        GuestCallback seek{target("utext_setNativeIndex"), {address, static_cast<std::uint64_t>(index)}};
        seek.wide_arguments = 2;
        (void)co_await service_callback::call_native(std::move(seek));
    }
    if (!object.pattern) { status.value = U_ILLEGAL_ARGUMENT_ERROR; co_return 0; }
    auto& cached = object.pattern->view;
    if (!cached) cached = std::exchange(pending.address, 0);
    co_return cached;
}

template<class Target, class Allocate, class Release>
service_callback::Task<bool> append_text(Regex& object, IcuStatus& status, std::uint64_t source,
        std::uint64_t dest, Target target, Allocate allocate, Release release, UText* local_output = nullptr) {
    MatchingScope guard(object);
    auto* native = object.native.get();
    icu_text::LocalText replacement;
    std::int64_t guest_index{};
    auto read_index = [&]() -> service_callback::Task<std::int64_t> {
        GuestCallback query{target("utext_getNativeIndex"), {source}};
        query.wide_result = true;
        co_return std::bit_cast<std::int64_t>(co_await service_callback::call_native(std::move(query)));
    };
    auto load_input = [&]() -> service_callback::Task<bool> {
        auto projected = co_await icu_text::project_input(status.memory, target(icu_text::project_export), source, status, release);
        co_return projected && icu_text::open_projection(&replacement.value, std::move(projected), &status.value);
    };
    if (source) {
        (void)uregex_start64(native, 0, &status.value);
        if (U_FAILURE(status.value)) co_return false;
        guest_index = co_await read_index();
        if (!(co_await load_input())) co_return false;
        utext_setNativeIndex(&replacement.value, guest_index);
    }
    // 提供者可觀察或移動替換游標；若原地寫入，下一次掃描也必須看見更新後的文字。
    auto sync = [&](bool after, bool write) -> service_callback::Task<bool> {
        if (!source) co_return true;
        if (after) {
            guest_index = co_await read_index();
            if (write && dest == source && U_SUCCESS(status.value) && !(co_await load_input())) co_return false;
            utext_setNativeIndex(&replacement.value, guest_index);
        } else {
            const auto index = utext_getNativeIndex(&replacement.value);
            if (index != guest_index) {
                GuestCallback seek{target("utext_setNativeIndex"), {source, static_cast<std::uint64_t>(index)}};
                seek.wide_arguments = 2;
                (void)co_await service_callback::call_native(std::move(seek));
                guest_index = index;
            }
        }
        co_return true;
    };
    auto append = [&](UText* output) {
        if (source) uregex_appendReplacementUText(native, &replacement.value, output, &status.value);
        else (void)uregex_appendTailUText(native, output, &status.value);
    };
    if (local_output) append(local_output);
    else {
        icu_text::WritableDestination writer;
        (void)co_await writer.write(status.memory, dest, target("utext_nativeLength"), target("utext_replace"),
                                   status, append, allocate, release, sync);
    }
    (void)co_await sync(false, false);
    co_return U_SUCCESS(status.value);
}

// getUText 寫入既有目的；groupUText 則將目的重開為整份輸入的唯讀淺複本。
template<class Target, class Allocate, class Release>
service_callback::Task<std::uint64_t> subject_view(Regex* object, IcuStatus& status, Target target,
        Allocate allocate, Release release, std::uint64_t dest, std::optional<std::int32_t> group,
        std::uint64_t size_out, std::shared_ptr<std::uint64_t> provider) {
    auto* native = object ? object->native.get() : nullptr;
    if (!group && dest) {
        if (!object || U_FAILURE(status.value)) {
            if (U_SUCCESS(status.value)) status.value = U_ILLEGAL_ARGUMENT_ERROR;
            co_return dest;
        }
        MatchingScope guard(*object);
        icu_text::WritableDestination writer;
        (void)co_await writer.write(status.memory, dest, target("utext_nativeLength"), target("utext_replace"), status,
            [&](UText* output) { (void)uregex_getUText(native, output, &status.value); }, allocate, release,
            [](bool, bool) -> service_callback::Task<bool> { co_return true; });
        co_return dest;
    }
    // C 介面在未設定輸入、無效 handle 或既有錯誤時保留群組長度；matcher 檢查失敗則寫零。
    std::int64_t length = INT64_MIN;
    std::unique_ptr<UText, decltype(&utext_close)> view(group
        ? uregex_groupUText(native, *group, nullptr, &length, &status.value)
        : uregex_getUText(native, nullptr, &status.value), utext_close);
    if (group && length != INT64_MIN) {
        if (!size_out) { status.value = U_ILLEGAL_ARGUMENT_ERROR; co_return dest; }
        status.memory.store(GuestAddress{size_out}, 64, static_cast<std::uint64_t>(length));
    }
    if (!view || (U_FAILURE(status.value) && dest)) co_return dest;
    const auto index = utext_getNativeIndex(view.get());
    const bool empty_on_error = U_FAILURE(status.value);
    // 無效的群組呼叫仍可能要求建立空 UText；獨立的狀態欄可保留呼叫端原錯誤。
    const auto scratch = empty_on_error ? allocate(4) : 0;
    if (empty_on_error && !scratch) co_return dest;
    struct Scratch {
        std::uint64_t address;
        Release& free;
        ~Scratch() { if (address) free(address); }
    } owner{scratch, release};
    const auto error_slot = scratch ? scratch : status.address.value;
    if (scratch) status.memory.store(GuestAddress{scratch}, 32, U_ZERO_ERROR);
    else status.commit();
    // 呼叫提供者的 clone 可重入並關閉 matcher。這份引用持續到客體完成複製。
    std::uint64_t result{};
    if (provider && !empty_on_error) {
        result = co_await service_callback::call_value(target("utext_clone"), {dest, *provider, 0, 1, error_slot});
    } else {
        const auto input = object && !empty_on_error ? object->guest_input : 0;
        const auto size = input ? static_cast<std::uint64_t>(static_cast<std::int64_t>(object->text->length)) : 0;
        GuestCallback open{target("utext_openUChars"), {dest, input, size, error_slot}};
        open.wide_arguments = 4;
        result = co_await service_callback::call_native(std::move(open));
    }
    const auto error = static_cast<UErrorCode>(signed_argument(status.memory.load(GuestAddress{error_slot}, 32)));
    if (!scratch) status.value = error;
    if (result && U_SUCCESS(error) && !empty_on_error) {
        GuestCallback seek{target("utext_setNativeIndex"), {result, static_cast<std::uint64_t>(index)}};
        seek.wide_arguments = 2;
        (void)co_await service_callback::call_native(std::move(seek));
    }
    co_return result;
}

bool matches_text(std::string_view name) {
    return name.starts_with("uregex_find") || name.starts_with("uregex_matches") ||
           name.starts_with("uregex_lookingAt") || name == "uregex_replaceAll" ||
           name == "uregex_replaceFirst" || name == "uregex_split";
}

template<class Work>
service_callback::Task<std::int32_t> run_match(Regex& object, UErrorCode& status, Work work) {
    if (!object.handlers.enabled()) co_return work();
    MatchingScope guard(object);
    icu_regex::CallbackBridge bridge(object.native.get(), object.handlers, status);
    if (U_FAILURE(status)) co_return 0;
    co_return co_await bridge.execute(work);
}

#include "icu_regex_transform.inc"

service_callback::Task<std::optional<std::uint32_t>> perform_match(GuestState& state, GuestMemory& memory,
        WindowsServiceDescriptor service, std::shared_ptr<Regex> object) {
    const WindowsCall call(state, memory, service);
    const auto name = service.symbol;
    const auto integer = [&](unsigned i) { return signed_argument(call(i)); };
    IcuStatus status(memory, call(service.arguments - 1));
    const auto initial = status.value;
    const auto done = [&](std::int32_t result = 0) {
        if (status.value != initial) status.commit();
        call.return_to(state, static_cast<std::uint32_t>(result));
        return std::optional<std::uint32_t>{};
    };
    if (U_FAILURE(initial)) co_return done();
    if (!object || object->matching) {
        status.value = object ? U_REGEX_INVALID_STATE : U_ILLEGAL_ARGUMENT_ERROR;
        co_return done();
    }
    auto* native = object->native.get();
    try {
        if (name == "uregex_replaceAll" || name == "uregex_replaceFirst") {
            const IcuTextInput<UChar> text(memory, call(1), integer(2), status.value);
            if (U_FAILURE(status.value)) co_return done();
            const auto capacity = integer(4);
            Output buffer(memory, call(3), capacity);
            const auto replace = name == "uregex_replaceAll" ? uregex_replaceAll : uregex_replaceFirst;
            const auto length = co_await run_match(*object, status.value, [&] {
                return replace(native, text.get(), text.length, buffer.data(), capacity, &status.value);
            });
            buffer.commit();
            co_return done(length);
        }
        if (name == "uregex_split") {
            const auto fields = call(4);
            const auto slots = integer(5);
            if (!fields || slots < 1) { status.value = U_ILLEGAL_ARGUMENT_ERROR; co_return done(); }
            const auto stride = memory.execution_mode() == ExecutionMode::X86 ? 4U : 8U;
            memory.check(GuestAddress{fields}, std::size_t(slots) * stride, Access::Write);
            UChar untouched{};
            std::vector<UChar*> pointers(static_cast<std::size_t>(slots), &untouched);
            std::int32_t required = -1;
            const auto address = call(1);
            const auto capacity = integer(2);
            Output buffer(memory, address, capacity);
            const auto count = co_await run_match(*object, status.value, [&] {
                return uregex_split(native, buffer.data(), capacity, &required, pointers.data(), slots, &status.value);
            });
            buffer.commit();
            if (call(3) && required >= 0) memory.store(GuestAddress{call(3)}, 32, static_cast<std::uint32_t>(required));
            for (std::size_t i = 0; i < pointers.size(); ++i) {
                if (pointers[i] == &untouched) continue;
                // 容量不足時 ICU 仍可回傳所需位置，可能超過實際輸出緩衝區。
                const auto offset = reinterpret_cast<std::uintptr_t>(pointers[i]) - reinterpret_cast<std::uintptr_t>(buffer.data());
                memory.store(GuestAddress{fields + i * stride}, static_cast<std::uint8_t>(stride * 8), pointers[i] ? address + offset : 0);
            }
            co_return done(count);
        }
        if (name == "uregex_findNext")
            co_return done(co_await run_match(*object, status.value, [&] { return uregex_findNext(native, &status.value); }));
        const auto position = name.ends_with("64") ? static_cast<std::int64_t>(call(1)) : integer(1);
        const auto match = name.starts_with("uregex_find") ? uregex_find64 :
                           name.starts_with("uregex_lookingAt") ? uregex_lookingAt64 : uregex_matches64;
        co_return done(co_await run_match(*object, status.value, [&] { return match(native, position, &status.value); }));
    } catch (const std::bad_alloc&) {
        status.value = U_MEMORY_ALLOCATION_ERROR;
    } catch (const std::system_error&) {
        status.value = U_INTERNAL_PROGRAM_ERROR;
    }
    co_return done();
}

}

struct IcuRegexState {
    bool used_callbacks{}, used_providers{};
    // 保留每個已取得的客體提供者，最後的 matcher 或讀取續行釋放引用後才呼叫其 close。
    std::vector<std::shared_ptr<std::uint64_t>> providers;
    service_callback::Task<bool> collect_providers(std::uint64_t close) {
        for (;;) {
            const auto ready = std::find_if(providers.begin(), providers.end(), [](const auto& p) { return p.use_count() == 1; });
            if (ready == providers.end()) co_return true;
            const auto address = **ready;
            providers.erase(ready);
            if (address) (void)co_await service_callback::call_value(close, {address});
        }
    }
    std::map<std::uint64_t, std::shared_ptr<Regex>> objects;
    std::shared_ptr<Regex> lookup(std::uint64_t handle) const {
        const auto found = objects.find(handle);
        return found == objects.end() ? nullptr : found->second;
    }
};

std::span<const WindowsServiceDescriptor> icu_regex_service_descriptors() {
    static constexpr WindowsServiceDescriptor entries[]{
#define ICU_REGEX_API(name) icu_regex::descriptor(#name, &name),
#define ICU_REGEX_SCALAR ICU_REGEX_API
#include "icu_regex_services.inc"
#undef ICU_REGEX_SCALAR
#undef ICU_REGEX_API
    };
    return entries;
}

std::optional<std::uint32_t> WindowsServices::invoke_icu_regex(GuestState& state, GuestMemory& memory,
                                                               const WindowsServiceDescriptor& service) {
    const auto name = service.symbol;
    if (name == "uregex_replaceAllUText" || name == "uregex_replaceFirstUText" || name == "uregex_splitUText") {
        return nested_continuation(state, memory, name,
            [this,service](GuestState& s, GuestMemory& m) -> service_callback::Task<std::optional<std::uint32_t>> {
                const WindowsCall call(s, m, service);
                const auto object = icu_regex_ ? icu_regex_->lookup(call(0)) : nullptr;
                IcuStatus status(m, call(service.arguments - 1));
                std::uint64_t result{};
                const auto base = icu_text::image_address(x86_);
                auto target = [&](const char* fn) { return base + runtime_.builtin_images.at(base).export_names.at(fn); };
                if (U_SUCCESS(status.value)) {
                    if (!object) status.value = U_ILLEGAL_ARGUMENT_ERROR;
                    else if (object->matching) status.value = U_REGEX_INVALID_STATE;
                    else try {
                        MatchingScope guard(*object);
                        auto allocate = [&](std::size_t n) { return allocate_heap(m, n); };
                        auto release = [&](std::uint64_t a) { (void)free_heap(m, a, s); };
                        if (service.symbol == "uregex_splitUText")
                            result = co_await split_text(*object, status, call(1), signed_argument(call(2)), target, allocate, release);
                        else result = co_await replace_text(*object, status, service.symbol == "uregex_replaceFirstUText",
                                                           call(1), call(2), target, allocate, release);
                    } catch (const std::bad_alloc&) { status.value = U_MEMORY_ALLOCATION_ERROR; }
                    catch (const std::system_error&) { status.value = U_INTERNAL_PROGRAM_ERROR; }
                    if (icu_regex_ && !icu_regex_->providers.empty())
                        (void)co_await icu_regex_->collect_providers(target("utext_close"));
                    status.commit();
                }
                call.return_to(s, result);
                co_return std::optional<std::uint32_t>{};
            });
    }
    if (name == "uregex_getUText" || name == "uregex_groupUText") {
        return nested_continuation(state, memory, name,
            [this,service](GuestState& s, GuestMemory& m) -> service_callback::Task<std::optional<std::uint32_t>> {
                const WindowsCall call(s, m, service);
                const auto object = icu_regex_ ? icu_regex_->lookup(call(0)) : nullptr;
                const bool group = service.symbol == "uregex_groupUText";
                std::uint64_t result = call(group ? 2 : 1);
                IcuStatus status(m, call(service.arguments - 1));
                const auto base = icu_text::image_address(x86_);
                auto target = [&](const char* fn) { return base + runtime_.builtin_images.at(base).export_names.at(fn); };
                try {
                    if (object && object->matching) {
                        if (U_SUCCESS(status.value)) status.value = U_REGEX_INVALID_STATE;
                    } else result = co_await subject_view(object.get(), status, target,
                        [&](std::size_t n) { return allocate_heap(m, n); },
                        [&](std::uint64_t a) { (void)free_heap(m, a, s); }, result,
                        group ? std::optional{signed_argument(call(1))} : std::nullopt, group ? call(3) : 0,
                        object ? object->guest_provider : nullptr);
                } catch (const std::bad_alloc&) { status.value = U_MEMORY_ALLOCATION_ERROR; }
                catch (const std::system_error&) { status.value = U_INTERNAL_PROGRAM_ERROR; }
                if (icu_regex_ && !icu_regex_->providers.empty())
                    (void)co_await icu_regex_->collect_providers(target("utext_close"));
                status.commit();
                call.return_to(s, result);
                co_return std::optional<std::uint32_t>{};
            });
    }
    if (name == "uregex_patternUText" || name == "uregex_appendReplacementUText" || name == "uregex_appendTailUText") {
        return nested_continuation(state, memory, name,
            [this,service](GuestState& s, GuestMemory& m) -> service_callback::Task<std::optional<std::uint32_t>> {
                const WindowsCall call(s, m, service);
                const auto handle = call(0);
                const auto object = icu_regex_ ? icu_regex_->lookup(handle) : nullptr;
                const bool tail = service.symbol == "uregex_appendTailUText";
                std::uint64_t result = tail ? call(1) : 0;
                IcuStatus status(m, call(service.arguments - 1));
                if (U_SUCCESS(status.value)) {
                    if (!object) status.value = U_ILLEGAL_ARGUMENT_ERROR;
                    else if (object->matching) status.value = U_REGEX_INVALID_STATE;
                    else try {
                        const auto base = icu_text::image_address(x86_);
                        const auto& exports = runtime_.builtin_images.at(base).export_names;
                        auto target = [&](const char* fn) { return base + exports.at(fn); };
                        auto allocate = [&](std::size_t size) { return allocate_heap(m, size); };
                        auto release = [&](std::uint64_t address) { (void)free_heap(m, address, s); };
                        if (service.symbol == "uregex_patternUText") {
                            result = co_await pattern_view(*object, status, x86_, target, allocate, release);
                        } else {
                            const auto dest = call(tail ? 1 : 2);
                            if (!dest || (!tail && !call(1))) status.value = U_ILLEGAL_ARGUMENT_ERROR;
                            else (void)co_await append_text(*object, status, tail ? 0 : call(1), dest, target, allocate, release);
                        }
                    } catch (const std::bad_alloc&) {
                        status.value = U_MEMORY_ALLOCATION_ERROR;
                    } catch (const std::system_error&) {
                        status.value = U_INTERNAL_PROGRAM_ERROR;
                    }
                    status.commit();
                }
                call.return_to(s, result);
                co_return std::optional<std::uint32_t>{};
            });
    }
    if (matches_text(name)) {
        const WindowsCall call(state, memory, service);
        const auto object = icu_regex_ ? icu_regex_->lookup(call(0)) : nullptr;
        auto execute = [service, object](GuestState& s, GuestMemory& m) {
            return perform_match(s, m, service, object);
        };
        if ((object && object->handlers.enabled()) ||
            (icu_regex_ && icu_regex_->used_callbacks && service_callback::completed(this, threads_.current, name, state)))
            return nested_continuation(state, memory, name, execute);
        auto task = execute(state, memory);
        task.address().resume();
        return task.take();
    }
    const bool release_input = name == "uregex_close" || name == "uregex_setText";
    if (release_input && icu_regex_ && icu_regex_->used_providers &&
        (!icu_regex_->providers.empty() || service_callback::completed(this, threads_.current, name, state))) {
        return nested_continuation(state, memory, name,
            [this,service](GuestState& s, GuestMemory& m) -> service_callback::Task<std::optional<std::uint32_t>> {
                auto finished = s;
                const auto result = invoke_icu_regex(finished, m, service, nullptr);
                const auto base = icu_text::image_address(x86_);
                (void)co_await icu_regex_->collect_providers(base + runtime_.builtin_images.at(base).export_names.at("utext_close"));
                finished.tsc = s.tsc;
                s = finished;
                co_return result;
            });
    }
    if (name != "uregex_openUText" && name != "uregex_setUText" && name != "uregex_refreshUText")
        return invoke_icu_regex(state, memory, service, nullptr);
    return nested_continuation(state, memory, name,
        [this,service](GuestState& s, GuestMemory& m) -> service_callback::Task<std::optional<std::uint32_t>> {
            const WindowsCall call(s, m, service);
            IcuStatus status(m, call(service.arguments - 1));
            const bool open = service.symbol == "uregex_openUText";
            const auto handle = call(0);
            const auto original = !open && icu_regex_ ? icu_regex_->lookup(handle) : nullptr;
            if (U_SUCCESS(status.value) && !open && !original) status.value = U_ILLEGAL_ARGUMENT_ERROR;
            if (U_SUCCESS(status.value) && original && original->matching) status.value = U_REGEX_INVALID_STATE;
            auto finished = s;
            bool invoked = false;
            const auto base = icu_text::image_address(x86_);
            auto target = [&](const char* fn) { return base + runtime_.builtin_images.at(base).export_names.at(fn); };
            if (U_SUCCESS(status.value)) {
                try {
                    std::shared_ptr<std::uint64_t> lease;
                    if (!open) {
                        lease = std::make_shared<std::uint64_t>(0);
                        icu_regex_->providers.push_back(lease);
                        icu_regex_->used_providers = true;
                    }
                    auto projection = co_await icu_text::project_input(m,
                        target(open ? icu_text::project_export : icu_text::retain_project_export),
                        open ? handle : call(1), status, [&](std::uint64_t a) { (void)free_heap(m, a, s); }, lease.get());
                    if (projection) {
                        if (!open && original != icu_regex_->lookup(handle)) status.value = U_ILLEGAL_ARGUMENT_ERROR;
                        else {
                            finished = s;
                            (void)invoke_icu_regex(finished, m, service, std::move(projection));
                            invoked = true;
                            if (lease && U_SUCCESS(static_cast<UErrorCode>(signed_argument(m.load(status.address, 32)))))
                                original->guest_provider = std::move(lease);
                        }
                    }
                } catch (const std::bad_alloc&) { status.value = U_MEMORY_ALLOCATION_ERROR; }
            }
            if (!open && icu_regex_ && !icu_regex_->providers.empty())
                (void)co_await icu_regex_->collect_providers(target("utext_close"));
            if (invoked) {
                finished.tsc = s.tsc;
                s = finished;
            } else {
                status.commit();
                call.return_to(s, 0);
            }
            co_return std::optional<std::uint32_t>{};
        });
}

std::optional<std::uint32_t> WindowsServices::invoke_icu_regex(GuestState& state, GuestMemory& memory,
        const WindowsServiceDescriptor& service, icu_text::SharedProjection projection) {
    const WindowsCall call(state, memory, service);
    const auto name = service.symbol;
    const auto handle = call(0);
    const auto object = icu_regex_ ? icu_regex_->lookup(handle) : nullptr;
    auto* native = object ? object->native.get() : nullptr;
    const auto release = [&](std::uint64_t address) {
        if (address) (void)free_heap(memory, address, state);
    };
    if (name == "uregex_close") {
        if (object) {
            if (object->pattern.use_count() == 1) release(object->pattern->view);
            object->pattern.reset();
            object->guest_provider.reset();
            release(object->pattern_copy);
            release(object->input_copy);
            icu_regex_->objects.erase(handle);
            release(handle);
        }
        call.return_to(state, 0);
        return {};
    }

    IcuStatus status(memory, call(service.arguments - 1));
    const auto initial = status.value;
    if (object && object->matching && !name.starts_with("uregex_open")) {
        if (U_SUCCESS(initial)) {
            status.value = U_REGEX_INVALID_STATE;
            status.commit();
        }
        call.return_to(state, 0, name.ends_with("64"));
        return {};
    }
#define ICU_REGEX_API(name)
#define ICU_REGEX_SCALAR(fn) if (name == #fn) { icu_regex::scalar(&fn, native, call, state, status); return {}; }
#include "icu_regex_services.inc"
#undef ICU_REGEX_SCALAR
#undef ICU_REGEX_API
    const auto done = [&](std::uint64_t result = 0) {
        if (status.value != initial) status.commit();
        call.return_to(state, result);
        return std::optional<std::uint32_t>{};
    };
    const auto invalid = [&] { status.value = U_ILLEGAL_ARGUMENT_ERROR; return done(); };
    const auto integer = [&](unsigned i) { return signed_argument(call(i)); };
    const bool append = name == "uregex_appendReplacement" || name == "uregex_appendTail";
    if (U_FAILURE(initial) && !(append && initial == U_BUFFER_OVERFLOW_ERROR)) return done();
    try {
        if (name == "uregex_open" || name == "uregex_openC" || name == "uregex_openUText" || name == "uregex_clone") {
            Regex result;
            if (name == "uregex_clone") {
                result.native.reset(uregex_clone(native, &status.value));
            } else {
                static_assert(sizeof(UParseError) == 72 && offsetof(UParseError, postContext) == 40);
                const auto address = call(service.arguments - 2);
                UParseError error{};
                read_icu_record(memory, address, error);
                auto* parse = address ? &error : nullptr;
                if (name == "uregex_openUText") {
                    icu_text::LocalText pattern;
                    if (icu_text::open_projection(&pattern.value, projection, &status.value))
                        result.native.reset(uregex_openUText(&pattern.value, static_cast<std::uint32_t>(call(1)), parse, &status.value));
                } else if (name == "uregex_openC") {
                    const IcuTextInput<char> pattern(memory, handle, -1, status.value);
                    result.native.reset(uregex_openC(pattern.get(), static_cast<std::uint32_t>(call(1)), parse, &status.value));
                } else {
                    const IcuTextInput<UChar> pattern(memory, handle, integer(1), status.value);
                    result.native.reset(uregex_open(pattern.get(), pattern.length, static_cast<std::uint32_t>(call(2)), parse, &status.value));
                }
                write_icu_record(memory, address, error);
            }
            if (!result.native || U_FAILURE(status.value)) return done();
            result.pattern = name == "uregex_clone" ? object->pattern : std::make_shared<GuestPattern>();
            if (!icu_regex_) icu_regex_ = std::make_shared<IcuRegexState>();
            if (status.value != initial) memory.check(status.address, 4, Access::Write);
            const auto address = allocate_heap(memory, 8);
            if (!address) throw std::bad_alloc{};
            try { icu_regex_->objects.emplace(address, std::make_shared<Regex>(std::move(result))); }
            catch (...) { release(address); throw; }
            return done(address);
        }
        if (!object) return invalid();
        if (name.ends_with("Callback")) {
            auto& handler = name.ends_with("MatchCallback") ? object->handlers.match : object->handlers.progress;
            if (name.starts_with("uregex_set")) {
                handler = {call(1), call(2)};
                if (handler.function) icu_regex_->used_callbacks = true;
            } else {
                const auto fn = call(1), context = call(2);
                if (!fn || !context) return invalid();
                const std::uint8_t bits = x86_ ? 32 : 64;
                memory.check(GuestAddress{fn}, bits / 8, Access::Write);
                memory.check(GuestAddress{context}, bits / 8, Access::Write);
                memory.store(GuestAddress{fn}, bits, handler.function);
                memory.store(GuestAddress{context}, bits, handler.context);
            }
            return done();
        }
        if (name == "uregex_setUText" || name == "uregex_refreshUText") {
            icu_text::LocalText input;
            if (!icu_text::open_projection(&input.value, projection, &status.value)) return done();
            if (name == "uregex_setUText") {
                uregex_setUText(native, &input.value, &status.value);
                if (U_SUCCESS(status.value)) object->text.reset();
            } else {
                uregex_refreshUText(native, &input.value, &status.value);
            }
            if (U_SUCCESS(status.value)) {
                object->projected = std::move(projection);
                object->guest_input = 0;
                release(std::exchange(object->input_copy, 0));
            }
            return done();
        }
        if (name == "uregex_setText") {
            auto text = std::make_unique<IcuTextInput<UChar>>(memory, call(1), integer(2), status.value);
            uregex_setText(native, text->get(), text->length, &status.value);
            if (U_SUCCESS(status.value)) {
                object->text = std::move(text);
                object->projected.reset();
                object->guest_provider.reset();
                object->guest_input = call(1);
                release(std::exchange(object->input_copy, 0));
            }
            return done();
        }
        if (name == "uregex_getText" || name == "uregex_pattern") {
            const bool pattern = name == "uregex_pattern";
            std::int32_t length{};
            const UChar* text;
            if (pattern) text = uregex_pattern(native, &length, &status.value);
            else if (object->projected) {
                // UTF-16 副本不能改變原生 matcher 使用的索引單位。
                text = object->projected->units.c_str();
                length = static_cast<std::int32_t>(object->projected->units.size());
            } else text = uregex_getText(native, &length, &status.value);
            if (!text || U_FAILURE(status.value)) return done();
            auto address = pattern ? object->pattern_copy : object->guest_input;
            auto& copy = pattern ? object->pattern_copy : object->input_copy;
            if (!address && !copy) {
                const auto count = std::size_t(length < 0 ? u_strlen(text) : length);
                const auto slot = allocate_heap(memory, (count + 1) * sizeof(UChar));
                if (!slot) throw std::bad_alloc{};
                try {
                    if (count) memory.write(GuestAddress{slot}, icu_bytes(text, count));
                    memory.store(GuestAddress{slot + count * 2}, 16, 0);
                } catch (...) { release(slot); throw; }
                copy = slot;
            }
            if (call(1)) memory.store(GuestAddress{call(1)}, 32, static_cast<std::uint32_t>(length));
            return done(address ? address : copy);
        }
        if (name == "uregex_groupNumberFromCName") {
            const IcuTextInput<char> text(memory, call(1), integer(2), status.value);
            if (!text.get()) return invalid();
            return done(static_cast<std::uint32_t>(uregex_groupNumberFromCName(native, text.get(), text.length, &status.value)));
        }
        if (name == "uregex_groupNumberFromName") {
            const IcuTextInput<UChar> text(memory, call(1), integer(2), status.value);
            return done(static_cast<std::uint32_t>(uregex_groupNumberFromName(native, text.get(), text.length, &status.value)));
        }

        if (append) {
            const bool tail = name == "uregex_appendTail";
            const auto slot = call(tail ? 1 : 3), remaining = call(tail ? 2 : 4);
            if (!slot || !remaining) return invalid();
            const std::uint8_t bits = x86_ ? 32U : 64U;
            auto capacity = signed_argument(memory.load(GuestAddress{remaining}, 32));
            if (U_FAILURE(initial) && capacity != 0) return done();
            const auto address = memory.load(GuestAddress{slot}, bits);
            Output buffer(memory, address, capacity);
            auto* dest = buffer.data();
            auto error = U_ZERO_ERROR;
            std::int32_t length{};
            if (tail) length = uregex_appendTail(native, &dest, &capacity, &status.value);
            else {
                const IcuTextInput<UChar> text(memory, call(1), integer(2), error);
                if (U_FAILURE(error)) return invalid();
                length = uregex_appendReplacement(native, text.get(), text.length, &dest, &capacity, &status.value);
            }
            buffer.commit();
            if (dest != buffer.data()) memory.store(GuestAddress{slot}, bits, address + static_cast<std::size_t>(dest - buffer.data()) * sizeof(UChar));
            memory.store(GuestAddress{remaining}, 32, static_cast<std::uint32_t>(capacity));
            return done(static_cast<std::uint32_t>(length));
        }
        if (name == "uregex_group") {
            Output buffer(memory, call(2), integer(3));
            const auto length = uregex_group(native, integer(1), buffer.data(), integer(3), &status.value);
            buffer.commit();
            return done(static_cast<std::uint32_t>(length));
        }
        throw HostCallFault("未知的 ICU 正規表示式入口");
    } catch (const std::bad_alloc&) {
        status.value = U_MEMORY_ALLOCATION_ERROR;
        return done();
    }
}
}
