// 色彩管理共用儲存、ICC 工具，以及 gdi32 的 ICM 函式：GetICMProfileA／W、SetICMProfileA／W、EnumICMProfilesA／W、
// UpdateICMRegKeyA／W（UpdateICMRegKey 為 A 版別名）。DC 的輸出設定檔隨 DC 狀態保存；顯示 DC 的裝置為 \\.\DISPLAY1。
#include "windows_icm_store.hpp"
#include "service_callback_task.hpp"
#include <algorithm>
#include <cmath>

namespace x64arm {
std::u16string icm_lower(std::u16string text){for(auto& c:text)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);return text;}
std::u16string icm_file_part(std::u16string_view path){const auto at=path.find_last_of(u"\\/");return std::u16string(at==path.npos?path:path.substr(at+1));}
std::u16string icm_profile_path(std::u16string_view name){
    if(name.find_first_of(u"\\/:")!=name.npos)return std::u16string(name);
    std::u16string path(icm_color_directory);path+=u'\\';path+=name;return path;
}
std::uint32_t icc_u32(std::span<const std::uint8_t> bytes,std::size_t at){
    if(at>bytes.size() || 4>bytes.size()-at)throw std::invalid_argument("ICC 欄位越界");
    return static_cast<std::uint32_t>(bytes[at])<<24|static_cast<std::uint32_t>(bytes[at+1])<<16|static_cast<std::uint32_t>(bytes[at+2])<<8|bytes[at+3];
}
void icc_put32(std::span<std::uint8_t> bytes,std::size_t at,std::uint32_t value){for(unsigned i=0;i<4;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(24-8*i));}
const std::vector<std::uint8_t>* IccProfile::find(std::uint32_t tag) const{for(const auto& [signature,index]:tags)if(signature==tag)return &data[index];return nullptr;}
std::size_t IccProfile::references(std::size_t index) const{return static_cast<std::size_t>(std::count_if(tags.begin(),tags.end(),[&](const auto& item){return item.second==index;}));}
std::optional<IccProfile> icc_parse(std::span<const std::uint8_t> bytes){
    try{
        if(bytes.size()<132)return std::nullopt;const auto size=icc_u32(bytes,0);if(size<132 || size>bytes.size() || icc_u32(bytes,36)!=0x61637370)return std::nullopt;
        const auto count=icc_u32(bytes,128);if(count>(size-132)/12)return std::nullopt;
        IccProfile profile;std::copy_n(bytes.begin(),128,profile.header.begin());std::map<std::pair<std::uint32_t,std::uint32_t>,std::size_t> blocks;
        for(std::uint32_t i=0;i<count;++i){
            const auto signature=icc_u32(bytes,132+i*12),offset=icc_u32(bytes,136+i*12),length=icc_u32(bytes,140+i*12);
            if(offset>size || length>size-offset)return std::nullopt;
            // 位移與長度相同的 tag 共用資料（reference）。
            auto [found,inserted]=blocks.try_emplace({offset,length},profile.data.size());
            if(inserted)profile.data.emplace_back(bytes.begin()+offset,bytes.begin()+offset+length);
            profile.tags.emplace_back(signature,found->second);
        }
        return profile;
    }catch(const std::invalid_argument&){return std::nullopt;}
}
std::vector<std::uint8_t> icc_build(const IccProfile& profile){
    std::vector<std::uint8_t> bytes(profile.header.begin(),profile.header.end());
    const auto table=bytes.size();bytes.resize(table+4+profile.tags.size()*12);icc_put32(bytes,table,static_cast<std::uint32_t>(profile.tags.size()));
    std::vector<std::uint32_t> offsets(profile.data.size(),UINT32_MAX);
    for(std::size_t i=0;i<profile.tags.size();++i){
        const auto& [signature,index]=profile.tags[i];
        if(offsets[index]==UINT32_MAX){bytes.resize((bytes.size()+3)&~std::size_t{3});offsets[index]=static_cast<std::uint32_t>(bytes.size());bytes.insert(bytes.end(),profile.data[index].begin(),profile.data[index].end());}
        icc_put32(bytes,table+4+i*12,signature);icc_put32(bytes,table+8+i*12,offsets[index]);icc_put32(bytes,table+12+i*12,static_cast<std::uint32_t>(profile.data[index].size()));
    }
    bytes.resize((bytes.size()+3)&~std::size_t{3});icc_put32(bytes,0,static_cast<std::uint32_t>(bytes.size()));return bytes;
}
unsigned icc_space_channels(std::uint32_t space){
    switch(space){case 0x47524159:return 1;case 0x52474220:case 0x58595a20:case 0x4c616220:case 0x4c757620:case 0x59436272:case 0x59787920:case 0x48535620:case 0x484c5320:case 0x434d5920:return 3;case 0x434d594b:return 4;default:break;}
    // n 色空間 '2CLR'～'FCLR'。
    if((space&0x00ffffffU)==0x00434c52U){const auto digit=space>>24;if(digit>='2' && digit<='9')return digit-'0';if(digit>='A' && digit<='F')return digit-'A'+10;}
    return 0;
}
IccProfile icc_empty(){IccProfile profile;icc_put32(profile.header,0,132);icc_put32(profile.header,8,0x02100000);icc_put32(profile.header,36,0x61637370);return profile;}
IccRgbParameters icc_srgb_parameters(){
    // IEC 61966-2.1 的原色經 Bradford 調適到 D50 PCS；TRC 為 sRGB 分段曲線。
    IccRgbParameters parameters;parameters.colorants={{{0.4360747,0.2225045,0.0139322},{0.3850649,0.7168786,0.0971045},{0.1430804,0.0606169,0.7141733}}};parameters.description="sRGB IEC61966-2.1";return parameters;
}
std::vector<std::uint8_t> icc_rgb_profile(const IccRgbParameters& parameters){
    auto profile=icc_empty();auto& header=profile.header;
    for(const auto& [at,value]:std::array<std::pair<std::size_t,std::uint32_t>,6>{{{12,0x6d6e7472},{16,0x52474220},{20,0x58595a20},{40,0x4d534654},{64,parameters.intent},{8,0x02100000}}})icc_put32(header,at,value);
    const auto fixed=[](double value){return static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(value*65536)));};
    for(unsigned i=0;i<3;++i)icc_put32(header,68+i*4,fixed(std::array{0.9642,1.0,0.8249}[i]));
    const auto add=[&](std::uint32_t signature,std::vector<std::uint8_t> data){profile.tags.emplace_back(signature,profile.data.size());profile.data.push_back(std::move(data));};
    const auto typed=[](std::uint32_t type,std::size_t size){std::vector<std::uint8_t> data(size);icc_put32(data,0,type);return data;};
    // textDescriptionType：ASCII 描述，Unicode 與 ScriptCode 部分為空。
    {auto data=typed(0x64657363,12+parameters.description.size()+1+8+3+67);icc_put32(data,8,static_cast<std::uint32_t>(parameters.description.size()+1));std::copy(parameters.description.begin(),parameters.description.end(),data.begin()+12);add(0x64657363,std::move(data));}
    {const std::string text="No copyright, use freely";auto data=typed(0x74657874,8+text.size()+1);std::copy(text.begin(),text.end(),data.begin()+8);add(0x63707274,std::move(data));}
    const auto xyz=[&](std::uint32_t signature,const std::array<double,3>& value){auto data=typed(0x58595a20,20);for(unsigned i=0;i<3;++i)icc_put32(data,8+i*4,fixed(value[i]));add(signature,std::move(data));};
    xyz(0x77747074,parameters.white);xyz(0x7258595a,parameters.colorants[0]);xyz(0x6758595a,parameters.colorants[1]);xyz(0x6258595a,parameters.colorants[2]);
    // curveType：gamma 為單一 u8Fixed8；sRGB 曲線以 1024 點表格表示。三個通道相同時共用同一份資料。
    const auto curve=[&](double gamma){
        if(gamma>0){auto data=typed(0x63757276,14);icc_put32(data,8,1);const auto value=static_cast<std::uint16_t>(std::clamp(std::lround(gamma*256),1L,65535L));data[12]=static_cast<std::uint8_t>(value>>8);data[13]=static_cast<std::uint8_t>(value);return data;}
        auto data=typed(0x63757276,12+1024*2);icc_put32(data,8,1024);
        for(unsigned i=0;i<1024;++i){const double v=i/1023.0,linear=v<=0.04045?v/12.92:std::pow((v+0.055)/1.055,2.4);const auto value=static_cast<std::uint16_t>(std::lround(linear*65535));data[12+i*2]=static_cast<std::uint8_t>(value>>8);data[13+i*2]=static_cast<std::uint8_t>(value);}
        return data;
    };
    const std::array<std::uint32_t,3> trc{0x72545243,0x67545243,0x62545243};
    if(parameters.gamma[0]==parameters.gamma[1] && parameters.gamma[1]==parameters.gamma[2]){const auto index=profile.data.size();profile.data.push_back(curve(parameters.gamma[0]));for(const auto signature:trc)profile.tags.emplace_back(signature,index);}
    else for(unsigned i=0;i<3;++i)add(trc[i],curve(parameters.gamma[i]));
    return icc_build(profile);
}

