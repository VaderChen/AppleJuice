#include "x64arm/windows_services.hpp"
#include "icu_guest_abi.hpp"
#include "icu_normalization_data.hpp"
#include "icu_normalization_layout.hpp"
#include "icu_set_state.hpp"
#include <unicode/unorm2.h>

namespace x64arm {
namespace {
using namespace icu_abi;
using namespace icu_normalization;

struct Normalizer {
    std::shared_ptr<const icu::Norm2AllModes> package;
    std::shared_ptr<const Normalizer> inner;
    OwnedIcuSet filter;
    std::unique_ptr<UNormalizer2,decltype(&unorm2_close)> filtered{nullptr,unorm2_close};
    const UNormalizer2* instance{};
    unsigned profile{UINT32_MAX};
};

const UNormalizer2* select_mode(const icu::Norm2AllModes& data,std::int32_t mode){
    const icu::Normalizer2* result=nullptr;
    switch(mode){
    case UNORM2_COMPOSE:result=&data.comp;break;
    case UNORM2_DECOMPOSE:result=&data.decomp;break;
    case UNORM2_FCD:result=&data.fcd;break;
    case UNORM2_COMPOSE_CONTIGUOUS:result=&data.fcc;break;
    default:break;
    }
    return reinterpret_cast<const UNormalizer2*>(result);
}

struct InputText {
    std::u16string storage;
    const UChar* text{};
    InputText(GuestMemory& memory,std::uint64_t address,std::int32_t length,UErrorCode& status){
        if(U_FAILURE(status))return;
        if(address?length< -1:length!=0){status=U_ILLEGAL_ARGUMENT_ERROR;return;}
        if(!address)return;
        if(length<0)storage=read_utf16(memory,GuestAddress{address},INT32_MAX);
        else if(length>0){
            const auto size=static_cast<std::size_t>(length)*sizeof(UChar);
            memory.check(GuestAddress{address},size,Access::Read);
            storage.resize(static_cast<std::size_t>(length));
            memory.read(GuestAddress{address},{reinterpret_cast<std::uint8_t*>(storage.data()),size});
        }
        text=storage.data();
    }
};

using Append=decltype(&unorm2_append);
std::uint32_t append_text(GuestMemory& memory,const WindowsCall& call,const UNormalizer2* normalizer,Append operation,IcuStatus& status){
    const auto first=call(1),second=call(4);
    const auto length=signed_argument(call(2)),capacity=signed_argument(call(3)),second_length=signed_argument(call(5));
    if(U_FAILURE(status.value))return 0;
    if((first?length< -1 || capacity<0:length!=0 || capacity!=0) ||
       (first && first==second)){
        status.value=U_ILLEGAL_ARGUMENT_ERROR;return 0;
    }
    const InputText suffix(memory,second,second_length,status.value);
    if(U_FAILURE(status.value))return 0;
    const auto maximum=static_cast<std::size_t>(capacity);
    std::u16string prefix;
    if(length<0){
        for(std::size_t i=0;i<maximum;++i){
            auto c=static_cast<UChar>(memory.load(GuestAddress{first+i*sizeof(UChar)},16));
            if(!c)break;
            prefix.push_back(c);
        }
    }else if(length>0 && length<=capacity){
        prefix=InputText(memory,first,length,status.value).storage;
    }
    std::vector<UChar> work(std::max<std::size_t>(1,std::min(maximum,std::max<std::size_t>(256,prefix.size()))));
    const auto initial=status.value;
    const auto run=[&]{
        std::copy(prefix.begin(),prefix.end(),work.begin());
        return operation(normalizer,first?work.data():nullptr,length,static_cast<std::int32_t>(std::min(maximum,work.size())),
            suffix.text,second_length,&status.value);
    };
    auto result=run();
    if(result>=0 && (status.value==U_BUFFER_OVERFLOW_ERROR || status.value==U_STRING_NOT_TERMINATED_WARNING)){
        const auto wanted=std::min(maximum,static_cast<std::size_t>(result)+1);
        if(wanted>work.size()){
            work.resize(wanted);
            status.value=initial;
            result=run();
        }
    }
    if(first && (U_SUCCESS(status.value) || status.value==U_BUFFER_OVERFLOW_ERROR)){
        const auto count=status.value==U_BUFFER_OVERFLOW_ERROR?prefix.size():(result>0?static_cast<std::size_t>(result):0);
        const auto copied=std::min({maximum,work.size(),count+1});
        if(copied)memory.write(GuestAddress{first},icu_bytes(work.data(),copied));
    }
    return static_cast<std::uint32_t>(result);
}
}

struct IcuNormalizationState {
    std::map<std::uint64_t,std::shared_ptr<Normalizer>> objects;
    std::map<const UNormalizer2*,std::uint64_t> instances;
    std::map<std::pair<std::string,std::string>,std::shared_ptr<const icu::Norm2AllModes>> packages;
};

std::span<const WindowsServiceDescriptor> icu_normalization_service_descriptors(){
    static constexpr WindowsServiceDescriptor entries[]{
#define ICU_NORM_API(name,count) {"icu.dll",#name,count,0,0,0,true},
#define ICU_NORM_QUERY ICU_NORM_API
#include "icu_normalization_services.inc"
#undef ICU_NORM_QUERY
#undef ICU_NORM_API
    };
    return entries;
}

std::optional<std::uint32_t> WindowsServices::invoke_icu_normalization(GuestState& state,GuestMemory& memory,
        const WindowsServiceDescriptor& service){
    const WindowsCall call(state,memory,service);
    const auto name=service.symbol;
    const auto arg=[&](unsigned i){return signed_argument(call(i));};
    const auto finish=[&](std::uint64_t result){call.return_to(state,result);return std::optional<std::uint32_t>{};};
    if(name=="unorm_compare"){
        IcuStatus status(memory,call(5));
        const InputText first(memory,call(0),arg(1),status.value),second(memory,call(2),arg(3),status.value);
        const auto result=unorm_compare(first.text,arg(1),second.text,arg(3),static_cast<std::uint32_t>(call(4)),&status.value);
        status.commit();return finish(static_cast<std::uint32_t>(result));
    }
    if(!icu_normalization_)icu_normalization_=std::make_shared<IcuNormalizationState>();
    auto& normalizers=*icu_normalization_;
    const auto found=normalizers.objects.find(call(0));
    const auto* native=found==normalizers.objects.end()?nullptr:found->second->instance;
    const auto install=[&](std::shared_ptr<Normalizer> object,IcuStatus& status){
        const auto* instance=object->instance;
        std::uint64_t handle{};
        if(instance){
            const auto existing=normalizers.instances.find(instance);
            if(existing!=normalizers.instances.end())handle=existing->second;
            else{
                const auto width=x86_?4U:8U;
                handle=allocate_heap(memory,8+width*query_count);
                if(!handle)status.value=U_MEMORY_ALLOCATION_ERROR;
                else{
                    memory.store(GuestAddress{handle},32,object->profile);
                    memory.store(GuestAddress{handle+4},32,0);
                    const auto gate=[&](GuestQuery index,std::string_view symbol){
                        memory.store(GuestAddress{handle+8+width*index},static_cast<std::uint8_t>(width*8),
                            resolve_service("icu.dll",symbol,true,false).value);
                    };
#define ICU_NORM_API(symbol,count)
#define ICU_NORM_QUERY(symbol,count) gate(query_##symbol,#symbol);
#include "icu_normalization_services.inc"
#undef ICU_NORM_QUERY
#undef ICU_NORM_API
                    const bool borrowed=!object->filtered;
                    normalizers.objects.emplace(handle,std::move(object));
                    if(borrowed)normalizers.instances.emplace(instance,handle);
                }
            }
        }
        status.commit();
        return finish(handle);
    };
    if(name=="unorm2_close"){
        if(native && found->second->filtered){
            normalizers.objects.erase(found);
            (void)free_heap(memory,call(0),state);
        }
        return finish(0);
    }
    if(name.starts_with("unorm2_get") && name.ends_with("Instance")){
        const bool named=name=="unorm2_getInstance";
        IcuStatus status(memory,call(named?3U:0U));
        if(U_FAILURE(status.value))return finish(0);
        const auto package_address=named?call(0):0;
        auto package=package_address?read_ansi(memory,package_address):std::string{};
        std::string data_name;
        auto mode=UNORM2_COMPOSE;
        if(named){
            if(call(1))data_name=read_ansi(memory,call(1));
            mode=static_cast<UNormalization2Mode>(arg(2));
        }else{
            data_name=name=="unorm2_getNFKCCasefoldInstance"?"nfkc_cf":
                (name=="unorm2_getNFKCInstance" || name=="unorm2_getNFKDInstance"?"nfkc":"nfc");
            if(name=="unorm2_getNFDInstance" || name=="unorm2_getNFKDInstance")mode=UNORM2_DECOMPOSE;
        }
        auto object=std::make_shared<Normalizer>();
        if(data_name.empty())status.value=U_ILLEGAL_ARGUMENT_ERROR;
        else if(!package_address || package=="ICUDATA" || package==U_ICUDATA_NAME){
            const auto id=std::ranges::find(profile_names,data_name);
            if(id!=std::end(profile_names) || data_name=="uts46"){
                object->instance=unorm2_getInstance(nullptr,data_name.c_str(),mode,&status.value);
                if(object->instance && id!=std::end(profile_names))
                    object->profile=static_cast<unsigned>(id-std::begin(profile_names))*4+mode;
            }else status.value=U_FILE_ACCESS_ERROR;
        }else{
            // 套件由客體的檔案服務讀取，資料物件與名稱快取皆隸屬此程序。
            auto key=std::pair{std::move(package),std::move(data_name)};
            for(auto& c:key.first)if(c=='\\')c='/';
            for(auto& c:key.second)if(c=='\\')c='/';
            if(const auto existing=normalizers.packages.find(key);existing!=normalizers.packages.end())
                object->package=existing->second;
            else if(!key.first.empty()){
                const auto& path=key.first;
                constexpr std::size_t limit=128U*1024*1024;
                const auto read=[&](std::string_view file){
                    const auto saved=last_error_;
                    auto bytes=read_guest_file(state,memory,utf8_to_utf16(file),0,limit);
                    last_error_=saved;
                    return bytes;
                };
                auto payload=read(path+"/"+key.second+".nrm");
                if(!payload){
                    const auto base=std::string_view(path).substr(path.rfind('/')+1);
                    if(const auto archive=read(path+".dat")){
                        const auto item=package_entry(*archive,std::string(base)+"/"+key.second+".nrm",status.value);
                        if(!item.empty())payload.emplace(item.begin(),item.end());
                    }
                }
                if(payload)object->package=load(std::move(*payload),status.value);
                else if(U_SUCCESS(status.value))status.value=U_FILE_ACCESS_ERROR;
                if(object->package)normalizers.packages.emplace(std::move(key),object->package);
            }else status.value=U_FILE_ACCESS_ERROR;
            if(object->package)object->instance=select_mode(*object->package,mode);
        }
        return install(std::move(object),status);
    }
    if(name=="unorm2_openFiltered"){
        IcuStatus status(memory,call(2));
        if(U_FAILURE(status.value))return finish(0);
        const auto* filter=icu_sets_?icu_sets_->get(call(1)):nullptr;
        auto object=std::make_shared<Normalizer>();
        if(native && filter){
            object->inner=found->second;
            object->filter.reset(uset_clone(filter));
            if(object->filter){
                uset_freeze(object->filter.get());
                object->filtered.reset(unorm2_openFiltered(native,object->filter.get(),&status.value));
                object->instance=object->filtered.get();
            }else status.value=U_MEMORY_ALLOCATION_ERROR;
        }else status.value=U_ILLEGAL_ARGUMENT_ERROR;
        return install(std::move(object),status);
    }
    if(name=="unorm2_composePair")return finish(native?static_cast<std::uint32_t>(unorm2_composePair(native,arg(1),arg(2))):UINT32_MAX);
    if(name=="unorm2_getCombiningClass")return finish(native?unorm2_getCombiningClass(native,arg(1)):0);
    if(name=="unorm2_hasBoundaryAfter")return finish(native && unorm2_hasBoundaryAfter(native,arg(1)));
    if(name=="unorm2_hasBoundaryBefore")return finish(native && unorm2_hasBoundaryBefore(native,arg(1)));
    if(name=="unorm2_isInert")return finish(native && unorm2_isInert(native,arg(1)));

    IcuStatus status(memory,call(service.arguments-1));
    if(U_FAILURE(status.value))return finish(0);
    if(!native){status.value=U_ILLEGAL_ARGUMENT_ERROR;status.commit();return finish(0);}
    if(name=="unorm2_append" || name=="unorm2_normalizeSecondAndAppend"){
        const auto result=append_text(memory,call,native,
            name=="unorm2_append"?unorm2_append:unorm2_normalizeSecondAndAppend,status);
        status.commit();return finish(result);
    }
    if(name=="unorm2_getDecomposition" || name=="unorm2_getRawDecomposition"){
        const auto operation=name=="unorm2_getDecomposition"?unorm2_getDecomposition:unorm2_getRawDecomposition;
        return finish(icu_output<UChar>(memory,call(2),arg(3),status,IcuOutput::String,
            [&](auto* out,auto capacity,auto* error){return operation(native,arg(1),out,capacity,error);}));
    }
    if(name=="unorm2_normalize" && call(1) && call(1)==call(3)){
        status.value=U_ILLEGAL_ARGUMENT_ERROR;status.commit();return finish(0);
    }
    const InputText source(memory,call(1),arg(2),status.value);
    std::uint32_t result{};
    if(U_SUCCESS(status.value)){
        if(name=="unorm2_normalize")
            return finish(icu_output<UChar>(memory,call(3),arg(4),status,IcuOutput::String,
                [&](auto* out,auto capacity,auto* error){return unorm2_normalize(native,source.text,arg(2),out,capacity,error);}));
        if(name=="unorm2_isNormalized")result=unorm2_isNormalized(native,source.text,arg(2),&status.value)!=0;
        else if(name=="unorm2_quickCheck")result=unorm2_quickCheck(native,source.text,arg(2),&status.value);
        else result=static_cast<std::uint32_t>(unorm2_spanQuickCheckYes(native,source.text,arg(2),&status.value));
    }
    status.commit();return finish(result);
}
}
