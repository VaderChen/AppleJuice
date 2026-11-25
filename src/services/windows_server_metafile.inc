// 工作階段的中繼檔使用共用配置序號及不可變的快照；客體指標僅在發出呼叫的程序解讀。
#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

namespace x64arm {
namespace {
constexpr std::uint32_t server_emf=0x5845464d,server_picture=0x5f50464d;
constexpr std::uint32_t server_type=0x00150000;
bool server_handle(std::uint64_t value){return value<=UINT32_MAX && (value&0x00ff0000)==server_type;}
bool server_tag(std::uint32_t tag){return tag==server_emf || tag==server_picture;}
std::string server_path(const std::string& session,std::uint64_t handle){
    return session+"/gdi-metafile-"+std::to_string(handle);
}
#if !defined(_WIN32)
struct MetafileDescriptor {
    int value;
    ~MetafileDescriptor(){if(value>=0)::close(value);}
};
bool transfer_metafile(int fd,void* bytes,std::size_t size,bool write){
    auto data=static_cast<std::uint8_t*>(bytes);
    while(size){
        const auto count=write ? ::write(fd,data,size) : ::read(fd,data,size);
        if(count<0 && errno==EINTR)continue;
        if(count<=0)return false;
        data+=count;size-=static_cast<std::size_t>(count);
    }
    return true;
}
bool publish_metafile(const std::string& path,MetafileState::ServerFile& file){
    const auto staging=path+".pending";
    MetafileDescriptor fd{::open(staging.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600)};
    if(fd.value<0)return false;
    const bool ok=transfer_metafile(fd.value,file.attributes.data(),sizeof(file.attributes),true) &&
                  transfer_metafile(fd.value,file.bytes.data(),file.bytes.size(),true) &&
                  ::rename(staging.c_str(),path.c_str())==0;
    if(!ok)::unlink(staging.c_str());
    return ok;
}
std::size_t read_server_metafile(const std::string& path,MetafileState::ServerFile& file,bool payload,std::optional<std::size_t> expected){
    MetafileDescriptor fd{::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK)};
    struct stat info{};
    constexpr auto header=sizeof(file.attributes);
    if(fd.value<0 || ::fstat(fd.value,&info) || !S_ISREG(info.st_mode) || info.st_uid!=::geteuid() ||
       (info.st_mode&077) || info.st_size<=static_cast<off_t>(header) ||
       info.st_size>static_cast<off_t>(header+windows_metafile_limit))return 0;
    const auto size=static_cast<std::size_t>(info.st_size)-header;
    if((expected && size!=*expected) || !transfer_metafile(fd.value,file.attributes.data(),header,false) ||
       !server_tag(file.attributes[0]))return 0;
    if(payload){
        file.bytes.resize(size);
        if(!transfer_metafile(fd.value,file.bytes.data(),size,false))return 0;
    }
    return size;
}
#endif
}
bool WindowsServices::delete_server_metafile(std::uint64_t handle){
    if(!server_handle(handle))return false;
#if !defined(_WIN32)
    if(runtime_.network.external_io)return ::unlink(server_path(mapping_session(),handle).c_str())==0;
#endif
    return metafile_playback_ && metafile_playback_->server_files.erase(static_cast<std::uint32_t>(handle));
}

