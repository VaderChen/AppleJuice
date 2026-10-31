#include "x64arm/startup_profile.hpp"
#include "x64arm/pe_loader.hpp"
#include "windows_loader_lock.hpp"
#ifdef X64ARM_HOST_SERVICES_TRANSLATION_IMAGE
#include "x64arm/smap.hpp"
#endif
#include "crt_library.hpp"
#include "pe_signature.hpp"
#include "builtin_version.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_paths.hpp"
#include "x64arm/windows_unwind.hpp"
#include "x64arm/windows_api_sets.hpp"
#include "x64arm/windows_sdk_modules.hpp"
#include "x64arm/retained_capacity.hpp"
#include <algorithm>
#include <charconv>
#include <sstream>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace x64arm {
namespace {
struct ModuleError : std::runtime_error {
    std::uint32_t code;
    ModuleError(std::uint32_t value,std::string message):std::runtime_error(std::move(message)),code(value){}
};
std::string module_key(std::u16string_view path) {
    auto value=utf16_to_utf8(path);std::replace(value.begin(),value.end(),'/','\\');
    for(auto& c:value)if(c>='A' && c<='Z')c=static_cast<char>(c+32);
    return value;
}
std::string basename(std::string_view path) {return std::string(path.substr(path.find_last_of("\\/")+1));}
bool diagnostic_system_import(std::string_view dll) {
    const auto key=module_key(utf8_to_utf16(dll));
    // 限裸 provider 名稱；路徑相依、應用程式 DLL 與 LoadLibrary 仍依實際檔案載入。
    return windows_sdk_import_module(key);
}
std::u16string directory(std::u16string_view path) {
    const auto at=path.find_last_of(u"\\/");return at==path.npos?std::u16string{}:std::u16string(path.substr(0,at+1));
}
std::string read_string(const GuestMemory& memory,std::uint64_t at) {
    std::string value;for(unsigned n=0;n<32768;++n){const auto c=memory.load(GuestAddress{at+n},8);if(!c)return value;value+=static_cast<char>(c);}
    throw HostCallFault("模組名稱缺少終止字元");
}
std::u16string read_name(const GuestMemory& memory,std::uint64_t at,bool wide) {return wide?read_utf16(memory,GuestAddress{at}):utf8_to_utf16(read_string(memory,at));}
struct BuiltinImage {
    std::vector<std::uint8_t> bytes;
    std::size_t code{};
    std::map<std::string,std::uint32_t> names;
    std::map<std::uint32_t,std::uint32_t> ordinals;
};
BuiltinImage builtin_headers(std::uint64_t base,std::string_view name,bool x86,const std::map<std::string,std::uint64_t>& targets,const std::map<std::uint64_t,std::uint32_t>& system_calls) {
    // PE 的 RVA 僅 32 位元；模組內的實際唯讀跳躍入口轉接至既有 API gateway，
    // 保留所有參數／暫存器與回傳框架。只有真正可呼叫的服務才列為匯出。
    std::map<std::uint32_t,std::uint64_t> ordinals;
    std::map<std::uint64_t,std::uint32_t> assigned;
    for(const auto& [symbol,target]:targets)if(symbol.starts_with('#')){
        unsigned value{};const auto parsed=std::from_chars(symbol.data()+1,symbol.data()+symbol.size(),value);
        if(parsed.ec!=std::errc{} || parsed.ptr!=symbol.data()+symbol.size() || !value || value>65535)throw ModuleError(87,"內建 DLL 匯出序號不合法");
        ordinals.emplace(value,target);assigned.try_emplace(target,value);
    }
    std::map<std::string,std::uint32_t> names;unsigned next=1;
    for(const auto& [symbol,target]:targets)if(!symbol.starts_with('#')){
        auto found=assigned.find(target);
        if(found==assigned.end()){
            while(ordinals.contains(next))++next;if(next>65535)throw ModuleError(8,"內建 DLL 匯出過多");
            ordinals.emplace(next,target);found=assigned.emplace(target,next++).first;
        }
        names.emplace(symbol,found->second);
    }
    const std::size_t count=ordinals.empty()?0:ordinals.rbegin()->first;
    constexpr std::size_t nt=0x80,optional=nt+24,edata=4096;
    const auto functions=edata+40,names_at=functions+count*4,indices=names_at+names.size()*4,dll_name=indices+names.size()*2;
    auto strings_end=dll_name+name.size()+1;for(const auto& [symbol,ordinal]:names){(void)ordinal;strings_end+=symbol.size()+1;}
    const auto resource_at=(strings_end+4095)&~std::size_t{4095};
    const auto resource=builtin_version_resource(name,static_cast<std::uint32_t>(resource_at));
    const auto code=(resource_at+resource.size()+4095)&~std::size_t{4095};
    const auto size=(code+assigned.size()*16+4095)&~std::size_t{4095};
    BuiltinImage result;result.code=code;result.bytes.resize(size);auto& bytes=result.bytes;
    const auto put=[&](std::size_t offset,unsigned width,std::uint64_t value){for(unsigned i=0;i<width;++i)bytes.at(offset+i)=static_cast<std::uint8_t>(value>>(i*8));};
    put(0,2,0x5a4d);put(0x3c,4,nt);put(nt,4,0x4550);put(nt+4,2,x86?0x14c:0x8664);put(nt+6,2,assigned.empty()?2:3);
    put(nt+20,2,x86?224:240);put(nt+22,2,x86?0x2102:0x2022);
    put(optional,2,x86?0x10b:0x20b);put(optional+4,4,size-code);put(optional+8,4,code-4096);put(optional+20,4,code);
    put(optional+(x86?28:24),x86?4:8,base);put(optional+32,4,4096);put(optional+36,4,512);
    put(optional+40,2,6);put(optional+42,2,1);put(optional+48,2,6);put(optional+50,2,1);
    put(optional+56,4,size);put(optional+60,4,4096);put(optional+68,2,3);put(optional+70,2,0x100);
    put(optional+(x86?92:108),4,16);
    if(count){put(optional+(x86?96:112),4,edata);put(optional+(x86?100:116),4,strings_end-edata);}
    put(optional+(x86?112:128),4,resource_at);put(optional+(x86?116:132),4,resource.size());
    const auto section=optional+(x86?224U:240U);
    const auto section_header=[&](std::size_t at,std::string_view label,std::size_t rva,std::size_t length,std::uint32_t flags){
        std::copy(label.begin(),label.end(),bytes.begin()+static_cast<std::ptrdiff_t>(at));put(at+8,4,length);put(at+12,4,rva);put(at+16,4,length);put(at+20,4,rva);put(at+36,4,flags);
    };
    section_header(section,".edata",4096,resource_at-4096,0x40000040);
    section_header(section+40,".rsrc",resource_at,code-resource_at,0x40000040);
    if(!assigned.empty())section_header(section+80,".text",code,size-code,0x60000020);
    std::copy(resource.begin(),resource.end(),bytes.begin()+static_cast<std::ptrdiff_t>(resource_at));
    std::copy(name.begin(),name.end(),bytes.begin()+static_cast<std::ptrdiff_t>(dll_name));
    put(edata+12,4,dll_name);put(edata+16,4,1);put(edata+20,4,count);put(edata+24,4,names.size());
    put(edata+28,4,functions);put(edata+32,4,names_at);put(edata+36,4,indices);
    std::map<std::uint64_t,std::uint32_t> entries;auto at=code;
    for(const auto& [target,ordinal]:assigned){
        (void)ordinal;entries.emplace(target,static_cast<std::uint32_t>(at));
        if(!x86 && name=="ntdll.dll" && system_calls.contains(target)){
            // 可複製、可攔截的標準 x64 系統服務入口；複本仍經 CPU SYSCALL 派送。
            const std::array<std::uint8_t,16> stub{0x4c,0x8b,0xd1,0xb8,0,0,0,0,0x0f,0x05,0xc3,0x66,0x66,0x90,0x66,0x90};
            std::copy(stub.begin(),stub.end(),bytes.begin()+static_cast<std::ptrdiff_t>(at));put(at+4,4,system_calls.at(target));
        }else{bytes[at]=0xff;bytes[at+1]=0x25;if(x86){put(at+2,4,base+at+6);put(at+6,4,target);}else {put(at+2,4,0);put(at+6,8,target);}}
        at+=16;
    }
    for(const auto& [ordinal,target]:ordinals){const auto rva=entries.at(target);put(functions+(ordinal-1)*4,4,rva);result.ordinals.emplace(ordinal,rva);}
    at=dll_name+name.size()+1;std::size_t index=0;
    for(const auto& [symbol,ordinal]:names){
        put(names_at+index*4,4,at);put(indices+index*2,2,ordinal-1);std::copy(symbol.begin(),symbol.end(),bytes.begin()+static_cast<std::ptrdiff_t>(at));at+=symbol.size()+1;++index;
        result.names.emplace(symbol,ordinal);
    }
    return result;
}
}
bool WindowsServices::builtin_module(std::string_view name) const {
    if(angle_module(name))return native_angle_!=nullptr;
    if(native_library_module(name))return native_libraries_.enabled(name);
    if(const auto host=windows_api_set_host(name);host!=name)return builtin_module(host);
    if(crt_library(name))return true;
    if(name=="kernel32.dll" || name=="wsock32.dll" || name.starts_with("api-ms-win-core-") || name.starts_with("api-ms-win-appmodel-runtime-"))return true;
    static const auto families=[] {
        std::set<std::string_view> result;
        for(const auto& service:runtime_service_descriptors())result.emplace(service.family);
        return result;
    }();
    return families.contains(name);
}
const RuntimeImage* WindowsServices::module_image(std::uint64_t address,bool containing) const {
    const auto matches=[&](const RuntimeImage& image){return image.base && (containing?address>=image.base && address-image.base<image.size:address==image.base);};
    if(matches(runtime_.image))return &runtime_.image;
    if(matches(runtime_.cxx_exceptions.image))return &runtime_.cxx_exceptions.image;
    if(matches(guest_crt_algorithms_))return &guest_crt_algorithms_;
    for(const auto& [base,image]:runtime_.builtin_images){(void)base;if(matches(image))return &image;}
    for(const auto& [base,module]:runtime_.loaded_modules){(void)base;if(matches(module.image))return &module.image;}
    return nullptr;
}
const RuntimeImage* WindowsServices::resource_image(std::uint64_t handle,bool containing) const {
    if(const auto* image=module_image(handle,containing))return image;
    if(!containing){const auto found=runtime_.resource_modules.find(handle);return found==runtime_.resource_modules.end()?nullptr:&found->second.image;}
    for(const auto& [token,module]:runtime_.resource_modules){(void)token;if(handle>=module.image.base && handle-module.image.base<module.image.size)return &module.image;}
    return nullptr;
}
#ifdef X64ARM_HOST_SERVICES_TRANSLATION_REGION
std::optional<TranslationRegion> WindowsServices::translation_region(GuestAddress address) const {
    // 使用載入器已驗證的靜態函式表；不在編譯查詢中執行動態 unwind callback。
    const auto* image=module_image(address.value,true);if(!image)return {};
    const auto* function=lookup_function(*image,address.value);
    if(!function || function->begin>=function->end || function->end>image->size ||
       image->base>UINT64_MAX-function->end)return {};
    return TranslationRegion{GuestAddress{image->base+function->begin},GuestAddress{image->base+function->end}};
}
#endif
#ifdef X64ARM_HOST_SERVICES_TRANSLATION_IMAGE
std::optional<TranslationImage> WindowsServices::translation_image(GuestAddress address) const {
    // 主程式不在 loaded_modules；以其載入範圍比對。
    if(const auto& main=runtime_.main_translation;main && address.value>=main->begin.value && address.value-main->begin.value<main->size)
        return main;
    auto next=runtime_.loaded_modules.upper_bound(address.value);
    if(next==runtime_.loaded_modules.begin())return {};
    const auto& module=std::prev(next)->second;
    if(address.value-module.image.base>=module.image.size)return {};
    return module.translation;
}
#endif
void WindowsServices::configure_loader(const std::filesystem::path& source,bool trap,const std::filesystem::path& source_directory) {
    runtime_.trap_unresolved_imports=trap;
    if(!source.empty())runtime_.application_source=std::filesystem::canonical(source).parent_path();
    else if(!source_directory.empty())runtime_.application_source=std::filesystem::canonical(source_directory);
    runtime_.application_directory=directory(runtime_.image.path);
    if(runtime_.application_directory.empty())runtime_.application_directory=u"C:\\";
}
std::uint64_t WindowsServices::find_module(std::u16string_view name) const {
    auto key=module_key(name);
    if(key.empty())return runtime_.image.base;
    const auto main=module_key(runtime_.image.path);
    if(key==main || (key.find('\\')==key.npos && key==basename(main)))return runtime_.image.base;
    if(basename(key).find('.')==std::string::npos)key+=".dll";
    if(const auto found=runtime_.modules.find(key);found!=runtime_.modules.end())return found->second;
    // 內建 DLL 以 provider 名稱登錄；完整 System32 路徑仍是同一映像。
    // 不把任意外部目錄的同名 DLL 視為系統模組。
    for(const auto& [base,image]:runtime_.builtin_images)if(key==module_key(image.path))return base;
    return 0;
}
void WindowsServices::initialize_module_tls(GuestMemory& memory,RuntimeModule& module) {
    if(!module.image.tls_index)return;
    auto slot=std::find(runtime_.static_tls_slots.begin()+1,runtime_.static_tls_slots.end(),false);
    if(slot==runtime_.static_tls_slots.end())throw ModuleError(8,"static TLS 模組超過 64 個");
    module.tls_slot=static_cast<std::uint32_t>(slot-runtime_.static_tls_slots.begin());*slot=true;
    memory.store(GuestAddress{module.image.tls_index},32,module.tls_slot);
    for(const auto& [id,thread]:threads_.items) {
        (void)id;if(thread.status==ThreadStatus::Exited)continue;
        const auto vector=memory.load(GuestAddress{thread.teb+(x86_?0x2c:0x58)},x86_?32:64);
        const auto tls=runtime_allocate(memory,module.image.tls_data,std::max<std::size_t>(1,module.image.tls_data.size()+module.image.tls_zero),module.image.tls_alignment);
        memory.store(GuestAddress{vector+module.tls_slot*(x86_?4U:8U)},x86_?32:64,tls);
    }
}
void WindowsServices::initialize_thread_module_tls(GuestMemory& memory,std::uint64_t vector) {
    for(const auto& [base,module]:runtime_.loaded_modules) {
        (void)base;if(module.tls_slot==UINT32_MAX)continue;
        const auto tls=runtime_allocate(memory,module.image.tls_data,std::max<std::size_t>(1,module.image.tls_data.size()+module.image.tls_zero),module.image.tls_alignment);
        memory.store(GuestAddress{vector+module.tls_slot*(x86_?4U:8U)},x86_?32:64,tls);
    }
}
#include "windows_module_file.inc"
std::uint64_t WindowsServices::module_export(GuestMemory& memory,std::uint64_t handle,std::string_view symbol,std::optional<std::uint16_t> ordinal,std::vector<std::uint64_t>& created,unsigned depth,GuestState* state) {
    if(depth>=32)throw ModuleError(127,"DLL export forwarder 循環或超過深度");
    const auto* image=module_image(handle);
    if(!image || runtime_.builtin_images.contains(handle)) {
        const auto found=std::find_if(runtime_.modules.begin(),runtime_.modules.end(),[&](const auto& item){return item.second==handle;});
        if(found==runtime_.modules.end())throw ModuleError(127,"沒有此模組的匯出");
        const auto name=ordinal?"#"+std::to_string(*ordinal):std::string(symbol);
        if(image){
            std::uint32_t number=ordinal.value_or(0);bool native=false;
            if(!ordinal){const auto exported=image->export_names.find(std::string(symbol));if(exported!=image->export_names.end()){number=exported->second;native=symbol.starts_with("Nt") || symbol.starts_with("Zw");}}
            else for(const auto& [exported,id]:image->export_names)if(id==number && (exported.starts_with("Nt") || exported.starts_with("Zw"))){native=true;break;}
            // 動態解析與 PE 匯出必須指向同一入口，讓程序內的攔截修改確實生效。
            if(native){const auto exported=image->export_ordinals.find(number);if(exported!=image->export_ordinals.end())return handle+exported->second;}
        }
        // 名稱／既有固定序號維持同一 canonical gateway；匯出表配置的其餘
        // 序號亦依模組內實際跳躍入口解析，不把已發布的 EAT 序號當缺少 API。
        try{return resolve(found->first,name).value;}catch(const std::invalid_argument&){
            if(image && ordinal){const auto exported=image->export_ordinals.find(*ordinal);if(exported!=image->export_ordinals.end()){const auto at=handle+exported->second;if(!x86_ && memory.load(GuestAddress{at},32)==0xb8d18b4cU)return stub_base_.value+(memory.load(GuestAddress{at+4},32)-1)*16;return memory.load(GuestAddress{at+6},x86_?32:64);}}
            throw ModuleError(127,"未支援的 PE import："+found->first+"!"+name);
        }
    }
    std::uint32_t number=ordinal.value_or(0);
    if(!ordinal) {const auto found=image->export_names.find(std::string(symbol));if(found==image->export_names.end())throw ModuleError(127,"找不到名稱匯出："+std::string(symbol));number=found->second;}
    if(const auto forward=image->export_forwarders.find(number);forward!=image->export_forwarders.end()) {
        const auto value=forward->second;const auto dot=value.find_last_of('.');
        if(dot==value.npos)throw ModuleError(193,"DLL forwarder 格式不合法");
        auto name=value.substr(0,dot);if(!name.ends_with(".dll"))name+=".dll";
        const auto target=load_module(memory,utf8_to_utf16(name),0,created,{},state);
        if(handle==runtime_.image.base)runtime_.main_dependencies.insert(target);else runtime_.loaded_modules.at(handle).dependencies.insert(target);
        auto exported=value.substr(dot+1);std::optional<std::uint16_t> index;
        if(exported.starts_with('#')) {
            unsigned parsed=0;const auto first=exported.data()+1,last=exported.data()+exported.size();const auto conversion=std::from_chars(first,last,parsed);
            if(conversion.ec!=std::errc{} || conversion.ptr!=last || parsed>65535)throw ModuleError(193,"forwarder ordinal 不合法");index=static_cast<std::uint16_t>(parsed);
        }
        return module_export(memory,target,exported,index,created,depth+1,state);
    }
    const auto found=image->export_ordinals.find(number);if(found==image->export_ordinals.end())throw ModuleError(127,"找不到序號匯出");
    return image->base+found->second;
}
void WindowsServices::notify_dll(GuestState& state,GuestMemory& memory,const RuntimeImage& image,std::uint32_t reason) {
    if(runtime_.dll_notifications.empty())return;
    // 資料僅在回呼期間借用。載入時尚未綁定 IAT；卸載時 DllMain 已完成，但映像仍可讀。
    const auto full=image.path;
    const auto name=full.substr(full.find_last_of(u"\\/")+1);
    if(full.size()>(UINT16_MAX-2U)/2U || name.size()>(UINT16_MAX-2U)/2U)
        throw ModuleError(206,"DLL 通知路徑超過 UNICODE_STRING 長度");
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto header=unit*5U,strings=header+unit*4U;
    std::vector<std::uint8_t> bytes(strings+(full.size()+name.size()+2)*2);
    const auto at=allocate_heap(memory,bytes.size());if(!at)throw std::bad_alloc{};
    const auto saved_error=last_error_;
    struct Cleanup {WindowsServices& self;GuestMemory& memory;const GuestState& state;std::uint64_t at;std::uint32_t error;
        ~Cleanup(){(void)self.free_heap(memory,at,state);self.last_error_=error;}} cleanup{*this,memory,state,at,saved_error};
    memory.write(GuestAddress{at},bytes);
    const auto store=[&](std::uint64_t offset,std::uint8_t width,std::uint64_t value){memory.store(GuestAddress{at+offset},width,value);};
    const auto unicode=[&](std::uint64_t descriptor,std::uint64_t buffer,std::u16string_view text){
        store(descriptor,16,text.size()*2);store(descriptor+2,16,(text.size()+1)*2);store(descriptor+unit,bits,at+buffer);
        for(std::size_t n=0;n<text.size();++n)store(buffer+n*2,16,text[n]);
    };
    store(unit,bits,at+header);store(unit*2,bits,at+header+unit*2);
    store(unit*3,bits,image.base);store(unit*4,32,image.size);
    unicode(header,strings,full);unicode(header+unit*2,strings+(full.size()+1)*2,name);
    // 回呼內取消註冊可立即生效；新註冊從下一次事件開始，不擴張本次派送。
    const auto last=runtime_.dll_notifications.rbegin()->first;
    std::uint64_t previous=0;
    for(;;){
        const auto found=runtime_.dll_notifications.upper_bound(previous);
        if(found==runtime_.dll_notifications.end() || found->first>last)break;
        previous=found->first;const auto registration=found->second;
        (void)call_guest_nested(state,memory,registration.function,{reason,at,registration.context});
    }
}
std::uint64_t WindowsServices::load_module(GuestMemory& memory,std::u16string requested,std::uint32_t flags,std::vector<std::uint64_t>& created,std::u16string_view search_directory,GuestState* state) {
    if(requested.empty())throw ModuleError(87,"DLL 名稱為空");
    // 明確的 LOAD_LIBRARY_SEARCH 旗標覆蓋程序預設值，相依 DLL 沿用同一搜尋政策。
    if(!(flags&0x1f08))flags|=runtime_.default_dll_directories;
    if(flags&0x1000)flags=(flags&~0x1000U)|0xe00;
    std::replace(requested.begin(),requested.end(),u'/',u'\\');
    auto key=module_key(requested);
    // LoadLibrary 對無副檔名補 .dll；SCM 的驅動 ImagePath 是確切檔名，不補副檔名。
    if(basename(key).find('.')==std::string::npos && !kernel_image_load_){requested+=u".dll";key+=".dll";}
    if(auto handle=find_module(requested))return handle;
    startup_profile::Scope measurement("module",key,true);
    const auto name=basename(key);const bool bare=key.find('\\')==key.npos;
    // VC 的可再散發 DLL 支援 app-local 部署；以整個客體模組維持 C++ ABI，
    // 不將同一個 DLL 的物件、資料匯出與函式拆給兩個不同 provider。
    const bool redistributable=name=="msvcp140.dll" || name=="msvcp140_1.dll" || name=="vcruntime140.dll" || name=="vcruntime140_1.dll";
    if(bare && redistributable && (!(flags&0xf00) || (flags&0x200))) {
        const auto local=runtime_.application_directory+requested;
        auto source=read_module_file(state,memory,local);
        if(!source.error){source.bytes.reset();return load_module(memory,local,flags,created,search_directory,state);}
        // 未提供客體搜尋根時仍可使用內建 CRT；真正的 ACL／traversal 拒絕不可降級。
        const bool unavailable=source.error==5 && !source.access_denied;
        if(source.error!=2 && source.error!=3 && !unavailable)throw ModuleError(source.error,"無法讀取 app-local 執行階段："+name);
    }
    if((bare || key=="c:\\windows\\system32\\"+name || (angle_module(name) && native_angle_)) && builtin_module(name)) {
        if(runtime_.signature_policy && native_library_module(name))throw ModuleError(577,"原生第三方模組不符合程序簽章政策");
        if(const auto host=windows_api_set_host(name);host!=name){const auto base=load_module(memory,utf8_to_utf16(host),flags,created,{},state);runtime_.modules.emplace(name,base);return base;}
        if(name.starts_with("api-ms-win-crt-")){const auto base=load_module(memory,u"ucrtbase.dll",flags,created,{},state);runtime_.modules.emplace(name,base);return base;}
        if(runtime_.builtin_images.size()>=windows_address_layout::module_slots)throw ModuleError(8,"內建模組數量超過固定容量");
        // 每個引擎版本共用固定模組配置，與各程序載入順序無關。匯出表放不進一格的模組占用連續多格：
        // 依描述表估算 builtin_headers 的大小（4 KiB 標頭＋匯出目錄每項 10 bytes 與名稱字串、頁對齊，再加每個入口 16 bytes），
        // 另留一頁給描述表以外的別名，避免臨界時超出。
        static const auto bases=[] {std::set<std::string> names{"kernel32.dll","wsock32.dll"};for(const auto name:modern_crt_libraries)names.emplace(name);for(const auto name:legacy_crt_libraries)names.emplace(name);for(const auto& item:runtime_service_descriptors())if(item.family.ends_with(".dll") || item.family.ends_with(".drv") || item.family.ends_with(".ocx") || item.family.ends_with(".cpl") || item.family.ends_with(".exe"))names.emplace(item.family);
            std::map<std::string,std::pair<std::size_t,std::size_t>,std::less<>> exports;
            for(const auto& item:runtime_service_descriptors()){auto& entry=exports[std::string(item.family)];++entry.first;entry.second+=item.symbol.size()+1;}
            std::map<std::string,std::pair<std::size_t,std::size_t>> result;std::size_t next=0;
            for(const auto& item:names){
                std::size_t span=1;
                if(const auto found=exports.find(item);found!=exports.end()){
                    const auto [count,strings]=found->second;
                    const auto bytes=((4096+40+count*10+item.size()+1+strings+4095)&~std::size_t{4095})+((count*16+4095)&~std::size_t{4095})+4096;
                    span=std::max<std::size_t>(1,(bytes+windows_address_layout::module_stride-1)/windows_address_layout::module_stride);
                }
                result.emplace(item,std::pair{next,span});next+=span;
            }
            return result;}();
        const auto slot=bases.find(name);if(slot==bases.end() || slot->second.first+slot->second.second>windows_address_layout::module_slots)throw ModuleError(8,"內建模組沒有固定配置："+name);
        const auto base=windows_address_layout::for_mode(memory.execution_mode()).images+slot->second.first*windows_address_layout::module_stride;
        const auto capacity=slot->second.second*windows_address_layout::module_stride;
        const auto targets=builtin_export_targets(name);
        const auto system_calls=!x86_ && name=="ntdll.dll"?builtin_system_calls():std::map<std::uint64_t,std::uint32_t>{};
        auto headers=builtin_headers(base,name,x86_,targets,system_calls);
        if(headers.bytes.size()>capacity || !memory.range_available(GuestAddress{base},headers.bytes.size()))throw ModuleError(8,"內建模組固定範圍已占用");
        RuntimeImage image;image.base=base;image.size=headers.bytes.size();image.path=u"C:\\Windows\\System32\\"+utf8_to_utf16(name);
        image.export_names=std::move(headers.names);image.export_ordinals=std::move(headers.ordinals);
        memory.map(GuestAddress{base},headers.code,permission(Access::Read),std::span(headers.bytes).first(headers.code));
        bool code_mapped=false;
        try {
            if(headers.bytes.size()>headers.code){memory.map(GuestAddress{base+headers.code},headers.bytes.size()-headers.code,read_execute,std::span(headers.bytes).subspan(headers.code));code_mapped=true;}
            runtime_.builtin_images.emplace(base,std::move(image));runtime_.modules.emplace(name,base);
        }catch(...){if(code_mapped)memory.unmap(GuestAddress{base+headers.code});memory.unmap(GuestAddress{base});runtime_.builtin_images.erase(base);throw;}
        if(state)notify_dll(*state,memory,runtime_.builtin_images.at(base),1);
        return base;
    }
    const bool resource_only=(flags&0x20)!=0;
    if(runtime_.loaded_modules.size()+runtime_.resource_modules.size()>=256 || created.size()>=256)throw ModuleError(8,"DLL 數量超過 256");
    std::vector<std::u16string> candidates;
    if(!bare)candidates.push_back(requested);
    else {
        const bool standard=(flags&0xf00)==0;
        if(!search_directory.empty() && (standard || (flags&0x100)))candidates.emplace_back(std::u16string(search_directory)+requested);
        if(standard || (flags&0x200))candidates.emplace_back(runtime_.application_directory+requested);
        if((standard || (flags&0x400)) && !runtime_.dll_directory.empty())candidates.emplace_back(runtime_.dll_directory+requested);
        if(standard && !runtime_.native_dll_path.empty()){
            std::u16string_view paths=runtime_.native_dll_path;
            while(!paths.empty()){
                const auto end=paths.find(u';');auto directory=paths.substr(0,end);
                if(!directory.empty()){std::u16string candidate(directory);if(candidate.back()!=u'\\' && candidate.back()!=u'/')candidate.push_back(u'\\');candidates.emplace_back(candidate+requested);}
                if(end==paths.npos)break;paths.remove_prefix(end+1);
            }
        }
        if(flags&0x400)for(const auto& [cookie,path]:runtime_.added_dll_directories){(void)cookie;candidates.emplace_back(path+requested);}
        if(standard || (flags&0x800))candidates.emplace_back(u"C:\\Windows\\System32\\"+requested);
        if(standard && !runtime_.exclude_current_dll_directory)candidates.push_back(requested);
    }
    FileSnapshot source;source.error=126;
    bool access_denied=false;
    for(const auto& candidate:candidates) {
        source=read_module_file(state,memory,candidate);
        if(!source.error)break;
        access_denied|=source.access_denied;
        if(source.error!=2 && source.error!=3 && source.error!=5)throw ModuleError(source.error,"無法讀取 DLL");
    }
    if(source.error)throw ModuleError(access_denied?5U:126U,(access_denied?"DLL 存取遭拒：":"找不到 DLL：")+key);
    std::u16string guest=utf8_to_utf16(source.key);std::replace(guest.begin(),guest.end(),u'/',u'\\');
    if(auto handle=find_module(guest))return handle;
    if(!resource_only && runtime_.signature_policy){
        const auto signature=authenticate_microsoft_pe(*source.bytes);const auto publisher=signature.publisher;const auto policy=runtime_.signature_policy;
        const bool allowed=publisher!=PePublisher::Untrusted && (!(policy&1U) || publisher==PePublisher::Microsoft) && (!(policy&2U) || publisher==PePublisher::Store);
        if(!allowed)throw ModuleError(577,"DLL 不符合程序簽章政策："+signature.reason);
    }
    ParsedPeImage parsed;
    try {
        const auto select_base=[&](GuestAddress preferred,std::uint32_t size,PeArchitecture architecture){
            if(!resource_only && (architecture==PeArchitecture::X86)!=x86_)throw ModuleError(193,"DLL 與程序架構不一致");
            if(memory.range_available(preferred,size))return preferred;
            auto selected=runtime_.next_module_base;
            while(!memory.range_available(GuestAddress{selected},size)){
                if(selected>std::min<std::uint64_t>(UINT64_C(0x600000000000),user_address_limit())-size)throw ModuleError(8,"沒有 DLL 映射位址");
                const auto mapping=memory.next_mapping(GuestAddress{selected});
                selected=mapping?((mapping->base.value+mapping->size+65535)&~UINT64_C(65535)):selected+65536;
            }
            return GuestAddress{selected};
        };
        parsed=parse_pe_image(*source.bytes,{},true,memory.limit(),resource_only,select_base,kernel_image_load_);
    }catch(const std::invalid_argument& error){throw ModuleError(193,error.what());}
    const auto base=parsed.metadata.loaded_base.value;
    RuntimeModule module;module.image=parsed.runtime;module.image.path=guest;module.name=name;module.loading=true;
#ifdef X64ARM_HOST_SERVICES_TRANSLATION_IMAGE
    if(!resource_only && !source.source.empty())module.translation=TranslationImage{
        source.source,image_identity(source.source,*source.bytes),parsed.metadata.loaded_base,
        parsed.metadata.preferred_base,parsed.metadata.image_size};
#endif
    // 檔案原始位元組只用到解析與身分核對；相依 DLL 載入前歸還本次讀取額度。
    source.bytes.reset();
    if(resource_only) {
        map_pe_image(memory,parsed);
        for(const auto& region:parsed.mappings)module.mappings.emplace_back(base+region.rva,region.bytes.size());
        const auto handle=base|UINT64_C(2);module.loading=false;
        try {runtime_.resource_modules.emplace(handle,std::move(module));created.push_back(handle);}
        catch(...) {for(const auto& region:parsed.mappings)memory.unmap(GuestAddress{base+region.rva});runtime_.resource_modules.erase(handle);throw;}
        runtime_.next_module_base=std::max(runtime_.next_module_base,(base+parsed.metadata.image_size+65535)&~UINT64_C(65535));
        runtime_.module_events.push_back("resource_load="+utf16_to_utf8(guest));return handle;
    }
    try {
        map_pe_image(memory,parsed);
        install_guard(memory,parsed.runtime);
        for(const auto& region:parsed.mappings)module.mappings.emplace_back(base+region.rva,region.bytes.size());
        runtime_.loaded_modules.emplace(base,std::move(module));created.push_back(base);
        runtime_.modules.emplace(name,base);runtime_.modules.emplace(module_key(guest),base);
        // 診斷：X64ARM_TRACE_MODULES=1 時印出載入模組的名稱、基址與映像大小，供剖析把客體 RIP 歸到模組。
        static const bool trace_modules=[] {const auto* value=std::getenv("X64ARM_TRACE_MODULES");return value && std::string_view(value)=="1";}();
        if(trace_modules)std::fprintf(stderr,"module_loaded pid=%d name=%s base=0x%llx size=0x%llx\n",static_cast<int>(process_id()),name.c_str(),
            static_cast<unsigned long long>(base),static_cast<unsigned long long>(parsed.metadata.image_size));
        auto& loaded=runtime_.loaded_modules.at(base);
        initialize_module_tls(memory,loaded);
        loaded.notification_ready=true;
        if(state)notify_dll(*state,memory,loaded.image,1);
        const auto altered=(flags&0x108)?directory(guest):std::u16string(search_directory);
        bind_pe_imports(memory,parsed,[&](const PeImport& imported) {
            const auto count=created.size();std::uint64_t dependency=0;
            try{dependency=load_module(memory,utf8_to_utf16(imported.dll),flags,created,altered,state);}
            catch(const ModuleError& error){
                // 明確選用 trap 時，只將未提供的 SDK 系統 API 綁到會停止的入口。
                // 真實 DLL 的遞迴依賴失敗不得被轉成 trap，也不建立虛構 provider。
                if(error.code==126 && created.size()==count && runtime_.trap_unresolved_imports && diagnostic_system_import(imported.dll))
                    return bind_import(imported.dll,imported.by_ordinal?"#"+std::to_string(imported.ordinal):imported.symbol,true);
                throw;
            }
            loaded.dependencies.insert(dependency);
            try{return GuestAddress{module_export(memory,dependency,imported.symbol,imported.by_ordinal?std::optional{imported.ordinal}:std::nullopt,created,0,state)};}
            catch(const ModuleError&) {
                if((!module_image(dependency) || runtime_.builtin_images.contains(dependency)) && runtime_.trap_unresolved_imports)return bind_import(imported.dll,imported.by_ordinal?"#"+std::to_string(imported.ordinal):imported.symbol,true);
                throw;
            }
        });
        loaded.loading=false;runtime_.module_order.push_back(base);
        runtime_.next_module_base=std::max(runtime_.next_module_base,(base+parsed.metadata.image_size+65535)&~UINT64_C(65535));
        std::ostringstream event;event<<"module_load="<<utf16_to_utf8(guest)<<" base=0x"<<std::hex<<base<<" size="<<std::dec<<parsed.metadata.image_size;runtime_.module_events.push_back(event.str());
    }catch(...) {
        if(!runtime_.loaded_modules.contains(base))for(const auto& region:parsed.mappings)if(memory.mapped(GuestAddress{base+region.rva}))memory.unmap(GuestAddress{base+region.rva});
        throw;
    }
    return base;
}
GuestAddress WindowsServices::bind_image_import(GuestMemory& memory,const PeImport& imported,std::uint64_t owner) {
    std::vector<std::uint64_t> created;
    try {
        const auto handle=load_module(memory,utf8_to_utf16(imported.dll),0,created);
        if(owner==runtime_.image.base)runtime_.main_dependencies.insert(handle);else runtime_.loaded_modules.at(owner).dependencies.insert(handle);
        try{return GuestAddress{module_export(memory,handle,imported.symbol,imported.by_ordinal?std::optional{imported.ordinal}:std::nullopt,created)};}
        catch(const ModuleError&) {
            if((!module_image(handle) || runtime_.builtin_images.contains(handle)) && runtime_.trap_unresolved_imports)return bind_import(imported.dll,imported.by_ordinal?"#"+std::to_string(imported.ordinal):imported.symbol,true);
            throw;
        }
    }catch(const ModuleError& error) {
        if(error.code==126 && created.empty() && runtime_.trap_unresolved_imports && diagnostic_system_import(imported.dll))
            return bind_import(imported.dll,imported.by_ordinal?"#"+std::to_string(imported.ordinal):imported.symbol,true);
        // 應用程式 DLL 缺少或真實 DLL 的相依失敗仍回報 Windows loader error。
        for(auto it=created.rbegin();it!=created.rend();++it)unmap_module(memory,*it);
        throw PeDependencyError(error.code,"PE 相依模組載入失敗："+std::string(error.what())+"，Windows error="+std::to_string(error.code));
    }
}
void WindowsServices::prepare_image_startup(GuestState& state) {
    if(initial_thread_entry_ || !runtime_.module_order.empty() || !runtime_.image.tls_callbacks.empty())state.rip=startup_entry_;
}
void WindowsServices::unmap_module(GuestMemory& memory,std::uint64_t base,GuestState* state) {
    if(const auto resource=runtime_.resource_modules.find(base);resource!=runtime_.resource_modules.end()) {
        for(const auto& [address,size]:resource->second.mappings)memory.unmap(GuestAddress{address},size);
        runtime_.module_events.push_back("resource_unload="+utf16_to_utf8(resource->second.image.path));runtime_.resource_modules.erase(resource);return;
    }
    const auto found=runtime_.loaded_modules.find(base);if(found==runtime_.loaded_modules.end())return;
    const auto& module=found->second;
    if(state && module.notification_ready)notify_dll(*state,memory,module.image,2);
    if(module.tls_slot!=UINT32_MAX) {
        for(const auto& [id,thread]:threads_.items){
            (void)id;if(thread.status==ThreadStatus::Exited)continue;
            const auto vector=memory.load(GuestAddress{thread.teb+(x86_?0x2c:0x58)},x86_?32:64);
            memory.store(GuestAddress{vector+module.tls_slot*(x86_?4U:8U)},x86_?32:64,0);
        }
        runtime_.static_tls_slots[module.tls_slot]=false;
    }
    for(const auto& [address,size]:module.mappings){memory.discard(GuestAddress{address},size);runtime_.virtual_page_protections.erase(runtime_.virtual_page_protections.lower_bound(address),runtime_.virtual_page_protections.lower_bound(address+size));}
    runtime_.module_events.push_back("module_unload="+utf16_to_utf8(module.image.path));
    runtime_.loaded_modules.erase(found);
    std::erase_if(runtime_.modules,[&](const auto& item){return item.second==base;});
    std::erase(runtime_.module_order,base);runtime_.main_dependencies.erase(base);
    for(auto& [handle,other]:runtime_.loaded_modules){(void)handle;other.dependencies.erase(base);}
}
void WindowsServices::append_module_detach(ModuleOperation& operation,std::uint64_t base,bool unmap,std::uint64_t reserved) {
    const auto found=runtime_.loaded_modules.find(base);if(found==runtime_.loaded_modules.end())return;
    const auto& module=found->second;
    if(module.attached) {
        for(const auto target:module.image.tls_callbacks)operation.steps.push_back({{target,{base,0,reserved},true},base});
        if(module.image.entry)operation.steps.push_back({{module.image.entry,{base,0,reserved},false,true},base});
    }
    operation.steps.push_back({{},base,false,false,unmap});
}
std::optional<std::uint32_t> WindowsServices::start_module_operation(GuestState& state,GuestMemory& memory,ModuleOperation operation) {
    // 映像已完成映射／匯入繫結，但 TLS 與 DllMain 尚未執行。
    // 重複 LoadLibrary 沒有 created 項目；資源映射不屬於可執行 DLL。
    if(!operation.unloading)for(const auto base:operation.created)
        if(runtime_.loaded_modules.contains(base))emit_debug_module(state,memory,base,true);
    if(!operation.unloading && operation.steps.empty()) {
        for(const auto handle:runtime_.module_order) {
            const auto& module=runtime_.loaded_modules.at(handle);
            if(module.attached || (!operation.startup && std::find(operation.created.begin(),operation.created.end(),handle)==operation.created.end()))continue;
            for(const auto target:module.image.tls_callbacks)operation.steps.push_back({{target,{handle,1,operation.startup?1U:0U},true},handle});
            if(module.image.entry)operation.steps.push_back({{module.image.entry,{handle,1,operation.startup?1U:0U},false,true},handle,true});
            operation.steps.push_back({{},handle,false,true});
        }
        if(operation.startup)for(const auto target:runtime_.image.tls_callbacks)operation.steps.push_back({{target,{runtime_.image.base,1,0},true}});
    }
    if(operation.steps.empty()) {
        if(operation.exit_thread)return exit_thread(state,memory,operation.thread_exit_code);
        if(!operation.startup)complete_call(state,memory,operation.result);
        return std::nullopt;
    }
    runtime_.loader_owner=threads_.current;++runtime_.loader_depth;
    publish_loader_lock(memory,peb_base_.value,x86_,thread_identifier(runtime_.loader_owner),runtime_.loader_depth);
    const auto id=runtime_.next_module_operation++;
    runtime_.module_operations.emplace(id,std::move(operation));
    return advance_module_operation(state,memory,id);
}
std::optional<std::uint32_t> WindowsServices::advance_module_operation(GuestState& state,GuestMemory& memory,std::uint64_t id) {
    auto& operation=runtime_.module_operations.at(id);
    while(operation.index<operation.steps.size()) {
        const auto step=operation.steps[operation.index];
        if(step.callback.target) {
            start_callbacks(state,memory,{step.callback},Continuation::ModuleOperation,id);return std::nullopt;
        }
        if(step.complete)runtime_.loaded_modules.at(step.module).attached=true;
        if(step.unmap){unmap_module(memory,step.module,&state);emit_debug_module(state,memory,step.module,false);}
        ++operation.index;
    }
    const auto result=operation.result;const auto error=operation.error;const bool startup=operation.startup;
    const auto end_thread=operation.exit_thread;const auto thread_exit_code=operation.thread_exit_code;
    runtime_.module_operations.erase(id);
    if(!--runtime_.loader_depth)runtime_.loader_owner=0;
    publish_loader_lock(memory,peb_base_.value,x86_,thread_identifier(runtime_.loader_owner),runtime_.loader_depth);
    if(error)last_error_=error;
    if(end_thread)return exit_thread(state,memory,thread_exit_code);
    if(startup){if(error)return 0xc0000142U;return std::nullopt;}
    complete_call(state,memory,error?0:result);return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::resume_module_operation(GuestState& state,GuestMemory& memory,const CallbackFrame& frame,std::uint64_t result) {
    auto& operation=runtime_.module_operations.at(frame.value);
    const auto step=operation.steps.at(operation.index);
    if(step.check_result && !static_cast<std::uint32_t>(result)) {
        runtime_.loaded_modules.at(step.module).attached=true;
        operation.error=1114;operation.result=0;operation.rollback=true;operation.unloading=true;
        operation.steps.clear();operation.index=0;
        std::set<std::uint64_t> cleanup(operation.created.begin(),operation.created.end());
        for(auto it=runtime_.module_order.rbegin();it!=runtime_.module_order.rend();++it)if(cleanup.erase(*it))append_module_detach(operation,*it,true);
        for(auto it=cleanup.rbegin();it!=cleanup.rend();++it)append_module_detach(operation,*it,true);
    }else ++operation.index;
    return advance_module_operation(state,memory,frame.value);
}
std::optional<std::uint32_t> WindowsServices::fail_module_initialization(GuestState& state,GuestMemory& memory,std::uint64_t id){
    auto& operation=runtime_.module_operations.at(id);const auto step=operation.steps.at(operation.index);
    // DllMain 逸出的例外由載入器攔下。與回 FALSE 不同，失敗 DLL 不接收 PROCESS_DETACH。
    if(auto module=runtime_.loaded_modules.find(step.module);module!=runtime_.loaded_modules.end())module->second.attached=false;
    runtime_.module_events.push_back("module_error=1114 DllMain 初始化例外");
    operation.error=1114;operation.result=0;operation.rollback=true;operation.unloading=true;operation.steps.clear();operation.index=0;
    std::set<std::uint64_t> cleanup(operation.created.begin(),operation.created.end());
    for(auto it=runtime_.module_order.rbegin();it!=runtime_.module_order.rend();++it)if(cleanup.erase(*it))append_module_detach(operation,*it,true);
    for(auto it=cleanup.rbegin();it!=cleanup.rend();++it)append_module_detach(operation,*it,true);
    return advance_module_operation(state,memory,id);
}
std::optional<std::uint32_t> WindowsServices::resume_loader_notification_exception(GuestState& state,GuestMemory& memory,std::size_t depth) {
    auto& frame=runtime_.callbacks.at(depth);
    const auto failed=frame.callbacks.at(frame.index);
    if((!failed.tls_initializer && !failed.dll_initializer) || failed.arguments.size()<2)throw HostCallFault("載入器例外邊界缺少初始化 callback");
    const auto same_group=[&](const GuestCallback& callback) {
        return callback.tls_initializer && callback.arguments.size()>=2 &&
            callback.arguments[0]==failed.arguments[0] && callback.arguments[1]==failed.arguments[1];
    };
    // TLS 略過同一 array 的剩餘項目；非 attach 的 DllMain 只略過目前通知。
    if(frame.continuation==Continuation::ModuleOperation) {
        const auto id=frame.value;auto& operation=runtime_.module_operations.at(id);
        ++operation.index;
        if(failed.tls_initializer)while(operation.index<operation.steps.size() && same_group(operation.steps[operation.index].callback))++operation.index;
        discard_callbacks(depth,state,memory);
        return advance_module_operation(state,memory,id);
    }
    if(failed.tls_initializer)while(frame.index+1<frame.callbacks.size() && same_group(frame.callbacks[frame.index+1]))++frame.index;
    state.reg(Gpr::Rax)=0;
    return resume_callback(state,memory);
}
std::vector<GuestCallback> WindowsServices::module_thread_callbacks(std::uint32_t reason) const {
    std::vector<GuestCallback> callbacks;
    auto append=[&](std::uint64_t base){const auto& module=runtime_.loaded_modules.at(base);if(!module.attached || module.no_thread_calls)return;
        for(const auto target:module.image.tls_callbacks)callbacks.push_back({target,{base,reason,0},true});
        if(module.image.entry)callbacks.push_back({module.image.entry,{base,reason,0},false,true});
    };
    if(reason==2)for(const auto handle:runtime_.module_order)append(handle);
    else for(auto it=runtime_.module_order.rbegin();it!=runtime_.module_order.rend();++it)append(*it);
    return callbacks;
}
bool WindowsServices::await_loader(GuestState& state,GuestMemory& memory) {
    if(!runtime_.loader_owner || runtime_.loader_owner==threads_.current)return false;
    auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::LoaderRetry;
    schedule(state,memory);return true;
}
std::optional<std::uint32_t> WindowsServices::invoke_module(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    if(await_loader(state,memory))return std::nullopt;
    // 檔案回呼可等待／重入；載入鎖涵蓋來源讀取、映射、匯入與操作交接。
    // DllMain／TLS 的非同步部分由既有 ModuleOperation 接續持有自己的深度。
    if(runtime_.loader_depth==UINT32_MAX)throw HostCallFault("loader lock 遞迴深度溢位");
    runtime_.loader_owner=threads_.current;++runtime_.loader_depth;
    publish_loader_lock(memory,peb_base_.value,x86_,thread_identifier(runtime_.loader_owner),runtime_.loader_depth);
    struct LoaderScope {WindowsServices& self;GuestMemory& memory;
        ~LoaderScope(){if(self.runtime_.loader_depth && !--self.runtime_.loader_depth)self.runtime_.loader_owner=0;
            publish_loader_lock(memory,self.peb_base_.value,self.x86_,self.thread_identifier(self.runtime_.loader_owner),self.runtime_.loader_depth);}} scope{*this,memory};
    const WindowsCall call(state,memory,service);const auto arg=[&](unsigned i){return call(i);};
    const auto symbol=service.symbol.starts_with("K32")?service.symbol.substr(3):service.symbol;const bool wide=symbol.ends_with('W');std::uint64_t result=0;
    std::vector<std::uint64_t> created;
    try {
        if(symbol.starts_with("EnumProcessModules") || symbol=="GetModuleInformation") {
            if(!current_process_handle(arg(0),0x410))throw ModuleError(current_process_handle(arg(0))?5:process_object_type(arg(0))==u"Process"?50:6,"程序模組查詢權限不足或不支援遠端程序");
            const unsigned width=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(width*8);
            if(symbol=="GetModuleInformation") {
                const auto* image=module_image(arg(1));if(!image)throw ModuleError(6,"無效模組 handle");
                const unsigned size=x86_?12U:24U;if(arg(3)<size)throw ModuleError(122,"MODULEINFO 緩衝區過小");
                memory.write(GuestAddress{arg(2)},std::vector<std::uint8_t>(size));
                memory.store(GuestAddress{arg(2)},bits,image->base);memory.store(GuestAddress{arg(2)+width},32,image->size);
                memory.store(GuestAddress{arg(2)+(x86_?8U:16U)},bits,image->entry);
            } else {
                const auto filter=symbol.ends_with("Ex")?static_cast<std::uint32_t>(arg(4)):0U;
                if(filter>3)throw ModuleError(87,"無效模組篩選旗標");
                std::vector<std::uint64_t> modules;
                // Win32 呼叫忽略 WOW64 篩選；64 位元客體只存在本架構模組。
                if(x86_ || filter!=1){
                    if(runtime_.image.base)modules.push_back(runtime_.image.base);
                    for(auto base:runtime_.module_order)if(runtime_.loaded_modules.contains(base))modules.push_back(base);
                    for(const auto& [base,image]:runtime_.builtin_images){(void)image;modules.push_back(base);}
                }
                const auto count=std::min<std::size_t>(modules.size(),static_cast<std::uint32_t>(arg(2))/width);
                if(count)memory.check(GuestAddress{arg(1)},count*width,Access::Write);
                memory.store(GuestAddress{arg(3)},32,modules.size()*width);
                for(std::size_t i=0;i<count;++i)memory.store(GuestAddress{arg(1)+i*width},bits,modules[i]);
            }
            result=1;
        }else if(symbol.starts_with("GetModuleFileName") || symbol.starts_with("GetModuleBaseName") || symbol.starts_with("GetProcessImageFileName") || symbol.starts_with("QueryFullProcessImageName")) {
            const bool full=symbol.starts_with("QueryFullProcessImageName"),process_path=full || symbol.starts_with("GetProcessImageFileName"),basename=symbol.starts_with("GetModuleBaseName");
            const bool extended=symbol.starts_with("GetModuleFileNameEx") || basename;
            if((extended || process_path) && !current_process_handle(arg(0),(process_path || !arg(1))?0x1000U:0x410U) && !((process_path || !arg(1)) && current_process_handle(arg(0),0x400U)))throw ModuleError(current_process_handle(arg(0))?5:process_object_type(arg(0))==u"Process"?50:6,"模組路徑查詢權限不足或不支援遠端程序");
            if(full && (arg(1)>1 || !arg(3)))throw ModuleError(87,"程序路徑格式或長度指標無效");
            const auto value=[&](unsigned i){return arg(i+(extended?1U:0U));};
            const auto handle=process_path || !value(0)?runtime_.image.base:value(0);const auto* image=module_image(handle);
            std::u16string path;
            if(image)path=image->path;
            else {
                const auto found=std::find_if(runtime_.modules.begin(),runtime_.modules.end(),[&](const auto& item){return item.second==handle;});
                if(found==runtime_.modules.end())throw ModuleError(126,"無效模組 handle");path=u"C:\\Windows\\System32\\"+utf8_to_utf16(found->first);
            }
            if(path.empty()) {const auto argv=memory.load(GuestAddress{runtime_.wargv_cell},x86_?32:64);path=read_utf16(memory,GuestAddress{memory.load(GuestAddress{argv},x86_?32:64)});if(path.find(u':')==path.npos)path=u"C:\\"+path;}
            if(basename){const auto at=path.find_last_of(u"\\/");if(at!=path.npos)path.erase(0,at+1);}
            if(process_path && (!full || arg(1))){if(path.size()<3 || path[1]!=u':')throw ModuleError(50,"無法對應此路徑的 NT 裝置名稱");path=windows_device_name(path[0])+path.substr(2);}
            const auto output=full?arg(2):process_path?arg(1):value(1);
            const auto capacity=full?static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)},32)):static_cast<std::uint32_t>(process_path?arg(2):value(2));
            if(!capacity)throw ModuleError(122,"模組路徑 buffer 為空");if(!output)throw ModuleError(87,"模組路徑指標為空");
            const auto ansi=utf16_to_utf8(path);const auto size=wide?path.size():ansi.size();result=std::min<std::uint64_t>(size,capacity);
            if(process_path && size>=capacity)throw ModuleError(122,"程序路徑 buffer 過小");
            const auto count=std::min<std::size_t>(size,capacity-1);if(extended || process_path)result=count;
            if(wide)memory.write(GuestAddress{output},utf16_bytes(path.substr(0,count)));
            else {std::vector<std::uint8_t> bytes(ansi.begin(),ansi.begin()+static_cast<std::ptrdiff_t>(count));bytes.push_back(0);memory.write(GuestAddress{output},bytes);}
            if(size>=capacity)last_error_=122;
            if(full){memory.store(GuestAddress{arg(3)},32,count);result=1;}
        }else if(symbol=="SetDefaultDllDirectories") {
            const auto flags=static_cast<std::uint32_t>(arg(0));
            if(!flags || (flags&~0x1e00U))throw ModuleError(87,"預設 DLL 搜尋旗標不合法");
            runtime_.default_dll_directories=(flags&0x1000)?((flags&~0x1000U)|0xe00U):flags;result=1;
        }else if(symbol=="AddDllDirectory") {
            if(!arg(0))throw ModuleError(87,"DLL 目錄指標為空");
            auto path=read_name(memory,arg(0),true);std::replace(path.begin(),path.end(),u'/',u'\\');
            const bool drive=path.size()>=3 && ((path[0]>=u'A' && path[0]<=u'Z') || (path[0]>=u'a' && path[0]<=u'z')) && path[1]==u':' && path[2]==u'\\';
            if(path.empty() || (!drive && path.front()!=u'\\'))throw ModuleError(87,"DLL 目錄必須是絕對路徑");
            if(path.front()==u'\\' && !path.starts_with(u"\\\\"))path=runtime_.current_directory.substr(0,2)+path;
            if(runtime_.added_dll_directories.size()>=1024)throw ModuleError(8,"DLL 搜尋目錄超過 1024");
            if(!path.ends_with(u'\\'))path+=u'\\';
            result=runtime_.allocate_wait_handle();runtime_.added_dll_directories.emplace(result,std::move(path));
        }else if(symbol=="RemoveDllDirectory") {
            if(!runtime_.added_dll_directories.erase(arg(0)))throw ModuleError(87,"DLL 目錄 cookie 無效");
            result=1;
        }else if(symbol.starts_with("SetDllDirectory")) {
            auto path=arg(0)?read_name(memory,arg(0),wide):std::u16string{};
            if(!path.empty()) {
                std::replace(path.begin(),path.end(),u'/',u'\\');if(!path.ends_with(u'\\'))path+=u'\\';
            }
            runtime_.dll_directory=std::move(path);runtime_.exclude_current_dll_directory=arg(0)!=0;result=1;
        }else if(symbol.starts_with("GetDllDirectory")) {
            auto path=runtime_.dll_directory;if(!path.empty() && path.back()==u'\\')path.pop_back();
            const auto ansi=utf16_to_utf8(path);const auto length=wide?path.size():ansi.size();const auto capacity=static_cast<std::uint32_t>(arg(0));
            result=length>=capacity?length+1:length;
            if(capacity>length){if(wide)memory.write(GuestAddress{arg(1)},utf16_bytes(path));else{std::vector<std::uint8_t> bytes(ansi.begin(),ansi.end());bytes.push_back(0);memory.write(GuestAddress{arg(1)},bytes);}}
            if(path.empty())result=0;
        }else if(symbol.starts_with("LoadLibrary")) {
            auto flags=symbol.starts_with("LoadLibraryEx")?static_cast<std::uint32_t>(arg(2)):0U;
            if(!arg(0) || (symbol.starts_with("LoadLibraryEx") && arg(1)) || (flags&~0x3ffbU) || ((flags&8) && (flags&0x1f00)) || (flags&0x42)==0x42)throw ModuleError(87,"LoadLibraryEx 參數或旗標不合法");
            if((flags&0x2091) || ((flags&0x42) && !(flags&0x20)))throw ModuleError(50,"尚未支援此 DLL 映射模式");
            auto name=read_name(memory,arg(0),wide);
            if((flags&0x100) && !(name.size()>=3 && name[1]==u':' && (name[2]==u'\\' || name[2]==u'/')))throw ModuleError(87,"DLL_LOAD_DIR 必須指定完整路徑");
            if(flags&0x1000)flags=(flags&~0x1000U)|0xe00;
            const auto handle=load_module(memory,std::move(name),flags,created,{},&state);
            if(auto found=runtime_.loaded_modules.find(handle);found!=runtime_.loaded_modules.end())++found->second.references;
            ModuleOperation operation;operation.created=std::move(created);operation.result=handle;
            return start_module_operation(state,memory,std::move(operation));
        }else if(symbol=="GetModuleHandleA" || symbol=="GetModuleHandleW" || symbol.starts_with("GetModuleHandleEx")) {
            const bool extended=symbol.starts_with("GetModuleHandleEx");const auto flags=extended?static_cast<std::uint32_t>(arg(0)):0U;
            const auto pointer=arg(extended?1:0);
            if(extended && ((flags&~7U) || (flags&3U)==3U || !arg(2)))throw ModuleError(87,"GetModuleHandleEx 參數不合法");
            if(extended)memory.check(GuestAddress{arg(2)},x86_?4:8,Access::Write);
            if(flags&4U){const auto* image=module_image(pointer,true);result=image?image->base:0;}
            else if(!pointer)result=runtime_.image.base;
            else {
                const auto name=read_name(memory,pointer,wide);result=find_module(name);
                const auto key=module_key(name);
                if(!result && key.find('\\')==key.npos && builtin_module(key))result=load_module(memory,name,0,created,{},&state);
            }
            if(extended) {
                memory.store(GuestAddress{arg(2)},x86_?32:64,result);
                if(auto found=runtime_.loaded_modules.find(result);found!=runtime_.loaded_modules.end()) {if(flags&1U)found->second.pinned=true;else if(!(flags&2U))++found->second.references;}
                result=result?1:0;
            }
            if(!result)last_error_=126;
        }else if(symbol=="GetProcAddress") {
            const auto ordinal=arg(1)<=65535?std::optional{static_cast<std::uint16_t>(arg(1))}:std::nullopt;
            const auto name=ordinal?std::string{}:read_string(memory,arg(1));
            const auto address=module_export(memory,arg(0),name,ordinal,created,0,&state);
            ModuleOperation operation;operation.created=std::move(created);operation.result=address;
            return start_module_operation(state,memory,std::move(operation));
        }else if(symbol=="DisableThreadLibraryCalls") {
            auto found=runtime_.loaded_modules.find(arg(0));
            if(found==runtime_.loaded_modules.end())throw ModuleError(126,"無效 DLL");
            if(found->second.image.tls_index || !found->second.image.tls_callbacks.empty())throw ModuleError(87,"DLL 使用 static TLS");
            found->second.no_thread_calls=true;result=1;
        }else if(symbol=="FreeLibrary" || symbol=="FreeLibraryAndExitThread") {
            if(runtime_.resource_modules.contains(arg(0))) {
                unmap_module(memory,arg(0),&state);
                if(symbol=="FreeLibraryAndExitThread")return exit_thread(state,memory,static_cast<std::uint32_t>(arg(1)));
                call.return_to(state,1);return std::nullopt;
            }
            auto found=runtime_.loaded_modules.find(arg(0));
            if(found==runtime_.loaded_modules.end()) {
                if(arg(0)!=runtime_.image.base && std::none_of(runtime_.modules.begin(),runtime_.modules.end(),[&](const auto& item){return item.second==arg(0);}))throw ModuleError(6,"無效 DLL handle");result=1;
            }else {
                if(found->second.references)--found->second.references;
                std::set<std::uint64_t> live=runtime_.main_dependencies;
                for(const auto& [base,module]:runtime_.loaded_modules)if(module.references || module.pinned || !module.attached)live.insert(base);
                // 外層卸載已取得這批 DLL 的 detach／unmap 責任。通知中的巢狀 FreeLibrary
                // 可以釋放其他 DLL，但不得重入同一批通知或提前移除其執行中的映像。
                for(const auto& [id,active]:runtime_.module_operations){
                    (void)id;if(!active.unloading)continue;
                    for(std::size_t i=active.index;i<active.steps.size();++i)if(active.steps[i].module)live.insert(active.steps[i].module);
                }
                bool changed=true;while(changed){changed=false;for(const auto& [base,module]:runtime_.loaded_modules)if(live.contains(base))for(const auto dependency:module.dependencies)changed=live.insert(dependency).second || changed;}
                ModuleOperation operation;operation.unloading=true;operation.result=1;
                operation.exit_thread=symbol=="FreeLibraryAndExitThread";
                operation.thread_exit_code=operation.exit_thread?static_cast<std::uint32_t>(arg(1)):0;
                for(auto it=runtime_.module_order.rbegin();it!=runtime_.module_order.rend();++it)if(!live.contains(*it))append_module_detach(operation,*it,true);
                return start_module_operation(state,memory,std::move(operation));
            }
        }else throw HostCallFault("未實作此模組 API");
    }catch(const ModuleError& error) {
        for(auto it=created.rbegin();it!=created.rend();++it)unmap_module(memory,*it,&state);
        last_error_=error.code;result=0;
        runtime_.module_events.push_back("module_error="+std::to_string(error.code)+" "+error.what());
    }catch(const std::invalid_argument& error) {
        for(auto it=created.rbegin();it!=created.rend();++it)unmap_module(memory,*it,&state);
        last_error_=193;result=0;runtime_.module_events.push_back("module_error=193 "+std::string(error.what()));
    }catch(const std::bad_alloc&) {
        for(auto it=created.rbegin();it!=created.rend();++it)unmap_module(memory,*it,&state);
        last_error_=8;result=0;runtime_.module_events.push_back("module_error=8 模組／TLS 配置超過客體記憶體預算");
    }
    if(symbol=="FreeLibraryAndExitThread")return exit_thread(state,memory,static_cast<std::uint32_t>(arg(1)));
    call.return_to(state,result);return std::nullopt;
}
std::pair<std::uint64_t,std::uint32_t> WindowsServices::load_kernel_image(GuestMemory& memory,const std::u16string& path){
    // 核心驅動映像經同一模組管理員映射、重定位與綁定匯入；不呼叫 DllMain，由驅動宿主執行 DriverEntry。
    std::vector<std::uint64_t> created;kernel_image_load_=true;
    struct Reset {bool& flag;~Reset(){flag=false;}} reset{kernel_image_load_};
    try{return {load_module(memory,path,8,created),0};}
    catch(const ModuleError& error){for(auto it=created.rbegin();it!=created.rend();++it)unmap_module(memory,*it);runtime_.module_events.push_back("module_error="+std::to_string(error.code)+" "+error.what());return {0,error.code};}
    catch(const std::invalid_argument& error){for(auto it=created.rbegin();it!=created.rend();++it)unmap_module(memory,*it);runtime_.module_events.push_back("module_error=193 "+std::string(error.what()));return {0,193};}
    catch(const std::bad_alloc&){for(auto it=created.rbegin();it!=created.rend();++it)unmap_module(memory,*it);return {0,8};}
}
std::string WindowsServices::module_diagnostics() const {
    std::string output;for(const auto& event:runtime_.module_events)output+=event+'\n';return output;
}

