#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "virtual_firmware.hpp"
#include <algorithm>
#include <sstream>
#include <cstdio>
#include <cstdlib>

namespace x64arm {
namespace {
constexpr unsigned locator=0,services=1,enumerator=2,object=3;
constexpr std::array<std::string_view,4> interfaces{
    "dc12a687-737f-11cf-884d-00aa004b2e24","9556dc99-828c-11cf-a37e-00aa003240c7",
    "027947e1-d731-11ce-a357-000000000001","dc12a681-737f-11cf-884d-00aa004b2e24"};
std::string guid(const GuestMemory& memory,std::uint64_t address){
    std::string result;constexpr char hex[]="0123456789abcdef";
    for(const unsigned offset:std::array<unsigned,16>{3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15}){
        if(result.size()==8 || result.size()==13 || result.size()==18 || result.size()==23)result+='-';
        const auto byte=memory.load(GuestAddress{address+offset},8);result+=hex[byte>>4];result+=hex[byte&15];
    }
    return result;
}
std::u16string key(std::u16string text){for(auto& c:text)if(c>=u'a' && c<=u'z')c-=32;return text;}
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
        return result;
    }();
    const auto found=names.find(property);return found==names.end()?property:found->second;
}
WmiValue string_value(std::u16string value,std::uint32_t cim=8){return {8,cim,0,std::move(value)};}
WmiValue number_value(std::uint32_t value,std::uint32_t cim=19){return {3,cim,value,{}};}
std::optional<std::vector<WmiRow>> rows(std::u16string name,const GuestMemory& memory,const std::u16string& name_space,std::span<const std::uint8_t> firmware,const ProcessorInformation& cpu){
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
        {"ole32.dll","CoSetProxyBlanket",8},
        {"internal-wmi","QueryInterface",3},{"internal-wmi","AddRef",1},{"internal-wmi","Release",1},
        {"internal-wmi","ConnectServer",9},{"internal-wmi","ExecQuery",6},{"internal-wmi","CreateInstanceEnum",5},
        {"internal-wmi","Reset",1},{"internal-wmi","Next",5},{"internal-wmi","Clone",2},{"internal-wmi","Skip",3},
        {"internal-wmi","BeginEnumeration",2},{"internal-wmi","NextProperty",6},{"internal-wmi","EndEnumeration",1},
        {"internal-wmi","Get",6},{"internal-wmi","Unsupported",1}
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_wmi(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
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
            if(guid(memory,arg(0))!="4590f811-1d3a-11d0-891f-00aa004b2e24")result=0x80040154U;
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
    auto& current=found->second;
    if(name=="QueryInterface"){
        if(!arg(2))result=0x80004003U;
        else {output(arg(2),0);const auto iid=guid(memory,arg(1));
            if(iid!=interfaces[current.kind] && iid!="00000000-0000-0000-c000-000000000046")result=0x80004002U;
            else {if(current.references==UINT32_MAX)throw HostCallFault("WMI 引用計數溢位");++current.references;output(arg(2),arg(0));}}
    } else if(name=="AddRef"){if(current.references==UINT32_MAX)throw HostCallFault("WMI 引用計數溢位");result=++current.references;}
    else if(name=="Release"){result=--current.references;if(!result){runtime_.wmi_objects.erase(found);(void)free_heap(memory,arg(0),state);}}
    else if(name=="CoSetProxyBlanket"){
        // 所有物件均在此程序內；不接受遠端認證資料或自訂 principal。
        if(arg(3) || arg(6) || arg(7))result=0x80004001U;
        else if(static_cast<std::uint32_t>(arg(4))>6 || static_cast<std::uint32_t>(arg(5))>4)result=0x80070057U;
    } else if(name=="ConnectServer" && current.kind==locator){
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
                const auto data=rows(class_name,memory,current.name_space,firmware,cpu);
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
