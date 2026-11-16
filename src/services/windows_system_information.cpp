#include "system_object_identity.hpp"
#include "x64arm/windows_services.hpp"
#include "x64arm/windows_cpu_time.hpp"
#include "x64arm/system_performance.hpp"
#include "x64arm/cpu_features.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/data_protection.hpp"
#include "x64arm/windows_guid.hpp"
#include "virtual_firmware.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <cstring>
#include <vector>

namespace x64arm {
// NtQuerySystemInformation：依資訊類別產生 Windows 的結構（x64 與 x86 各自的配置），數值沿用對應 API 的同一來源。
// 緩衝不足時與 Windows 相同回 STATUS_INFO_LENGTH_MISMATCH 並寫回所需長度（防護層與反作弊常以長度 0 先查大小）。
// 0 基本資訊、1 處理器資訊：與 GetSystemInfo／GetNativeSystemInfo 相同。3 時刻：與 KUSER_SHARED_DATA 的時間同源。
// 5 程序與執行緒清單：沿用 Toolhelp 的程序／執行緒快照。8 處理器效能：每個 vCPU 的宿主 CPU 時間。
// 35／149 核心偵錯器：未啟用、不存在（與 KUSER_SHARED_DATA 的 KdDebuggerEnabled 一致）。103 程式碼完整性：已啟用、非測試簽章。
std::uint32_t WindowsServices::query_system_information(GuestMemory& memory,std::uint32_t information_class,std::uint64_t buffer,
                                                        std::uint32_t length,std::uint64_t return_length){
    constexpr std::uint32_t ok=0,invalid_class=0xc0000003U,length_mismatch=0xc0000004U,access_violation=0xc0000005U,unsuccessful=0xc0000001U;
    try{
    const bool narrow=x86_;const unsigned unit=narrow?4U:8U;
    std::vector<std::uint8_t> data;
    const auto put=[&](std::size_t offset,unsigned bytes,std::uint64_t value){
        if(data.size()<offset+bytes)data.resize(offset+bytes);
        for(unsigned i=0;i<bytes;++i)data[offset+i]=static_cast<std::uint8_t>(value>>(i*8));};
    const auto finish=[&]()->std::uint32_t{
        if(return_length)memory.store(GuestAddress{return_length},32,data.size());
        if(length<data.size())return length_mismatch;
        if(!buffer)return access_violation;
        memory.write(GuestAddress{buffer},data);return ok;};
    const auto processors=std::max(1U,runtime_.processor_count);
    const auto affinity=processors>=64?UINT64_MAX:(UINT64_C(1)<<processors)-1;
    switch(information_class){
    case 16:case 64:{ // 傳統／延伸 handle 清單，兩種版面沿用同一個真實物件快照。
        initialize_tokens();if(runtime_.token_objects.at(effective_token_id()).integrity<0x2000U)return 0xc0000022U;
        std::vector<SystemHandleRecord> handles;
        if(const auto error=capture_system_handles(memory,handles))return error==8?0xc000009aU:error==50?0xc00000bbU:unsuccessful;
        const bool extended=information_class==64;const std::size_t header=extended?2*unit:unit,row=extended?3*unit+16:narrow?16:24;
        data.resize(header+row*handles.size());put(0,extended?unit:4,handles.size());
        for(std::size_t n=0;n<handles.size();++n){
            const auto& item=handles[n];const auto at=header+n*row;
            if(extended){put(at,unit,item.object);put(at+unit,unit,item.pid);put(at+2*unit,unit,item.handle);put(at+3*unit,4,item.access);put(at+3*unit+6,2,item.type);put(at+3*unit+8,4,item.flags);}
            else{put(at,2,item.pid);put(at+4,1,item.type);put(at+5,1,item.flags);put(at+6,2,item.handle);put(at+8,unit,item.object);put(at+8+unit,4,item.access);}
        }
        return finish();
    }
    case 11:{ // RTL_PROCESS_MODULES：只列出同一 sandbox 已載入的核心驅動及映射依賴。
        initialize_tokens();if(runtime_.token_objects.at(effective_token_id()).integrity<0x2000U)return 0xc0000022U;
        std::vector<KernelModuleRecord> modules;
        if(const auto error=capture_kernel_modules(memory,modules))return error==8?0xc000009aU:error==50?0xc00000bbU:unsuccessful;
        const std::size_t row=3*unit+16+256;data.resize(unit+modules.size()*row);
        put(0,4,modules.size());
        for(std::size_t n=0;n<modules.size();++n){
            const auto& module=modules[n];const auto at=unit+n*row;
            if(module.size>UINT32_MAX)return unsuccessful;
            // Section／MappedBase 沒有獨立的使用者映射，維持 NULL；ImageBase 是實際客體映像位址。
            put(at+2*unit,unit,module.base);put(at+3*unit,4,module.size);
            put(at+3*unit+8,2,n);put(at+3*unit+10,2,n);put(at+3*unit+12,2,std::min(module.references,UINT32_C(65535)));
            auto path=module.path;if(path.size()>2 && path[1]==u':')path=u"\\??\\"+path;
            auto encoded=utf16_to_utf8(path);if(encoded.size()>255)encoded.resize(255);
            const auto slash=encoded.find_last_of("\\/");put(at+3*unit+14,2,slash==encoded.npos?0:slash+1);
            std::copy(encoded.begin(),encoded.end(),data.begin()+static_cast<std::ptrdiff_t>(at+3*unit+16));
        }
        return finish();
    }
    case 0:{ // SYSTEM_BASIC_INFORMATION
        std::uint64_t physical=memory.limit();
        if(const auto snapshot=native_system_performance();!snapshot.error && snapshot.pages[3] && snapshot.pages[9])physical=snapshot.pages[3]*snapshot.pages[9];
        put(0,4,0);put(4,4,156250);put(8,4,4096);put(12,4,std::min<std::uint64_t>(physical/4096,UINT32_MAX));put(16,4,1);
        put(20,4,std::min<std::uint64_t>(physical/4096,UINT32_MAX));put(24,4,65536);
        const std::size_t pointers=narrow?28:32;
        put(pointers,unit,65536);put(pointers+unit,unit,user_address_limit()-1);put(pointers+2*unit,unit,narrow?affinity&UINT32_MAX:affinity);
        put(pointers+3*unit,1,processors);data.resize(narrow?44:64);
        return finish();
    }
    case 1:{ // SYSTEM_PROCESSOR_INFORMATION：架構、等級、修訂、最大處理器數、功能位元。
        put(0,2,9);put(2,2,6);put(4,2,0);put(6,2,processors);
        std::uint32_t features=0;for(unsigned index=0;index<32;++index)if(virtual_processor_feature(index,narrow))features|=1U<<index;
        put(8,4,features);return finish();
    }
    case 3:{ // SYSTEM_TIMEOFDAY_INFORMATION：開機時刻＝目前時刻－中斷時間；客體時區為 UTC。
        const auto time=runtime_.host_clock?runtime_.system_time_state_fresh():runtime_.system_time_state();
        put(0,8,time.filetime-time.monotonic);put(8,8,time.filetime);put(16,8,0);put(24,4,0);put(28,4,0);put(32,8,0);put(40,8,0);
        return finish();
    }
    case 5:{ // SYSTEM_PROCESS_INFORMATION 串列，每個程序之後接 SYSTEM_THREAD_INFORMATION，再接映像名稱。
        std::vector<ProcessSnapshot::Process> processes;std::vector<ProcessSnapshot::Thread> threads;
        if(capture_process_snapshot(memory,processes) || capture_thread_snapshot(memory,threads))return unsuccessful;
        // Windows 的清單固定以 System Idle Process（pid 0，無映像名稱，每個處理器一條閒置執行緒）與 System（pid 4）開頭。
        if(std::none_of(processes.begin(),processes.end(),[](const auto& p){return p.id==0;})){
            std::vector<ProcessSnapshot::Process> system{{0,0,processors,0,u""},{4,0,1,8,u"System"}};
            processes.insert(processes.begin(),system.begin(),system.end());
            for(std::uint32_t cpu=0;cpu<processors;++cpu)threads.push_back({cpu,0,0});
            threads.push_back({8,4,16});
        }
        if(std::getenv("X64ARM_TRACE_SYSCALL"))for(const auto& process:processes){
            std::size_t count=0;for(const auto& thread:threads)count+=thread.owner==process.id;
            std::fprintf(stderr,"[system_process] pid=%u parent=%u threads=%zu priority=%u image=%s\n",process.id,process.parent,count,process.priority,utf16_to_utf8(process.executable).c_str());}
        const std::size_t entry_size=narrow?0xb8:0x100,thread_size=narrow?0x40:0x50;
        std::size_t offset=0,previous=SIZE_MAX;
        for(const auto& process:processes){
            offset=(offset+7)&~std::size_t{7};
            if(previous!=SIZE_MAX)put(previous,4,offset-previous);
            previous=offset;
            std::vector<const ProcessSnapshot::Thread*> owned;
            for(const auto& thread:threads)if(thread.owner==process.id)owned.push_back(&thread);
            auto name=process.executable;if(const auto slash=name.find_last_of(u"\\/");slash!=name.npos)name=name.substr(slash+1);
            const auto names=offset+entry_size+owned.size()*thread_size;
            put(offset+entry_size-1,1,0);                                  // 先撐開整個項目
            put(offset+4,4,owned.size());put(offset+0x14,4,owned.size());
            // 建立時間、使用者／核心時間與週期數（0x18／0x20／0x28／0x30）、HandleCount：本程序與其他引擎程序沿用
            // GetProcessTimes、GetProcessHandleCount 的同一來源；查詢不到的程序（已結束或回應逾時）該筆留 0，不讓整份清單失敗。
            if(process.id==process_id() || (process.id>4 && process.executable.size())){
                std::array<std::uint64_t,5> accounting{};std::uint32_t handle_count=0,thread_count=0;
                const bool local=process.id==process_id();
                if(local?local_process_accounting(accounting)!=0:remote_process_accounting(memory,process.id,accounting)!=0)accounting={};
                if(query_process_counters(memory,process.id,handle_count,thread_count)!=0)handle_count=0;
                if(local && !accounting[0])accounting[0]=runtime_.process_creation_time;
                put(offset+0x18,8,accounting[4]);put(offset+0x20,8,accounting[0]);put(offset+0x28,8,accounting[3]);put(offset+0x30,8,accounting[2]);
                put(offset+(narrow?0x4c:0x60),4,handle_count);
                if(local){
                    // VM 計數與 GetProcessMemoryInfo 相同的 backing 記帳（客體沒有 kernel pool 與分頁檔 fault）。
                    const auto usage=memory.usage();const std::size_t vm=narrow?0x58:0x70;const unsigned step=unit;
                    put(offset+vm,unit,usage.peak_virtual_size);put(offset+vm+step,unit,usage.virtual_size);
                    const std::size_t sizes=vm+(narrow?0x0c:0x18);
                    put(offset+sizes,unit,usage.peak_committed);put(offset+sizes+step,unit,usage.committed);
                    const std::size_t pagefile=sizes+6*step;
                    put(offset+pagefile,unit,usage.private_commit);put(offset+pagefile+step,unit,usage.peak_private_commit);put(offset+pagefile+2*step,unit,usage.private_commit);
                }
            }
            const std::size_t image=0x38;
            if(!name.empty()){put(offset+image,2,name.size()*2);put(offset+image+2,2,name.size()*2+2);put(offset+image+(narrow?4:8),unit,buffer+names);}
            const std::size_t after_image=narrow?0x40:0x48;
            put(offset+after_image,4,process.priority);
            put(offset+after_image+(narrow?4:8),unit,process.id);put(offset+after_image+(narrow?8:16),unit,process.parent);
            const std::size_t handles=narrow?0x4c:0x60;
            put(offset+handles+4,4,process.id==0 || process.id==4?0:1); // SessionId
            put(offset+handles+8,unit,process.id);                      // UniqueProcessKey
            for(std::size_t i=0;i<owned.size();++i){
                const auto at=offset+entry_size+i*thread_size;const auto& thread=*owned[i];
                const std::size_t client=narrow?0x20:0x28;
                put(at+client,unit,thread.owner);put(at+client+unit,unit,thread.id);
                put(at+client+2*unit,4,thread.priority);put(at+client+2*unit+4,4,thread.priority);
                put(at+client+2*unit+12,4,thread.id==threads_.current && process.id==process_id()?2U:5U); // ThreadState：執行中／等待
                put(at+thread_size-1,1,0);
            }
            for(std::size_t i=0;i<name.size();++i)put(names+i*2,2,name[i]);
            offset=names+(name.empty()?0:name.size()*2+2);if(data.size()<offset)data.resize(offset);
        }
        if(data.empty())data.resize(entry_size);
        return finish();
    }
    case 90:{ // SYSTEM_BOOT_ENVIRONMENT_INFORMATION；與 DPAPI 的開機生命期及 GetFirmwareType 共用來源。
        if(!runtime_.network.external_io)return 0xc00000bbU;
        try{
            auto identity=native_data_protection_boot();
            while(!identity.empty() && (identity.back()=='\n' || identity.back()=='\r'))identity.pop_back();
            const auto guid=parse_windows_guid(utf8_to_utf16(identity),false);
            if(!guid)return unsuccessful;
            data.resize(32);
            std::copy(guid->begin(),guid->end(),data.begin());
            put(16,4,static_cast<std::uint32_t>(virtual_firmware::type));
            // 虛擬韌體沒有休眠恢復、進階啟動選單或量測開機階段，對應 BootFlags 皆未設定。
            return finish();
        }catch(const DataProtectionError&){return 0xc00000bbU;}
    }
    case 35: // SYSTEM_KERNEL_DEBUGGER_INFORMATION：KernelDebuggerEnabled、KernelDebuggerNotPresent。
        put(0,1,0);put(1,1,1);return finish();
    case 149: // SYSTEM_KERNEL_DEBUGGER_INFORMATION_EX：DebuggerAllowed、DebuggerEnabled、DebuggerPresent。
        put(0,1,0);put(1,1,0);put(2,1,0);return finish();
    case 103: // SYSTEM_CODEINTEGRITY_INFORMATION：Length（呼叫端先填 8）、CodeIntegrityOptions＝CODEINTEGRITY_OPTION_ENABLED。
        if(length>=8 && buffer && static_cast<std::uint32_t>(memory.load(GuestAddress{buffer},32))!=8)return 0xc000000dU;
        put(0,4,8);put(4,4,1);return finish();
    case 8:{ // SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION（每個 vCPU 48 bytes）
        if(runtime_.processor_count>1)return 0xc0000002U; // 尚未分攤各 vCPU 的宿主 CPU 時間，不能回傳單核心大小的假資料。
        if(return_length)memory.store(GuestAddress{return_length},32,48);
        if(length<48)return length_mismatch;
        if(!buffer)return access_violation;
        memory.check(GuestAddress{buffer},48,Access::Write);
        std::uint64_t user=runtime_.clock_ticks,kernel=0,idle=0;
        if(runtime_.host_clock){
            const auto cpu=host_process_cpu_time();if(!cpu)return 0xc0000002U;
            const auto current_user=cpu->user,current_kernel=cpu->kernel;
            if(runtime_.processor_sample_initialized){
                const auto user_delta=current_user-runtime_.processor_sample_user,kernel_delta=current_kernel-runtime_.processor_sample_kernel;
                const auto elapsed=runtime_.clock_ticks-runtime_.processor_sample_clock;
                runtime_.processor_user+=user_delta;runtime_.processor_kernel+=kernel_delta;
                // 這個程序是單一 vCPU 的執行器；等待與未獲宿主 CPU 的時間算 idle。
                runtime_.processor_idle+=elapsed>user_delta+kernel_delta?elapsed-user_delta-kernel_delta:0;
            }
            runtime_.processor_sample_initialized=true;runtime_.processor_sample_clock=runtime_.clock_ticks;
            runtime_.processor_sample_user=current_user;runtime_.processor_sample_kernel=current_kernel;
            user=runtime_.processor_user;idle=runtime_.processor_idle;kernel=runtime_.processor_kernel+idle;
        }
        memory.write(GuestAddress{buffer},std::array<std::uint8_t,48>{});
        memory.store(GuestAddress{buffer},64,idle);memory.store(GuestAddress{buffer+8},64,kernel);memory.store(GuestAddress{buffer+16},64,user);
        return ok;
    }
    default:return invalid_class;
    }
    }catch(const MemoryFault&){return access_violation;}
    catch(const std::bad_alloc&){return 0xc0000017U;}
}
}
