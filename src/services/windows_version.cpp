#include "x64arm/windows_services.hpp"
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
        {"version.dll","GetFileVersionInfoSizeA",2},{"version.dll","GetFileVersionInfoSizeW",2},
        {"version.dll","GetFileVersionInfoA",4},{"version.dll","GetFileVersionInfoW",4},
        {"version.dll","VerQueryValueA",4},{"version.dll","VerQueryValueW",4}
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
std::optional<std::uint32_t> WindowsServices::invoke_version(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const bool wide=service.symbol.ends_with('W');std::uint64_t result=0;
    const auto text=[&](std::uint64_t pointer){return wide?read_utf16(memory,GuestAddress{pointer}):utf8_to_utf16_lossy(read_ansi(memory,pointer));};
    try {
        if(service.symbol.starts_with("VerQueryValue")){
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
    arg.return_to(state,result);return std::nullopt;
}
}
