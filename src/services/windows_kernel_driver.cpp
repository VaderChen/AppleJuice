#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "guest_service_store.hpp"
#include "virtual_firmware.hpp"
#include "x64arm/processor_information.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <map>
#include <thread>

namespace x64arm {
// 核心模式驅動宿主：在呼叫端程序內載入 x64 驅動映像，提供 ntoskrnl／HAL 子集、IRP 分派與虛擬硬體存取。
// 驅動程式碼與一般客體程式碼共用直譯／JIT；特權指令由 CPU 例外交回此處依虛擬平台模擬。
// 參考：https://learn.microsoft.com/windows-hardware/drivers/kernel/ （IRP、DRIVER_OBJECT、IO_STACK_LOCATION 版面）
struct WindowsServices::KernelDriverState {
    struct Driver {std::u16string service;std::uint64_t image{},image_size{},object{},unload{};bool running{};};
    struct Device {std::uint64_t driver{};std::u16string name;};
    struct OpenFile {std::uint64_t device{},file_object{};};
    struct Request {
        enum class Kind {Entry,Create,Control,Close,Unload} kind{};
        std::u16string service;std::uint64_t driver{},handle{},device{},file_object{},block{},system_buffer{};
        std::uint64_t input{},output{},returned{},overlapped{},iosb{},event{};std::uint32_t input_size{},output_size{},code{};bool nt{};
        std::vector<std::uint64_t> irps;std::size_t index{};
    };
    std::map<std::u16string,Driver> drivers; // 以摺疊後的服務名稱為鍵。
    std::map<std::uint64_t,Device> devices;  // DEVICE_OBJECT 位址。
    std::map<std::u16string,std::uint64_t> links; // 摺疊後的 DOS 裝置名稱 → DEVICE_OBJECT。
    std::map<std::uint64_t,OpenFile> files;  // 客體 handle。
    std::map<std::uint64_t,std::uint64_t> io_mappings; // MmMapIoSpace 的虛擬位址 → 配置。
    std::map<std::uint64_t,Request> requests;std::uint64_t next_request{1};
    std::uint32_t pci_address{}; // 0xCF8 CONFIG_ADDRESS 鎖存值。
    std::chrono::steady_clock::time_point shared_refresh{};
};
std::span<const WindowsServiceDescriptor> kernel_driver_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"ntoskrnl.exe","IoCreateDevice",7},{"ntoskrnl.exe","IoDeleteDevice",1},{"ntoskrnl.exe","IoCreateSymbolicLink",2},{"ntoskrnl.exe","IoDeleteSymbolicLink",1},
        {"ntoskrnl.exe","IofCompleteRequest",2},{"ntoskrnl.exe","RtlInitUnicodeString",2},{"ntoskrnl.exe","RtlUnwindEx",6},{"ntoskrnl.exe","PsGetVersion",4},
        {"ntoskrnl.exe","SeSinglePrivilegeCheck",2},{"ntoskrnl.exe","KeBugCheckEx",5},{"ntoskrnl.exe","MmMapIoSpace",3},{"ntoskrnl.exe","MmUnmapIoSpace",2},
        {"ntoskrnl.exe","ExAllocatePool",2},{"ntoskrnl.exe","ExAllocatePoolWithTag",3},{"ntoskrnl.exe","ExFreePool",1},{"ntoskrnl.exe","ExFreePoolWithTag",2},
        {"ntoskrnl.exe","KeGetCurrentIrql",0},{"ntoskrnl.exe","MmMapLockedPagesSpecifyCache",6},{"ntoskrnl.exe","DbgPrint",1,0,0,0,true},
        {"hal.dll","HalGetBusDataByOffset",6},{"hal.dll","HalSetBusDataByOffset",6},{"hal.dll","KeStallExecutionProcessor",1},{"hal.dll","KeQueryPerformanceCounter",1}
    };return table;
}
namespace {
namespace gs=guest_service;
constexpr std::uint64_t driver_object_size=0x150,device_object_size=0x150,file_object_size=0xd8,irp_size=0xd0,stack_size=0x48,mdl_size=0x30,security_context_size=0x18;
constexpr std::uint32_t status_success=0,status_pending=0x103,status_invalid_device_request=0xc0000010,status_name_collision=0xc0000035,status_name_not_found=0xc0000034;
bool nt_success(std::uint32_t status){return static_cast<std::int32_t>(status)>=0;}
std::u16string fold(std::u16string_view text){return gs::fold(text);}
// DOS 裝置符號連結統一成去除命名空間前綴後的名稱，例如 \DosDevices\Global\X、\??\X、\\.\X 皆對應 x。
std::u16string link_key(std::u16string_view name){
    auto value=fold(name);
    for(const std::u16string_view prefix:{u"\\dosdevices\\",u"\\??\\",u"\\global??\\",u"\\\\.\\",u"\\\\?\\"})if(value.starts_with(prefix)){value.erase(0,prefix.size());break;}
    if(value.starts_with(u"global\\"))value.erase(0,7);
    return value;
}
// 虛擬平台：未建模的 I/O 埠如同沒有裝置回應的 LPC 匯流排，讀回全 1、寫入忽略；PCI 匯流排目前沒有裝置。
std::uint32_t pci_config_read(std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t,unsigned width){return width>=4?UINT32_MAX:(1U<<(width*8))-1U;}
}
void WindowsServices::sync_kernel_shared_data(GuestMemory& memory,bool throttled){
    // KUSER_SHARED_DATA 的核心模式別名（KeQueryTickCount 等巨集直接讀取）；內容與使用者頁同步，TickCount 依 InterruptTime 換算。
    constexpr std::uint64_t alias=UINT64_C(0xfffff78000000000),user=0x7ffe0000;
    auto& k=kernel_state();const auto now=std::chrono::steady_clock::now();
    if(throttled && now-k.shared_refresh<std::chrono::milliseconds(1))return;k.shared_refresh=now;
    if(!memory.mapped(GuestAddress{alias}))memory.map(GuestAddress{alias},GuestMemory::guest_page_size,permission(Access::Read));
    std::vector<std::uint8_t> page(GuestMemory::guest_page_size);memory.read(GuestAddress{user},page);
    const auto word=[&](std::size_t at){std::uint32_t v=0;for(unsigned i=0;i<4;++i)v|=std::uint32_t{page[at+i]}<<(i*8);return v;};
    const auto put=[&](std::size_t at,std::uint32_t v){for(unsigned i=0;i<4;++i)page[at+i]=static_cast<std::uint8_t>(v>>(i*8));};
    const auto interrupt=(std::uint64_t{word(0x0c)}<<32)|word(0x08);const auto ticks=interrupt/156250U;
    put(0x320,static_cast<std::uint32_t>(ticks));put(0x324,static_cast<std::uint32_t>(ticks>>32));put(0x328,static_cast<std::uint32_t>(ticks>>32));
    if(!word(0x04))put(0x04,0x0fa00000U); // TickCountMultiplier：15.625 ms × 2^24。
    memory.update_readonly(GuestAddress{alias},page);
}
WindowsServices::KernelDriverState& WindowsServices::kernel_state(){
    // 程序狀態複製（快照或子程序）時採 copy-on-write，避免兩份服務共用同一驅動狀態。
    if(!kernel_)kernel_=std::make_shared<KernelDriverState>();else if(kernel_.use_count()>1)kernel_=std::make_shared<KernelDriverState>(*kernel_);
    return *kernel_;
}
std::optional<std::uint32_t> WindowsServices::invoke_kernel_api(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(service.symbol=="RtlUnwindEx")return invoke_runtime(state,memory,{"kernel32.dll","RtlUnwindEx",6});
    if(service.symbol=="RtlInitUnicodeString")return invoke_runtime(state,memory,{"ntdll.dll","RtlInitUnicodeString",2});
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    auto& k=kernel_state();
    const auto read_string=[&](std::uint64_t unicode)->std::u16string{
        if(!unicode)return {};const auto length=memory.load(GuestAddress{unicode},16),buffer=memory.load(GuestAddress{unicode+8},64);
        std::u16string text;for(std::uint64_t i=0;i<length/2;++i)text.push_back(static_cast<char16_t>(memory.load(GuestAddress{buffer+i*2},16)));return text;
    };
    try{
        if(name=="IoCreateDevice"){
            const auto driver=arg(0);const auto extension=static_cast<std::uint32_t>(arg(1));const auto device_name=read_string(arg(2));
            if(!driver || !arg(6))return finish(0xc000000dU);
            if(!device_name.empty() && std::any_of(k.devices.begin(),k.devices.end(),[&](const auto& d){return fold(d.second.name)==fold(device_name);}))return finish(status_name_collision);
            const auto extension_size=(static_cast<std::uint64_t>(extension)+15)&~std::uint64_t{15};
            const auto device=allocate_heap(memory,device_object_size+extension_size);if(!device)return finish(0xc000009aU);
            memory.fill(GuestAddress{device},0,static_cast<std::size_t>(device_object_size+extension_size));
            memory.store(GuestAddress{device},16,3);memory.store(GuestAddress{device+2},16,static_cast<std::uint16_t>(device_object_size+extension_size));
            memory.store(GuestAddress{device+8},64,driver);memory.store(GuestAddress{device+0x10},64,memory.load(GuestAddress{driver+8},64));
            memory.store(GuestAddress{device+0x30},32,0x80); // DO_DEVICE_INITIALIZING，驅動完成初始化後自行清除。
            memory.store(GuestAddress{device+0x34},32,static_cast<std::uint32_t>(arg(4)));
            memory.store(GuestAddress{device+0x40},64,extension?device+device_object_size:0);memory.store(GuestAddress{device+0x48},32,static_cast<std::uint32_t>(arg(3)));
            memory.store(GuestAddress{device+0x4c},8,1); // StackSize
            memory.store(GuestAddress{driver+8},64,device);memory.store(GuestAddress{arg(6)},64,device);
            k.devices[device]={driver,device_name};return finish(status_success);
        }
        if(name=="IoDeleteDevice"){
            const auto found=k.devices.find(arg(0));if(found==k.devices.end())return finish(0);
            // 從 DRIVER_OBJECT 的裝置單向串列移除。
            auto link=found->second.driver+8;for(unsigned guard=0;guard<4096;++guard){const auto current=memory.load(GuestAddress{link},64);if(!current)break;if(current==arg(0)){memory.store(GuestAddress{link},64,memory.load(GuestAddress{current+0x10},64));break;}link=current+0x10;}
            std::erase_if(k.links,[&](const auto& item){return item.second==arg(0);});k.devices.erase(found);(void)free_heap(memory,arg(0),state);return finish(0);
        }
        if(name=="IoCreateSymbolicLink" || name=="IoDeleteSymbolicLink"){
            const auto key=link_key(read_string(arg(0)));if(key.empty() || key.find(u'\\')!=std::u16string::npos)return finish(0xc0000033U);
            if(name=="IoDeleteSymbolicLink")return finish(k.links.erase(key)?status_success:status_name_not_found);
            if(k.links.contains(key))return finish(status_name_collision);
            const auto target=fold(read_string(arg(1)));const auto device=std::find_if(k.devices.begin(),k.devices.end(),[&](const auto& d){return fold(d.second.name)==target;});
            if(device==k.devices.end())return finish(status_name_not_found);
            k.links[key]=device->first;return finish(status_success);
        }
        if(name=="IofCompleteRequest")return finish(0); // 同步完成：IoStatus 已由驅動寫入 IRP，由分派返回後讀取。
        if(name=="PsGetVersion"){
            const auto info=allocate_heap(memory,0x120);if(!info)return finish(0);memory.fill(GuestAddress{info},0,0x120);memory.store(GuestAddress{info},32,0x11c);
            (void)nested_service(state,memory,"ntdll.dll","RtlGetVersion",{info});
            for(unsigned i=0;i<3;++i)if(arg(i))memory.store(GuestAddress{arg(i)},32,memory.load(GuestAddress{info+4+i*4},32));
            if(arg(3))memory.fill(GuestAddress{arg(3)},0,16);
            (void)free_heap(memory,info,state);return finish(0); // FALSE：非 checked build。
        }
        if(name=="SeSinglePrivilegeCheck"){
            if(!(arg(1)&255))return finish(1); // KernelMode
            initialize_tokens();return finish(runtime_.token_objects.at(effective_token_id()).has_privilege(arg(0),true)?1:0);
        }
        if(name=="KeBugCheckEx"){
            char text[160];std::snprintf(text,sizeof text,"核心驅動呼叫 KeBugCheckEx：0x%llx (0x%llx, 0x%llx, 0x%llx, 0x%llx)",static_cast<unsigned long long>(arg(0)),static_cast<unsigned long long>(arg(1)),static_cast<unsigned long long>(arg(2)),static_cast<unsigned long long>(arg(3)),static_cast<unsigned long long>(arg(4)));
            throw HostCallFault(text);
        }
        if(name=="MmMapIoSpace"){
            const auto physical=arg(0),size=arg(1);if(!size || size>16U*1024U*1024U || physical>UINT64_MAX-size)return finish(0);
            const auto bytes=physical_memory_snapshot(physical,static_cast<std::size_t>(size),memory);
            const auto address=allocate_heap(memory,size);if(!address)return finish(0);memory.write(GuestAddress{address},bytes);
            k.io_mappings[address]=size;return finish(address);
        }
        if(name=="MmMapLockedPagesSpecifyCache"){
            // 使用者緩衝與驅動共用同一客體位址空間：已映射者回 MappedSystemVa，否則為 StartVa＋ByteOffset。
            const auto mdl=arg(0);if(!mdl)return finish(0);
            if(memory.load(GuestAddress{mdl+0xa},16)&1U)return finish(memory.load(GuestAddress{mdl+0x18},64));
            return finish(memory.load(GuestAddress{mdl+0x20},64)+memory.load(GuestAddress{mdl+0x2c},32));
        }
        if(name=="MmUnmapIoSpace"){if(k.io_mappings.erase(arg(0)))(void)free_heap(memory,arg(0),state);return finish(0);}
        if(name.starts_with("ExAllocatePool")){const auto size=std::max<std::uint64_t>(arg(1),1);const auto p=allocate_heap(memory,size);if(p)memory.fill(GuestAddress{p},0,static_cast<std::size_t>(size));return finish(p);}
        if(name.starts_with("ExFreePool")){if(arg(0))(void)free_heap(memory,arg(0),state);return finish(0);}
        if(name=="KeGetCurrentIrql")return finish(0); // PASSIVE_LEVEL：IRP 分派與 DriverEntry 皆在呼叫端執行緒。
        if(name=="DbgPrint"){std::fprintf(stderr,"[kernel] DbgPrint %s\n",read_ansi(memory,arg(0),1024).c_str());return finish(0);}
        if(name=="HalGetBusDataByOffset" || name=="HalSetBusDataByOffset"){
            const bool set=name=="HalSetBusDataByOffset";const auto type=static_cast<std::uint32_t>(arg(0)),bus=static_cast<std::uint32_t>(arg(1)),slot=static_cast<std::uint32_t>(arg(2));
            const auto buffer=arg(3);const auto offset=static_cast<std::uint32_t>(arg(4)),length=static_cast<std::uint32_t>(arg(5));
            if(type!=4 || bus>255 || offset>=256 || !buffer)return finish(0); // 只有 PCIConfiguration。
            const auto device=slot&31U,function=(slot>>5)&7U;const auto count=std::min<std::uint32_t>(length,256-offset);
            if(pci_config_read(bus,device,function,0,2)==0xffffU){
                // 空插槽：Windows 回傳 2 並在 VendorID 填入 PCI_INVALID_VENDORID。
                if(set)return finish(0);if(offset==0 && count>=2){memory.store(GuestAddress{buffer},16,0xffff);return finish(2);}return finish(0);
            }
            if(set)return finish(count);
            for(std::uint32_t i=0;i<count;++i)memory.store(GuestAddress{buffer+i},8,pci_config_read(bus,device,function,offset+i,1));return finish(count);
        }
        if(name=="KeStallExecutionProcessor"){std::this_thread::sleep_for(std::chrono::microseconds(std::min<std::uint64_t>(static_cast<std::uint32_t>(arg(0)),1000000)));sync_kernel_shared_data(memory,true);return finish(0);}
        if(name=="KeQueryPerformanceCounter"){
            const auto value=allocate_heap(memory,16);if(!value)return finish(0);
            (void)nested_service(state,memory,"kernel32.dll","QueryPerformanceCounter",{value});
            if(arg(0))(void)nested_service(state,memory,"kernel32.dll","QueryPerformanceFrequency",{arg(0)});
            const auto counter=memory.load(GuestAddress{value},64);(void)free_heap(memory,value,state);return finish(counter);
        }
    }catch(const MemoryFault&){return finish(0xc0000005U);}
    throw HostCallFault("尚未實作此核心模式 API");
}
std::vector<std::uint8_t> WindowsServices::physical_memory_snapshot(std::uint64_t physical,std::size_t size,GuestMemory& memory){
    // 實體位址空間：0–0x9FFFF 為一般記憶體（讀回 0），E0000–FFFFF 為 BIOS 區段，含 SMBIOS 3.0／2.1 進入點與結構表；
    // 其他未建模範圍如同無裝置回應的匯流排讀回全 1。
    std::vector<std::uint8_t> bytes(size,0xff);
    static thread_local std::vector<std::uint8_t> bios;bios.assign(0x20000,0);
    const auto info=filesystem_.information("C:\\");
    const auto raw=virtual_firmware::smbios(memory.processor_count(),installed_physical_memory(memory),info.metadata.id^(info.metadata.device<<32),guest_processor_information(memory));
    if(raw.size()>8 && raw.size()-8<=0x10000){
        const auto table=std::span(raw).subspan(8);const std::uint64_t table_address=0xe0000,entry=0xf0000;
        std::copy(table.begin(),table.end(),bios.begin());
        const auto put=[&](std::size_t at,std::uint64_t value,unsigned width){for(unsigned i=0;i<width;++i)bios[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
        const auto checksum=[&](std::size_t at,std::size_t length,std::size_t field){std::uint8_t sum=0;for(std::size_t i=0;i<length;++i)if(at+i!=field)sum=static_cast<std::uint8_t>(sum+bios[at+i]);bios[field]=static_cast<std::uint8_t>(0x100-sum);};
        auto at=static_cast<std::size_t>(entry-0xe0000);const auto major=raw[1],minor=raw[2];
        // SMBIOS 3.0 64 位進入點。
        for(unsigned i=0;i<5;++i)bios[at+i]=static_cast<std::uint8_t>("_SM3_"[i]);bios[at+6]=0x18;bios[at+7]=major;bios[at+8]=minor;bios[at+9]=raw[3];bios[at+10]=1;
        put(at+12,table.size(),4);put(at+16,table_address,8);checksum(at,0x18,at+5);
        // SMBIOS 2.1 32 位進入點與 _DMI_ 中間結構，提供只認舊格式的工具。
        at+=0x20;for(unsigned i=0;i<4;++i)bios[at+i]=static_cast<std::uint8_t>("_SM_"[i]);bios[at+5]=0x1f;bios[at+6]=major;bios[at+7]=minor;
        std::uint16_t largest=0;std::uint16_t structures=0;
        for(std::size_t p=0;p+4<=table.size();){const auto length=table[p+1];std::size_t end=p+length;while(end+1<table.size() && (table[end] || table[end+1]))++end;end+=2;largest=std::max<std::uint16_t>(largest,static_cast<std::uint16_t>(end-p));++structures;if(table[p]==127)break;p=end;}
        put(at+8,largest,2);for(unsigned i=0;i<5;++i)bios[at+0x10+i]=static_cast<std::uint8_t>("_DMI_"[i]);put(at+0x16,table.size(),2);put(at+0x18,table_address,4);put(at+0x1c,structures,2);
        bios[at+0x1e]=static_cast<std::uint8_t>(((major&15)<<4)|(minor&15));checksum(at+0x10,0x0f,at+0x15);checksum(at,0x1f,at+4);
    }
    for(std::size_t i=0;i<size;++i){
        const auto address=physical+i;
        if(address<0xa0000)bytes[i]=0;
        else if(address>=0xe0000 && address<0x100000)bytes[i]=bios[static_cast<std::size_t>(address-0xe0000)];
    }
    return bytes;
}
bool WindowsServices::emulate_kernel_instruction(GuestState& state,GuestMemory& memory){
    if(!kernel_ || x86_)return false;auto& k=kernel_state();
    const auto rip=state.rip.value;
    const bool in_driver=std::any_of(k.drivers.begin(),k.drivers.end(),[&](const auto& d){return d.second.image && rip>=d.second.image && rip-d.second.image<d.second.image_size;});
    if(!in_driver)return false;
    std::array<std::uint8_t,15> code{};std::size_t length=0;
    try{for(;length<code.size();++length)code[length]=static_cast<std::uint8_t>(memory.load(GuestAddress{rip+length},8));}catch(const MemoryFault&){}
    std::size_t at=0;bool operand16=false;std::uint8_t rex=0;
    while(at<length && (code[at]==0x66 || (code[at]>=0x40 && code[at]<=0x4f))){if(code[at]==0x66)operand16=true;else rex=code[at];++at;}
    if(at>=length)return false;
    const auto op=code[at];
    const auto rax=state.reg(Gpr::Rax);
    const auto port_width=[&](bool byte){return byte?1U:operand16?2U:4U;};
    const auto read_port=[&](std::uint32_t port,unsigned width)->std::uint32_t{
        // PCI 設定機制 #1：0xCF8 鎖存位址，0xCFC–0xCFF 為資料視窗。
        if(port==0xcf8 && width==4)return k.pci_address;
        if(port>=0xcfc && port<=0xcff){
            if(!(k.pci_address&0x80000000U))return width>=4?UINT32_MAX:(1U<<(width*8))-1U;
            const auto bus=(k.pci_address>>16)&255U,device=(k.pci_address>>11)&31U,function=(k.pci_address>>8)&7U,offset=(k.pci_address&0xfcU)+(port-0xcfc);
            std::uint32_t value=0;for(unsigned i=0;i<width;++i)value|=pci_config_read(bus,device,function,offset+i,1)<<(i*8);return value;
        }
        return width>=4?UINT32_MAX:(1U<<(width*8))-1U;
    };
    const auto write_port=[&](std::uint32_t port,unsigned width,std::uint32_t value){if(port==0xcf8 && width==4)k.pci_address=value&0x80fffffcU;};
    const auto store_result=[&](unsigned width,std::uint32_t value){
        if(width==4)state.reg(Gpr::Rax)=value; // 32 位寫入清除上半部。
        else if(width==2)state.reg(Gpr::Rax)=(rax&~std::uint64_t{0xffff})|value;
        else state.reg(Gpr::Rax)=(rax&~std::uint64_t{0xff})|value;
    };
    const auto dx=static_cast<std::uint32_t>(state.reg(Gpr::Rdx)&0xffffU);
    std::size_t size=0;
    if(op==0xe4 || op==0xe5){if(at+1>=length)return false;store_result(port_width(op==0xe4),read_port(code[at+1],port_width(op==0xe4)));size=at+2;}
    else if(op==0xec || op==0xed){store_result(port_width(op==0xec),read_port(dx,port_width(op==0xec)));size=at+1;}
    else if(op==0xe6 || op==0xe7){if(at+1>=length)return false;write_port(code[at+1],port_width(op==0xe6),static_cast<std::uint32_t>(rax));size=at+2;}
    else if(op==0xee || op==0xef){write_port(dx,port_width(op==0xee),static_cast<std::uint32_t>(rax));size=at+1;}
    else if(op==0xfa || op==0xfb){size=at+1;} // CLI／STI：驅動在呼叫端執行緒以 PASSIVE_LEVEL 執行，不改變宿主中斷。
    else if(op==0x0f && at+1<length && (code[at+1]==0x09 || code[at+1]==0x08)){size=at+2;} // WBINVD／INVD：宿主快取一致。
    else if(op==0x0f && at+2<length && code[at+1]==0x20 && (code[at+2]&0xc0U)==0xc0U && ((code[at+2]>>3)&7U)==0 && (rex&4U)){
        // MOV r64, CR8（REX.R 選 CR8）：回報 PASSIVE_LEVEL。
        const auto reg=static_cast<unsigned>(code[at+2]&7U)+((rex&1U)?8U:0U);state.reg(static_cast<Gpr>(reg))=0;size=at+3;
    }
    else return false; // RDMSR／WRMSR 等：虛擬 CPU 的 CPUID 未宣告 MSR，維持 #GP 交給驅動的例外處理。
    sync_kernel_shared_data(memory,true);state.rip=GuestAddress{rip+size};return true;
}
bool WindowsServices::kernel_device_path(std::u16string_view path) const{
    if(!kernel_)return false;const auto key=link_key(path);
    if(kernel_->links.contains(key))return true;
    const auto folded=fold(path);
    for(const std::u16string_view prefix:{u"\\\\.\\globalroot",u"\\\\?\\globalroot"})if(folded.starts_with(prefix)){const auto device=folded.substr(prefix.size());
        return std::any_of(kernel_->devices.begin(),kernel_->devices.end(),[&](const auto& d){return fold(d.second.name)==device;});}
    return false;
}
bool WindowsServices::kernel_device_handle(std::uint64_t handle) const{return kernel_ && kernel_->files.contains(handle);}
std::uint64_t WindowsServices::kernel_irp(GuestMemory& memory,std::uint8_t major,std::uint64_t device,std::uint64_t file_object){
    const auto block=allocate_heap(memory,irp_size+stack_size+mdl_size+security_context_size);if(!block)throw std::bad_alloc();
    memory.fill(GuestAddress{block},0,static_cast<std::size_t>(irp_size+stack_size+mdl_size+security_context_size));
    const auto stack=block+irp_size;
    memory.store(GuestAddress{block},16,6);memory.store(GuestAddress{block+2},16,static_cast<std::uint16_t>(irp_size+stack_size));
    memory.store(GuestAddress{block+0x40},8,1); // RequestorMode=UserMode
    memory.store(GuestAddress{block+0x42},8,1);memory.store(GuestAddress{block+0x43},8,1); // StackCount／CurrentLocation
    memory.store(GuestAddress{block+0xb8},64,stack);memory.store(GuestAddress{block+0xc0},64,file_object);
    memory.store(GuestAddress{stack},8,major);memory.store(GuestAddress{stack+0x28},64,device);memory.store(GuestAddress{stack+0x30},64,file_object);
    return block;
}
std::uint64_t WindowsServices::kernel_dispatch(GuestMemory& memory,std::uint64_t device,std::uint8_t major){
    const auto driver=memory.load(GuestAddress{device+8},64);return memory.load(GuestAddress{driver+0x70+std::uint64_t{major}*8},64);
}
std::optional<std::uint32_t> WindowsServices::start_kernel_driver(GuestState& state,GuestMemory& memory,const std::u16string& service,const std::u16string& image_path){
    const auto fail=[&](std::uint32_t error){last_error_=error;WindowsCall(state,memory,3).return_to(state,0);return std::optional<std::uint32_t>{};};
    // 32 位程序無法承載 x64 核心映像；真實系統的核心在獨立位址空間執行。
    if(x86_)return fail(50);
    auto& k=kernel_state();
    if(const auto found=k.drivers.find(fold(service));found!=k.drivers.end() && found->second.running)return fail(1056);
    // ImagePath：\??\C:\x.sys、\SystemRoot\...、System32\drivers\x.sys 或一般 Win32 路徑。
    auto path=image_path;const auto folded=fold(path);
    if(folded.starts_with(u"\\??\\"))path.erase(0,4);
    else if(folded.starts_with(u"\\systemroot\\"))path=u"C:\\Windows\\"+path.substr(12);
    else if(folded.starts_with(u"system32\\"))path=u"C:\\Windows\\"+path;
    const auto [image,load_error]=load_kernel_image(memory,path);if(load_error)return fail(load_error==126?2U:load_error);
    const auto module=runtime_.loaded_modules.find(image);if(module==runtime_.loaded_modules.end() || !module->second.image.entry)return fail(193);
    // DRIVER_OBJECT、DRIVER_EXTENSION 與 RegistryPath／DriverName 字串置於同一配置。
    const std::u16string registry=u"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\"+service,driver_name=u"\\Driver\\"+service;
    const auto strings=driver_object_size+0x40;const auto total=strings+(registry.size()+driver_name.size()+2)*2+32;
    const auto object=allocate_heap(memory,total);if(!object)return fail(8);memory.fill(GuestAddress{object},0,static_cast<std::size_t>(total));
    const auto extension=object+driver_object_size,registry_text=object+strings,driver_text=registry_text+(registry.size()+1)*2,registry_string=driver_text+(driver_name.size()+1)*2;
    for(std::size_t i=0;i<registry.size();++i)memory.store(GuestAddress{registry_text+i*2},16,registry[i]);
    for(std::size_t i=0;i<driver_name.size();++i)memory.store(GuestAddress{driver_text+i*2},16,driver_name[i]);
    const auto unicode=[&](std::uint64_t at,std::uint64_t text,std::size_t length){memory.store(GuestAddress{at},16,length*2);memory.store(GuestAddress{at+2},16,length*2+2);memory.store(GuestAddress{at+8},64,text);};
    memory.store(GuestAddress{object},16,4);memory.store(GuestAddress{object+2},16,static_cast<std::uint16_t>(driver_object_size));
    memory.store(GuestAddress{object+0x18},64,image);memory.store(GuestAddress{object+0x20},32,static_cast<std::uint32_t>(module->second.image.size));
    memory.store(GuestAddress{object+0x30},64,extension);unicode(object+0x38,driver_text,driver_name.size());memory.store(GuestAddress{object+0x58},64,module->second.image.entry);
    memory.store(GuestAddress{extension},64,object);unicode(extension+0x18,registry_text+(registry.size()-service.size())*2,service.size());
    unicode(registry_string,registry_text,registry.size());
    // 預設分派為 IopInvalidDeviceRequest：未設定的主要功能回傳 STATUS_INVALID_DEVICE_REQUEST。
    k.drivers[fold(service)]=KernelDriverState::Driver{service,image,module->second.image.size,object,0,false};
    const auto id=k.next_request++;auto& request=k.requests[id];request.kind=KernelDriverState::Request::Kind::Entry;request.service=service;request.driver=object;
    sync_kernel_shared_data(memory);
    start_callbacks(state,memory,{{module->second.image.entry,{object,registry_string}}},Continuation::KernelRequest,id);return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::stop_kernel_driver(GuestState& state,GuestMemory& memory,const std::u16string& service,std::uint64_t status_output){
    const auto fail=[&](std::uint32_t error){last_error_=error;WindowsCall(state,memory,3).return_to(state,0);return std::optional<std::uint32_t>{};};
    if(!kernel_)return fail(1062);auto& k=kernel_state();const auto found=k.drivers.find(fold(service));if(found==k.drivers.end() || !found->second.running)return fail(1062);
    const auto unload=memory.load(GuestAddress{found->second.object+0x68},64);if(!unload)return fail(1052); // 沒有 DriverUnload 的驅動不可停止。
    if(std::any_of(k.files.begin(),k.files.end(),[&](const auto& f){return k.devices.contains(f.second.device) && k.devices.at(f.second.device).driver==found->second.object;}))return fail(1061); // 仍有開啟的裝置 handle。
    const auto id=k.next_request++;auto& request=k.requests[id];request.kind=KernelDriverState::Request::Kind::Unload;request.service=service;request.driver=found->second.object;request.output=status_output;
    sync_kernel_shared_data(memory);
    start_callbacks(state,memory,{{unload,{found->second.object}}},Continuation::KernelRequest,id);return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::open_kernel_device(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service,std::u16string_view path,const FileOpenArguments& values){
    auto& k=kernel_state();const WindowsCall call(state,memory,service);
    const auto invalid=[&](std::uint32_t error){last_error_=error;call.return_to(state,UINT64_MAX);return std::optional<std::uint32_t>{};};
    // Win32 處置值轉為 NT CreateDisposition（FILE_CREATE、FILE_OVERWRITE_IF、FILE_OPEN、FILE_OPEN_IF、FILE_OVERWRITE）。
    constexpr std::array<std::uint32_t,6> dispositions{0,2,5,1,3,4};const auto disposition=static_cast<std::uint32_t>(values[4]);if(disposition<1 || disposition>5)return invalid(87);
    std::uint64_t device=0;
    if(const auto link=k.links.find(link_key(path));link!=k.links.end())device=link->second;
    else {const auto folded=fold(path);for(const auto& [address,info]:k.devices)if(folded.ends_with(fold(info.name)))device=address;}
    if(!device)return invalid(2);
    const auto file_object=allocate_heap(memory,file_object_size);if(!file_object)return invalid(8);
    memory.fill(GuestAddress{file_object},0,file_object_size);memory.store(GuestAddress{file_object},16,5);memory.store(GuestAddress{file_object+2},16,file_object_size);memory.store(GuestAddress{file_object+8},64,device);
    const auto irp=kernel_irp(memory,0,device,file_object);const auto stack=irp+irp_size,security=irp+irp_size+stack_size+mdl_size;
    memory.store(GuestAddress{security+0x10},32,static_cast<std::uint32_t>(values[1]));memory.store(GuestAddress{stack+8},64,security);
    memory.store(GuestAddress{stack+0x10},32,(dispositions[disposition]<<24)|static_cast<std::uint32_t>(values[5]&0xffffffU));memory.store(GuestAddress{stack+0x1a},16,static_cast<std::uint16_t>(values[2]));
    const auto handle=runtime_.allocate_wait_handle();
    const auto id=k.next_request++;auto& request=k.requests[id];request.kind=KernelDriverState::Request::Kind::Create;request.handle=handle;request.device=device;request.file_object=file_object;request.irps={irp};
    const auto dispatch=kernel_dispatch(memory,device,0);
    if(!dispatch){memory.store(GuestAddress{irp+0x30},32,status_invalid_device_request);return resume_kernel_request(state,memory,id,status_invalid_device_request);}
    sync_kernel_shared_data(memory);
    start_callbacks(state,memory,{{dispatch,{device,irp}}},Continuation::KernelRequest,id);return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::invoke_kernel_device_io(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    auto& k=kernel_state();const WindowsCall arg(state,memory,service);const bool nt=service.symbol=="NtDeviceIoControlFile";
    const auto file=k.files.at(arg(0));
    if(service.symbol!="DeviceIoControl" && !nt){
        // 驅動裝置只支援 IOCTL；讀寫與檔案資訊查詢回報裝置不支援。
        last_error_=1;arg.return_to(state,service.symbol.starts_with("Nt")?status_invalid_device_request:0);return std::nullopt;
    }
    const auto code=static_cast<std::uint32_t>(arg(nt?5:1));const auto input=arg(nt?6:2),output=arg(nt?8:4);
    const auto input_size=static_cast<std::uint32_t>(arg(nt?7:3)),output_size=static_cast<std::uint32_t>(arg(nt?9:5));
    const auto fail=[&](std::uint32_t error){last_error_=error;arg.return_to(state,nt?0xc000000dU:0U);return std::optional<std::uint32_t>{};};
    if(!nt && !arg(6) && !arg(7))return fail(87);if((input_size && !input) || (output_size && !output))return fail(998);
    const auto method=code&3U;const auto irp=kernel_irp(memory,0xe,file.device,file.file_object);const auto stack=irp+irp_size;
    const auto id=k.next_request++;auto& request=k.requests[id];request.kind=KernelDriverState::Request::Kind::Control;request.irps={irp};request.nt=nt;
    request.output=output;request.output_size=output_size;request.code=code;request.device=file.device;
    if(nt){request.iosb=arg(4);request.event=arg(1);}else{request.returned=arg(6);request.overlapped=arg(7);}
    std::uint64_t system_buffer=0;
    if(method!=3){
        // METHOD_BUFFERED 以 max(輸入, 輸出) 系統緩衝；DIRECT 只複製輸入，輸出由 MDL 描述使用者緩衝。
        const auto size=method==0?std::max(input_size,output_size):input_size;
        if(size){system_buffer=allocate_heap(memory,size);if(!system_buffer){k.requests.erase(id);(void)free_heap(memory,irp,state);return fail(8);}
            memory.fill(GuestAddress{system_buffer},0,size);if(input_size){std::vector<std::uint8_t> bytes(input_size);memory.read(GuestAddress{input},bytes);memory.write(GuestAddress{system_buffer},bytes);}}
        if(method!=0 && output_size){
            const auto mdl=irp+irp_size+stack_size;memory.store(GuestAddress{mdl+8},16,mdl_size);memory.store(GuestAddress{mdl+0xa},16,0x5); // MAPPED_TO_SYSTEM_VA|SOURCE_IS_NONPAGED_POOL
            memory.store(GuestAddress{mdl+0x18},64,output);memory.store(GuestAddress{mdl+0x20},64,output&~std::uint64_t{4095});memory.store(GuestAddress{mdl+0x28},32,output_size);memory.store(GuestAddress{mdl+0x2c},32,static_cast<std::uint32_t>(output&4095U));
            memory.store(GuestAddress{irp+8},64,mdl);
        }
    }else memory.store(GuestAddress{stack+0x20},64,input);
    request.system_buffer=system_buffer;memory.store(GuestAddress{irp+0x18},64,system_buffer);memory.store(GuestAddress{irp+0x70},64,method==3 || method==0?output:0);
    memory.store(GuestAddress{stack+8},32,output_size);memory.store(GuestAddress{stack+0x10},32,input_size);memory.store(GuestAddress{stack+0x18},32,code);
    const auto dispatch=kernel_dispatch(memory,file.device,0xe);
    if(!dispatch){memory.store(GuestAddress{irp+0x30},32,status_invalid_device_request);return resume_kernel_request(state,memory,id,status_invalid_device_request);}
    sync_kernel_shared_data(memory);
    start_callbacks(state,memory,{{dispatch,{file.device,irp}}},Continuation::KernelRequest,id);return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::close_kernel_device(GuestState& state,GuestMemory& memory,std::uint64_t handle){
    auto& k=kernel_state();const auto file=k.files.at(handle);k.files.erase(handle);
    const auto id=k.next_request++;auto& request=k.requests[id];request.kind=KernelDriverState::Request::Kind::Close;request.file_object=file.file_object;request.device=file.device;
    std::vector<GuestCallback> calls;
    for(const std::uint8_t major:{std::uint8_t{0x12},std::uint8_t{2}}){
        const auto dispatch=kernel_dispatch(memory,file.device,major);const auto irp=kernel_irp(memory,major,file.device,file.file_object);request.irps.push_back(irp);
        if(dispatch)calls.push_back({dispatch,{file.device,irp}});
    }
    if(calls.empty())return resume_kernel_request(state,memory,id,0);
    sync_kernel_shared_data(memory);
    start_callbacks(state,memory,std::move(calls),Continuation::KernelRequest,id);return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::resume_kernel_request(GuestState& state,GuestMemory& memory,std::uint64_t id,std::uint64_t value){
    auto& k=kernel_state();auto request=std::move(k.requests.at(id));k.requests.erase(id);
    const auto status=static_cast<std::uint32_t>(value);
    const auto dos=[&](std::uint32_t code){return static_cast<std::uint32_t>(nested_service(state,memory,"ntdll.dll","RtlNtStatusToDosError",{code}));};
    const auto release=[&]{for(const auto irp:request.irps)(void)free_heap(memory,irp,state);if(request.system_buffer)(void)free_heap(memory,request.system_buffer,state);};
    using Kind=KernelDriverState::Request::Kind;
    if(request.kind==Kind::Entry || request.kind==Kind::Unload){
        auto& driver=k.drivers.at(fold(request.service));const bool entry=request.kind==Kind::Entry;const bool ok=!entry || nt_success(status);
        if(ok){driver.running=entry;driver.unload=memory.load(GuestAddress{request.driver+0x68},64);}
        if(!driver.running){
            // 失敗或已卸載：移除裝置、連結並釋放映像。
            for(auto it=k.devices.begin();it!=k.devices.end();){if(it->second.driver==request.driver){const auto device=it->first;std::erase_if(k.links,[&](const auto& item){return item.second==device;});(void)free_heap(memory,device,state);it=k.devices.erase(it);}else ++it;}
            const auto image=driver.image;(void)free_heap(memory,request.driver,state);k.drivers.erase(fold(request.service));unmap_module(memory,image);
        }
        {
            RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_);
            if(!transaction.error)if(const auto record=runtime_.registry.keys.find(gs::path(request.service));record!=runtime_.registry.keys.end()){
                const auto owner=process_identity::query(process_id()).identity;
                gs::put_number(record->second,u"x64arm_pid",owner.pid);gs::put_number(record->second,u"x64arm_birth",owner.birth,true);
                const std::uint32_t accepted=ok && entry && memory.load(GuestAddress{request.driver+0x68},64)?1U:0U;
                gs::put_status(record->second,std::array<std::uint32_t,7>{1,ok && entry?4U:1U,accepted,ok?0U:dos(status),ok?0U:status,0,0});(void)gs::commit(runtime_.registry,transaction);
            }
        }
        if(!ok)last_error_=dos(status);
        // ControlService(STOP) 回填停止後的 SERVICE_STATUS。
        if(!entry && request.output){const std::array<std::uint32_t,7> words{1,1,0,0,0,0,0};for(unsigned i=0;i<7;++i)memory.store(GuestAddress{request.output+i*4},32,words[i]);}
        complete_call(state,memory,ok?1:0);return std::nullopt;
    }
    if(request.kind==Kind::Create){
        const auto irp_status=static_cast<std::uint32_t>(memory.load(GuestAddress{request.irps[0]+0x30},32));const auto result=status==status_pending?irp_status:status;
        release();
        if(!nt_success(result)){(void)free_heap(memory,request.file_object,state);last_error_=dos(result);complete_call(state,memory,UINT64_MAX);return std::nullopt;}
        k.files[request.handle]={request.device,request.file_object};complete_call(state,memory,request.handle);return std::nullopt;
    }
    if(request.kind==Kind::Close){release();(void)free_heap(memory,request.file_object,state);complete_call(state,memory,1);return std::nullopt;}
    const auto irp=request.irps[0];
    const auto irp_status=static_cast<std::uint32_t>(memory.load(GuestAddress{irp+0x30},32));const auto information=memory.load(GuestAddress{irp+0x38},64);
    // 同步驅動在返回前完成 IRP；仍為 STATUS_PENDING 表示需要非同步完成，目前宿主不排程。
    const auto result=status==status_pending?(irp_status==status_pending?0xc00000bbU:irp_status):status;
    const auto method=request.code&3U;std::uint64_t transferred=method==0?std::min<std::uint64_t>(information,request.output_size):information;
    if(method==0 && request.output && transferred && (nt_success(result) || result==0x80000005U)){
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(transferred));memory.read(GuestAddress{request.system_buffer},bytes);memory.write(GuestAddress{request.output},bytes);
    }
    if(!nt_success(result) && result!=0x80000005U)transferred=0;
    release();
    if(request.nt){
        if(request.iosb){memory.store(GuestAddress{request.iosb},64,result);memory.store(GuestAddress{request.iosb+8},64,transferred);}
        if(request.event)(void)nested_service(state,memory,"kernel32.dll","SetEvent",{request.event});
        complete_call(state,memory,result);return std::nullopt;
    }
    if(request.returned)memory.store(GuestAddress{request.returned},32,transferred);
    if(request.overlapped){memory.store(GuestAddress{request.overlapped},64,result);memory.store(GuestAddress{request.overlapped+8},64,transferred);
        if(const auto event=memory.load(GuestAddress{request.overlapped+24},64))(void)nested_service(state,memory,"kernel32.dll","SetEvent",{event});}
    if(!nt_success(result))last_error_=dos(result);
    complete_call(state,memory,nt_success(result)?1:0);return std::nullopt;
}
}