std::uint64_t WindowsServices::server_metafile_call(GuestState& state,GuestMemory& memory,const WindowsCall& arg,std::string_view name){
    const std::uint8_t pointer_bits=x86_?32:64;
    const unsigned offset=x86_?12:16,picture_size=x86_?16:24;
    const auto api=[&](std::string_view dll,std::string_view symbol,std::initializer_list<std::uint64_t> args){
        return nested_service(state,memory,dll,symbol,args);
    };
    const auto gdi=[&](std::string_view symbol,std::initializer_list<std::uint64_t> args){return api("gdi32.dll",symbol,args);};
    const auto global=[&](std::string_view symbol,std::initializer_list<std::uint64_t> args){return api("kernel32.dll",symbol,args);};
    const auto fail=[&](std::uint32_t code)->std::uint64_t{last_error_=code;return 0;};
    MetafileState::ServerFile file;
    const auto handle=arg(0);
    const bool native_create=name=="NtGdiCreateServerMetaFile",picture_input=name=="GdiConvertMetaFilePict";
    if(native_create || picture_input || name=="GdiConvertEnhMetaFile"){
        if(native_create){
            const auto size=static_cast<std::uint32_t>(arg(1));
            if(!server_tag(static_cast<std::uint32_t>(handle)) || !arg(2) || !size || size>windows_metafile_limit)return 0;
            file.attributes={static_cast<std::uint32_t>(handle),static_cast<std::uint32_t>(arg(3)),static_cast<std::uint32_t>(arg(4)),static_cast<std::uint32_t>(arg(5))};
            file.bytes.resize(size);memory.read(GuestAddress{arg(2)},file.bytes);
        }else if(picture_input){
            if(global("GlobalSize",{handle})<picture_size)return fail(6);
            const auto ptr=global("GlobalLock",{handle});if(!ptr)return 0;
            std::uint64_t wmf{};
            try{
                file.attributes[0]=server_picture;
                for(unsigned i=0;i<3;++i)file.attributes[i+1]=static_cast<std::uint32_t>(memory.load(GuestAddress{ptr+4*i},32));
                wmf=memory.load(GuestAddress{ptr+offset},pointer_bits);
            }catch(...){global("GlobalUnlock",{handle});throw;}
            global("GlobalUnlock",{handle});
            const auto it=gui_.windows_metafiles.find(wmf);if(it==gui_.windows_metafiles.end())return fail(6);
            file.bytes=it->second;
        }else{
            const auto it=gui_.metafiles.find(handle);if(it==gui_.metafiles.end())return fail(6);
            file.attributes[0]=server_emf;file.bytes=it->second.bytes;
        }
        const auto make_handle=[](std::uint32_t id){return server_type|(id&0xffff)|((id&0xff0000)<<8);};
#if !defined(_WIN32)
        if(runtime_.network.external_io){
            const auto [id,error]=allocate_luid();
            if(error)return fail(error);
            const auto sequence=id-UINT64_C(0x100000000);
            if(sequence>0xffffff)return fail(1450);
            const auto object=make_handle(static_cast<std::uint32_t>(sequence));
            if(!publish_metafile(server_path(mapping_session(),object),file))return fail(29);
            return object;
        }
#endif
        if(!metafile_playback_)metafile_playback_=std::make_shared<MetafileState>();
        if(metafile_playback_->next_server_file>0xffffff)return fail(1450);
        const auto object=make_handle(metafile_playback_->next_server_file++);
        metafile_playback_->server_files.emplace(object,std::move(file));return object;
    }

    if(!server_handle(handle))return 0;
    const bool native_get=name=="NtGdiGetServerMetaFileBits";
    const auto requested=native_get?static_cast<std::uint32_t>(arg(1)):0;
    const bool payload=!native_get || requested!=0;
    std::size_t size{};
#if !defined(_WIN32)
    if(runtime_.network.external_io){
        size=read_server_metafile(server_path(mapping_session(),handle),file,payload,requested?std::optional<std::size_t>(requested):std::nullopt);
    }else
#endif
    if(metafile_playback_){
        const auto it=metafile_playback_->server_files.find(static_cast<std::uint32_t>(handle));
        if(it!=metafile_playback_->server_files.end()){
            size=it->second.bytes.size();
            if(requested && requested!=size)return 0;
            if(payload)file=it->second;
        }
    }
    if(!size)return 0;
    if(native_get){
        if(!payload)return size;
        for(unsigned i=0;i<4;++i)memory.check(GuestAddress{arg(3+i)},4,Access::Write);
        memory.check(GuestAddress{arg(2)},size,Access::Write);
        for(unsigned i=0;i<4;++i)memory.store(GuestAddress{arg(3+i)},32,file.attributes[i]);
        memory.write(GuestAddress{arg(2)},file.bytes);return size;
    }
    const bool enhanced=file.attributes[0]==server_emf;
    const bool want_emf=name=="GdiCreateLocalEnhMetaFile";
    if(enhanced && want_emf)return import_enhanced_metafile(std::move(file.bytes)).value_or(0);

    GuestScratch scratch(*this,memory,state);
    const auto allocate=[&](std::size_t bytes){const auto p=scratch.allocate(bytes);if(!p)throw std::bad_alloc();return p;};
    const auto store_picture=[&](std::uint64_t p,std::uint64_t wmf){
        for(unsigned i=0;i<3;++i)memory.store(GuestAddress{p+4*i},32,file.attributes[i+1]);
        memory.store(GuestAddress{p+offset},pointer_bits,wmf);
    };
    std::uint64_t wmf{};
    if(enhanced){
        const auto emf=import_enhanced_metafile(std::move(file.bytes));if(!emf)return fail(13);
        try{
            const auto& bytes=gui_.metafiles.at(*emf).bytes;
            file.attributes={server_picture,8,emf_get(bytes,32)-emf_get(bytes,24),emf_get(bytes,36)-emf_get(bytes,28)};
            const auto count=gdi("GetWinMetaFileBits",{*emf,0,0,8,0});
            if(count){
                const auto data=allocate(count);
                if(gdi("GetWinMetaFileBits",{*emf,count,data,8,0})==count)wmf=gdi("SetMetaFileBitsEx",{count,data});
            }
        }catch(...){gui_.metafiles.erase(*emf);throw;}
        gui_.metafiles.erase(*emf);
    }else{
        const auto data=allocate(size);memory.write(GuestAddress{data},file.bytes);
        if(want_emf){
            const auto info=allocate(picture_size);store_picture(info,0);
            return gdi("SetWinMetaFileBits",{size,data,0,info});
        }
        wmf=gdi("SetMetaFileBitsEx",{size,data});
    }
    if(!wmf)return 0;
    std::uint64_t result{},ptr{};
    try{
        result=global("GlobalAlloc",{0x2042,picture_size});
        if(result)ptr=global("GlobalLock",{result});
        if(ptr)store_picture(ptr,wmf);
    }catch(...){
        if(ptr)global("GlobalUnlock",{result});
        if(result)global("GlobalFree",{result});
        gui_.windows_metafiles.erase(wmf);throw;
    }
    if(!ptr){
        if(result)global("GlobalFree",{result});
        gui_.windows_metafiles.erase(wmf);return 0;
    }
    global("GlobalUnlock",{result});return result;
}
}
