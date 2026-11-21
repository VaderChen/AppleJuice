#include "x64arm/windows_services.hpp"
#include "icu_charset_detection_layout.hpp"
#include "icu_guest_abi.hpp"
#include "service_callback_task.hpp"
#include <unicode/uenum.h>

namespace x64arm {
namespace {
using namespace icu_abi;
using icu_charset_detection::Detector;
using icu_charset_detection::Match;

struct OwnedDetector {
    std::unique_ptr<UCharsetDetector,decltype(&ucsdet_close)> native{nullptr,ucsdet_close};
    std::string input;
    std::map<const UCharsetMatch*,std::uint64_t> matches;
    std::uint64_t result_array{};
    std::size_t result_capacity{};
};
}

struct IcuCharsetDetectionState {
    std::map<std::uint64_t,OwnedDetector> detectors;
    std::map<std::uint64_t,const UCharsetMatch*> matches;
    std::map<std::string,std::uint64_t,std::less<>> strings;
};

std::span<const WindowsServiceDescriptor> icu_charset_detection_service_descriptors(){
    static constexpr WindowsServiceDescriptor entries[]{
#define ICU_CHARDET_API(name,count) {"icu.dll",#name,count,0,0,0,true},
#define ICU_CHARDET_QUERY ICU_CHARDET_API
#include "icu_charset_detection_services.inc"
#undef ICU_CHARDET_QUERY
#undef ICU_CHARDET_API
    };
    return entries;
}

std::optional<std::uint32_t> WindowsServices::invoke_icu_charset_detection(GuestState& state,GuestMemory& memory,
        const WindowsServiceDescriptor& service){
    const auto name=service.symbol;
    if(name=="ucsdet_getAllDetectableCharsets")
        return nested_continuation(state,memory,"ICU 字元編碼列舉",
            [this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall call(s,m,service);
                IcuStatus status(m,call(1));
                std::unique_ptr<UEnumeration,decltype(&uenum_close)> names(
                    ucsdet_getAllDetectableCharsets(nullptr,&status.value),uenum_close);
                const auto result=names?co_await export_icu_enumeration(s,m,*names,status):0;
                status.commit();
                call.return_to(s,result);
                co_return std::nullopt;
            });

    const WindowsCall call(state,memory,service);
    const auto finish=[&](std::uint64_t result=0){
        call.return_to(state,result);
        return std::optional<std::uint32_t>{};
    };
    if(!icu_charset_detection_)icu_charset_detection_=std::make_shared<IcuCharsetDetectionState>();
    auto& objects=*icu_charset_detection_;
    auto detector=objects.detectors.find(call(0));
    const auto discard_matches=[&](OwnedDetector& owner){
        for(const auto& [native,handle]:owner.matches){
            (void)native;
            objects.matches.erase(handle);
            (void)free_heap(memory,handle,state);
        }
        owner.matches.clear();
    };
    if(name=="ucsdet_close"){
        if(detector!=objects.detectors.end()){
            auto& owner=detector->second;
            discard_matches(owner);
            if(owner.result_array)(void)free_heap(memory,owner.result_array,state);
            objects.detectors.erase(detector);
            (void)free_heap(memory,call(0),state);
        }
        return finish();
    }
    if(name=="ucsdet_isInputFilterEnabled" || name=="ucsdet_enableInputFilter"){
        if(detector==objects.detectors.end())return finish();
        Detector view;
        read_icu_record(memory,call(0),view);
        if(view.magic!=Detector::signature)return finish();
        if(name=="ucsdet_isInputFilterEnabled")return finish(static_cast<std::uint8_t>(view.filter));
        const auto previous=view.enable_filter(static_cast<UBool>(call(1)));
        write_icu_record(memory,call(0),view);
        return finish(static_cast<std::uint8_t>(previous));
    }

    IcuStatus status(memory,call(service.arguments-1));
    const auto result=[&](std::uint64_t value=0){status.commit();return finish(value);};
    const auto invalid=[&](){status.value=U_ILLEGAL_ARGUMENT_ERROR;return result();};
    if(U_FAILURE(status.value))return finish();

    try{
        if(name=="ucsdet_open"){
            OwnedDetector owner;
            owner.native.reset(ucsdet_open(&status.value));
            if(!owner.native)return result();
            const auto handle=allocate_heap(memory,sizeof(Detector));
            if(!handle){status.value=U_MEMORY_ALLOCATION_ERROR;return result();}
            try{
                write_icu_record(memory,handle,Detector{});
                objects.detectors.emplace(handle,std::move(owner));
            }catch(...){
                (void)free_heap(memory,handle,state);
                throw;
            }
            return result(handle);
        }

        if(name=="ucsdet_getName" || name=="ucsdet_getLanguage" || name=="ucsdet_getConfidence" || name=="ucsdet_getUChars"){
            const auto match=objects.matches.find(call(0));
            if(match==objects.matches.end())return invalid();
            Match view;
            read_icu_record(memory,call(0),view);
            if(view.magic!=Match::signature)return invalid();
            if(name=="ucsdet_getName")return finish(view.name);
            if(name=="ucsdet_getLanguage")return finish(view.language);
            if(name=="ucsdet_getConfidence")return finish(static_cast<std::uint32_t>(view.confidence));
            return finish(icu_preserving_output<UChar>(memory,call(1),signed_argument(call(2)),status,
                [&](auto* buffer,auto capacity,auto* error){return ucsdet_getUChars(match->second,buffer,capacity,error);}));
        }

        if(detector==objects.detectors.end())return invalid();
        auto& owner=detector->second;
        if(name=="ucsdet_setText" || name=="ucsdet_setDeclaredEncoding"){
            const IcuTextInput<char> input(memory,call(1),signed_argument(call(2)),status.value);
            if(U_FAILURE(status.value))return result();
            if(name=="ucsdet_setDeclaredEncoding"){
                ucsdet_setDeclaredEncoding(owner.native.get(),input.get(),input.length,&status.value);
            }else{
                std::string bytes;
                if(input.get()){
                    if(input.length<0)bytes=input.get();
                    else bytes.assign(input.get(),static_cast<std::size_t>(input.length));
                }
                owner.input.swap(bytes);
                ucsdet_setText(owner.native.get(),input.get()?owner.input.data():nullptr,input.length,&status.value);
                discard_matches(owner);
            }
            return result();
        }

        Detector view;
        read_icu_record(memory,call(0),view);
        if(view.magic!=Detector::signature)return invalid();
        if(view.filter_dirty){
            (void)ucsdet_enableInputFilter(owner.native.get(),view.filter);
            view.filter_dirty=false;
            write_icu_record(memory,call(0),view);
            discard_matches(owner);
        }

        const auto intern=[&](const char* source){
            if(!source)return UINT64_C(0);
            if(const auto found=objects.strings.find(source);found!=objects.strings.end())return found->second;
            const std::string_view text(source);
            const auto pointer=runtime_allocate(memory,icu_bytes(source,text.size()+1));
            objects.strings.emplace(std::string(text),pointer);
            return pointer;
        };
        const auto adopt_match=[&](const UCharsetMatch* native){
            if(!native || U_FAILURE(status.value))return UINT64_C(0);
            if(const auto found=owner.matches.find(native);found!=owner.matches.end())return found->second;
            Match snapshot;
            snapshot.name=intern(ucsdet_getName(native,&status.value));
            snapshot.language=intern(ucsdet_getLanguage(native,&status.value));
            snapshot.confidence=ucsdet_getConfidence(native,&status.value);
            if(U_FAILURE(status.value))return UINT64_C(0);
            const auto handle=allocate_heap(memory,sizeof(Match));
            if(!handle){status.value=U_MEMORY_ALLOCATION_ERROR;return UINT64_C(0);}
            try{
                write_icu_record(memory,handle,snapshot);
                owner.matches.emplace(native,handle);
                objects.matches.emplace(handle,native);
            }catch(...){
                owner.matches.erase(native);
                (void)free_heap(memory,handle,state);
                throw;
            }
            return handle;
        };
        if(name=="ucsdet_detect")return result(adopt_match(ucsdet_detect(owner.native.get(),&status.value)));
        if(name=="ucsdet_detectAll"){
            if(!call(1))return invalid();
            memory.check(GuestAddress{call(1)},4,Access::Write);
            std::int32_t count=-1;
            const auto* matches=ucsdet_detectAll(owner.native.get(),&count,&status.value);
            if(!matches || U_FAILURE(status.value)){
                if(count>=0)memory.store(GuestAddress{call(1)},32,static_cast<std::uint32_t>(count));
                return result();
            }
            const auto size=static_cast<std::size_t>(count);
            std::vector<std::uint64_t> addresses;
            addresses.reserve(size);
            for(std::size_t n=0;n<size;++n){
                const auto handle=adopt_match(matches[n]);
                if(!handle)return result();
                addresses.push_back(handle);
            }
            const auto bits=static_cast<std::uint8_t>(x86_?32:64);
            const auto width=bits/8U;
            if(size>owner.result_capacity){
                const auto array=allocate_heap(memory,size*width);
                if(!array){status.value=U_MEMORY_ALLOCATION_ERROR;return result();}
                if(owner.result_array)(void)free_heap(memory,owner.result_array,state);
                owner.result_array=array;
                owner.result_capacity=size;
            }
            for(std::size_t n=0;n<size;++n)memory.store(GuestAddress{owner.result_array+n*width},bits,addresses[n]);
            memory.store(GuestAddress{call(1)},32,static_cast<std::uint32_t>(count));
            return result(owner.result_array);
        }
    }catch(const std::bad_alloc&){
        status.value=U_MEMORY_ALLOCATION_ERROR;
        return result();
    }
    throw HostCallFault("ICU 字元編碼偵測服務未登錄");
}
}
