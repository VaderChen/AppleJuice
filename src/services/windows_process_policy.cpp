#include "x64arm/windows_services.hpp"
#include <algorithm>

namespace x64arm {
namespace {
bool query_policy(std::uint32_t policy) {
    // 未啟用的保護可查詢其真實狀態；無後端的設定不會因此宣告支援。
    switch(policy){
        case 0:case 1:case 2:case 3:case 4:case 5:case 6:case 7:case 8:case 9:case 10:
        case 13:case 14:case 15:case 16:case 17:case 18:return true;
        default:return false;
    }
}
}
std::uint32_t WindowsServices::local_process_api_information(GuestMemory& memory,std::uint32_t cls,bool query,std::array<std::uint64_t,5>& values){
    if(cls>=12 || cls==5 || cls==6)return 87;
    if(cls==0){
        if(query){values={5,0,0,0,0};return 0;}
        if(!values[0] || values[0]>5)return 87;
        return 50; // 個別 NT page-aging priority 尚無正確後端，不只保存數值後宣告成功。
    }
    if(cls==2){
        if(!query)return 87;
        std::array<std::uint64_t,6> status{};const auto error=memory_status_fields(memory,status);if(error)return error;
        const auto usage=memory.usage();const auto limit=memory.private_commit_limit();
        const auto private_available=usage.private_commit>=limit?0:limit-usage.private_commit;
        values={std::min(status[3],private_available),usage.private_commit,usage.peak_private_commit,usage.committed,0};return 0;
    }
    if(cls==4){
        if(query){values={1,runtime_.process_power_control,runtime_.process_power_state,0,0};return 0;}
        if(values[0]!=1 || values[1]&~UINT64_C(5) || values[2]&~values[1])return 87;
        if(runtime_.network.external_io){
            HostExecutionScope probe;
            const auto policy=!(values[1]&1U)?ExecutionPolicy::Automatic:values[2]&1U?ExecutionPolicy::Efficient:ExecutionPolicy::Responsive;
            if(!probe.apply(policy))return 50;
        }
        runtime_.process_power_control=static_cast<std::uint32_t>(values[1]);
        runtime_.process_power_state=static_cast<std::uint32_t>(values[2]);
        notify_concurrency_changed();return 0;
    }
    if(cls==7){if(!query)return 87;values={UINT64_C(0xfffffffe),0,0,0,0};return 0;}
    if(cls==9){if(!query)return 87;values={x86_?UINT64_C(0x14c):UINT64_C(0x8664),x86_?5U:3U,0,0,0};return 0;}
    return 50;
}
bool WindowsServices::dynamic_code_blocked() const {
    if(!(runtime_.dynamic_code_policy&1U))return false;
    const auto thread=threads_.items.find(threads_.current);
    return !(runtime_.dynamic_code_policy&2U) || thread==threads_.items.end() || !thread->second.dynamic_code_opt_out;
}
std::optional<std::uint32_t> WindowsServices::invoke_process_policy(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    const WindowsCall call(state,memory,service);const auto name=service.symbol;std::uint64_t result=0;
    const auto fail=[&](std::uint32_t error){last_error_=error;};
    const auto validate_process=[&](std::uint64_t handle){
        if(process_object_type(handle)!=u"Process"){fail(6);return false;}
        if(!process_handle_pid(handle,0x400)){fail(5);return false;}
        if(!current_process_handle(handle)){fail(50);return false;}
        return true;
    };
    try {
        if(name=="GetProcessInformation" || name=="SetProcessInformation"){
            const bool query=name=="GetProcessInformation";
            const auto cls=static_cast<std::uint32_t>(call(1)),size=static_cast<std::uint32_t>(call(3));
            const auto expected=cls==2?32U:cls==4?12U:cls==9?8U:cls==0 || cls==7?4U:0U;
            if(cls>=12 || cls==5 || cls==6)fail(87);
            else if(!expected)fail(50);
            else if(!query && (cls==2 || cls==7 || cls==9))fail(87);
            else if(!call(2) || size!=expected)fail(87);
            else{
                memory.check(GuestAddress{call(2)},size,query?Access::Write:Access::Read);
                std::uint32_t pid{};std::uint64_t birth{};
                auto error=process_memory_target(memory,call(0),query?0x1000U:0x200U,pid,birth);
                if(query && error==5)error=process_memory_target(memory,call(0),0x400U,pid,birth);
                if(error)fail(error);
                else{
                    std::array<std::uint64_t,5> values{};
                    if(!query)for(unsigned i=0;i<size/4;++i)values[i]=memory.load(GuestAddress{call(2)+4*i},32);
                    if(pid==process_id())error=local_process_api_information(memory,cls,query,values);
                    else {std::uint32_t mode{};error=remote_process_request(memory,pid,birth,19,cls|(query?0x10000U:0U),values,mode);}
                    if(error)fail(error);
                    else{
                        if(query){
                            memory.write(GuestAddress{call(2)},std::vector<std::uint8_t>(size));
                            if(cls==9){memory.store(GuestAddress{call(2)},16,values[0]);memory.store(GuestAddress{call(2)+4},32,values[1]);}
                            else {const unsigned unit=cls==2?8U:4U;for(unsigned i=0;i<size/unit;++i)memory.store(GuestAddress{call(2)+i*unit},static_cast<std::uint8_t>(unit*8),values[i]);}
                        }
                        result=1;
                    }
                }
            }
        }else if(name=="GetSystemDEPPolicy")result=1; // 所有客體取指均要求 Execute，且沒有 ATL NX 例外。
        else if(name=="GetProcessDEPPolicy"){
            if(validate_process(call(0))){
                if(!x86_)fail(50);
                else if(!call(1) || !call(2))fail(87);
                else {memory.check(GuestAddress{call(1)},4,Access::Write);memory.check(GuestAddress{call(2)},4,Access::Write);
                    memory.store(GuestAddress{call(1)},32,3);memory.store(GuestAddress{call(2)},32,1);result=1;}
            }
        }else if(name=="SetProcessDEPPolicy"){
            if(!x86_)fail(50);else if(call(0)&~UINT64_C(3) || call(0)==2)fail(87);else fail(5); // 系統 AlwaysOn 不允許覆寫。
        }else {
            const bool query=name=="GetProcessMitigationPolicy";const unsigned offset=query?1:0;
            const auto policy=static_cast<std::uint32_t>(call(offset));const auto buffer=call(offset+1),size=call(offset+2);
            if(query && !validate_process(call(0))){}
            else if(!query_policy(policy))fail(policy>=19?87U:50U);
            else if(!buffer || (policy==5?(size!=8 && size!=16):size!=(policy==0?8U:4U)))fail(87);
            else if(query){
                std::vector<std::uint8_t> output(static_cast<std::size_t>(size));std::uint32_t flags=0;
                if(policy==0){flags=3;output[4]=1;}
                else if(policy==2)flags=runtime_.dynamic_code_policy;
                else if(policy==13)flags=runtime_.child_process_policy;
                else if(policy==8)flags=runtime_.signature_policy;
                else if(policy==7)flags=runtime_.image.guard_cf || std::any_of(runtime_.loaded_modules.begin(),runtime_.loaded_modules.end(),[](const auto& entry){return entry.second.image.guard_cf;})?1U:0U;
                // OptionsMask 僅代表 CreateProcess 的 mitigation attribute；該傳輸目前未實作，不能宣告全一。
                for(unsigned i=0;i<4;++i)output[i]=static_cast<std::uint8_t>(flags>>(i*8));
                memory.write(GuestAddress{buffer},output);result=1;
            }else if(policy==5)fail(87); // 僅可查詢。
            else {
                memory.check(GuestAddress{buffer},static_cast<std::size_t>(size),Access::Read);
                const auto flags=static_cast<std::uint32_t>(memory.load(GuestAddress{buffer},32));
                if(policy==0){if(flags&~3U)fail(87);else fail(5);}
                else if(policy==2){
                    if(flags&~15U || (flags&2U && !(flags&1U)))fail(87);
                    else if(flags&12U)fail(50); // 尚未提供遠端降級及稽核事件後端。
                    else if(runtime_.dynamic_code_policy&~flags)fail(5);
                    else {
                        bool writable_code=false;
                        if((flags&1U) && !(runtime_.dynamic_code_policy&1U))for(auto mapping=memory.next_mapping(GuestAddress{0});mapping;mapping=memory.next_mapping(GuestAddress{mapping->base.value+mapping->size}))
                            if((mapping->permissions&6U)==6U){writable_code=true;break;}
                        // 不默默改既有映射，也不把仍可直接寫入的 RWX 宣告為受保護。
                        if(writable_code)fail(50);else {runtime_.dynamic_code_policy=flags;result=1;}
                    }
                }else if(policy==8){
                    if(flags&~31U)fail(87);
                    else if(flags&24U)fail(50); // 尚無 Windows Code Integrity 稽核事件傳輸。
                    else if(runtime_.signature_policy&~flags)fail(5);
                    else {runtime_.signature_policy=flags;result=1;}
                }else if(policy==13){
                    if(flags&~7U)fail(87);else if(flags&6U)fail(50);
                    else if(runtime_.child_process_policy&~flags)fail(5);
                    else {initialize_tokens();runtime_.token_objects.at(1).child_process_restricted=(flags&1U)!=0;runtime_.child_process_policy=flags;result=1;}
                }else fail(50);
            }
        }
    }catch(const MemoryFault&){fail(998);}catch(const std::bad_alloc&){fail(8);}
    call.return_to(state,result);return std::nullopt;
}
}