std::uint64_t WindowsServices::IcmStore::root() const{return self.x86_?std::uint64_t{0x80000002U}:UINT64_C(0xffffffff80000002);}
std::uint64_t WindowsServices::IcmStore::text(std::u16string_view value){const auto address=scratch.text(value);if(!address)throw std::bad_alloc();return address;}
std::uint64_t WindowsServices::IcmStore::key(std::u16string_view child){std::u16string path=u"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ICM";if(!child.empty()){path+=u'\\';path+=child;}return text(path);}
std::uint32_t WindowsServices::IcmStore::advapi(std::string_view symbol,std::initializer_list<std::uint64_t> arguments){return static_cast<std::uint32_t>(self.nested_service(state,memory,"advapi32.dll",symbol,arguments));}
std::optional<std::u16string> WindowsServices::IcmStore::read_string(std::u16string_view child,std::u16string_view name){
    const auto size=scratch.allocate(4),buffer=scratch.allocate(2080);if(!size || !buffer)throw std::bad_alloc();memory.store(GuestAddress{size},32,2080);
    if(advapi("RegGetValueW",{root(),key(child),text(name),2,0,buffer,size}))return std::nullopt;return read_utf16(memory,GuestAddress{buffer},1039);
}
std::optional<std::vector<std::u16string>> WindowsServices::IcmStore::read_list(std::u16string_view child,std::u16string_view name){
    std::u16string path=u"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ICM";if(!child.empty()){path+=u'\\';path+=child;}return read_machine_list(path,name);
}
std::optional<std::vector<std::u16string>> WindowsServices::IcmStore::read_machine_list(std::u16string_view path,std::u16string_view name){
    const auto size=scratch.allocate(4);if(!size)throw std::bad_alloc();memory.store(GuestAddress{size},32,0);const auto subkey=text(path);
    if(const auto status=advapi("RegGetValueW",{root(),subkey,text(name),0x20,0,0,size});status && status!=234)return std::nullopt;
    const auto bytes=static_cast<std::uint32_t>(memory.load(GuestAddress{size},32));if(bytes>1024*1024)return std::nullopt;const auto buffer=scratch.allocate(bytes+4);if(!buffer)throw std::bad_alloc();
    if(advapi("RegGetValueW",{root(),subkey,text(name),0x20,0,buffer,size}))return std::nullopt;
    const auto units=read_utf16_counted(memory,GuestAddress{buffer},static_cast<std::uint32_t>(memory.load(GuestAddress{size},32))/2);
    std::vector<std::u16string> values;std::u16string current;for(const auto c:units){if(c){current.push_back(c);continue;}if(current.empty())break;values.push_back(std::move(current));current.clear();}
    if(!current.empty())values.push_back(std::move(current));return values;
}
std::optional<std::uint32_t> WindowsServices::IcmStore::read_number(std::u16string_view child,std::u16string_view name){
    const auto size=scratch.allocate(4),buffer=scratch.allocate(4);if(!size || !buffer)throw std::bad_alloc();memory.store(GuestAddress{size},32,4);
    if(advapi("RegGetValueW",{root(),key(child),text(name),0x10,0,buffer,size}))return std::nullopt;return static_cast<std::uint32_t>(memory.load(GuestAddress{buffer},32));
}
bool WindowsServices::IcmStore::write_string(std::u16string_view child,std::u16string_view name,std::u16string_view value){
    const auto status=advapi("RegSetKeyValueW",{root(),key(child),text(name),1,text(value),(value.size()+1)*2});if(status)self.last_error_=status;return !status;
}
bool WindowsServices::IcmStore::write_list(std::u16string_view child,std::u16string_view name,const std::vector<std::u16string>& values){
    if(values.empty()){(void)remove(child,name);return true;}
    std::u16string joined;for(const auto& value:values){joined+=value;joined.push_back(0);}
    const auto status=advapi("RegSetKeyValueW",{root(),key(child),text(name),7,text(joined),(joined.size()+1)*2});if(status)self.last_error_=status;return !status;
}
bool WindowsServices::IcmStore::remove(std::u16string_view child,std::u16string_view name){return advapi("RegDeleteKeyValueW",{root(),key(child),text(name)})==0;}
std::vector<std::u16string> WindowsServices::IcmStore::names(std::u16string_view child){
    std::vector<std::u16string> result;const auto handle=scratch.allocate(8),buffer=scratch.allocate(520),length=scratch.allocate(4);if(!handle || !buffer || !length)throw std::bad_alloc();
    if(advapi("RegOpenKeyExW",{root(),key(child),0,0x20019,handle}))return result;const auto opened=memory.load(GuestAddress{handle},self.x86_?32:64);
    for(std::uint32_t index=0;index<4096;++index){memory.store(GuestAddress{length},32,260);if(advapi("RegEnumValueW",{opened,index,buffer,length,0,0,0,0}))break;result.push_back(read_utf16(memory,GuestAddress{buffer},259));}
    (void)advapi("RegCloseKey",{opened});return result;
}
bool WindowsServices::IcmStore::exists(std::u16string_view path){return static_cast<std::uint32_t>(self.nested_service(state,memory,"kernel32.dll","GetFileAttributesW",{text(path)}))!=UINT32_MAX;}
std::optional<std::vector<std::uint8_t>> WindowsServices::IcmStore::read_profile(std::u16string_view path){
    auto bytes=self.read_guest_file(state,memory,path,128,64*1024*1024);if(!bytes)return std::nullopt;
    if(!icc_parse(*bytes)){self.last_error_=icm_error_invalid_profile;return std::nullopt;}return bytes;
}
std::vector<std::u16string> WindowsServices::IcmStore::registered(){return names(u"RegisteredProfiles");}
bool WindowsServices::IcmStore::installed(std::u16string_view file){return read_string(u"RegisteredProfiles",icm_file_part(file)).has_value();}
bool WindowsServices::IcmStore::install(std::u16string_view source){
    const auto path=icm_profile_path(source),file=icm_file_part(source);if(file.empty()){self.last_error_=87;return false;}if(!read_profile(path))return false;
    const auto target=icm_profile_path(file);
    if(icm_lower(target)!=icm_lower(path)){
        std::u16string partial;for(const auto part:std::array<std::u16string_view,5>{u"C:\\Windows",u"\\System32",u"\\spool",u"\\drivers",u"\\color"}){partial+=part;(void)self.nested_service(state,memory,"kernel32.dll","CreateDirectoryW",{text(partial),0});}
        if(!self.nested_service(state,memory,"kernel32.dll","CopyFileW",{text(path),text(target),0}))return false;
    }
    return write_string(u"RegisteredProfiles",file,file);
}
bool WindowsServices::IcmStore::uninstall(std::u16string_view name,bool remove_file){
    const auto file=icm_file_part(name);if(!installed(file)){self.last_error_=icm_error_profile_not_found;return false;}
    (void)remove(u"RegisteredProfiles",file);
    for(const auto& device:names(u"ProfileAssociations")){auto list=associations(device);const auto before=list.size();std::erase_if(list,[&](const auto& item){return icm_lower(item)==icm_lower(file);});if(list.size()!=before)(void)write_list(u"ProfileAssociations",device,list);}
    if(remove_file)(void)self.nested_service(state,memory,"kernel32.dll","DeleteFileW",{text(icm_profile_path(file))});
    return true;
}
std::vector<std::u16string> WindowsServices::IcmStore::associations(std::u16string_view device){return read_list(u"ProfileAssociations",device).value_or(std::vector<std::u16string>{});}
bool WindowsServices::IcmStore::associate(std::u16string_view profile,std::u16string_view device,bool first){
    const auto file=icm_file_part(profile);if(device.empty()){self.last_error_=87;return false;}if(!installed(file)){self.last_error_=icm_error_profile_not_found;return false;}
    auto list=associations(device);std::erase_if(list,[&](const auto& item){return icm_lower(item)==icm_lower(file);});
    if(first)list.insert(list.begin(),file);else list.push_back(file);return write_list(u"ProfileAssociations",device,list);
}
bool WindowsServices::IcmStore::disassociate(std::u16string_view profile,std::u16string_view device){
    const auto file=icm_file_part(profile);auto list=associations(device);const auto before=list.size();
    std::erase_if(list,[&](const auto& item){return icm_lower(item)==icm_lower(file);});if(list.size()==before){self.last_error_=icm_error_not_associated;return false;}
    return write_list(u"ProfileAssociations",device,list);
}
std::optional<std::u16string> WindowsServices::IcmStore::standard_profile(std::uint32_t space){
    char16_t name[9]{};for(unsigned i=0;i<8;++i)name[i]=u"0123456789abcdef"[(space>>(28-4*i))&15U];
    if(auto mapped=read_string(u"StandardColorSpaces",name))return mapped;
    if(space!=lcs_srgb && space!=lcs_windows_color_space){self.last_error_=2;return std::nullopt;}
    const auto path=icm_profile_path(icm_srgb_file);if(exists(path))return path;
    std::u16string partial;for(const auto part:std::array<std::u16string_view,5>{u"C:\\Windows",u"\\System32",u"\\spool",u"\\drivers",u"\\color"}){partial+=part;(void)self.nested_service(state,memory,"kernel32.dll","CreateDirectoryW",{text(partial),0});}
    if(!self.write_guest_file(state,memory,path,icc_rgb_profile(icc_srgb_parameters())))return std::nullopt;return path;
}
bool WindowsServices::IcmStore::set_standard_profile(std::uint32_t space,std::u16string_view path){
    char16_t name[9]{};for(unsigned i=0;i<8;++i)name[i]=u"0123456789abcdef"[(space>>(28-4*i))&15U];return write_string(u"StandardColorSpaces",name,icm_profile_path(path));
}
std::optional<std::u16string> WindowsServices::IcmStore::device_default(std::u16string_view device){
    if(const auto list=associations(device);!list.empty())return icm_profile_path(list.front());return standard_profile(lcs_srgb);
}
namespace {std::u16string cmm_name(std::uint32_t id){std::u16string name;for(int shift=24;shift>=0;shift-=8)name.push_back(static_cast<char16_t>((id>>shift)&0xffU));return name;}}
std::optional<std::u16string> WindowsServices::IcmStore::cmm(std::uint32_t id){return read_string(u"ICMatchers",cmm_name(id));}
bool WindowsServices::IcmStore::register_cmm(std::uint32_t id,std::u16string_view module){if(module.empty()){self.last_error_=87;return false;}return write_string(u"ICMatchers",cmm_name(id),module);}
bool WindowsServices::IcmStore::unregister_cmm(std::uint32_t id){if(!remove(u"ICMatchers",cmm_name(id))){self.last_error_=icm_error_invalid_cmm;return false;}return true;}

