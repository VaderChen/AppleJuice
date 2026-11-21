#include "x64arm/windows_services.hpp"
#include "icu_guest_abi.hpp"
#include "icu_idna_layout.hpp"
#include <unicode/uidna.h>

namespace x64arm {
namespace {
using namespace icu_abi;
using IdnaHandle=std::unique_ptr<UIDNA,decltype(&uidna_close)>;

static_assert(sizeof(UIDNAInfo)==16 && offsetof(UIDNAInfo,errors)==4);
}

struct IcuIdnaState {
    std::map<std::uint64_t,IdnaHandle> handles;
};

std::span<const WindowsServiceDescriptor> icu_idna_service_descriptors(){
    static constexpr WindowsServiceDescriptor entries[]{
#define ICU_IDNA_API(name,count) {"icu.dll",#name,count,0,0,0,true},
#define ICU_IDNA_QUERY(name,count,type,label) {"icu.dll",#name,count,0,0,0,true},
#include "icu_idna_services.inc"
#undef ICU_IDNA_QUERY
#undef ICU_IDNA_API
    };
    return entries;
}

std::optional<std::uint32_t> WindowsServices::invoke_icu_idna(GuestState& state,GuestMemory& memory,
        const WindowsServiceDescriptor& service){
    const WindowsCall call(state,memory,service);
    const auto name=service.symbol;
    const auto finish=[&](std::uint64_t value=0){
        call.return_to(state,value);
        return std::optional<std::uint32_t>{};
    };
    if(name=="uidna_close"){
        if(icu_idna_ && icu_idna_->handles.erase(call(0)))(void)free_heap(memory,call(0),state);
        return finish();
    }
    IcuStatus status(memory,call(service.arguments-1));
    const auto result=[&](std::uint64_t value=0){status.commit();return finish(value);};
    const auto invalid=[&](){status.value=U_ILLEGAL_ARGUMENT_ERROR;return result();};
    if(U_FAILURE(status.value))return finish();
    try{
        if(name=="uidna_openUTS46"){
            IdnaHandle native(uidna_openUTS46(static_cast<std::uint32_t>(call(0)),&status.value),uidna_close);
            if(!native)return result();
            if(!icu_idna_)icu_idna_=std::make_shared<IcuIdnaState>();
            icu_idna::View view;
#define ICU_IDNA_API(...)
#define ICU_IDNA_QUERY(name,count,type,label) \
            view.fallback[static_cast<std::size_t>(icu_idna::Conversion::name)]=host_service_stub("icu.dll",#name).value;
#include "icu_idna_services.inc"
#undef ICU_IDNA_QUERY
#undef ICU_IDNA_API
            const auto handle=allocate_heap(memory,sizeof(view));
            if(!handle){status.value=U_MEMORY_ALLOCATION_ERROR;return result();}
            try{
                write_icu_record(memory,handle,view);
                icu_idna_->handles.emplace(handle,std::move(native));
            }
            catch(...){(void)free_heap(memory,handle,state);throw;}
            return result(handle);
        }
        if(!icu_idna_)return invalid();
        const auto found=icu_idna_->handles.find(call(0));
        if(found==icu_idna_->handles.end())return invalid();
        const auto* idna=found->second.get();
        const auto source=call(1),dest=call(3),details=call(5);
        const auto input_length=signed_argument(call(2)),capacity=signed_argument(call(4));
        if(!details || (source?input_length< -1:input_length!=0) ||
           (dest?capacity<0:capacity!=0) || (source && source==dest))return invalid();
        const auto info_size=std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(memory.load(GuestAddress{details},16)));
        if(info_size<static_cast<std::int16_t>(sizeof(UIDNAInfo)))return invalid();
        const auto extent=static_cast<std::size_t>(info_size);
        memory.check(GuestAddress{details},extent,Access::Write);
        UIDNAInfo info=UIDNA_INFO_INITIALIZER;
        const UIDNAInfo clean=UIDNA_INFO_INITIALIZER;
        const auto convert=[&]<class Char>(auto operation){
            const IcuTextInput<Char> input(memory,source,input_length,status.value);
            if(U_FAILURE(status.value))return std::uint32_t{};
            return icu_preserving_output<Char>(memory,dest,capacity,status,
                [&](Char* output,std::int32_t count,UErrorCode* error){
                    info=clean;
                    return operation(idna,input.get(),input.length,output,count,&info,error);
                });
        };
        const bool label=name.starts_with("uidna_label");
        const bool ascii=name.find("ToASCII")!=name.npos;
        // UTF-8 的 Unicode 入口在歷史 ABI 中沒有 UTF8 前的底線。
        const auto output_length=name.ends_with("UTF8")?convert.template operator()<char>(label?
            (ascii?uidna_labelToASCII_UTF8:uidna_labelToUnicodeUTF8):
            (ascii?uidna_nameToASCII_UTF8:uidna_nameToUnicodeUTF8)):
            convert.template operator()<UChar>(label?(ascii?uidna_labelToASCII:uidna_labelToUnicode):
                                                     (ascii?uidna_nameToASCII:uidna_nameToUnicode));
        info.size=info_size;
        if(extent>sizeof(info))memory.write(GuestAddress{details+sizeof(info)},
            std::vector<std::uint8_t>(extent-sizeof(info)));
        write_icu_record(memory,details,info);
        return result(output_length);
    }catch(const std::bad_alloc&){
        status.value=U_MEMORY_ALLOCATION_ERROR;
        return result();
    }
}
}
