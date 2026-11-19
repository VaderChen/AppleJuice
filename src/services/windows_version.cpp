#include "x64arm/windows_services.hpp"
#include "service_callback_task.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_api_sets.hpp"
#include "builtin_version.hpp"
#include "crt_library.hpp"
#include <algorithm>
#include <array>
#include <set>

namespace x64arm {
std::span<const WindowsServiceDescriptor> version_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"internal-shell-version","SHGetFileDescriptionA",5},{"internal-shell-version","SHGetFileDescriptionW",5},
        {"version.dll","GetFileVersionInfoSizeA",2},{"version.dll","GetFileVersionInfoSizeW",2},
        {"version.dll","GetFileVersionInfoA",4},{"version.dll","GetFileVersionInfoW",4},
        {"version.dll","GetFileVersionInfoSizeExA",3},{"version.dll","GetFileVersionInfoSizeExW",3},
        {"version.dll","GetFileVersionInfoExA",5},{"version.dll","GetFileVersionInfoExW",5},
        {"version.dll","VerQueryValueA",4},{"version.dll","VerQueryValueW",4},
        // 安裝位置判斷與安裝（暫存複本經 LZ 解壓、版本／語系／字碼頁／類型比對、取代與刪除舊檔）。
        {"version.dll","VerFindFileA",8,0,0,0x1},{"version.dll","VerFindFileW",8,0,0,0x1},
        {"version.dll","VerInstallFileA",8,0,0,0x1},{"version.dll","VerInstallFileW",8,0,0,0x1}
    };return table;
}
namespace {
struct VersionFailure {std::uint32_t error;};
std::uint32_t number(std::span<const std::uint8_t> bytes,std::size_t at,unsigned width){
    if(at>bytes.size() || width>bytes.size()-at)throw VersionFailure{13};
    std::uint32_t result=0;for(unsigned i=0;i<width;++i)result|=std::uint32_t{bytes[at+i]}<<(8*i);return result;
}
void put(std::vector<std::uint8_t>& bytes,std::size_t at,std::uint32_t value){for(unsigned i=0;i<4;++i)bytes.at(at+i)=static_cast<std::uint8_t>(value>>(i*8));}
std::size_t align4(std::size_t value){return (value+3)&~std::size_t{3};}
std::uint64_t address(std::uint64_t base,std::size_t offset){if(offset>UINT64_MAX-base)throw VersionFailure{13};return base+offset;}
bool equal(std::u16string_view a,std::u16string_view b){return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](char16_t x,char16_t y){return unicode_case_unit(x,false)==unicode_case_unit(y,false);});}
struct PeReader {
    const HostFile& file;std::uint64_t size;std::uint32_t headers{},resource_rva{},resource_size{};
    struct Section {std::uint32_t rva,virtual_size,offset,size;};std::vector<Section> sections;
    std::vector<std::uint8_t> read(std::uint64_t offset,std::size_t length)const {
        if(offset>size || length>size-offset || length>1048576)throw VersionFailure{13};
        std::vector<std::uint8_t> bytes(length);std::size_t at=0;
        while(at<length){const auto result=file.read(std::span(bytes).subspan(at),offset+at);if(result.error)throw VersionFailure{result.error};if(!result.bytes)throw VersionFailure{13};at+=static_cast<std::size_t>(result.bytes);}return bytes;
    }
    std::uint64_t offset(std::uint32_t rva,std::uint32_t length)const {
        if(static_cast<std::uint64_t>(rva)+length>UINT64_C(0x100000000))throw VersionFailure{13};
        std::optional<std::uint64_t> result;
        if(rva<headers && length<=headers-rva)result=rva;
        for(const auto& section:sections)if(rva>=section.rva && rva-section.rva<std::max(section.virtual_size,section.size)){
            const auto relative=rva-section.rva;if(relative>section.size || length>section.size-relative)throw VersionFailure{13};
            if(result)throw VersionFailure{13};result=static_cast<std::uint64_t>(section.offset)+relative;
        }
        if(!result || *result>size || length>size-*result)throw VersionFailure{13};return *result;
    }
    explicit PeReader(const HostFile& input,std::uint64_t length):file(input),size(length){
        if(size<64)throw VersionFailure{193};const auto dos=read(0,64);if(number(dos,0,2)!=0x5a4d)throw VersionFailure{193};
        const auto pe=number(dos,60,4);if(pe<64 || pe>size || size-pe<24)throw VersionFailure{193};
        const auto coff=read(pe,24);if(number(coff,0,4)!=0x4550)throw VersionFailure{193};
        const auto count=number(coff,6,2),optional_size=number(coff,20,2);if(!count || count>96 || optional_size<96)throw VersionFailure{193};
        const auto optional=read(static_cast<std::uint64_t>(pe)+24,optional_size);const auto magic=number(optional,0,2);
        const unsigned directories=magic==0x10b?96U:magic==0x20b?112U:0U;if(!directories || optional_size<directories)throw VersionFailure{193};
        headers=number(optional,60,4);if(headers>size)throw VersionFailure{193};
        if(number(optional,directories-4,4)<=2 || optional_size<directories+24)throw VersionFailure{1812};
        resource_rva=number(optional,directories+16,4);resource_size=number(optional,directories+20,4);
        if(!resource_rva || resource_size<16)throw VersionFailure{1812};
        if(static_cast<std::uint64_t>(resource_rva)+resource_size>UINT64_C(0x100000000))throw VersionFailure{13};
        const auto table=read(static_cast<std::uint64_t>(pe)+24+optional_size,count*40U);
        for(std::size_t i=0;i<count;++i){const auto at=i*40;Section s{number(table,at+12,4),number(table,at+8,4),number(table,at+20,4),number(table,at+16,4)};
            if(s.offset>size || s.size>size-s.offset || static_cast<std::uint64_t>(s.rva)+std::max(s.size,s.virtual_size)>UINT64_C(0x100000000))throw VersionFailure{13};sections.push_back(s);}
    }
    std::vector<std::uint8_t> resource(std::uint32_t at,std::uint32_t length)const {
        if(at>resource_size || length>resource_size-at)throw VersionFailure{13};return read(offset(resource_rva+at,length),length);
    }
    std::vector<std::pair<std::uint32_t,std::uint32_t>> directory(std::uint32_t at)const {
        const auto header=resource(at,16);const auto count=number(header,12,2)+number(header,14,2);if(count>4096)throw VersionFailure{13};
        if(at>UINT32_MAX-16)throw VersionFailure{13};const auto table=resource(at+16,count*8);
        std::vector<std::pair<std::uint32_t,std::uint32_t>> result;
        for(std::size_t i=0;i<count;++i){const auto key=number(table,i*8,4),value=number(table,i*8+4,4);if(key&0x80000000U){const auto name=key&0x7fffffffU;const auto length=number(resource(name,2),0,2);if(name>UINT32_MAX-2)throw VersionFailure{13};(void)resource(name+2,length*2);}result.emplace_back(key,value);}return result;
    }
    std::vector<std::uint8_t> version()const {
        std::uint32_t at=0;std::set<std::uint32_t> seen;
        for(const auto key:{16U,1U}){
            if(!seen.insert(at).second)throw VersionFailure{13};const auto entries=directory(at);
            const auto found=std::find_if(entries.begin(),entries.end(),[&](const auto& entry){return entry.first==key;});if(found==entries.end())throw VersionFailure{1812};
            if(!(found->second&0x80000000U))throw VersionFailure{13};at=found->second&0x7fffffffU;
        }
        if(!seen.insert(at).second)throw VersionFailure{13};const auto languages=directory(at);if(languages.empty())throw VersionFailure{1812};
        auto selected=languages.end();for(const auto preferred:{0U,0x409U}){selected=std::find_if(languages.begin(),languages.end(),[&](const auto& entry){return entry.first==preferred;});if(selected!=languages.end())break;}
        if(selected==languages.end())selected=std::find_if(languages.begin(),languages.end(),[](const auto& entry){return !(entry.first&0x80000000U);});
        if(selected==languages.end() || (selected->second&0x80000000U))throw VersionFailure{13};
        const auto data=resource(selected->second,16);const auto rva=number(data,0,4),length=number(data,4,4);if(length<6)throw VersionFailure{13};
        const auto raw=offset(rva,length);const auto prefix=read(raw,6);const auto count=number(prefix,0,2);if(count<6 || count>length)throw VersionFailure{13};return read(raw,count);
    }
};
struct VersionNode {
    std::size_t begin{},end{},value{},length{},key_end{};bool text{};
    std::u16string key;std::vector<VersionNode> children;
};
VersionNode parse_node(std::span<const std::uint8_t> bytes,std::size_t at,std::size_t limit,unsigned depth,std::size_t& count){
    if(depth>16 || ++count>4096 || at>limit || limit-at<6)throw VersionFailure{13};
    const auto length=number(bytes,at,2),value_length=number(bytes,at+2,2),type=number(bytes,at+4,2);
    if(length<6 || length>limit-at || type>1)throw VersionFailure{13};
    VersionNode node;node.begin=at;node.end=at+length;node.length=value_length;node.text=type==1;at+=6;
    for(;;){if(at>node.end || node.end-at<2)throw VersionFailure{13};const auto c=number(bytes,at,2);at+=2;if(!c)break;node.key+=static_cast<char16_t>(c);}
    node.key_end=at-2;node.value=align4(at);const auto value_bytes=std::size_t{value_length}*(node.text?2U:1U);
    if(value_bytes && (node.value>node.end || value_bytes>node.end-node.value))throw VersionFailure{13};
    if(node.text && value_length && number(bytes,node.value+value_bytes-2,2)!=0)throw VersionFailure{13};
    at=align4(node.value+value_bytes);
    while(at<node.end){if(node.end-at<6){for(auto i=at;i<node.end;++i)if(bytes[i])throw VersionFailure{13};break;}auto child=parse_node(bytes,at,node.end,depth+1,count);at=align4(child.end);node.children.push_back(std::move(child));}
    return node;
}
VersionNode parse_version(std::span<const std::uint8_t> bytes){
    std::size_t count=0;auto root=parse_node(bytes,0,bytes.size(),0,count);
    if(root.key!=u"VS_VERSION_INFO" || root.text || (root.length && (root.length!=52 || number(bytes,root.value,4)!=0xfeef04bd)))throw VersionFailure{13};
    for(const auto& child:root.children){
        if(equal(child.key,u"VarFileInfo"))for(const auto& value:child.children)if(equal(value.key,u"Translation") && (value.text || value.length%4))throw VersionFailure{13};
    }
    return root;
}
constexpr std::uint32_t trailer_magic=0x56523658U;
std::size_t trailer(std::size_t length){return align4(length);}
std::size_t buffer_size(std::size_t length){return trailer(length)+16+length*2;}
const VersionNode* query(const VersionNode& root,std::u16string_view path){
    auto* current=&root;
    while(!path.empty()){while(!path.empty() && path.front()==u'\\')path.remove_prefix(1);if(path.empty())break;const auto end=path.find(u'\\');const auto part=path.substr(0,end);
        const auto found=std::find_if(current->children.begin(),current->children.end(),[&](const auto& node){return equal(node.key,part);});if(found==current->children.end())return nullptr;current=&*found;if(end==path.npos)break;path.remove_prefix(end);}
    return current;
}
}
std::optional<std::uint32_t> WindowsServices::invoke_version_install(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    // VerFindFile(uFlags, szFileName, szWinDir, szAppDir, szCurDir, puCurDirLen, szDestDir, puDestDirLen)
    // VerInstallFile(uFlags, szSrcFileName, szDestFileName, szSrcDir, szDestDir, szCurDir, szTmpFile, puTmpFileLen)
    // 檔案操作、LZ 解壓與版本資訊沿用既有 kernel32／version 服務（LZ 會回呼客體，以協程續行）；A 版字串依本專案 ACP（UTF-8）轉換。
    return nested_continuation(state,memory,"version 安裝",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
    const WindowsCall call(s,m,service);const bool wide=service.symbol.ends_with('W');const bool install=service.symbol.starts_with("VerInstallFile");
    const auto finish=[&](std::uint32_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
    std::vector<std::uint64_t> blocks;
    struct Release {WindowsServices& self;GuestMemory& memory;GuestState& state;std::vector<std::uint64_t>& blocks;~Release(){for(const auto block:blocks)(void)self.free_heap(memory,block,state);}} release{*this,m,s,blocks};
    const auto scratch=[&](std::size_t size)->std::uint64_t{const auto block=allocate_heap(m,std::max<std::size_t>(size,2));if(!block)throw std::bad_alloc{};m.fill(GuestAddress{block},0,std::max<std::size_t>(size,2));blocks.push_back(block);return block;};
    const auto api=[&](std::string_view dll,std::string_view symbol,std::vector<std::uint64_t> arguments)->service_callback::Task<std::uint64_t>{
        co_return co_await service_callback::call_value(resolve(dll,symbol).value,std::move(arguments));};
    const auto text=[&](std::uint64_t pointer){return !pointer?std::u16string{}:wide?read_utf16(m,GuestAddress{pointer},32768):utf8_to_utf16_lossy(read_ansi(m,pointer,32768));};
    const auto guest=[&](std::u16string_view value){const auto bytes=utf16_bytes(value);const auto block=scratch(bytes.size());m.write(GuestAddress{block},bytes);return block;};
    const auto join=[](std::u16string directory,std::u16string_view name){if(!directory.empty() && directory.back()!=u'\\' && directory.back()!=u'/')directory.push_back(u'\\');return directory+std::u16string(name);};
    const auto fold=[](std::u16string value){for(auto& c:value)c=unicode_case_unit(c,false);while(!value.empty() && (value.back()==u'\\' || value.back()==u'/'))value.pop_back();return value;};
    const auto last_error=[&]{return static_cast<std::uint32_t>(m.load(GuestAddress{(x86_?s.fs_base:s.gs_base)+(x86_?0x34U:0x68U)},32));};
    const auto exists=[&](const std::u16string& path)->service_callback::Task<bool>{co_return static_cast<std::uint32_t>(co_await api("kernel32.dll","GetFileAttributesW",{guest(path)}))!=UINT32_MAX;};
    const auto directory_of=[&](std::string_view symbol)->service_callback::Task<std::u16string>{
        const auto buffer=scratch(1040);const auto length=co_await api("kernel32.dll",symbol,{buffer,520});co_return length && length<520?read_utf16(m,GuestAddress{buffer},520):std::u16string{};};
    // 寫入呼叫端緩衝區：*length 為字元數（含 NUL）；空間不足時回報需要的長度，回傳 false。
    const auto output=[&](std::uint64_t buffer,std::uint64_t length,std::u16string_view value)->bool{
        m.check(GuestAddress{length},4,Access::Write);const auto capacity=static_cast<std::uint32_t>(m.load(GuestAddress{length},32));
        std::vector<std::uint8_t> bytes;if(wide)bytes=utf16_bytes(value);else {const auto str=utf16_to_utf8(value);bytes.assign(str.begin(),str.end());bytes.push_back(0);}
        const auto needed=static_cast<std::uint32_t>(bytes.size()/(wide?2U:1U));m.store(GuestAddress{length},32,needed);
        if(!buffer || capacity<needed){if(buffer && capacity)m.store(GuestAddress{buffer},wide?16:8,0);return false;}
        m.write(GuestAddress{buffer},bytes);return true;
    };
    // 版本摘要：FileVersion（MS<<32|LS）、dwFileType 與第一組 Translation（語系、字碼頁）；沒有版本資源時為空。
    struct Summary {std::uint64_t version{};std::uint32_t type{},language{},codepage{};};
    const auto summary=[&](const std::u16string& path)->service_callback::Task<std::optional<Summary>>{
        const auto name=guest(path);const auto size=static_cast<std::uint32_t>(co_await api("version.dll","GetFileVersionInfoSizeW",{name,0}));if(!size)co_return std::nullopt;
        const auto data=scratch(size);if(!co_await api("version.dll","GetFileVersionInfoW",{name,0,size,data}))co_return std::nullopt;
        const auto pointer=scratch(8),length=scratch(4);Summary out;
        if(co_await api("version.dll","VerQueryValueW",{data,guest(u"\\"),pointer,length})){const auto fixed=m.load(GuestAddress{pointer},x86_?32:64);
            out.version=(std::uint64_t{static_cast<std::uint32_t>(m.load(GuestAddress{fixed+8},32))}<<32)|static_cast<std::uint32_t>(m.load(GuestAddress{fixed+12},32));
            out.type=static_cast<std::uint32_t>(m.load(GuestAddress{fixed+36},32));}
        if(co_await api("version.dll","VerQueryValueW",{data,guest(u"\\VarFileInfo\\Translation"),pointer,length}) && m.load(GuestAddress{length},32)>=4){
            const auto translation=m.load(GuestAddress{pointer},x86_?32:64);out.language=static_cast<std::uint32_t>(m.load(GuestAddress{translation},16));out.codepage=static_cast<std::uint32_t>(m.load(GuestAddress{translation+2},16));}
        co_return out;
    };
    const auto in_use=[&](const std::u16string& path)->service_callback::Task<bool>{
        const auto handle=co_await api("kernel32.dll","CreateFileW",{guest(path),0x80000000U,0,0,3,0x80,0});
        if(handle==(x86_?UINT32_MAX:UINT64_MAX))co_return last_error()==32U;(void)co_await api("kernel32.dll","CloseHandle",{handle});co_return false;
    };
    std::uint32_t result=0;
    try {
        const auto flags=static_cast<std::uint32_t>(call(0));
        if(!install){
            const auto name=text(call(1));const auto windows=call(2)?text(call(2)):co_await directory_of("GetWindowsDirectoryW");const auto application=text(call(3));
            const auto system=call(2)?join(windows,u"System32"):co_await directory_of("GetSystemDirectoryW");
            const bool shared=flags&1U;const auto destination=shared?system:application;
            // 目前版本的位置：應用程式目錄、系統目錄、Windows 目錄，再依 PATH；只取目錄，不含檔名。
            std::vector<std::u16string> places;if(!application.empty())places.push_back(application);places.push_back(system);places.push_back(windows);
            {const auto buffer=scratch(65536);const auto length=co_await api("kernel32.dll","GetEnvironmentVariableW",{guest(u"PATH"),buffer,32768});
                if(length && length<32768){const auto path=read_utf16(m,GuestAddress{buffer},32768);for(std::size_t at=0;at<=path.size();){const auto end=std::min(path.find(u';',at),path.size());if(end>at)places.push_back(path.substr(at,end-at));at=end+1;}}}
            std::u16string current;
            if(!name.empty())for(const auto& place:places)if(co_await exists(join(place,name))){current=place;while(current.size()>3 && (current.back()==u'\\' || current.back()==u'/'))current.pop_back();break;}
            if(!current.empty() && fold(current)!=fold(destination))result|=1U;           // VFF_CURNEDEST
            if(!current.empty() && co_await in_use(join(current,name)))result|=2U;         // VFF_FILEINUSE
            if(!output(call(4),call(5),current))result|=4U;                                // VFF_BUFFTOOSMALL
            if(!output(call(6),call(7),destination))result|=4U;
        }else {
            const auto source_name=text(call(1)),destination_name=text(call(2)),source_directory=text(call(3)),destination_directory=text(call(4)),current_directory=text(call(5));
            const auto source=join(source_directory,source_name),destination=join(destination_directory,destination_name);
            const auto current=current_directory.empty()?destination:join(current_directory,destination_name);
            m.check(GuestAddress{call(7)},4,Access::Write);
            auto temporary_name=call(6)?text(call(6)):std::u16string{};std::u16string temporary;
            if(!temporary_name.empty())temporary=join(destination_directory,temporary_name); // 上一輪留下的暫存複本
            else {
                if(!co_await exists(source))co_return finish(0x10000U);                    // VIF_CANNOTREADSRC
                const auto buffer=scratch(1040);
                if(!co_await api("kernel32.dll","GetTempFileNameW",{guest(destination_directory),guest(u"VER"),0,buffer}))co_return finish(0x800U); // VIF_CANNOTCREATE
                temporary=read_utf16(m,GuestAddress{buffer},520);
                // 經 LZ 函式複製：壓縮來源會解壓，一般檔案原樣複製。
                const auto open=scratch(136);
                const auto from=static_cast<std::int32_t>(co_await api("kernel32.dll","LZOpenFileW",{guest(source),open,0}));
                if(from<0){(void)co_await api("kernel32.dll","DeleteFileW",{guest(temporary)});co_return finish(0x10000U);}
                const auto to=static_cast<std::int32_t>(co_await api("kernel32.dll","LZOpenFileW",{guest(temporary),open,0x1000}));
                if(to<0){(void)co_await api("kernel32.dll","LZClose",{static_cast<std::uint32_t>(from)});(void)co_await api("kernel32.dll","DeleteFileW",{guest(temporary)});co_return finish(0x800U);}
                const auto copied=static_cast<std::int32_t>(co_await api("kernel32.dll","LZCopy",{static_cast<std::uint32_t>(from),static_cast<std::uint32_t>(to)}));
                (void)co_await api("kernel32.dll","LZClose",{static_cast<std::uint32_t>(from)});(void)co_await api("kernel32.dll","LZClose",{static_cast<std::uint32_t>(to)});
                if(copied<0){(void)co_await api("kernel32.dll","DeleteFileW",{guest(temporary)});co_return finish(copied==-4?0x100U:0x10000U);} // LZERROR_WRITE→VIF_OUTOFSPACE
                temporary_name=temporary.substr(temporary.find_last_of(u'\\')+1);
            }
            if(!output(call(6),call(7),temporary_name))result|=0x40000U;                    // VIF_BUFFTOOSMALL
            // 版本比對：現有檔案較新、語系／字碼頁／檔案類型不同時保留暫存複本，由呼叫端決定是否強制安裝。
            if(!(flags&1U) && co_await exists(current)){
                const auto incoming=co_await summary(temporary);const auto installed=co_await summary(current);
                if(incoming && installed){
                    if(installed->version>incoming->version)result|=0x4U;                       // VIF_SRCOLD
                    if(installed->language!=incoming->language)result|=0x8U;                    // VIF_DIFFLANG
                    if(installed->codepage!=incoming->codepage)result|=0x10U;                   // VIF_DIFFCODEPG
                    if(installed->type!=incoming->type)result|=0x20U;                           // VIF_DIFFTYPE
                }
                if(result&0x3cU)co_return finish(result|0x2U|0x1U);                          // VIF_MISMATCH | VIF_TEMPFILE
            }
            if(co_await exists(destination) && !co_await api("kernel32.dll","DeleteFileW",{guest(destination)})){
                const auto error=last_error();co_return finish(result|0x1U|(error==32U?0x80U:error==5U?0x200U:0x1000U));
            }
            if(!co_await api("kernel32.dll","MoveFileW",{guest(temporary),guest(destination)}))co_return finish(result|0x1U|0x2000U); // VIF_CANNOTRENAME
            if(fold(current)!=fold(destination) && !(flags&2U) && co_await exists(current) && !co_await api("kernel32.dll","DeleteFileW",{guest(current)}))result|=0x4000U; // VIF_CANNOTDELETECUR
        }
    }catch(const MemoryFault&){result=install?0x200U:0U;}
    catch(const std::bad_alloc&){result=install?0x8000U:0U;}
    co_return finish(result);
    });
}
std::optional<std::uint32_t> WindowsServices::invoke_version(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(service.symbol.starts_with("VerFindFile") || service.symbol.starts_with("VerInstallFile"))return invoke_version_install(state,memory,service);
    const WindowsCall call(state,memory,service);const bool wide=service.symbol.ends_with('W');std::uint64_t result=0;
    // *Ex 版本多一個前置 dwFlags（LOCALISED 1、NEUTRAL 2、PREFETCHED 4）；版本資源只有一份語系無關的原始資料，其餘參數與舊版相同。
    const bool extended=service.symbol.starts_with("GetFileVersionInfo") && service.symbol.find("Ex")!=std::string_view::npos;
    const auto arg=[&](unsigned index){return call(index+(extended?1U:0U));};
    if(extended && (call(0)&~UINT64_C(7))){last_error_=87;call.return_to(state,0);return std::nullopt;}
    const auto text=[&](std::uint64_t pointer){return wide?read_utf16(memory,GuestAddress{pointer}):utf8_to_utf16_lossy(read_ansi(memory,pointer));};
    const auto read_version=[&](const std::u16string& requested){
        std::vector<std::u16string> candidates;
        if(requested.find_first_of(u"\\/:")==requested.npos){if(!runtime_.application_directory.empty())candidates.push_back(runtime_.application_directory+requested);if(!runtime_.dll_directory.empty())candidates.push_back(runtime_.dll_directory+requested);candidates.push_back(u"C:\\Windows\\System32\\"+requested);if(!runtime_.exclude_current_dll_directory)candidates.push_back(requested);}else candidates.push_back(requested);
        auto key=utf16_to_utf8(requested);std::replace(key.begin(),key.end(),'/','\\');
        for(auto& c:key)if(c>='A' && c<='Z')c=static_cast<char>(c+32);
        auto name=key.substr(key.find_last_of('\\')+1);
        if(name.find('.')==name.npos){name+=".dll";key+=".dll";}
        std::vector<std::uint8_t> bytes;
        // 與 LoadLibrary 共用 provider 命名空間；私有路徑的同名檔案仍讀其實際 PE。
        if((key.find('\\')==key.npos || key=="c:\\windows\\system32\\"+name) && builtin_module(name)){
            const auto host=windows_api_set_host(name);if(host!=name)name=host;
            if(name.starts_with("api-ms-win-crt-"))name="ucrtbase.dll";
            if(name!="kernel32.dll" && name!="wsock32.dll" && !crt_library(name) &&
               !std::any_of(runtime_service_descriptors().begin(),runtime_service_descriptors().end(),[&](const auto& item){return item.family==name;}))throw VersionFailure{126};
            bytes=builtin_version_info(name);
        }else {
            OpenHostFile opened;opened.error=2;
            for(const auto& candidate:candidates){opened=filesystem_.open_reader(utf16_to_utf8(candidate),runtime_.application_source,utf16_to_utf8(runtime_.application_directory));if(!opened.error)break;if(opened.error!=2 && opened.error!=3)break;}
            if(opened.error)throw VersionFailure{opened.error};bytes=PeReader(*opened.file,opened.metadata.size).version();
        }
        return bytes;
    };
    try {
        if(service.family=="internal-shell-version"){
            if(!arg(0) || !arg(4))throw VersionFailure{87};
            const auto path=text(arg(0));
            if(path.empty())throw VersionFailure{161};
            std::u16string description;
            GuestScratch scratch(*this,memory,state);
            try{
                const auto bytes=read_version(path);
                const auto root=parse_version(bytes);
                const auto try_key=[&](std::u16string_view key){
                    const auto* node=query(root,key);
                    if(node && node->text && node->length)
                        for(std::size_t i=0;i<node->length;++i){
                            const auto c=static_cast<char16_t>(number(bytes,node->value+2*i,2));
                            if(!c)break;
                            description.push_back(c);
                        }
                };
                if(arg(1))try_key(text(arg(1)));
                const auto* translations=query(root,u"\\VarFileInfo\\Translation");
                if(description.empty() && translations && translations->length>=4){
                    constexpr char16_t digits[]=u"0123456789ABCDEF";
                    std::u16string key=u"\\StringFileInfo\\";
                    for(const auto offset:{0U,2U}){
                        const auto id=number(bytes,translations->value+offset,2);
                        for(const auto shift:{12,8,4,0})key.push_back(digits[(id>>shift)&15]);
                    }
                    try_key(key+u"\\FileDescription");
                }
                for(const auto id:{u"040904B0",u"040904E4",u"04090000"})
                    if(description.empty())try_key(u"\\StringFileInfo\\"+std::u16string(id)+u"\\FileDescription");
            }catch(const VersionFailure&){
                const auto filename=scratch.text(path);
                if(!filename)throw std::bad_alloc();
                if(static_cast<std::uint32_t>(nested_service(state,memory,"kernel32.dll","GetFileAttributesW",{filename}))==UINT32_MAX)
                    throw VersionFailure{last_error_};
            }
            if(description.empty()){
                description=path.substr(path.find_last_of(u"\\/")+1);
                const auto extension=description.rfind(u'.');
                if(extension!=description.npos)description.resize(extension);
            }
            const auto trim=[&](std::u16string_view suffix){
                if(suffix.empty() || suffix.size()>description.size())return false;
                const auto pos=description.size()-suffix.size();
                if(!equal(std::u16string_view(description).substr(pos),suffix))return false;
                description.resize(pos);
                while(!description.empty() && description.back()==u' ')description.pop_back();
                return true;
            };
            const auto subkey=scratch.text(u"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileAssociation");
            const auto cut=scratch.text(u"CutList"),size=scratch.allocate(4);
            if(!subkey || !cut || !size)throw std::bad_alloc();
            const auto get_list=[&](std::uint64_t dest){
                const auto hklm=x86_?0x80000002ULL:0xffffffff80000002ULL;
                return nested_service(state,memory,"advapi32.dll","RegGetValueW",{hklm,subkey,cut,0x22,0,dest,size});
            };
            memory.store(GuestAddress{size},32,0);
            bool use_argument=true;
            if(!get_list(0)){
                const auto capacity=memory.load(GuestAddress{size},32);
                if(capacity>=2 && capacity<=output_limit){
                    const auto data=scratch.allocate(capacity);
                    if(!data)throw std::bad_alloc();
                    if(!get_list(data)){
                        use_argument=false;
                        const auto units=memory.load(GuestAddress{size},32)/2;
                        for(std::uint64_t at=0;at<units;){
                            const auto item=read_utf16(memory,GuestAddress{data+2*at},units-at);
                            if(item.empty() || trim(item))break;
                            at+=item.size()+1;
                        }
                    }
                }
            }
            if(use_argument && arg(2)){
                auto cursor=arg(2);
                for(;;){
                    const auto suffix=text(cursor);
                    if(suffix.empty() || trim(suffix) || !wide)break;
                    cursor+=(suffix.size()+1)*2;
                }
            }
            auto bytes=wide?utf16_bytes(description):std::vector<std::uint8_t>{};
            if(!wide){const auto ansi=utf16_to_utf8(description);bytes.assign(ansi.begin(),ansi.end());bytes.push_back(0);}
            const unsigned unit=wide?2U:1U;
            auto count=bytes.size()/unit;
            if(arg(3)){
                count=std::min(count,static_cast<std::size_t>(memory.load(GuestAddress{arg(4)},32)));
                if(count){
                    if(!wide)while(count>1 && (bytes[count-1]&0xc0U)==0x80U)--count;
                    bytes.resize(count*unit);
                    std::fill(bytes.end()-unit,bytes.end(),0);
                    memory.write(GuestAddress{arg(3)},bytes);
                }
            }
            memory.store(GuestAddress{arg(4)},32,count);
            result=1;
        }else if(service.symbol.starts_with("VerQueryValue")){
            if(!arg(0) || !arg(2))throw VersionFailure{87};memory.check(GuestAddress{arg(2)},x86_?4U:8U,Access::Write);if(arg(3))memory.check(GuestAddress{arg(3)},4,Access::Write);
            const auto length=memory.load(GuestAddress{arg(0)},16);if(length<6)throw VersionFailure{13};
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));memory.read(GuestAddress{arg(0)},bytes);const auto root=parse_version(bytes);
            const auto path=arg(1)?text(arg(1)):std::u16string{};const auto* node=query(root,path);if(!node)throw VersionFailure{1813};
            auto pointer=address(arg(0),node->length?node->value:node->key_end);auto count=node->length;
            if(!wide && node->text){
                // 原始版本區有效，不代表其後的 ANSI 工作區亦可在 64-bit 位址範圍內表示。
                (void)address(arg(0),buffer_size(bytes.size()));
                const auto extra=address(arg(0),trailer(bytes.size()));memory.check(GuestAddress{extra},16,Access::Read);
                if(memory.load(GuestAddress{extra},32)!=trailer_magic || memory.load(GuestAddress{address(extra,4)},32)!=bytes.size() || memory.load(GuestAddress{address(extra,8)},32)!=buffer_size(bytes.size()))throw VersionFailure{13};
                std::u16string value;for(std::size_t i=0;i<node->length;++i)value+=static_cast<char16_t>(number(bytes,node->value+i*2,2));
                const auto encoded=utf16_to_utf8(value);pointer=address(extra,16+node->value*2);count=encoded.size();
                if(count>(bytes.size()-std::min(node->value,bytes.size()))*2)throw VersionFailure{13};
                if(encoded.empty()){pointer=address(extra,16+node->key_end*2);memory.store(GuestAddress{pointer},8,0);}else memory.write(GuestAddress{pointer},std::span(reinterpret_cast<const std::uint8_t*>(encoded.data()),encoded.size()));
            }
            memory.store(GuestAddress{arg(2)},x86_?32:64,pointer);if(arg(3))memory.store(GuestAddress{arg(3)},32,count);result=1;
        }else {
            const bool size_only=service.symbol.starts_with("GetFileVersionInfoSize");
            if(size_only && arg(1))memory.store(GuestAddress{arg(1)},32,0);
            if(!arg(0))throw VersionFailure{87};const auto requested=text(arg(0));if(requested.empty())throw VersionFailure{161};
            if(!size_only && !arg(3))throw VersionFailure{13};
            auto bytes=read_version(requested);
            (void)parse_version(bytes);
            const auto required=buffer_size(bytes.size());
            if(size_only)result=required;
            else {
                const auto capacity=static_cast<std::uint32_t>(arg(2)),raw_size=static_cast<std::uint32_t>(bytes.size());bytes.resize(required);
                // 截短輸出仍可讀已完整的 Unicode 原始區，但不可宣稱 ANSI scratch 已配置。
                put(bytes,trailer(raw_size),capacity>=required?trailer_magic:0);put(bytes,trailer(raw_size)+4,raw_size);put(bytes,trailer(raw_size)+8,static_cast<std::uint32_t>(required));
                memory.write(GuestAddress{arg(3)},std::span(bytes).first(std::min<std::size_t>(capacity,bytes.size())));result=1;
            }
            last_error_=0;
        }
    }catch(const VersionFailure& failure){last_error_=failure.error;if(service.symbol.starts_with("VerQueryValue") && arg(3))memory.store(GuestAddress{arg(3)},32,0);}
    catch(const std::invalid_argument&){last_error_=1113;if(service.symbol.starts_with("VerQueryValue") && arg(3))memory.store(GuestAddress{arg(3)},32,0);}
    catch(const std::bad_alloc&){last_error_=8;}
    call.return_to(state,result);return std::nullopt;
}
}
