#include "x64arm/windows_services.hpp"
#include "icu_guest_abi.hpp"
#include "icu_numbering_system_layout.hpp"
#include "service_callback_task.hpp"
#include <unicode/uenum.h>
#include <unicode/unumsys.h>
#include <set>

namespace x64arm {
struct IcuNumberingSystemState {
    std::set<std::uint64_t> handles;
};

std::span<const WindowsServiceDescriptor> icu_numbering_system_service_descriptors(){
    static constexpr WindowsServiceDescriptor entries[]{
#define ICU_NUMBERING_API(name,count) {"icu.dll",#name,count,0,0,0,true},
#define ICU_NUMBERING_QUERY(name,count) {"icu.dll",#name,count,0,0,0,true},
#include "icu_numbering_system_services.inc"
#undef ICU_NUMBERING_QUERY
#undef ICU_NUMBERING_API
    };
    return entries;
}

std::optional<std::uint32_t> WindowsServices::invoke_icu_numbering_system(GuestState& state,GuestMemory& memory,
        const WindowsServiceDescriptor& service){
    using namespace icu_abi;
    using namespace icu_numbering_system;
    const auto name=service.symbol;
    if(name=="unumsys_openAvailableNames")
        return nested_continuation(state,memory,"ICU 數字系統列舉",
            [this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall call(s,m,service);
                IcuStatus status(m,call(0));
                const auto before=status.value;
                std::unique_ptr<UEnumeration,decltype(&uenum_close)> names(
                    U_SUCCESS(before)?unumsys_openAvailableNames(&status.value):nullptr,uenum_close);
                std::uint64_t handle{};
                if(names)handle=co_await export_icu_enumeration(s,m,*names,status);
                else if(status.value!=before)status.commit();
                call.return_to(s,handle);
                co_return std::nullopt;
            });

    const WindowsCall call(state,memory,service);
    const auto finish=[&](std::uint64_t result=0){
        call.return_to(state,result);
        return std::optional<std::uint32_t>{};
    };

    if(name=="unumsys_close"){
        const auto handle=call(0);
        if(icu_numbering_systems_ && icu_numbering_systems_->handles.contains(handle) &&
           free_heap(memory,handle,state))icu_numbering_systems_->handles.erase(handle);
        return finish();
    }
    if(name=="unumsys_open" || name=="unumsys_openByName"){
        IcuStatus status(memory,call(1));
        const auto before=status.value;
        const auto result=[&](std::uint64_t address=0){
            if(status.value!=before)status.commit();
            return finish(address);
        };
        if(U_FAILURE(before))return finish();
        try{
            std::unique_ptr<UNumberingSystem,decltype(&unumsys_close)> system(nullptr,unumsys_close);
            const auto input=call(0);
            const auto locale=input?read_ansi(memory,input):std::string{};
            if(name=="unumsys_openByName"){
                if(!input)status.value=U_UNSUPPORTED_ERROR;
                else system.reset(unumsys_openByName(locale.c_str(),&status.value));
            }else system.reset(unumsys_open(input?locale.c_str():icu_default_locale(),&status.value));
            if(!system || U_FAILURE(status.value))return result();

            UErrorCode probe=U_ZERO_ERROR;
            const auto length=unumsys_getDescription(system.get(),nullptr,0,&probe);
            if(length<0 || length==INT32_MAX || (U_FAILURE(probe) && probe!=U_BUFFER_OVERFLOW_ERROR)){
                status.value=length==INT32_MAX?U_MEMORY_ALLOCATION_ERROR:probe;
                return result();
            }
            std::vector<char16_t> description(static_cast<std::size_t>(length)+1);
            probe=U_ZERO_ERROR;
            const auto copied=unumsys_getDescription(system.get(),description.data(),length+1,&probe);
            if(U_FAILURE(probe) || copied!=length){
                status.value=U_FAILURE(probe)?probe:U_INTERNAL_PROGRAM_ERROR;
                return result();
            }

            const std::string_view system_name(unumsys_getName(system.get()));
            const auto offset=(sizeof(View)+system_name.size()+2)&~std::size_t{1};
            if(!icu_numbering_systems_)icu_numbering_systems_=std::make_shared<IcuNumberingSystemState>();
            if(status.value!=before)memory.check(status.address,4,Access::Write);
            const auto handle=allocate_heap(memory,offset+description.size()*sizeof(char16_t));
            if(!handle){status.value=U_MEMORY_ALLOCATION_ERROR;return result();}
            try{
                const View record{View::signature,unumsys_getRadix(system.get()),
                    unumsys_isAlgorithmic(system.get())?1U:0U,length,static_cast<std::uint32_t>(offset)};
                write_icu_record(memory,handle,record);
                memory.write(GuestAddress{handle+sizeof(View)},icu_bytes(system_name.data(),system_name.size()+1));
                memory.write(GuestAddress{handle+offset},icu_bytes(description.data(),description.size()));
                icu_numbering_systems_->handles.insert(handle);
            }catch(...){
                (void)free_heap(memory,handle,state);
                throw;
            }
            return result(handle);
        }catch(const std::bad_alloc&){
            status.value=U_MEMORY_ALLOCATION_ERROR;
            return result();
        }
    }

    const auto handle=call(0);
    View record{};
    const auto owned=icu_numbering_systems_ && icu_numbering_systems_->handles.contains(handle);
    if(owned)read_icu_record(memory,handle,record);
    const auto valid=owned && record.magic==View::signature;
    if(name=="unumsys_getName")return finish(valid?handle+sizeof(View):0);
    if(name=="unumsys_getRadix")return finish(valid?static_cast<std::uint32_t>(record.radix):0);
    if(name=="unumsys_isAlgorithmic")return finish(valid && record.algorithmic!=0);

    IcuStatus status(memory,call(3));
    const auto before=status.value;
    if(U_FAILURE(before))return finish(UINT32_MAX);
    if(!valid){
        status.value=U_ILLEGAL_ARGUMENT_ERROR;
        status.commit();
        return finish(UINT32_MAX);
    }

    const auto output=call(1);
    const auto plan=plan_description(record.description_length,signed_argument(call(2)),output!=0,status.value);
    if(plan.units || plan.terminate){
        const auto bytes=std::size_t(plan.units)*sizeof(char16_t);
        memory.check(GuestAddress{output},bytes+(plan.terminate?sizeof(char16_t):0),Access::Write);
        if(plan.units){
            std::vector<std::uint8_t> text(bytes);
            memory.read(GuestAddress{handle+record.description_offset},text);
            memory.write(GuestAddress{output},text);
        }
        if(plan.terminate)memory.store(GuestAddress{output+bytes},16,0);
    }
    if(status.value!=before)status.commit();
    return finish(static_cast<std::uint32_t>(record.description_length));
}
}
