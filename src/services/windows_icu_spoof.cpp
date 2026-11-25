#include "x64arm/windows_services.hpp"
#include "icu_guest_abi.hpp"
#include "icu_set_state.hpp"
#include "icu_spoof_data.hpp"
#include <unicode/uspoof.h>
#include <cstring>

namespace x64arm {
namespace {
using namespace icu_abi;
using OwnedChecker=std::unique_ptr<USpoofChecker,decltype(&uspoof_close)>;
using OwnedResult=std::unique_ptr<USpoofCheckResult,decltype(&uspoof_closeCheckResult)>;

struct SpoofData {
    icu_spoof::Data bytes;
    icu_spoof::Data identity;
};

struct Checker {
    // ICU 複製檢查器時會共用資料，最後一個檢查器釋放後才能釋放客體快照。
    std::shared_ptr<const SpoofData> data;
    OwnedChecker native{nullptr,uspoof_close};
    std::uint64_t chars{},locales{};
};

struct CheckResult {
    OwnedResult native{nullptr,uspoof_closeCheckResult};
    std::uint64_t numerics{};
};

void open_spoof_data(Checker& checker,icu_spoof::Data bytes,UErrorCode& status){
    if(!icu_spoof::valid_data(bytes)){status=U_INVALID_FORMAT_ERROR;return;}
    auto data=std::make_shared<SpoofData>();
    data->bytes=std::move(bytes);
    // ICU 查表時會讀取第一筆。空映射以等價的恆等映射承接，序列化保留原始資料。
    if(icu_spoof::empty_data(data->bytes))data->identity=icu_spoof::identity_data();
    const auto& provider=data->identity.empty()?data->bytes:data->identity;
    checker.native.reset(uspoof_openFromSerialized(provider.data(),static_cast<std::int32_t>(provider.size()),
                                                 nullptr,&status));
    checker.data=std::move(data);
}

icu_spoof::Data read_spoof_data(const GuestMemory& memory,std::uint64_t address,std::int32_t length,UErrorCode& status){
    if(!address){status=U_ILLEGAL_ARGUMENT_ERROR;return {};}
    if(length<static_cast<std::int32_t>(icu_spoof::header_size)){status=U_INVALID_FORMAT_ERROR;return {};}
    std::array<std::uint8_t,icu_spoof::header_size> header{};
    memory.read(GuestAddress{address},header);
    const auto size=icu_spoof::serialized_size(header,static_cast<std::size_t>(length));
    if(!size){status=U_INVALID_FORMAT_ERROR;return {};}
    memory.check(GuestAddress{address},size,Access::Read);
    icu_spoof::Data bytes(size);
    memory.read(GuestAddress{address},bytes);
    return bytes;
}

static_assert(sizeof(UParseError)==72 && offsetof(UParseError,postContext)==40);
}

struct IcuSpoofState {
    std::map<std::uint64_t,Checker> checkers;
    std::map<std::uint64_t,CheckResult> results;
};

std::span<const WindowsServiceDescriptor> icu_spoof_service_descriptors(){
    static constexpr WindowsServiceDescriptor entries[]{
#define ICU_SPOOF_API(name,count) {"icu.dll",#name,count,0,0,0,true},
#include "icu_spoof_services.inc"
#undef ICU_SPOOF_API
    };
    return entries;
}

std::optional<std::uint32_t> WindowsServices::invoke_icu_spoof(GuestState& state,GuestMemory& memory,
        const WindowsServiceDescriptor& service){
    const WindowsCall call(state,memory,service);
    const auto name=service.symbol;
    const auto finish=[&](std::uint64_t result=0){
        call.return_to(state,result);
        return std::optional<std::uint32_t>{};
    };
    const auto i=[&](unsigned arg){return signed_argument(call(arg));};
    const auto checker_at=[&](std::uint64_t handle)->Checker*{
        if(!icu_spoof_)return nullptr;
        const auto found=icu_spoof_->checkers.find(handle);
        return found==icu_spoof_->checkers.end()?nullptr:&found->second;
    };
    const auto result_at=[&](std::uint64_t handle)->CheckResult*{
        if(!icu_spoof_)return nullptr;
        const auto found=icu_spoof_->results.find(handle);
        return found==icu_spoof_->results.end()?nullptr:&found->second;
    };
    const auto forget_set=[&](std::uint64_t handle){
        if(!handle || !icu_sets_)return;
        const auto found=icu_sets_->objects.find(handle);
        if(found==icu_sets_->objects.end())return;
        if(found->second.ranges)(void)free_heap(memory,found->second.ranges,state);
        icu_sets_->objects.erase(found);
        (void)free_heap(memory,handle,state);
    };
    const auto forget_settings=[&](Checker& checker){
        forget_set(std::exchange(checker.chars,0));
        if(const auto address=std::exchange(checker.locales,0))(void)free_heap(memory,address,state);
    };
    if(name=="uspoof_close" || name=="uspoof_closeCheckResult"){
        const auto handle=call(0);
        if(name=="uspoof_close"){
            if(auto* checker=checker_at(handle)){
                forget_settings(*checker);
                icu_spoof_->checkers.erase(handle);
                (void)free_heap(memory,handle,state);
            }
        }else if(auto* result=result_at(handle)){
            forget_set(result->numerics);
            icu_spoof_->results.erase(handle);
            (void)free_heap(memory,handle,state);
        }
        return finish();
    }
    if(name=="uspoof_getRestrictionLevel" || name=="uspoof_setRestrictionLevel"){
        auto* checker=checker_at(call(0));
        if(name=="uspoof_getRestrictionLevel")
            return finish(static_cast<std::uint32_t>(uspoof_getRestrictionLevel(checker?checker->native.get():nullptr)));
        if(checker)uspoof_setRestrictionLevel(checker->native.get(),static_cast<URestrictionLevel>(i(1)));
        return finish();
    }

    const bool legacy_check=name=="uspoof_check" || name=="uspoof_checkUTF8";
    if(legacy_check && call(3))memory.store(GuestAddress{call(3)},32,0);
    IcuStatus status(memory,call(service.arguments-1));
    const auto initial=status.value;
    const auto done=[&](std::uint64_t result=0){
        if(status.value!=initial)status.commit();
        return finish(result);
    };
    if(U_FAILURE(initial))return finish(name=="uspoof_getCheckResultRestrictionLevel"?USPOOF_UNRESTRICTIVE:0);
    const auto illegal=[&](){status.value=U_ILLEGAL_ARGUMENT_ERROR;return done();};
    const auto out_of_memory=[&](){status.value=U_MEMORY_ALLOCATION_ERROR;return done();};
    const auto adopt=[&](auto& entries,auto& object){
        if(!object.native || U_FAILURE(status.value))return std::uint64_t{};
        if(status.value!=initial)memory.check(status.address,4,Access::Write);
        const auto handle=allocate_heap(memory,8);
        if(!handle){status.value=U_MEMORY_ALLOCATION_ERROR;return std::uint64_t{};}
        try{entries.emplace(handle,std::move(object));}
        catch(...){(void)free_heap(memory,handle,state);throw;}
        return handle;
    };
    const auto set_handle=[&](const USet* set,std::uint64_t& handle){
        if(!set || U_FAILURE(status.value))return std::uint64_t{};
        if(!handle){
            handle=adopt_icu_set(state,memory,{{},set,0});
            if(!handle)status.value=U_MEMORY_ALLOCATION_ERROR;
        }
        return handle;
    };
    try{
        if(name=="uspoof_openCheckResult"){
            if(!icu_spoof_)icu_spoof_=std::make_shared<IcuSpoofState>();
            CheckResult entry;
            entry.native.reset(uspoof_openCheckResult(&status.value));
            return done(adopt(icu_spoof_->results,entry));
        }
        if(name=="uspoof_open" || name=="uspoof_openFromSerialized" || name=="uspoof_openFromSource" || name=="uspoof_clone"){
            if(!icu_spoof_)icu_spoof_=std::make_shared<IcuSpoofState>();
            Checker entry;
            std::int32_t serialized_length{};
            if(name=="uspoof_open")entry.native.reset(uspoof_open(&status.value));
            else if(name=="uspoof_clone"){
                const auto* original=checker_at(call(0));
                if(!original)return illegal();
                entry.native.reset(uspoof_clone(original->native.get(),&status.value));
                entry.data=original->data;
            }else if(name=="uspoof_openFromSerialized"){
                auto bytes=read_spoof_data(memory,call(0),i(1),status.value);
                if(U_FAILURE(status.value))return done();
                serialized_length=static_cast<std::int32_t>(bytes.size());
                open_spoof_data(entry,std::move(bytes),status.value);
                if(entry.native && call(2))memory.check(GuestAddress{call(2)},4,Access::Write);
            }else{
                const IcuTextInput<char> input(memory,call(0),i(1),status.value);
                if(U_FAILURE(status.value))return done();
                UParseError parse{};
                read_icu_record(memory,call(5),parse);
                std::int32_t error_type{};
                const auto* source=input.get()?input.get():"";
                const auto size=input.length<0?std::strlen(source):static_cast<std::size_t>(input.length);
                auto bytes=icu_spoof::compile_source({source,size},error_type,parse,status.value);
                write_icu_record(memory,call(4),error_type);
                write_icu_record(memory,call(5),parse);
                if(U_SUCCESS(status.value))open_spoof_data(entry,std::move(bytes),status.value);
            }
            const auto handle=adopt(icu_spoof_->checkers,entry);
            if(handle && name=="uspoof_openFromSerialized")write_icu_record(memory,call(2),serialized_length);
            return done(handle);
        }

        if(name=="uspoof_getInclusionSet" || name=="uspoof_getRecommendedSet"){
            const auto* native=name=="uspoof_getInclusionSet"?uspoof_getInclusionSet(&status.value):uspoof_getRecommendedSet(&status.value);
            if(!native || U_FAILURE(status.value))return done();
            if(!icu_sets_)icu_sets_=std::make_shared<IcuSetState>();
            auto [entry,inserted]=icu_sets_->borrowed.try_emplace(native,0);
            (void)inserted;
            return done(set_handle(native,entry->second));
        }
        if(name.starts_with("uspoof_getCheckResult")){
            auto* entry=result_at(call(0));
            if(!entry){
                status.value=U_ILLEGAL_ARGUMENT_ERROR;
                return done(name=="uspoof_getCheckResultRestrictionLevel"?USPOOF_UNRESTRICTIVE:0);
            }
            if(name=="uspoof_getCheckResultChecks")
                return done(static_cast<std::uint32_t>(uspoof_getCheckResultChecks(entry->native.get(),&status.value)));
            if(name=="uspoof_getCheckResultRestrictionLevel")
                return done(static_cast<std::uint32_t>(uspoof_getCheckResultRestrictionLevel(entry->native.get(),&status.value)));
            return done(set_handle(uspoof_getCheckResultNumerics(entry->native.get(),&status.value),entry->numerics));
        }

        auto* checker=checker_at(call(0));
        if(!checker)return illegal();
        auto* native=checker->native.get();
        if(name=="uspoof_getChecks")return done(static_cast<std::uint32_t>(uspoof_getChecks(native,&status.value)));
        if(name=="uspoof_setChecks"){
            uspoof_setChecks(native,i(1),&status.value);
            return done();
        }
        if(name=="uspoof_getAllowedChars")return done(set_handle(uspoof_getAllowedChars(native,&status.value),checker->chars));
        if(name=="uspoof_getAllowedLocales"){
            if(checker->locales)return done(checker->locales);
            const auto* text=uspoof_getAllowedLocales(native,&status.value);
            if(!text || U_FAILURE(status.value))return done();
            const auto bytes=std::strlen(text)+1;
            const auto address=allocate_heap(memory,bytes);
            if(!address)return out_of_memory();
            try{memory.write(GuestAddress{address},icu_bytes(text,bytes));}
            catch(...){(void)free_heap(memory,address,state);throw;}
            return done(checker->locales=address);
        }
        if(name=="uspoof_setAllowedChars" || name=="uspoof_setAllowedLocales"){
            if(!call(1))return illegal();
            const bool chars=name=="uspoof_setAllowedChars";
            const auto* set=chars && icu_sets_?icu_sets_->get(call(1)):nullptr;
            if(chars && !set)return illegal();
            OwnedChecker updated(uspoof_clone(native,&status.value),uspoof_close);
            if(!updated || U_FAILURE(status.value))return done();
            if(chars){
                const auto* locales=uspoof_getAllowedLocales(updated.get(),&status.value);
                if(locales && *locales)uspoof_setAllowedLocales(updated.get(),"",&status.value);
                if(U_SUCCESS(status.value))uspoof_setAllowedChars(updated.get(),set,&status.value);
            }else{
                const auto locales=read_ansi(memory,call(1),INT32_MAX);
                uspoof_setAllowedLocales(updated.get(),locales.c_str(),&status.value);
            }
            if(U_SUCCESS(status.value)){
                forget_settings(*checker);
                checker->native=std::move(updated);
            }
            return done();
        }
        if(name=="uspoof_serialize"){
            return finish(icu_output<std::uint8_t>(memory,call(1),i(2),status,IcuOutput::Array,
                [&](auto* output,auto capacity,auto* error){
                    if(!checker->data)return uspoof_serialize(native,output,capacity,error);
                    const auto& bytes=checker->data->bytes;
                    const auto required=static_cast<std::int32_t>(bytes.size());
                    if(capacity<required)*error=U_BUFFER_OVERFLOW_ERROR;
                    else std::memcpy(output,bytes.data(),bytes.size());
                    return required;
                }));
        }

        const bool utf8=name.ends_with("UTF8");
        if(name.starts_with("uspoof_check")){
            CheckResult* entry=nullptr;
            if(!legacy_check && call(3)){
                entry=result_at(call(3));
                if(!entry)return illegal();
            }
            OwnedResult updated(entry?uspoof_openCheckResult(&status.value):nullptr,uspoof_closeCheckResult);
            if(entry && !updated)return done();
            const auto check=[&]<class Char>(auto operation){
                const IcuTextInput<Char> text(memory,call(1),utf8?std::max(i(2),-1):i(2),status.value);
                return U_SUCCESS(status.value)?operation(native,text.get(),text.length,updated.get(),&status.value):0;
            };
            const auto flags=utf8?check.template operator()<char>(uspoof_check2UTF8):
                                  check.template operator()<UChar>(uspoof_check2);
            if(entry && U_SUCCESS(status.value)){
                if(entry->numerics){
                    const auto* set=uspoof_getCheckResultNumerics(updated.get(),&status.value);
                    if(!set || U_FAILURE(status.value))return done();
                    auto& published=icu_sets_->objects.at(entry->numerics);
                    if(!publish_icu_set(state,memory,entry->numerics,published,set))return out_of_memory();
                    published.borrowed=set;
                }
                entry->native=std::move(updated);
            }
            return done(static_cast<std::uint32_t>(flags));
        }
        if(name.starts_with("uspoof_areConfusable")){
            const auto compare=[&]<class Char>(auto operation){
                const IcuTextInput<Char> left(memory,call(1),i(2),status.value),right(memory,call(3),i(4),status.value);
                return U_SUCCESS(status.value)?operation(native,left.get(),left.length,right.get(),right.length,&status.value):0;
            };
            const auto flags=utf8?compare.template operator()<char>(uspoof_areConfusableUTF8):
                                  compare.template operator()<UChar>(uspoof_areConfusable);
            return done(static_cast<std::uint32_t>(flags));
        }
        if(name.starts_with("uspoof_getSkeleton")){
            if(call(4) && !i(5))return illegal();
            const auto skeleton=[&]<class Char>(auto operation){
                const IcuTextInput<Char> text(memory,call(2),i(3),status.value);
                if(U_FAILURE(status.value))return done();
                const auto make=[&](Char* output,std::int32_t capacity,UErrorCode* error){
                    return operation(native,static_cast<std::uint32_t>(call(1)),text.get(),text.length,output,capacity,error);
                };
                if constexpr(sizeof(Char)==1)return finish(icu_preserving_output<Char>(memory,call(4),i(5),status,make));
                else return finish(icu_output<Char>(memory,call(4),i(5),status,IcuOutput::String,make));
            };
            return utf8?skeleton.template operator()<char>(uspoof_getSkeletonUTF8):
                        skeleton.template operator()<UChar>(uspoof_getSkeleton);
        }
        throw HostCallFault("ICU 混淆字元服務未登錄");
    }catch(const std::bad_alloc&){return out_of_memory();}
}
}
