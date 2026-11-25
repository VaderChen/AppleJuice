#include "x64arm/windows_services.hpp"
#include "icu_converter_selector_binary.hpp"
#include "icu_guest_abi.hpp"
#include "icu_set_state.hpp"
#include "service_callback_task.hpp"
#include <unicode/ucnvsel.h>

namespace x64arm {
namespace {
using namespace icu_abi;

struct Selector {
    // 直接開啟的 ICU 物件會借用序列化內容，必須先關閉物件再釋放快照。
    std::vector<std::uint8_t> bytes;
    std::unique_ptr<UConverterSelector,decltype(&ucnvsel_close)> native{nullptr,ucnvsel_close};
};

void reopen_selector(Selector& selector,const GuestMemory& memory,std::uint64_t address,std::int32_t capacity,
                     UErrorCode& status){
    using namespace icu_selector;
    if(!address || (address&3) || capacity<=0){status=U_ILLEGAL_ARGUMENT_ERROR;return;}
    if(capacity<static_cast<std::int32_t>(header_probe)){status=U_INDEX_OUTOFBOUNDS_ERROR;return;}
    const auto length=static_cast<std::size_t>(capacity);
    std::array<std::uint8_t,header_probe> header{};
    memory.read(GuestAddress{address},header);
    const auto size=header_size(header,length,status);
    if(!size)return;
    std::array<std::uint8_t,index_size> indexes{};
    if(address>UINT64_MAX-size){status=U_ILLEGAL_ARGUMENT_ERROR;return;}
    memory.read(GuestAddress{address+size},indexes);
    const auto item=layout(indexes,size,length,header[8]!=0,status);
    if(!item.size)return;
    memory.check(GuestAddress{address},item.size,Access::Read);
    selector.bytes.resize(item.size);
    memory.read(GuestAddress{address},selector.bytes);
    // 共享客體記憶體可能在讀取期間更新。以完整快照重新核對切割位置。
    const auto bytes=Bytes(selector.bytes);
    const auto copy_header=header_size(bytes,bytes.size(),status);
    if(!copy_header)return;
    const auto copy_layout=layout(bytes.subspan(copy_header,index_size),copy_header,bytes.size(),bytes[8]!=0,status);
    if(U_FAILURE(status))return;
    if(!valid_payload(bytes,copy_layout)){status=U_INVALID_FORMAT_ERROR;return;}
    selector.native.reset(ucnvsel_openFromSerialized(bytes.data(),static_cast<std::int32_t>(bytes.size()),&status));
}

template<class Char,class Select>
UEnumeration* select_text(const GuestMemory& memory,const WindowsCall& call,const UConverterSelector* selector,
                          UErrorCode& status,Select select){
    // ICU 把任何負長度解釋為 NUL 結尾的文字。
    const auto length=std::max(signed_argument(call(2)),-1);
    const IcuTextInput<Char> input(memory,call(1),length,status);
    return U_SUCCESS(status)?select(selector,input.get(),input.length,&status):nullptr;
}
}

struct IcuConverterSelectorState {
    std::map<std::uint64_t,std::shared_ptr<Selector>> handles;

