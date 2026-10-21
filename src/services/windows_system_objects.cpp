#include "x64arm/windows_services.hpp"
#include "x64arm/security_descriptor.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>

namespace x64arm {
namespace {
std::uint64_t vtable_method(const GuestMemory& memory,std::uint64_t object,unsigned slot,unsigned unit){
    const auto bits=static_cast<std::uint8_t>(unit*8);const auto table=memory.load(GuestAddress{object},bits);
    if(!table)throw MemoryFault(GuestAddress{object},Access::Read,unit,"IUnknown vtable 為空");
    return memory.load(GuestAddress{table+std::uint64_t{slot}*unit},bits);
}
}
// 物件比較、執行緒列舉、暫時取得 privilege、WER 執行階段例外模組與 GPU 排程優先權等系統物件 API。
std::span<const WindowsServiceDescriptor> system_object_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"kernel32.dll","CompareObjectHandles",2},
        {"ntdll.dll","NtCompareObjects",2},{"ntdll.dll","ZwCompareObjects",2},
        {"ntdll.dll","NtGetNextThread",6},{"ntdll.dll","ZwGetNextThread",6},
        {"ntdll.dll","RtlAcquirePrivilege",4},{"ntdll.dll","RtlReleasePrivilege",1},
        {"kernel32.dll","WerRegisterRuntimeExceptionModule",2},{"kernel32.dll","WerUnregisterRuntimeExceptionModule",2},
        {"user32.dll","SetLastErrorEx",2},
        {"gdi32.dll","D3DKMTSetProcessSchedulingPriorityClass",2},{"gdi32.dll","D3DKMTGetProcessSchedulingPriorityClass",2},
        {"msdmo.dll","MoInitMediaType",2},{"msdmo.dll","MoFreeMediaType",1},{"msdmo.dll","MoCopyMediaType",2},
        {"msdmo.dll","MoCreateMediaType",2},{"msdmo.dll","MoDeleteMediaType",1},{"msdmo.dll","MoDuplicateMediaType",2}
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_system_objects(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;
    const auto u32=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    try{
        if(symbol=="CompareObjectHandles" || symbol.ends_with("CompareObjects")){
            const bool native=symbol!="CompareObjectHandles";
            const auto resolve=[&](std::uint64_t handle)->std::optional<std::pair<std::u16string,std::uint64_t>>{
                const auto current=x86_?UINT64_C(0xffffffff):UINT64_MAX;
                if(handle==current || current_process_handle(handle))return std::pair{std::u16string{u"Process"},std::uint64_t{process_id()}};
                const auto type=object_type(handle);if(type.empty())return std::nullopt;
                const auto metadata=object_metadata(handle);return std::pair{std::u16string(type),metadata?metadata->identity:handle};
            };
            const auto left=resolve(arg(0)),right=resolve(arg(1));
            if(!left || !right){if(native)return finish(0xC0000008U);last_error_=6;return finish(0);}
            const bool same=*left==*right;
            if(native)return finish(same?0U:0xC00001EAU); // STATUS_NOT_SAME_OBJECT
            return finish(same?1:0);
        }
        if(symbol.ends_with("GetNextThread")){
            // 只列舉本程序的客體執行緒；跨程序需要遠端執行緒快照，尚無共用來源。
            if(u32(4))return finish(0xC00000F3U);
            if(!arg(5))return finish(0xC000000DU);memory.check(GuestAddress{arg(5)},unit,Access::Write);memory.store(GuestAddress{arg(5)},bits,0);
            const auto current=x86_?UINT64_C(0xffffffff):UINT64_MAX;
            if(arg(0)!=current && !current_process_handle(arg(0))){
                if(!process_handle_pid(arg(0),0))return finish(0xC0000008U);
                if(!process_handle_pid(arg(0),0x400))return finish(0xC0000022U);
                return finish(0xC00000BBU);
            }
            std::uint64_t after=0;
            if(arg(1)){after=thread_id(arg(1));if(!after)return finish(0xC0000008U);}
            if(u32(3)&~0x1f2U)return finish(0xC00000F0U); // OBJ_* 以外的屬性。
            for(auto it=after?threads_.items.upper_bound(after):threads_.items.begin();it!=threads_.items.end();++it){
                if(it->second.status==ThreadStatus::Exited || it->second.terminating)continue;
                if(runtime_.process_thread_handles.size()>=1024)return finish(0xC000009AU);
                try{const auto handle=open_local_thread(it->first,u32(2),(u32(3)&2U)!=0);memory.store(GuestAddress{arg(5)},bits,handle);return finish(0);}
                catch(const SecurityDescriptorError& error){return finish(error.code==5?0xC0000022U:error.code==8?0xC000009AU:0xC000000DU);}
            }
            return finish(0x8000001AU); // STATUS_NO_MORE_ENTRIES
        }
        if(symbol=="RtlAcquirePrivilege"){
            const auto flags=u32(2),count=u32(1);
            if((flags&~3U) || !count || count>64 || !arg(0) || !arg(3))return finish(0xC000000DU);
            memory.check(GuestAddress{arg(3)},unit,Access::Write);memory.store(GuestAddress{arg(3)},bits,0);
            memory.check(GuestAddress{arg(0)},std::size_t{count}*4,Access::Read);
            initialize_tokens();
            PrivilegeAcquisition record;record.process=(flags&2U)!=0;
            auto& thread=threads_.items.at(threads_.current);
            std::uint64_t token_id=1;
            if(!record.process){
                record.previous_thread_token=thread.impersonation_token;
                if(thread.impersonation_token && !(flags&1U)){token_id=thread.impersonation_token;record.in_place=true;}
                else {
                    // 依 RtlAcquirePrivilege 契約先以程序權杖複本模擬自身，再於該 token 上啟用 privilege。
                    auto copy=runtime_.token_objects.at(1);copy.type=2;copy.level=3;
                    token_id=runtime_.next_token_object++;runtime_.token_objects.emplace(token_id,std::move(copy));
                }
            }else record.in_place=true;
            auto& token=runtime_.token_objects.at(token_id);
            auto changed=token.privileges;
            for(std::uint32_t i=0;i<count;++i){
                const std::uint64_t luid=memory.load(GuestAddress{arg(0)+i*4},32);
                const auto item=std::find_if(changed.begin(),changed.end(),[&](const auto& value){return value.luid==luid;});
                if(item==changed.end()){
                    if(!record.in_place){runtime_.token_objects.erase(token_id);collect_tokens();}
                    return finish(0xC0000061U); // STATUS_PRIVILEGE_NOT_HELD
                }
                record.previous.emplace_back(item->luid,item->attributes);item->attributes|=2U;
            }
            const auto cookie=allocate_heap(memory,16);
            if(!cookie){if(!record.in_place){runtime_.token_objects.erase(token_id);collect_tokens();}return finish(0xC000009AU);}
            if(changed!=token.privileges){token.privileges=std::move(changed);++token.modified_id;}
            record.token=token_id;
            if(record.previous_thread_token){
                // 保留原模擬 token 的參照，釋放時才能還原，期間不被回收。
                record.keeper=runtime_.allocate_wait_handle();runtime_.token_handles[record.keeper]=0;runtime_.token_handle_objects[record.keeper]=record.previous_thread_token;
            }
            if(!record.process && !record.in_place){thread.impersonation_token=token_id;collect_tokens();refresh_file_security_context();}
            record.thread=threads_.current;
            privilege_acquisitions_.emplace(cookie,std::move(record));
            memory.store(GuestAddress{arg(3)},bits,cookie);return finish(0);
        }
        if(symbol=="RtlReleasePrivilege"){
            const auto found=privilege_acquisitions_.find(arg(0));if(found==privilege_acquisitions_.end())return finish(0);
            auto record=std::move(found->second);privilege_acquisitions_.erase(found);(void)free_heap(memory,arg(0),state);
            if(record.in_place){
                if(const auto token=runtime_.token_objects.find(record.token);token!=runtime_.token_objects.end()){
                    auto changed=token->second.privileges;
                    for(const auto& [luid,attributes]:record.previous)for(auto& item:changed)if(item.luid==luid)item.attributes=attributes;
                    if(changed!=token->second.privileges){token->second.privileges=std::move(changed);++token->second.modified_id;}
                }
            }else if(const auto thread=threads_.items.find(record.thread);thread!=threads_.items.end() && thread->second.impersonation_token==record.token){
                thread->second.impersonation_token=record.previous_thread_token;if(record.thread==threads_.current)refresh_file_security_context();
            }
            if(record.keeper){runtime_.token_handles.erase(record.keeper);runtime_.token_handle_objects.erase(record.keeper);}
            collect_tokens();return finish(0);
        }
        if(symbol=="WerRegisterRuntimeExceptionModule" || symbol=="WerUnregisterRuntimeExceptionModule"){
            if(!arg(0))return finish(0x80070057U);
            const auto path=read_utf16(memory,GuestAddress{arg(0)},260);if(path.empty() || path.size()>=260)return finish(0x80070057U);
            auto key=path;for(auto& c:key)c=unicode_case_unit(c,false);
            const auto found=std::find_if(wer_runtime_modules_.begin(),wer_runtime_modules_.end(),[&](const auto& item){return item.first==key && item.second==arg(1);});
            if(symbol.starts_with("WerUnregister")){if(found==wer_runtime_modules_.end())return finish(0x80070490U);wer_runtime_modules_.erase(found);return finish(0);}
            if(found!=wer_runtime_modules_.end())return finish(0x800700B7U); // 已註冊相同模組與 context。
            if(wer_runtime_modules_.size()>=16)return finish(0x80070103U); // WER 每程序最多 16 個 runtime exception module。
            wer_runtime_modules_.emplace_back(std::move(key),arg(1));return finish(0);
        }
        if(symbol=="SetLastErrorEx"){last_error_=u32(0);return finish(0);}
        if(symbol.starts_with("D3DKMT")){
            const bool set=symbol.starts_with("D3DKMTSet");
            const auto pid=process_handle_pid(arg(0),0);if(!pid)return finish(0xC0000008U);
            if(!process_handle_pid(arg(0),set?0x200U:0x1000U))return finish(0xC0000022U);
            if(set){
                if(u32(1)>5)return finish(0xC000000DU);
                // REALTIME 需要 SeIncreaseBasePriorityPrivilege（LUID 14）且已啟用。
                if(u32(1)==5){initialize_tokens();if(!runtime_.token_objects.at(effective_token_id()).has_privilege(14,true))return finish(0xC0000061U);}
                gpu_scheduling_classes_[pid]=u32(1);return finish(0);
            }
            if(!arg(1))return finish(0xC000000DU);memory.check(GuestAddress{arg(1)},4,Access::Write);
            const auto found=gpu_scheduling_classes_.find(pid);memory.store(GuestAddress{arg(1)},32,found==gpu_scheduling_classes_.end()?2U:found->second);return finish(0);
        }
        // msdmo：DMO_MEDIA_TYPE 與 AM_MEDIA_TYPE 相同，格式區塊以 CoTaskMemAlloc 配置。
        const std::uint64_t unknown_at=x86_?60:64,size_at=unknown_at+unit,format_at=size_at+unit,type_size=format_at+unit;
        const auto task_allocate=[&](std::uint64_t size){const auto pointer=allocate_heap(memory,size?size:1);if(pointer)gui_.task_allocations.insert(pointer);return pointer;};
        const auto task_free=[&](std::uint64_t pointer){if(pointer && free_heap(memory,pointer,state))gui_.task_allocations.erase(pointer);};
        const auto init=[&](std::uint64_t type,std::uint64_t size)->std::uint32_t{
            if(size>0x7fffffffU)return 0x8007000EU;
            std::uint64_t format=0;if(size){format=task_allocate(size);if(!format)return 0x8007000EU;}
            memory.store(GuestAddress{type+unknown_at},bits,0);memory.store(GuestAddress{type+size_at},32,size);memory.store(GuestAddress{type+format_at},bits,format);
            return 0;
        };
        if(symbol=="MoInitMediaType"){
            if(!arg(0))return finish(0x80004003U);memory.check(GuestAddress{arg(0)},type_size,Access::Write);return finish(init(arg(0),u32(1)));
        }
        if(symbol=="MoCreateMediaType"){
            if(!arg(0))return finish(0x80004003U);memory.check(GuestAddress{arg(0)},unit,Access::Write);memory.store(GuestAddress{arg(0)},bits,0);
            const auto type=task_allocate(type_size);if(!type)return finish(0x8007000EU);
            memory.write(GuestAddress{type},std::vector<std::uint8_t>(static_cast<std::size_t>(type_size)));
            if(const auto error=init(type,u32(1))){task_free(type);return finish(error);}
            memory.store(GuestAddress{arg(0)},bits,type);return finish(0);
        }
        if(symbol=="MoFreeMediaType" || symbol=="MoDeleteMediaType"){
            if(!arg(0))return finish(0x80004003U);memory.check(GuestAddress{arg(0)},type_size,Access::Write);
            const auto object=memory.load(GuestAddress{arg(0)+unknown_at},bits);
            task_free(memory.load(GuestAddress{arg(0)+format_at},bits));
            memory.store(GuestAddress{arg(0)+format_at},bits,0);memory.store(GuestAddress{arg(0)+size_at},32,0);memory.store(GuestAddress{arg(0)+unknown_at},bits,0);
            if(symbol=="MoDeleteMediaType")task_free(arg(0));
            if(object){start_callbacks(state,memory,{{vtable_method(memory,object,2,unit),{object}}},Continuation::Return,0);return std::nullopt;}
            return finish(0);
        }
        if(symbol=="MoCopyMediaType" || symbol=="MoDuplicateMediaType"){
            const bool duplicate=symbol=="MoDuplicateMediaType";
            if(!arg(0) || !arg(1))return finish(0x80004003U);
            memory.check(GuestAddress{arg(1)},type_size,Access::Read);
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(type_size));memory.read(GuestAddress{arg(1)},bytes);
            const auto size=memory.load(GuestAddress{arg(1)+size_at},32),source=memory.load(GuestAddress{arg(1)+format_at},bits);
            const auto object=memory.load(GuestAddress{arg(1)+unknown_at},bits);
            std::vector<std::uint8_t> format(static_cast<std::size_t>(size));if(size){if(!source)return finish(0x80004003U);memory.read(GuestAddress{source},format);}
            std::uint64_t destination=arg(0);
            if(duplicate){memory.check(GuestAddress{arg(0)},unit,Access::Write);memory.store(GuestAddress{arg(0)},bits,0);destination=task_allocate(type_size);if(!destination)return finish(0x8007000EU);}
            else memory.check(GuestAddress{destination},type_size,Access::Write);
            std::uint64_t copy=0;if(size){copy=task_allocate(size);if(!copy){if(duplicate)task_free(destination);return finish(0x8007000EU);}memory.write(GuestAddress{copy},format);}
            memory.write(GuestAddress{destination},bytes);memory.store(GuestAddress{destination+format_at},bits,copy);
            if(duplicate)memory.store(GuestAddress{arg(0)},bits,destination);
            if(object){start_callbacks(state,memory,{{vtable_method(memory,object,1,unit),{object}}},Continuation::Return,0);return std::nullopt;}
            return finish(0);
        }
    }catch(const MemoryFault&){
        if(symbol.starts_with("Mo") || symbol.starts_with("Wer"))return finish(0x80004003U);
        if(symbol=="CompareObjectHandles"){last_error_=998;return finish(0);}
        return finish(0xC0000005U);
    }catch(const std::bad_alloc&){return finish(symbol.starts_with("Mo")?0x8007000EU:0xC000009AU);}
    throw HostCallFault("未知的系統物件入口");
}
}