std::array<std::uint8_t,32> WindowsServices::image_identity(const std::filesystem::path& source,std::span<const std::uint8_t> bytes){
#if defined(__APPLE__)
    static const bool enabled=[] {const auto* value=std::getenv("X64ARM_IDENTITY_CACHE");return !value || std::string_view(value)!="0";}();
    struct stat info{};
    if(!enabled || ::stat(source.c_str(),&info)<0 || !S_ISREG(info.st_mode) || static_cast<std::uint64_t>(info.st_size)!=bytes.size())
        return SmapCache::source_identity(bytes);
    static const std::filesystem::path directory=[] {
        const auto n=::confstr(_CS_DARWIN_USER_TEMP_DIR,nullptr,0);if(!n)return std::filesystem::path{};
        std::vector<char> text(n);::confstr(_CS_DARWIN_USER_TEMP_DIR,text.data(),n);
        std::error_code error;auto path=std::filesystem::path(text.data())/"x64arm-identity-cache";std::filesystem::create_directories(path,error);
        return error?std::filesystem::path{}:path;
    }();
    if(directory.empty())return SmapCache::source_identity(bytes);
    char name[160];
    std::snprintf(name,sizeof(name),"%llx-%llx-%llx-%lld.%ld-%lld.%ld",static_cast<unsigned long long>(info.st_dev),static_cast<unsigned long long>(info.st_ino),
        static_cast<unsigned long long>(info.st_size),static_cast<long long>(info.st_mtimespec.tv_sec),static_cast<long>(info.st_mtimespec.tv_nsec),
        static_cast<long long>(info.st_ctimespec.tv_sec),static_cast<long>(info.st_ctimespec.tv_nsec));
    const auto path=directory/name;
    std::array<std::uint8_t,32> identity{};
    if(const int fd=::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);fd>=0){
        const auto read=::read(fd,identity.data(),identity.size());::close(fd);
        if(read==static_cast<ssize_t>(identity.size()))return identity;
    }
    identity=SmapCache::source_identity(bytes);
    // 寫入暫存檔再改名；同時寫入的程序結果相同。
    const auto temporary=path.string()+".tmp-"+std::to_string(::getpid());
    if(const int fd=::open(temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);fd>=0){
        const bool written=::write(fd,identity.data(),identity.size())==static_cast<ssize_t>(identity.size());::close(fd);
        if(!written || ::rename(temporary.c_str(),path.c_str())<0)(void)::unlink(temporary.c_str());
    }
    return identity;
#else
    (void)source;return SmapCache::source_identity(bytes);
#endif
}
}