std::span<const WindowsServiceDescriptor> icm_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"gdi32.dll","GetICMProfileA",3},{"gdi32.dll","GetICMProfileW",3},{"gdi32.dll","SetICMProfileA",2},{"gdi32.dll","SetICMProfileW",2},
        {"gdi32.dll","EnumICMProfilesA",3},{"gdi32.dll","EnumICMProfilesW",3},{"gdi32.dll","UpdateICMRegKeyA",4},{"gdi32.dll","UpdateICMRegKeyW",4},
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_icm(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const bool wide=name.ends_with('W');
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto fail=[&](std::uint32_t error){last_error_=error;return finish(0);};
    const auto text=[&](std::uint64_t pointer){return wide?read_utf16(memory,GuestAddress{pointer}):utf8_to_utf16(read_ansi(memory,pointer));};
    const auto valid_dc=[&](std::uint64_t h){return gui_.contexts.contains(h) || gui_.dc_objects.contains(h) || (h>=0x10000000 && gui_.windows.contains(h-0x10000000));};
    IcmStore store(*this,state,memory);
    // DC 的輸出設定檔：先看 SetICMProfile，再看顯示裝置預設（第一個關聯，沒有時為 sRGB）。
    const auto dc_profile=[&](std::uint64_t dc)->std::optional<std::u16string>{
        if(const auto found=gui_.contexts.find(dc);found!=gui_.contexts.end() && !found->second.icm_profile.empty())return found->second.icm_profile;
        return store.device_default(icm_display_device);
    };
    try{
        if(name.starts_with("GetICMProfile")){
            // {hdc, LPDWORD 字元數, 緩衝區}：緩衝區為 NULL 時只回報所需字元數（含 NUL）。
            if(!valid_dc(arg(0)))return fail(6);if(!arg(1))return fail(87);const auto profile=dc_profile(arg(0));if(!profile)return fail(icm_error_not_associated);
            std::vector<std::uint8_t> bytes;if(wide)bytes=utf16_bytes(*profile);else {const auto narrow=utf16_to_utf8(*profile);bytes.assign(narrow.begin(),narrow.end());bytes.push_back(0);}
            const auto needed=static_cast<std::uint32_t>(bytes.size()/(wide?2:1)),capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)},32));
            memory.store(GuestAddress{arg(1)},32,needed);if(!arg(2))return finish(1);if(capacity<needed)return fail(122);
            memory.write(GuestAddress{arg(2)},bytes);return finish(1);
        }
        if(name.starts_with("SetICMProfile")){
            if(!valid_dc(arg(0)))return fail(6);if(!arg(1))return fail(87);const auto path=icm_profile_path(text(arg(1)));if(!store.read_profile(path))return finish(0);
            gui_.contexts[arg(0)].icm_profile=path;return finish(1);
        }
        if(name.starts_with("EnumICMProfiles")){
            // ICMENUMPROC(LPTSTR 檔名, LPARAM)：DC 設定檔、裝置預設、其他關聯與已安裝設定檔依序不重複；沒有任何設定檔回 -1，回呼回 0 即停止。
            if(!valid_dc(arg(0)))return fail(6);if(!arg(1))return fail(87);
            std::vector<std::u16string> profiles;const auto add=[&](std::u16string path){const auto key=icm_lower(path);if(std::none_of(profiles.begin(),profiles.end(),[&](const auto& item){return icm_lower(item)==key;}))profiles.push_back(std::move(path));};
            if(const auto profile=dc_profile(arg(0)))add(*profile);
            for(const auto& file:store.associations(icm_display_device))add(icm_profile_path(file));for(const auto& file:store.registered())add(icm_profile_path(file));
            if(profiles.empty())return finish(UINT32_MAX);
            return nested_continuation(state,memory,"EnumICMProfiles",[this,service,profiles,wide](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall call(s,m,service);std::size_t largest=0;for(const auto& path:profiles)largest=std::max(largest,path.size()*4+4);
                const auto buffer=allocate_heap(m,largest);if(!buffer){last_error_=8;call.return_to(s,0);co_return std::nullopt;}
                std::uint32_t result=1;
                for(const auto& path:profiles){
                    std::vector<std::uint8_t> bytes;if(wide)bytes=utf16_bytes(path);else {const auto narrow=utf16_to_utf8(path);bytes.assign(narrow.begin(),narrow.end());bytes.push_back(0);}
                    m.write(GuestAddress{buffer},bytes);result=static_cast<std::uint32_t>(co_await service_callback::call(call(1),{buffer,call(2)}));if(!result)break;
                }
                (void)free_heap(m,buffer,s);call.return_to(s,result);co_return std::nullopt;
            });
        }
        // UpdateICMRegKey {reserved, lpszCMID, lpszFileName, command}。
        const auto command=static_cast<std::uint32_t>(arg(3));if(command<1 || command>7)return fail(87);
        if(command==5 || command==6){
            // ICM_REGISTERICMATCHER／UNREGISTERICMATCHER：lpszCMID 指向 CMM 識別 DWORD（四字元，高位元組在前）。
            if(!arg(1))return fail(87);const auto id=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)},32));
            if(command==6)return finish(store.unregister_cmm(id));if(!arg(2))return fail(87);return finish(store.register_cmm(id,text(arg(2))));
        }
        // ICM_QUERYMATCH：lpszFileName 指向 DEVMODE；沙箱只有一個顯示裝置，有關聯設定檔即為相符。
        if(command==7)return store.associations(icm_display_device).empty()?fail(icm_error_profile_not_found):finish(1);
        if(!arg(2))return fail(87);const auto source=text(arg(2));if(source.empty())return fail(87);
        if(command==1)return finish(store.install(source));
        if(command==2)return finish(store.uninstall(source,false));
        if(command==3)return store.installed(source)?finish(1):fail(icm_error_profile_not_found);
        // ICM_SETDEFAULTPROFILE：設定檔須已安裝，成為顯示裝置的第一個關聯。
        return finish(store.associate(source,icm_display_device,true));
    }catch(const MemoryFault&){return fail(998);}catch(const std::bad_alloc&){return fail(8);}
}
}
