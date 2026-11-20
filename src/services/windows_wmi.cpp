#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/gpu_memory.hpp"
#include "virtual_firmware.hpp"
#include "service_callback_task.hpp"
#include "windows_display_identity.hpp"
#include <algorithm>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <chrono>

namespace x64arm {
namespace {
constexpr unsigned locator=0,services=1,enumerator=2,object=3;
constexpr std::array<std::string_view,4> interfaces{
    "dc12a687-737f-11cf-884d-00aa004b2e24","9556dc99-828c-11cf-a37e-00aa003240c7",
    "027947e1-d731-11ce-a357-000000000001","dc12a681-737f-11cf-884d-00aa004b2e24"};
// WbemCli／WbemProv 的 Locator coclass 都提供 IWbemLocator；本機連線沿用相同的
// 虛擬使用者、命名空間與認證限制，不因類別名稱另建後端或提升權限。
constexpr std::array<std::string_view,4> locator_classes{
    "4590f811-1d3a-11d0-891f-00aa004b2e24","cb8555cc-9128-11d1-ad9b-00c04fd8fdff",
    "cd184336-9128-11d1-ad9b-00c04fd8fdff","443e7b79-de31-11d2-b340-00104bcc4b4a"};
std::string guid(const GuestMemory& memory,std::uint64_t address){
    std::string result;constexpr char hex[]="0123456789abcdef";
    for(const unsigned offset:std::array<unsigned,16>{3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15}){
        if(result.size()==8 || result.size()==13 || result.size()==18 || result.size()==23)result+='-';
        const auto byte=memory.load(GuestAddress{address+offset},8);result+=hex[byte>>4];result+=hex[byte&15];
    }
    return result;
}
std::u16string key(std::u16string text){for(auto& c:text)if(c>=u'a' && c<=u'z')c-=32;return text;}
struct WmiProperty {std::u16string_view name;std::uint32_t cim;};
constexpr WmiProperty video_properties[]{
    {u"AcceleratorCapabilities",0x2012},{u"AdapterCompatibility",8},{u"AdapterDACType",8},{u"AdapterRAM",19},
    {u"Availability",18},{u"CapabilityDescriptions",0x2008},{u"Caption",8},{u"ColorTableEntries",19},
    {u"ConfigManagerErrorCode",19},{u"ConfigManagerUserConfig",11},{u"CreationClassName",8},
    {u"CurrentBitsPerPixel",19},{u"CurrentHorizontalResolution",19},{u"CurrentNumberOfColors",21},
    {u"CurrentNumberOfColumns",19},{u"CurrentNumberOfRows",19},{u"CurrentRefreshRate",19},{u"CurrentScanMode",18},
    {u"CurrentVerticalResolution",19},{u"Description",8},{u"DeviceID",8},{u"DeviceSpecificPens",19},{u"DitherType",19},
    {u"DriverDate",101},{u"DriverVersion",8},{u"ErrorCleared",11},{u"ErrorDescription",8},{u"ICMIntent",19},{u"ICMMethod",19},
    {u"InfFilename",8},{u"InfSection",8},{u"InstallDate",101},{u"InstalledDisplayDrivers",8},{u"LastErrorCode",19},
    {u"MaxMemorySupported",19},{u"MaxNumberControlled",19},{u"MaxRefreshRate",19},{u"MinRefreshRate",19},{u"Monochrome",11},
    {u"Name",8},{u"NumberOfColorPlanes",18},{u"NumberOfVideoPages",19},{u"PNPDeviceID",8},
    {u"PowerManagementCapabilities",0x2012},{u"PowerManagementSupported",11},{u"ProtocolSupported",18},
    {u"ReservedSystemPaletteEntries",19},{u"SpecificationVersion",19},{u"Status",8},{u"StatusInfo",18},
    {u"SystemCreationClassName",8},{u"SystemName",8},{u"SystemPaletteEntries",19},{u"TimeOfLastReset",101},
    {u"VideoArchitecture",18},{u"VideoMemoryType",18},{u"VideoMode",18},{u"VideoModeDescription",8},{u"VideoProcessor",8}};
std::u16string property_spelling(const std::u16string& property){
    static const std::map<std::u16string,std::u16string> names=[] {
        std::map<std::u16string,std::u16string> result;
        for(const auto* text:{u"DeviceID",u"Name",u"Manufacturer",u"Architecture",u"AddressWidth",u"DataWidth",
            u"NumberOfCores",u"NumberOfEnabledCore",u"NumberOfLogicalProcessors",u"SocketDesignation",u"MaxClockSpeed",
            u"CurrentClockSpeed",u"L2CacheSize",u"L3CacheSize",u"Model",u"SystemType",u"NumberOfProcessors",
            u"TotalPhysicalMemory",u"Capacity",u"DeviceLocator",u"BankLabel",u"Tag",u"ConfiguredClockSpeed",
            u"Speed",u"MemoryType",u"SMBIOSMemoryType",u"FormFactor",u"PartNumber",u"Product",u"Version",
            u"SMBIOSBIOSVersion",u"Caption",u"OSArchitecture",u"TotalVisibleMemorySize",u"FreePhysicalMemory",u"__CLASS",
            u"SMBiosData",u"Size",u"SmbiosMajorVersion",u"SmbiosMinorVersion",u"DmiRevision",u"Used20CallingMethod",u"InstanceName",u"Active"})result.emplace(key(text),text);
        for(const auto& field:video_properties)result.emplace(key(std::u16string(field.name)),field.name);
        return result;
    }();
    const auto found=names.find(property);return found==names.end()?property:found->second;
}
WmiValue string_value(std::u16string value,std::uint32_t cim=8){return {8,cim,0,std::move(value)};}
WmiValue number_value(std::uint32_t value,std::uint32_t cim=19){return {3,cim,value,{}};}
WmiRow video_controller_row(const std::array<std::uint32_t,5>& mode,std::uint64_t gpu_memory){
    WmiRow row;
    // CIM 的可空硬體屬性保留型別並回 VT_NULL；不虛構 DAC、INF、記憶體上限或未取得的硬體能力。
    for(const auto& property:video_properties)row.emplace(key(std::u16string(property.name)),WmiValue{1,property.cim,0,{}});
    const auto text=[&](std::u16string_view name,std::u16string_view value){row[key(std::u16string(name))]=string_value(std::u16string(value));};
    const auto number=[&](std::u16string_view name,std::uint32_t value,std::uint32_t cim=19){row[key(std::u16string(name))]=number_value(value,cim);};
    for(auto name:{u"Name",u"Caption",u"Description",u"VideoProcessor"})text(name,display_identity::adapter_name);
    text(u"DeviceID",u"VideoController1");text(u"PNPDeviceID",display_identity::instance);text(u"AdapterCompatibility",display_identity::vendor_name);
    text(u"DriverVersion",display_identity::version);
    text(u"CreationClassName",u"Win32_VideoController");text(u"SystemCreationClassName",u"Win32_ComputerSystem");text(u"Status",u"OK");
    // 與 DXGI 相同的 UMA 配接器：獨立顯存為 0，共用顯存配額不冒充 AdapterRAM。
    const auto adapter=display_identity::adapter_description(false,gpu_memory);std::uint32_t dedicated=0;
    for(unsigned n=0;n<4;++n)dedicated|=std::uint32_t{adapter[272+n]}<<(n*8);
    number(u"AdapterRAM",dedicated);number(u"Availability",3,18);number(u"ConfigManagerErrorCode",0);number(u"LastErrorCode",0);
    if(mode[0] && mode[1]){
        number(u"CurrentBitsPerPixel",mode[2]);number(u"CurrentHorizontalResolution",mode[0]);number(u"CurrentVerticalResolution",mode[1]);number(u"CurrentRefreshRate",mode[3]);
        row[u"CURRENTNUMBEROFCOLORS"]=string_value(utf8_to_utf16(std::to_string(UINT64_C(1)<<std::min(mode[2],32U))),21);
    }
    number(u"CurrentNumberOfColumns",0);number(u"CurrentNumberOfRows",0);number(u"NumberOfColorPlanes",1,18);
    number(u"CurrentScanMode",mode[4]==1?4U:mode[4]==2 || mode[4]==3?3U:2U,18);
    for(auto name:{u"ConfigManagerUserConfig",u"Monochrome",u"PowerManagementSupported"})row[key(name)]={11,11,0,{}};
    number(u"StatusInfo",3,18);number(u"VideoArchitecture",1,18);number(u"VideoMemoryType",2,18);number(u"MaxNumberControlled",0);
    const auto moment=std::chrono::sys_seconds{std::chrono::seconds{static_cast<std::int64_t>(display_identity::date_filetime/10000000)-INT64_C(11644473600)}};
    const auto day=std::chrono::floor<std::chrono::days>(moment);const std::chrono::year_month_day date{day};const std::chrono::hh_mm_ss time_of_day{moment-day};
    char timestamp[32];std::snprintf(timestamp,sizeof timestamp,"%04d%02u%02u%02lld%02lld%02lld.000000+000",static_cast<int>(date.year()),static_cast<unsigned>(date.month()),static_cast<unsigned>(date.day()),static_cast<long long>(time_of_day.hours().count()),static_cast<long long>(time_of_day.minutes().count()),static_cast<long long>(time_of_day.seconds().count()));
    row[u"DRIVERDATE"]=string_value(utf8_to_utf16(timestamp),101);return row;
}
std::optional<std::vector<WmiRow>> rows(std::u16string name,const GuestMemory& memory,const std::u16string& name_space,std::span<const std::uint8_t> firmware,const ProcessorInformation& cpu,const std::array<std::uint32_t,5>& display,std::uint64_t gpu_memory){
    name=key(std::move(name));WmiRow row;
    if(name_space==u"ROOT\\WMI"){
        if(name==u"MSSMBIOS_RAWSMBIOSTABLES"){
            if(firmware.size()<8)return std::nullopt;
            WmiValue data;data.type=0x2011;data.cim=0x2011;data.bytes.assign(firmware.begin()+8,firmware.end());
            row={{u"SMBIOSDATA",std::move(data)},{u"SIZE",number_value(static_cast<std::uint32_t>(firmware.size()-8))},
                {u"SMBIOSMAJORVERSION",number_value(firmware[1],17)},{u"SMBIOSMINORVERSION",number_value(firmware[2],17)},
                {u"DMIREVISION",number_value(firmware[3],17)},{u"USED20CALLINGMETHOD",{11,11,firmware[0]?0xffffU:0U,{}}},
                {u"ACTIVE",{11,11,0xffff,{}}},{u"INSTANCENAME",string_value(u"x64arm_SMBIOS_0")},{u"__CLASS",string_value(name)}};
            return std::vector<WmiRow>{std::move(row)};
        }
        return std::nullopt;
    }
    // 核心數依客體排程配額；名稱與已知時脈和 SMBIOS 共用同一份硬體描述。
    if(name==u"WIN32_PROCESSOR")row={
        {u"DEVICEID",string_value(u"CPU0")},{u"NAME",string_value(utf8_to_utf16(cpu.name))},
        {u"MANUFACTURER",string_value(utf8_to_utf16(cpu.manufacturer))},{u"ARCHITECTURE",number_value(9,18)},
        {u"ADDRESSWIDTH",number_value(64,18)},{u"DATAWIDTH",number_value(64,18)},
        {u"NUMBEROFCORES",number_value(memory.processor_count())},{u"NUMBEROFENABLEDCORE",number_value(memory.processor_count())},
        {u"NUMBEROFLOGICALPROCESSORS",number_value(memory.processor_count())},{u"SOCKETDESIGNATION",string_value(utf8_to_utf16(cpu.socket))},
        {u"MAXCLOCKSPEED",cpu.maximum_mhz?number_value(cpu.maximum_mhz):WmiValue{1,19,0,{}}},
        {u"CURRENTCLOCKSPEED",cpu.nominal_mhz?number_value(cpu.nominal_mhz):WmiValue{1,19,0,{}}},
        {u"L2CACHESIZE",number_value(virtual_processor_caches(memory.processor_count())[2].size/1024)},
        {u"L3CACHESIZE",number_value(virtual_processor_caches(memory.processor_count())[3].size/1024)}};
    else if(name==u"WIN32_VIDEOCONTROLLER")row=video_controller_row(display,gpu_memory);
    else if(name==u"WIN32_COMPUTERSYSTEM")row={
        {u"MANUFACTURER",string_value(utf8_to_utf16(virtual_firmware::manufacturer))},{u"MODEL",string_value(utf8_to_utf16(virtual_firmware::product))},
        {u"SYSTEMTYPE",string_value(u"x64-based PC")},{u"NUMBEROFPROCESSORS",number_value(1)},
        {u"NUMBEROFLOGICALPROCESSORS",number_value(memory.processor_count())},
        {u"TOTALPHYSICALMEMORY",string_value(utf8_to_utf16(std::to_string(memory.limit())),21)}};
    else if(name==u"WIN32_PHYSICALMEMORY")row={
        {u"CAPACITY",string_value(utf8_to_utf16(std::to_string(memory.limit())),21)},
        {u"DEVICELOCATOR",string_value(u"Virtual memory")},{u"BANKLABEL",string_value(u"Virtual Bank 0")},
        {u"TAG",string_value(u"Physical Memory 0")},{u"MANUFACTURER",string_value(u"x64arm")},
        {u"CONFIGUREDCLOCKSPEED",number_value(0)},{u"SPEED",number_value(0)},
        {u"MEMORYTYPE",number_value(0,18)},{u"SMBIOSMEMORYTYPE",number_value(2)},
        {u"FORMFACTOR",number_value(0,18)},{u"PARTNUMBER",string_value(u"")}};
    else if(name==u"WIN32_BASEBOARD")row={{u"MANUFACTURER",string_value(utf8_to_utf16(virtual_firmware::manufacturer))},{u"PRODUCT",string_value(utf8_to_utf16(virtual_firmware::board))},{u"VERSION",string_value(utf8_to_utf16(virtual_firmware::version))}};
    else if(name==u"WIN32_BIOS")row={{u"MANUFACTURER",string_value(utf8_to_utf16(virtual_firmware::manufacturer))},{u"SMBIOSBIOSVERSION",string_value(utf8_to_utf16(virtual_firmware::bios))}};
    else if(name==u"WIN32_OPERATINGSYSTEM")row={{u"CAPTION",string_value(u"Windows compatibility environment")},{u"OSARCHITECTURE",string_value(u"64-bit")},
        {u"TOTALVISIBLEMEMORYSIZE",string_value(utf8_to_utf16(std::to_string(memory.limit()/1024)),21)},
        {u"FREEPHYSICALMEMORY",string_value(utf8_to_utf16(std::to_string(memory.remaining_capacity()/1024)),21)}};
    else return std::nullopt;
    row[u"__CLASS"]=string_value(name);return std::vector<WmiRow>{std::move(row)};
}
}
std::span<const WindowsServiceDescriptor> wmi_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"ole32.dll","CoCopyProxy",2},{"ole32.dll","CoSetProxyBlanket",8},{"ole32.dll","CoQueryProxyBlanket",8},
        {"internal-wmi","QueryInterface",3},{"internal-wmi","AddRef",1},{"internal-wmi","Release",1},
        {"internal-wmi","ConnectServer",9},{"internal-wmi","ExecQuery",6},{"internal-wmi","CreateInstanceEnum",5},
        {"internal-wmi","Reset",1},{"internal-wmi","Next",5},{"internal-wmi","Clone",2},{"internal-wmi","Skip",3},
        {"internal-wmi","BeginEnumeration",2},{"internal-wmi","NextProperty",6},{"internal-wmi","EndEnumeration",1},
        {"internal-wmi","Get",6},{"internal-wmi","Unsupported",1}
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_wmi(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(service.symbol=="CoCopyProxy" || service.symbol=="CoSetProxyBlanket" || service.symbol=="CoQueryProxyBlanket"){
        return service_callback::invoke<std::optional<std::uint32_t>>(
            this,threads_.current,state,memory,service.symbol,
            [this](GuestState& s,GuestMemory& m,std::uint64_t target,std::vector<std::uint64_t> args,std::uint64_t id){start_callbacks(s,m,{{target,std::move(args)}},Continuation::NestedResult,id);},
            [this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall call(s,m,service);const auto done=[&](std::uint32_t hr){call.return_to(s,hr);return std::optional<std::uint32_t>{};};
                const bool setting=service.symbol=="CoSetProxyBlanket";const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
                const bool copying=service.symbol=="CoCopyProxy";
                if(!call(0) || (copying && !call(1)))co_return done(0x80070057U);
                try{
                    if(auto found=runtime_.wmi_objects.find(call(0));found!=runtime_.wmi_objects.end()){
                        auto& proxy=found->second;
                        if(copying){
                            m.store(GuestAddress{call(1)},bits,0);
                            const auto original=proxy.original_proxy?proxy.original_proxy:call(0);
                            auto& target=runtime_.wmi_objects.at(original);
                            if(runtime_.wmi_objects.size()>=4096 || target.references==UINT32_MAX)co_return done(0x8007000eU);
                            WmiObject copy;
                            copy.original_proxy=original;
                            copy.kind=target.kind;
                            if(runtime_.com_security){
                                copy.authn_level=(*runtime_.com_security)[0];
                                copy.imp_level=(*runtime_.com_security)[1];
                                copy.security_capabilities=(*runtime_.com_security)[2]&0x60U;
                            }
                            const auto table=runtime_.wmi_vtables.at(target.kind);
                            const auto pointer=allocate_heap(m,unit);
                            if(!pointer)co_return done(0x8007000eU);
                            try{
                                m.store(GuestAddress{pointer},bits,table);
                                runtime_.wmi_objects.emplace(pointer,std::move(copy));
                            }catch(...){(void)free_heap(m,pointer,s);throw;}
                            ++target.references;
                            m.store(GuestAddress{call(1)},bits,pointer);
                        }else if(setting){
                            // 本機 WMI 不建立遠端身分或 principal；絕不接受卻捨棄呼叫端的認證。
                            const auto sentinel=static_cast<std::uint64_t>(x86_?UINT32_MAX:UINT64_MAX);
                            if((call(3) && call(3)!=sentinel) || (call(6) && call(6)!=sentinel))co_return done(0x80004001U);
                            const auto defaults=runtime_.com_security.value_or(std::array<std::uint32_t,3>{3,3,0});
                            auto authn=static_cast<std::uint32_t>(call(1)),authz=static_cast<std::uint32_t>(call(2));
                            auto level=static_cast<std::uint32_t>(call(4)),imp=static_cast<std::uint32_t>(call(5)),cap=static_cast<std::uint32_t>(call(7));
                            if(authn==UINT32_MAX)authn=10;if(authz==UINT32_MAX)authz=0;if(!level || level==UINT32_MAX)level=defaults[0]?defaults[0]:3;if(!imp || imp==UINT32_MAX)imp=defaults[1]?defaults[1]:3;
                            if(cap==0x800U)cap=defaults[2]&0x60U;
                            if(level>6 || imp>4 || (cap&~0x60U) || (cap&0x60U)==0x60U)co_return done(0x80070057U);
                            if((authn!=0 && !ComAuthenticationService::supports(authn)) || authz)co_return done(0x800706d3U);
                            proxy.authn_service=authn;proxy.authz_service=authz;proxy.authn_level=level;proxy.imp_level=imp;proxy.security_capabilities=cap;
                        }else{
                            for(unsigned i=1;i<8;++i)if(call(i))m.check(GuestAddress{call(i)},i==3 || i==6?unit:4,Access::Write);
                            const std::array<std::uint64_t,7> values{proxy.authn_service,proxy.authz_service,0,proxy.authn_level,proxy.imp_level,0,proxy.security_capabilities};
                            for(unsigned i=1;i<8;++i)if(call(i))m.store(GuestAddress{call(i)},i==3 || i==6?bits:32,values[i-1]);
                        }co_return done(0);
                    }
                    // 任意客體 COM 代理遵守官方 QI → Query/SetBlanket → Release 契約，保留提供者的 HRESULT。
                    const auto scratch=allocate_heap(m,unit+16);if(!scratch)co_return done(0x8007000eU);
                    struct Scratch {WindowsServices& self;GuestState& state;GuestMemory& memory;std::uint64_t pointer;~Scratch(){(void)self.free_heap(memory,pointer,state);}} cleanup{*this,s,m,scratch};
                    constexpr WindowsGuid client_security{0x3d,0x01,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
                    m.store(GuestAddress{scratch},bits,0);m.write(GuestAddress{scratch+unit},client_security);
                    const auto method=[&](std::uint64_t proxy_pointer,unsigned slot){const auto table=m.load(GuestAddress{proxy_pointer},bits);return m.load(GuestAddress{table+slot*unit},bits);};
                    std::vector<std::uint64_t> args{0};for(unsigned i=0;i<(copying?2U:8U);++i)args.push_back(call(i));
                    auto hr=static_cast<std::uint32_t>(co_await service_callback::call(method(call(0),0),{call(0),scratch+unit,scratch}));
                    if(hr&0x80000000U)co_return done(hr);const auto security=m.load(GuestAddress{scratch},bits);if(!security)co_return done(0x80004002U);
                    args[0]=security;
                    // 先解析兩個入口，避免 provider 方法返回後才發現無法釋放。
                    const auto target=method(security,copying?5:setting?4:3),release=method(security,2);
                    hr=static_cast<std::uint32_t>(co_await service_callback::call(target,std::move(args)));
                    (void)co_await service_callback::call(release,{security});co_return done(hr);
                }catch(const MemoryFault&){co_return done(0x80004003U);}catch(const HostCallFault&){co_return done(0x80004003U);}catch(const std::bad_alloc&){co_return done(0x8007000eU);}
            });
    }
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;std::uint32_t result=0;
    const std::uint8_t width=x86_?32:64;const unsigned unit=width/8;
    const auto output=[&](std::uint64_t at,std::uint64_t value){memory.store(GuestAddress{at},width,value);};
    const auto bstr=[&](std::u16string_view text)->std::uint64_t {
        const auto bytes=utf16_bytes(text);const auto allocation=allocate_heap(memory,bytes.size()+8);
        if(!allocation)return 0;
        memory.store(GuestAddress{allocation},32,0x52545342);memory.store(GuestAddress{allocation+4},32,bytes.size()-2);
        memory.write(GuestAddress{allocation+8},bytes);return allocation+8;
    };
    const auto create=[&](WmiObject value)->std::uint64_t{
        if(runtime_.wmi_objects.size()>=4096)return 0;
        if(runtime_.com_security){value.authn_level=(*runtime_.com_security)[0];value.imp_level=(*runtime_.com_security)[1];}
        if(!runtime_.wmi_vtables.contains(value.kind)){
            std::map<unsigned,std::string_view> methods{{0,"QueryInterface"},{1,"AddRef"},{2,"Release"}};unsigned count=3;
            if(value.kind==locator){count=4;methods[3]="ConnectServer";}
            if(value.kind==services){count=26;methods[18]="CreateInstanceEnum";methods[20]="ExecQuery";}
            if(value.kind==enumerator){count=8;methods[3]="Reset";methods[4]="Next";methods[6]="Clone";methods[7]="Skip";}
            if(value.kind==object){count=27;methods[4]="Get";methods[8]="BeginEnumeration";methods[9]="NextProperty";methods[10]="EndEnumeration";}
            const auto table=runtime_allocate(memory,{},count*unit);
            for(unsigned i=0;i<count;++i)output(table+i*unit,resolve("internal-wmi",methods.contains(i)?methods.at(i):"Unsupported").value);
            runtime_.wmi_vtables[value.kind]=table;
        }
        const auto pointer=allocate_heap(memory,unit);if(!pointer)return 0;
        output(pointer,runtime_.wmi_vtables.at(value.kind));runtime_.wmi_objects.emplace(pointer,std::move(value));return pointer;
    };
    if(name=="CoCreateInstance"){
        if(!arg(4))result=0x80004003U;
        else {
            output(arg(4),0);
            if(std::ranges::find(locator_classes,guid(memory,arg(0)))==locator_classes.end())result=0x80040154U;
            else if(arg(1))result=0x80040110U;
            else if(!(static_cast<std::uint32_t>(arg(2))&1))result=0x80040154U;
            else if(guid(memory,arg(3))!=interfaces[locator] && guid(memory,arg(3))!="00000000-0000-0000-c000-000000000046")result=0x80004002U;
            else if(!gui_.com_initializations.contains(threads_.current) || gui_.com_initializations.at(threads_.current)<16)result=0x800401f0U;
            else {WmiObject value;value.kind=locator;const auto pointer=create(std::move(value));if(pointer)output(arg(4),pointer);else result=0x8007000eU;}
        }
        arg.return_to(state,result);return std::nullopt;
    }
    const auto found=runtime_.wmi_objects.find(arg(0));
    if(found==runtime_.wmi_objects.end())throw HostCallFault("WMI 介面物件已失效");
    auto& proxy=found->second;
    const auto original=proxy.original_proxy?proxy.original_proxy:arg(0);
    auto& current=runtime_.wmi_objects.at(original);
    const auto release=[&](std::uint64_t pointer){
        auto item=runtime_.wmi_objects.find(pointer);
        const auto remaining=--item->second.references;
        if(!remaining){runtime_.wmi_objects.erase(item);(void)free_heap(memory,pointer,state);}
        return remaining;
    };
    if(name=="QueryInterface"){
        if(!arg(2))result=0x80004003U;
        else {output(arg(2),0);const auto iid=guid(memory,arg(1));
            if(iid!=interfaces[current.kind] && iid!="00000000-0000-0000-c000-000000000046")result=0x80004002U;
            else {if(current.references==UINT32_MAX)throw HostCallFault("WMI 引用計數溢位");++current.references;output(arg(2),original);}}
    } else if(name=="AddRef"){if(proxy.references==UINT32_MAX)throw HostCallFault("WMI 引用計數溢位");result=++proxy.references;}
    else if(name=="Release"){result=release(arg(0));if(!result && original!=arg(0))release(original);}
    else if(name=="ConnectServer" && current.kind==locator){
        if(!arg(8))result=0x80004003U;
        else {output(arg(8),0);auto path=key(read_utf16(memory,GuestAddress{arg(1)},1024));std::replace(path.begin(),path.end(),u'/',u'\\');
            if(path.starts_with(u"\\\\.\\"))path.erase(0,4);
            if(path!=u"ROOT\\CIMV2" && path!=u"ROOT\\WMI")result=0x8004100eU;
            else if(arg(2) || arg(3) || arg(4) || arg(6) || arg(7) || (static_cast<std::uint32_t>(arg(5))&~0x80U))result=0x80041008U;
            else {WmiObject value;value.kind=services;value.name_space=path;const auto pointer=create(std::move(value));if(pointer)output(arg(8),pointer);else result=0x8007000eU;}}
    } else if((name=="ExecQuery" || name=="CreateInstanceEnum") && current.kind==services){
        const bool query=name=="ExecQuery";const auto out=arg(query?5:4);
        if(!out)result=0x80004003U;
        else {output(out,0);if(arg(query?4:3) || (static_cast<std::uint32_t>(arg(query?3:2))&~0x30U))result=0x80041008U;std::u16string class_name;std::vector<std::u16string> columns;
            if(query){
                if(key(read_utf16(memory,GuestAddress{arg(1)},32))!=u"WQL")result=0x80041018U;
                else {
                    auto sql=utf16_to_utf8(key(read_utf16(memory,GuestAddress{arg(2)},8192)));
                    for(auto& c:sql)if(c==',')c=' ';
                    std::istringstream input(sql);std::string word;input>>word;
                    if(word!="SELECT")result=0x80041017U;
                    else {
                        while(input>>word && word!="FROM")columns.push_back(utf8_to_utf16(word));
                        if(word!="FROM" || columns.empty() || !(input>>word))result=0x80041017U;
                        else {class_name=utf8_to_utf16(word);if(input>>word)result=0x80041017U;}
                    }
                }
            } else class_name=key(read_utf16(memory,GuestAddress{arg(1)},256));
            if(!result){
                const auto root_info=filesystem_.information("C:\\");
                const auto cpu=guest_processor_information(memory);
                const auto firmware=virtual_firmware::smbios(memory.processor_count(),installed_physical_memory(memory),root_info.metadata.id^(root_info.metadata.device<<32),cpu);
                std::array<std::uint32_t,5> display{};
                if(class_name==u"WIN32_VIDEOCONTROLLER"){
                    const auto native=native_gui_?native_gui_->current_display_mode():std::optional<GuiDisplayMode>{};const auto& monitors=gui_monitors();
                    if(!monitors.empty()){const auto& monitor=monitors.front();
                        display={monitor.primary && native?native->width:static_cast<std::uint32_t>(monitor.rect[2]-monitor.rect[0]),monitor.primary && native?native->height:static_cast<std::uint32_t>(monitor.rect[3]-monitor.rect[1]),32,monitor.primary && native?(native->refresh_numerator+native->refresh_denominator/2)/std::max(1U,native->refresh_denominator):monitor.refresh,native?native->scanline:0U};}
                    if(gui_.display_mode)std::copy(gui_.display_mode->begin(),gui_.display_mode->end(),display.begin());
                }
                const auto data=rows(class_name,memory,current.name_space,firmware,cpu,display,gpu_memory_stats().limit);
                if(std::getenv("X64ARM_TRACE_WMI"))std::fprintf(stderr,"[wmi] query namespace=%s class=%s rows=%lld\n",utf16_to_utf8(current.name_space).c_str(),utf16_to_utf8(class_name).c_str(),data?static_cast<long long>(data->size()):-1LL);
                if(!data)result=0x80041010U;
                else {WmiObject value;value.kind=enumerator;value.rows=*data;
                    if(query && !(columns.size()==1 && columns[0]==u"*"))for(auto& row:value.rows){
                        for(const auto& column:columns)if(!row.contains(column))result=0x80041017U;
                        std::erase_if(row,[&](const auto& entry){return !entry.first.starts_with(u"__") && std::find(columns.begin(),columns.end(),entry.first)==columns.end();});
                    }
                    if(!result){const auto pointer=create(std::move(value));if(pointer)output(out,pointer);else result=0x8007000eU;}}
            }
        }
    } else if(name=="Reset" && current.kind==enumerator)current.cursor=0;
    else if(name=="Clone" && current.kind==enumerator){if(!arg(1))result=0x80004003U;else {auto copy=current;copy.references=1;const auto pointer=create(std::move(copy));output(arg(1),pointer);if(!pointer)result=0x8007000eU;}}
    else if(name=="Skip" && current.kind==enumerator){const auto count=static_cast<std::uint32_t>(arg(2));const auto available=current.rows.size()-current.cursor;const auto skipped=std::min<std::size_t>(count,available);current.cursor+=skipped;result=skipped==count?0:1;}
    else if(name=="Next" && current.kind==enumerator){
        const auto count=static_cast<std::uint32_t>(arg(2));
        if(!arg(4) || (count && !arg(3)) || count>4096)result=0x80041008U;
        else {memory.check(GuestAddress{arg(3)},count*unit,Access::Write);memory.store(GuestAddress{arg(4)},32,0);std::uint32_t returned=0;
            while(returned<count && current.cursor<current.rows.size()){
                WmiObject value;value.kind=object;value.rows={current.rows[current.cursor]};const auto pointer=create(std::move(value));
                if(!pointer){result=0x8007000eU;break;}output(arg(3)+returned*unit,pointer);++current.cursor;++returned;
            }
            memory.store(GuestAddress{arg(4)},32,returned);if(!result && returned<count)result=1;
        }
    } else if(name=="BeginEnumeration" && current.kind==object) {
        const auto flags=static_cast<std::uint32_t>(arg(1));
        if(flags!=0 && flags!=0x40 && flags!=0x30)result=0x80041008U;
        else {
            current.cursor=0;current.property_names.clear();current.enumerating=true;
            for(const auto& [property,value]:current.rows[0]){
                (void)value;const bool system=property.starts_with(u"__");
                if((flags==0x40 && system) || (flags==0x30 && !system))continue;
                current.property_names.push_back(property);
            }
        }
    } else if(name=="EndEnumeration" && current.kind==object){current.enumerating=false;current.property_names.clear();current.cursor=0;}
    else if((name=="Get" || name=="NextProperty") && current.kind==object){
        const bool next=name=="NextProperty";
        if(arg(3))memory.write(GuestAddress{arg(3)},std::vector<std::uint8_t>(x86_?16:24));
        if(next && arg(2))output(arg(2),0);
        if(next && !current.enumerating)result=0x8004101dU;
        else if(next && static_cast<std::uint32_t>(arg(1)))result=0x80041008U;
        else if(next && current.cursor>=current.property_names.size())result=0x40005;
        else {
            const auto property=next?current.property_names[current.cursor]:key(read_utf16(memory,GuestAddress{arg(1)},256));
            const auto entry=current.rows[0].find(property);
            if(entry==current.rows[0].end())result=0x80041002U;
            else {
                if(arg(4))memory.check(GuestAddress{arg(4)},4,Access::Write);
                if(arg(5))memory.check(GuestAddress{arg(5)},4,Access::Write);
                if(next && arg(2))memory.check(GuestAddress{arg(2)},unit,Access::Write);
                const auto& value=entry->second;auto payload=value.number;std::uint64_t property_string=0;
                if(next && arg(2)){property_string=bstr(property_spelling(property));if(!property_string)result=0x8007000eU;}
                if(!result && arg(3) && value.type==8){payload=bstr(value.text);if(!payload)result=0x8007000eU;}
                if(!result && arg(3) && value.type==0x2011){payload=create_safearray_bytes(state,memory,value.bytes);if(!payload)result=0x8007000eU;}
                if(result){if(property_string)(void)free_heap(memory,property_string-8,state);}
                else {
                    if(arg(4))memory.store(GuestAddress{arg(4)},32,value.cim);
                    if(arg(5))memory.store(GuestAddress{arg(5)},32,property.starts_with(u"__")?0x40:0);
                    if(arg(3)){memory.store(GuestAddress{arg(3)},16,value.type);memory.store(GuestAddress{arg(3)+8},value.type==8 || value.type==0x2011?width:64,payload);}
                    if(next){if(arg(2))output(arg(2),property_string);++current.cursor;}
                }
            }
        }
    } else {if(x86_)throw HostCallFault("尚未支援此 Win32 WMI 方法");result=0x80004001U;}
    arg.return_to(state,result);return std::nullopt;
}
}
