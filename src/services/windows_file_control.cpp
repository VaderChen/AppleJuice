#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_paths.hpp"
#include <algorithm>

namespace x64arm {
std::uint32_t WindowsServices::set_file_valid_data(std::uint64_t handle,std::uint64_t length){
    const auto found=files_.find(handle);if(found==files_.end())return 6;const auto& file=found->second;
    if(!file.native || !(file.access&(0x40000000U|2U)))return 5;
    if(!file.manage_volume_at_open)return 1314;
    if(file.path.starts_with("\\\\") || file.path.starts_with("//"))return 87;
    return file.native->set_valid_data(length);
}
std::uint32_t WindowsServices::set_file_information(GuestMemory& memory,std::uint64_t handle,std::uint32_t kind,std::uint64_t input,std::uint32_t size){
    auto found=files_.find(handle);if(found==files_.end())return 6;auto& file=found->second;if(!file.native)return 5;
    const auto required=kind==0?40U:kind==3?(x86_?12U:20U):kind==4?1U:kind==5 || kind==6?8U:0U;
    if(!required)return 87;if(size<required)return 24;if(!input)return 87;memory.check(GuestAddress{input},size,Access::Read);
    std::uint32_t error=0;
    if(kind==0){
        if(!(file.access&(0x40000000U|0x100U)))return 5;
        error=file.native->set_basic(memory.load(GuestAddress{input},64),memory.load(GuestAddress{input+8},64),memory.load(GuestAddress{input+16},64),memory.load(GuestAddress{input+24},64),static_cast<std::uint32_t>(memory.load(GuestAddress{input+32},32)));
    }else if(kind==4){
        if(!(file.access&0x10000U))return 5;error=filesystem_.set_disposition(*file.native,file.path,memory.load(GuestAddress{input},8)!=0);
    }else if(kind==5 || kind==6){
        if(!(file.access&(0x40000000U|2U)))return 5;const auto length=memory.load(GuestAddress{input},64);error=kind==5?file.native->allocate(length):file.native->truncate(length);
    }else {
        if(!(file.access&0x10000U))return 5;
        const auto name_offset=x86_?12U:20U,length_offset=x86_?8U:16U,root_offset=x86_?4U:8U;
        const auto length=memory.load(GuestAddress{input+length_offset},32),root=memory.load(GuestAddress{input+root_offset},x86_?32:64);
        if(!length || (length&1U) || length>size-name_offset || length>65534)return 87;
        std::u16string name(static_cast<std::size_t>(length/2),0);for(std::size_t i=0;i<name.size();++i)name[i]=static_cast<char16_t>(memory.load(GuestAddress{input+name_offset+2*i},16));if(name.find(u'\0')!=name.npos)return 123;
        auto destination=utf16_to_utf8(name);std::replace(destination.begin(),destination.end(),'\\','/');
        const auto current=filesystem_.final_path(*file.native,file.path);if(current.error)return current.error;
        if(root){const auto directory=files_.find(root);if(directory==files_.end() || !directory->second.native || !(directory->second.metadata.attributes&0x10U))return 6;const auto parent=filesystem_.final_path(*directory->second.native,directory->second.path);if(parent.error)return parent.error;if(destination.find(':')!=destination.npos || destination.front()=='/')return 87;destination=parent.key+"/"+destination;}
        else if(destination.find(':')==destination.npos && destination.front()!='/')destination=current.key.substr(0,current.key.find_last_of('/')+1)+destination;
        const auto original=current.metadata;
        error=filesystem_.rename(current.key,destination,memory.load(GuestAddress{input},8)!=0,false,false,file.native.get());
        if(!error){const auto updated=filesystem_.final_path(*file.native,destination);if(updated.error)return updated.error;
            for(auto& [other,entry]:files_){(void)other;if(entry.metadata.id==original.id && entry.metadata.device==original.device){entry.path=updated.key;entry.key=updated.key;for(auto& c:entry.key)if(c>='A' && c<='Z')c=static_cast<char>(c+32);entry.directory_loaded=false;}}
        }
    }
    if(!error)file.metadata=file.native->information().metadata;return error;
}
std::optional<std::uint32_t> WindowsServices::invoke_file_control(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const auto finish=[&](std::uint64_t result){arg.return_to(state,result);return std::optional<std::uint32_t>{};};
    const auto failed=[&](std::uint32_t error){last_error_=error;return finish(name=="ReOpenFile"?(x86_?UINT32_MAX:UINT64_MAX):0);};
    if(name=="SetFileInformationByHandle"){const auto error=set_file_information(memory,arg(0),static_cast<std::uint32_t>(arg(1)),arg(2),static_cast<std::uint32_t>(arg(3)));return error?failed(error):finish(1);}
    const auto found=files_.find(arg(0));if(found==files_.end() || (!found->second.native && !found->second.identity))return failed(6);auto& file=found->second;const auto original=file.native?file.native:file.identity;
    try{
        if(name=="SetFileValidData"){
            const auto error=set_file_valid_data(arg(0),arg(1));return error?failed(error):finish(1);
        }
        if(name=="ReOpenFile"){
            const auto access=static_cast<std::uint32_t>(arg(1)),sharing=static_cast<std::uint32_t>(arg(2)),flags=static_cast<std::uint32_t>(arg(3));
            if(sharing&~7U)return failed(87);if((access&~0xc01f01bfU) || (flags&~0xde200000U))return failed(50);if(files_.size()>=1024)return failed(4);
            const auto category=[](std::uint32_t rights){return ((rights&(0x80000000U|1U|0x20U))?1U:0U)|((rights&(0x40000000U|2U|4U))?2U:0U)|((rights&0x10000U)?4U:0U);};const auto requested=access|((flags&0x04000000U)?0x10000U:0U);
            for(const auto& [other,peer]:files_){(void)other;if(peer.metadata.id==file.metadata.id && peer.metadata.device==file.metadata.device && ((category(requested)&~peer.sharing) || (category(peer.access)&~sharing)))return failed(32);}
            const auto opened=filesystem_.reopen(*original,file.path,access,sharing,flags);if(opened.error)return failed(opened.error);
            FileHandle replacement;replacement.key=opened.key;if(file.native)replacement.native=opened.file;else replacement.identity=opened.file;replacement.metadata=file.native?opened.metadata:file.metadata;replacement.bytes=file.native?std::make_shared<const std::vector<std::uint8_t>>():file.bytes;replacement.path=opened.key;replacement.sharing=sharing;replacement.access=requested;replacement.mode=(flags&0x40000000U)?0U:0x20U;
            refresh_file_security_context();replacement.manage_volume_at_open=runtime_.token_objects.at(effective_token_id()).has_privilege(28,true);
            const auto handle=runtime_.allocate_wait_handle();replacement.object=handle;runtime_.win_file_positions.emplace(handle,0);try{files_.emplace(handle,std::move(replacement));}catch(...){runtime_.win_file_positions.erase(handle);throw;}return finish(handle);
        }
        const auto flags=static_cast<std::uint32_t>(arg(3)),capacity=static_cast<std::uint32_t>(arg(2));const auto volume=flags&7U;if((flags&~15U) || (volume!=0 && volume!=1 && volume!=2 && volume!=4))return failed(87);
        const auto current=filesystem_.final_path(*original,file.path);if(current.error)return failed(current.error);auto path=utf8_to_utf16(current.key);std::replace(path.begin(),path.end(),u'/',u'\\');
        if(flags&8U){auto opened=utf8_to_utf16(file.path);std::replace(opened.begin(),opened.end(),u'/',u'\\');if(opened.size()>=3 && opened[1]==u':')path=std::move(opened);}
        const auto location=filesystem_.volume_location(current.key,true);
        if(volume==0){if(!windows_volume_letter(std::u16string_view(path))){if(path.size()<3 || path[1]!=u':')return failed(3);if(path[0]>=u'a' && path[0]<=u'z')path[0]=static_cast<char16_t>(path[0]-32);path=u"\\\\?\\"+path;}}
        else if(!location.error){auto relative=utf8_to_utf16(location.path);std::replace(relative.begin(),relative.end(),u'/',u'\\');if(volume==1)path=windows_volume_name(static_cast<char16_t>(location.volume))+relative;else if(volume==2)path=windows_device_name(static_cast<char16_t>(location.volume))+u"\\"+relative;else path=u"\\"+relative;}
        else {if(path.size()<3 || path[1]!=u':')return failed(location.error);auto id=path[0];const auto ns=filesystem_.volume_state();char drive=static_cast<char>(id);if(drive>='a' && drive<='z')drive=static_cast<char>(drive-32);if(const auto alias=ns.drives.find(drive);alias!=ns.drives.end())id=static_cast<char16_t>(alias->second);if(volume==1)path=windows_volume_name(id)+path.substr(3);else if(volume==2)path=windows_device_name(id)+path.substr(2);else path=path.substr(2);}
        const bool wide=name.ends_with('W');
        auto bytes=wide?utf16_bytes(path):std::vector<std::uint8_t>{};if(!wide){const auto text=utf16_to_utf8(path);bytes.assign(text.begin(),text.end());bytes.push_back(0);}
        const auto required=wide?bytes.size()/2:bytes.size();if(capacity<required)return finish(required);if(!arg(1))return failed(87);memory.write(GuestAddress{arg(1)},bytes);return finish(required-1);
    }catch(const std::bad_alloc&){return failed(8);}catch(const std::invalid_argument&){return failed(123);}
}
}