    std::shared_ptr<Selector> get(std::uint64_t handle)const{
        const auto found=handles.find(handle);
        return found==handles.end()?nullptr:found->second;
    }
};

std::span<const WindowsServiceDescriptor> icu_converter_selector_service_descriptors(){
    static constexpr WindowsServiceDescriptor entries[]{
#define ICU_SELECTOR_API(name,count) {"icu.dll",#name,count,0,0,0,true},
#include "icu_converter_selector_services.inc"
#undef ICU_SELECTOR_API
    };
    return entries;
}

std::optional<std::uint32_t> WindowsServices::invoke_icu_converter_selector(GuestState& state,GuestMemory& memory,
        const WindowsServiceDescriptor& service){
    const auto name=service.symbol;
    if(name=="ucnvsel_selectForString" || name=="ucnvsel_selectForUTF8")
        return nested_continuation(state,memory,"ICU 字元編碼列舉",
            [this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall call(s,m,service);
                IcuStatus status(m,call(3));
                const auto initial=status.value;
                std::uint64_t result{};
                try{
                    if(U_SUCCESS(status.value)){
                        const auto selector=icu_converter_selectors_?icu_converter_selectors_->get(call(0)):nullptr;
                        if(!selector)status.value=U_ILLEGAL_ARGUMENT_ERROR;
                        else{
                            std::unique_ptr<UEnumeration,decltype(&uenum_close)> choices(
                                service.symbol=="ucnvsel_selectForString"?
                                    select_text<UChar>(m,call,selector->native.get(),status.value,ucnvsel_selectForString):
                                    select_text<char>(m,call,selector->native.get(),status.value,ucnvsel_selectForUTF8),
                                uenum_close);
                            if(choices)result=co_await export_icu_enumeration(s,m,*choices,status);
                        }
                    }
                }catch(const std::bad_alloc&){status.value=U_MEMORY_ALLOCATION_ERROR;}
                if(status.value!=initial)status.commit();
                call.return_to(s,result);
                co_return std::nullopt;
            });

    const WindowsCall call(state,memory,service);
    const auto finish=[&](std::uint64_t result=0){
        call.return_to(state,result);
        return std::optional<std::uint32_t>{};
    };
    if(name=="ucnvsel_close"){
        const auto handle=call(0);
        if(icu_converter_selectors_ && icu_converter_selectors_->handles.contains(handle) && free_heap(memory,handle,state))
            icu_converter_selectors_->handles.erase(handle);
        return finish();
    }

    IcuStatus status(memory,call(service.arguments-1));
    const auto initial=status.value;
    const auto done=[&](std::uint64_t result=0){
        if(status.value!=initial)status.commit();
        return finish(result);
    };
    if(U_FAILURE(initial))return finish();
    const auto illegal=[&](){status.value=U_ILLEGAL_ARGUMENT_ERROR;return done();};
    try{
        if(name=="ucnvsel_serialize"){
            const auto selector=icu_converter_selectors_?icu_converter_selectors_->get(call(0)):nullptr;
            const auto capacity=signed_argument(call(2));
            if(!selector || ((call(1)&3) && capacity>0))return illegal();
            return finish(icu_output<std::uint8_t>(memory,call(1),capacity,status,IcuOutput::Array,
                [&](auto* bytes,auto size,auto* error){return ucnvsel_serialize(selector->native.get(),bytes,size,error);}));
        }

        auto selector=std::make_shared<Selector>();
        if(name=="ucnvsel_openFromSerialized"){
            reopen_selector(*selector,memory,call(0),signed_argument(call(1)),status.value);
        }else if(name=="ucnvsel_open"){
            const auto count=signed_argument(call(1));
            const auto which=signed_argument(call(3));
            if(count<0 || count>icu_selector::max_encodings || (count && !call(0)) ||
               which<UCNV_ROUNDTRIP_SET || which>UCNV_ROUNDTRIP_AND_FALLBACK_SET)return illegal();
            const auto* excluded=call(2) && icu_sets_?icu_sets_->get(call(2)):nullptr;
            if(call(2) && !excluded)return illegal();

            std::vector<std::string> names;
            names.reserve(static_cast<std::size_t>(count));
            const auto width=x86_?4U:8U;
            const auto pointer_bits=static_cast<std::uint8_t>(width*8);
            if(count)memory.check(GuestAddress{call(0)},width*static_cast<std::size_t>(count),Access::Read);
            std::size_t remaining=INT32_MAX-3U;
            for(std::int32_t index=0;index<count;++index){
                const auto address=memory.load(GuestAddress{call(0)+width*static_cast<std::uint64_t>(index)},pointer_bits);
                if(!address)return illegal();
                if(!remaining){status.value=U_MEMORY_ALLOCATION_ERROR;return done();}
                names.push_back(read_ansi(memory,address,remaining-1));
                remaining-=names.back().size()+1;
            }
            std::vector<const char*> pointers;
            pointers.reserve(names.size());
            for(const auto& value:names)pointers.push_back(value.c_str());

            // 選擇器只排除 code point，集合內的多字元項目不參與判定。
            OwnedIcuSet code_points;
            if(excluded && uset_hasStrings(excluded)){
                code_points.reset(uset_cloneAsThawed(excluded));
                if(!code_points){status.value=U_MEMORY_ALLOCATION_ERROR;return done();}
                uset_removeAllStrings(code_points.get());
                excluded=code_points.get();
            }
            selector->native.reset(ucnvsel_open(pointers.data(),count,excluded,
                static_cast<UConverterUnicodeSet>(which),&status.value));
        }else throw HostCallFault("ICU 字元編碼選擇器服務未登錄");

        if(!selector->native || U_FAILURE(status.value))return done();
        if(!icu_converter_selectors_)icu_converter_selectors_=std::make_shared<IcuConverterSelectorState>();
        if(status.value!=initial)memory.check(status.address,4,Access::Write);
        const auto handle=allocate_heap(memory,8);
        if(!handle){status.value=U_MEMORY_ALLOCATION_ERROR;return done();}
        try{
            icu_converter_selectors_->handles.emplace(handle,std::move(selector));
        }catch(...){
            (void)free_heap(memory,handle,state);
            throw;
        }
        return done(handle);
    }catch(const std::bad_alloc&){
        status.value=U_MEMORY_ALLOCATION_ERROR;
        return done();
    }
}
}
