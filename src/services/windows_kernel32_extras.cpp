#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "service_callback_task.hpp"
#include <algorithm>

// kernel32 補充家族：WOW64 檔案系統重新導向狀態、8.3 名稱檢查、封裝（AppModel）查詢與同步屏障。
// 全部在宿主端執行；8.3 檢查沿用 ntdll 的 RtlIsNameLegalDOS8Dot3，同步屏障的等待經客體回呼續行交給既有等待流程。
namespace x64arm {
namespace {
constexpr std::uint32_t error_invalid_parameter=87,error_insufficient_buffer=122,error_not_found=1168,appmodel_no_package=15700;
// 封裝全名：Name_Version_Architecture_ResourceId_PublisherId；版本為 a.b.c.d，架構為 x86／x64／arm／arm64／neutral。
struct PackageName {std::u16string name,resource,publisher_id;std::array<std::uint16_t,4> version{};std::uint32_t architecture{};};
std::optional<PackageName> parse_package_full_name(std::u16string_view text){
    std::vector<std::u16string_view> parts;std::size_t at=0;
    for(;;){const auto next=text.find(u'_',at);parts.push_back(text.substr(at,next==text.npos?text.npos:next-at));if(next==text.npos)break;at=next+1;}
    if(parts.size()!=5 || parts[0].empty() || parts[0].size()>50 || parts[4].size()!=13)return std::nullopt;
    PackageName result;result.name=std::u16string(parts[0]);result.resource=std::u16string(parts[3]);result.publisher_id=std::u16string(parts[4]);
    unsigned field=0;std::uint32_t value=0;bool digit=false;
    for(const auto c:parts[1]){
        if(c==u'.'){if(!digit || field==3)return std::nullopt;result.version[field++]=static_cast<std::uint16_t>(value);value=0;digit=false;continue;}
        if(c<u'0' || c>u'9')return std::nullopt;value=value*10+static_cast<std::uint32_t>(c-u'0');if(value>65535)return std::nullopt;digit=true;
    }
    if(field!=3 || !digit)return std::nullopt;result.version[3]=static_cast<std::uint16_t>(value);
    constexpr std::pair<std::u16string_view,std::uint32_t> architectures[]{{u"x86",0},{u"arm",5},{u"x64",9},{u"neutral",11},{u"arm64",12}};
    const auto found=std::find_if(std::begin(architectures),std::end(architectures),[&](const auto& item){return item.first==parts[2];});
    if(found==std::end(architectures))return std::nullopt;result.architecture=found->second;
    for(const auto c:result.publisher_id)if(!((c>=u'0' && c<=u'9') || (c>=u'a' && c<=u'z')))return std::nullopt;
    return result;
}
}
std::span<const WindowsServiceDescriptor> kernel32_extra_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"kernel32.dll","Wow64DisableWow64FsRedirection",1},{"kernel32.dll","Wow64RevertWow64FsRedirection",1},{"kernel32.dll","Wow64EnableWow64FsRedirection",1},
        {"ntdll.dll","RtlWow64EnableFsRedirection",1},{"ntdll.dll","RtlWow64EnableFsRedirectionEx",2},
        {"kernel32.dll","CheckNameLegalDOS8Dot3A",5,0,0,0x4},{"kernel32.dll","CheckNameLegalDOS8Dot3W",5,0,0,0x4},
        {"kernel32.dll","GetPackageFamilyName",3},{"kernel32.dll","GetPackageFullName",3},{"kernel32.dll","GetPackagePathByFullName",3},
        {"kernel32.dll","GetPackagesByPackageFamily",5},{"kernel32.dll","LoadPackagedLibrary",2,0,0,0x2},
        {"kernel32.dll","PackageIdFromFullName",4,0,0,0x2},
        {"kernel32.dll","InitializeSynchronizationBarrier",3,0,0,0x6},{"kernel32.dll","EnterSynchronizationBarrier",2,0,0,0x2},
        {"kernel32.dll","DeleteSynchronizationBarrier",1},
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_kernel32_extra(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const auto name=service.symbol;
    if(name=="EnterSynchronizationBarrier"){
        // 最後一個抵達的執行緒回 TRUE 並放行本輪所有等待者；其他執行緒等待本輪事件後回 FALSE。
        // 每輪使用各自的手動重設事件，下一輪抵達的執行緒不會被上一輪的訊號放行。
        return nested_continuation(state,memory,"SyncBarrier",
        [this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall call(s,m,service);const auto saved_error=last_error_;
                const auto found=synchronization_barriers_.find(call(0));
                if(found==synchronization_barriers_.end()){last_error_=error_invalid_parameter;call.return_to(s,0);co_return std::optional<std::uint32_t>{};}
                auto& barrier=found->second;
                if(!--barrier.remaining){
                    // 本輪完成時，上一輪的等待者都已進入本輪（屏障要全部執行緒抵達才完成），上一輪事件可以關閉。
                    for(const auto retired:barrier.retired)(void)nested_service(s,m,"kernel32.dll","CloseHandle",{retired});
                    barrier.retired.clear();
                    const auto event=barrier.event;barrier.remaining=barrier.total;++barrier.generation;
                    barrier.event=nested_service(s,m,"kernel32.dll","CreateEventW",{0,1,0,0});if(x86_)barrier.event&=UINT32_MAX;
                    (void)nested_service(s,m,"kernel32.dll","SetEvent",{event});barrier.retired.push_back(event);
                    last_error_=saved_error;call.return_to(s,1);co_return std::optional<std::uint32_t>{};
                }
                const auto event=barrier.event;const auto stub=host_service_stub("kernel32.dll","WaitForSingleObject");
                (void)co_await service_callback::call(stub.value?stub.value:resolve("kernel32.dll","WaitForSingleObject").value,{event,UINT32_MAX});
                last_error_=saved_error;call.return_to(s,0);co_return std::optional<std::uint32_t>{};
            });
    }
    const WindowsCall arg(state,memory,service);const auto saved_error=last_error_;
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto done=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto fail=[&](std::uint32_t error){last_error_=error;return done(0);};
    const auto u32=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    try{
        if(name.starts_with("Wow64") || name.starts_with("RtlWow64")){
            // WOW64 檔案系統重新導向只存在於 32 位元程序；64 位元程序呼叫時 Windows 回 ERROR_INVALID_FUNCTION（NT：STATUS_NOT_IMPLEMENTED）。
            // 本引擎的 32 位元程序與 64 位元程序看到同一個 System32（內建模組不分檔案系統視圖），狀態只影響後續查詢與還原。
            const bool native=name.starts_with("Rtl");
            if(!x86_){if(native)return done(0xc0000002U);return fail(1);}
            auto& disabled=wow64_fs_redirection_[threads_.current];
            if(name=="Wow64DisableWow64FsRedirection"){if(!arg(0))return fail(error_invalid_parameter);memory.store(GuestAddress{arg(0)},bits,disabled?1U:0U);disabled=true;return done(1);}
            if(name=="Wow64RevertWow64FsRedirection"){disabled=arg(0)!=0;return done(1);}
            if(name=="Wow64EnableWow64FsRedirection"){disabled=(arg(0)&0xffU)==0;return done(1);}
            if(name=="RtlWow64EnableFsRedirection"){disabled=(arg(0)&0xffU)==0;return done(0);}
            if(!arg(1))return done(0xc0000005U);
            memory.store(GuestAddress{arg(1)},bits,disabled?1U:0U);disabled=arg(0)!=0;return done(0);
        }
        if(name.starts_with("CheckNameLegalDOS8Dot3")){
            // (Name, OemName, OemNameSize, NameContainsSpaces*, NameLegal*)：與 RtlIsNameLegalDOS8Dot3 同一套 8.3 規則。
            const bool wide=name.ends_with('W');if(!arg(0) || !arg(4))return fail(error_invalid_parameter);
            if(arg(1) && !u32(2))return fail(error_invalid_parameter);
            std::u16string text;
            if(wide)text=read_utf16(memory,GuestAddress{arg(0)},260);else text=utf8_to_utf16_lossy(read_ansi(memory,arg(0),260));
            GuestScratch temporary(*this,memory,state);const auto string=temporary.allocate(16+text.size()*2+2+16+256);if(!string)return fail(8);
            const auto buffer=string+16,oem=buffer+text.size()*2+2,oem_buffer=oem+16,spaces=oem_buffer+250;
            memory.write(GuestAddress{buffer},utf16_bytes(text));
            memory.store(GuestAddress{string},16,text.size()*2);memory.store(GuestAddress{string+2},16,text.size()*2+2);memory.store(GuestAddress{string+unit},bits,buffer);
            memory.store(GuestAddress{oem},16,0);memory.store(GuestAddress{oem+2},16,240);memory.store(GuestAddress{oem+unit},bits,oem_buffer);memory.store(GuestAddress{spaces},8,0);
            const auto legal=nested_service(state,memory,"ntdll.dll","RtlIsNameLegalDOS8Dot3",{string,oem,spaces});
            if(arg(1)){
                const auto length=static_cast<std::uint32_t>(memory.load(GuestAddress{oem},16));
                if(length+1>u32(2))return fail(error_insufficient_buffer);
                std::vector<std::uint8_t> bytes(length);if(length)memory.read(GuestAddress{oem_buffer},bytes);bytes.push_back(0);memory.write(GuestAddress{arg(1)},bytes);
            }
            if(arg(3))memory.store(GuestAddress{arg(3)},32,memory.load(GuestAddress{spaces},8)?1U:0U);
            memory.store(GuestAddress{arg(4)},32,legal&0xffU?1U:0U);last_error_=saved_error;return done(1);
        }
        if(name=="GetPackageFamilyName" || name=="GetPackageFullName"){
            // (Process, Length*, Name)：引擎內沒有已封裝的程序；有效程序 handle 回 APPMODEL_ERROR_NO_PACKAGE，與 GetCurrentPackage* 一致。
            if(!arg(1))return done(error_invalid_parameter);
            if(arg(0)!=(x86_?UINT32_MAX:UINT64_MAX) && !process_handle_pid(arg(0),0x1000U))return done(6);
            return done(appmodel_no_package);
        }
        if(name=="GetPackagePathByFullName"){
            // (PackageFullName, PathLength*, Path)：格式錯誤為 ERROR_INVALID_PARAMETER；引擎未安裝任何封裝，合法名稱回 ERROR_NOT_FOUND。
            if(!arg(0) || !arg(1))return done(error_invalid_parameter);
            if(!parse_package_full_name(read_utf16(memory,GuestAddress{arg(0)},128)))return done(error_invalid_parameter);
            return done(error_not_found);
        }
        if(name=="GetPackagesByPackageFamily"){
            // (FamilyName, Count*, FullNames, BufferLength*, Buffer)：沒有已安裝封裝，回 0 筆。
            if(!arg(0) || !arg(1) || !arg(3))return done(error_invalid_parameter);
            memory.store(GuestAddress{arg(1)},32,0);memory.store(GuestAddress{arg(3)},32,0);return done(0);
        }
        if(name=="LoadPackagedLibrary"){
            // 只供已封裝的程序使用；Reserved 必須為 0。
            if(u32(1))return fail(error_invalid_parameter);return fail(appmodel_no_package);
        }
        if(name=="PackageIdFromFullName"){
            // (FullName, Flags, BufferLength*, Buffer)：PACKAGE_ID {reserved, processorArchitecture, version, name, publisher, resourceId, publisherId}，
            // 字串放在結構之後。全名只含 PublisherId，Publisher 欄位為 NULL。
            if(!arg(0) || !arg(2))return done(error_invalid_parameter);
            const auto parsed=parse_package_full_name(read_utf16(memory,GuestAddress{arg(0)},128));if(!parsed)return done(error_invalid_parameter);
            const std::size_t header=x86_?32U:48U;
            const auto needed=static_cast<std::uint32_t>(header+(parsed->name.size()+1+parsed->resource.size()+1+parsed->publisher_id.size()+1)*2);
            const auto capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)},32));memory.store(GuestAddress{arg(2)},32,needed);
            if(capacity<needed || !arg(3))return done(error_insufficient_buffer);
            std::vector<std::uint8_t> bytes(needed);
            const auto put=[&](std::size_t at,unsigned width,std::uint64_t value){for(unsigned i=0;i<width;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(8*i));};
            put(4,4,parsed->architecture);
            put(8,8,(std::uint64_t{parsed->version[0]}<<48)|(std::uint64_t{parsed->version[1]}<<32)|(std::uint64_t{parsed->version[2]}<<16)|parsed->version[3]);
            auto at=header;const auto text=[&](const std::u16string& value,std::size_t field){
                put(16+field*unit,unit,arg(3)+at);const auto data=utf16_bytes(value);std::copy(data.begin(),data.end(),bytes.begin()+static_cast<std::ptrdiff_t>(at));at+=data.size();};
            text(parsed->name,0);text(parsed->resource,2);text(parsed->publisher_id,3);
            memory.write(GuestAddress{arg(3)},bytes);return done(0);
        }
        if(name=="InitializeSynchronizationBarrier"){
            // (Barrier*, TotalThreads, SpinCount)：SpinCount 為 -1 時使用預設值；屏障狀態以客體結構位址為鍵。
            if(!arg(0) || !u32(1))return fail(error_invalid_parameter);
            memory.write(GuestAddress{arg(0)},std::vector<std::uint8_t>(x86_?20U:32U));
            auto event=nested_service(state,memory,"kernel32.dll","CreateEventW",{0,1,0,0});if(x86_)event&=UINT32_MAX;if(!event)return fail(8);
            if(const auto old=synchronization_barriers_.find(arg(0));old!=synchronization_barriers_.end()){
                (void)nested_service(state,memory,"kernel32.dll","CloseHandle",{old->second.event});
                for(const auto retired:old->second.retired)(void)nested_service(state,memory,"kernel32.dll","CloseHandle",{retired});
                synchronization_barriers_.erase(old);
            }
            synchronization_barriers_[arg(0)]={u32(1),u32(1),0,event,{}};
            memory.store(GuestAddress{arg(0)},32,u32(1));last_error_=saved_error;return done(1);
        }
        if(name=="DeleteSynchronizationBarrier"){
            // 一律回 TRUE；釋放本屏障的事件。
            if(const auto found=synchronization_barriers_.find(arg(0));found!=synchronization_barriers_.end()){
                (void)nested_service(state,memory,"kernel32.dll","CloseHandle",{found->second.event});
                for(const auto retired:found->second.retired)(void)nested_service(state,memory,"kernel32.dll","CloseHandle",{retired});
                synchronization_barriers_.erase(found);
            }
            last_error_=saved_error;return done(1);
        }
    }catch(const MemoryFault&){return fail(998);}
    throw HostCallFault("未知的 kernel32 補充服務");
}
}
