#include "x64arm/private_key_store.hpp"
#include "x64arm/readonly_files.hpp"
#include "x64arm/crypto.hpp"
#include "x64arm/file_security.hpp"
#include "windows_sddl.hpp"
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace x64arm {
namespace {
using Bytes=std::vector<std::uint8_t>;
constexpr std::uint32_t missing=0x80090016,exists=0x8009000f,invalid_data=0x80090005;
constexpr std::size_t limit=16U*1024U*1024U;
struct Failure {std::uint32_t code;};
struct Secret {Bytes bytes;~Secret(){OPENSSL_cleanse(bytes.data(),bytes.size());}};
void require(bool valid,std::uint32_t code=invalid_data){if(!valid)throw Failure{code};}
void check(std::uint32_t code){if(code)throw Failure{code==2 || code==3?missing:code};}
Bytes hash(std::span<const std::uint8_t> bytes){CryptoHash value("SHA256",false,{});value.update(bytes);return value.digest();}
std::string hex(std::span<const std::uint8_t> bytes){std::string value;value.reserve(bytes.size()*2);for(auto b:bytes){value+="0123456789abcdef"[b>>4];value+="0123456789abcdef"[b&15];}return value;}
std::string filename(std::string_view name){require(name.size()<=4096 && name.find('\0')==name.npos,87);return hex(hash({reinterpret_cast<const std::uint8_t*>(name.data()),name.size()}))+".key";}
Bytes descriptor(const PrivateKeyStoreContext& context,bool common,bool directory){
    // 共用目錄只允許列舉、穿越與新增子項；既有私鑰沿用獨立的保護 DACL。
    const auto flags=directory?"OICI":"";
    auto text="O:"+context.user_sid+"D:P(A;"+flags+";FA;;;"+context.user_sid+")(A;"+flags+";FA;;;SY)(A;"+flags+";FA;;;BA)";
    if(common)text+="(A;"+std::string(flags)+";"+(directory?"0x1200af":"FR")+";;;WD)";
    return sddl_descriptor(text);
}
std::string directory(const PrivateKeyStoreContext& context,std::uint32_t type,bool machine,bool create){
    (void)sddl_sid(context.user_sid);auto& files=context.files;
    std::string path="C:";
    const std::array<std::string,6> parts{"ProgramData","MacWin","Crypto","v1",machine?"machine":"user-"+context.user_sid,std::to_string(type)};
    for(std::size_t i=0;i<parts.size();++i){
        path+="/"+parts[i];auto info=files.information(path);
        if((info.error==2 || info.error==3) && create){const auto sd=descriptor(context,i<4 || machine,true);const auto status=files.create_directory(path,[&]{return sd;});if(status && status!=183)check(status);info=files.information(path);}
        check(info.error);require((info.metadata.attributes&0x410U)==0x10U,5);
    }
    return path;
}
struct Lock {
    std::shared_ptr<HostFile> file;int fd{-1};
    Lock(const PrivateKeyStoreContext& context,const std::string& parent,bool machine){
        const auto path=parent+"/.lock";auto opened=context.files.open_host(path,0x80000000U,3,0);
        if(opened.error==2 || opened.error==3){const auto sd=descriptor(context,machine,false);opened=context.files.open_host(path,0xc0000000U,1,0,7,[&]{return sd;});if(opened.error==80 || opened.error==183)opened=context.files.open_host(path,0x80000000U,3,0);}
        check(opened.error);file=std::move(opened.file);fd=file->duplicate_descriptor();require(fd>=0,5);
        struct stat info{};if(::fstat(fd,&info) || !S_ISREG(info.st_mode) || info.st_nlink!=1){::close(fd);fd=-1;throw Failure{5};}
        int result;do{result=::flock(fd,LOCK_EX);}while(result && errno==EINTR);if(result){::close(fd);fd=-1;throw Failure{32};}
    }
    ~Lock(){if(fd>=0){(void)::flock(fd,LOCK_UN);::close(fd);}}
};
void word(Bytes& out,std::uint32_t n){for(unsigned i=0;i<4;++i)out.push_back(static_cast<std::uint8_t>(n>>(8*i)));}
std::uint32_t word(std::span<const std::uint8_t> bytes,std::size_t at){std::uint32_t n=0;for(unsigned i=0;i<4;++i)n|=std::uint32_t(bytes[at+i])<<(8*i);return n;}
struct Record {std::string name;Secret data;};
Record read(const PrivateKeyStoreContext& context,const std::string& path){
    const auto info=context.files.information(path);check(info.error);require(!(info.metadata.attributes&0x410U) && info.metadata.links==1,5);
    auto opened=context.files.open_reader(path);check(opened.error);
    require(opened.metadata.size>=48 && opened.metadata.size<=limit+4096+48);Secret encoded;encoded.bytes.resize(static_cast<std::size_t>(opened.metadata.size));
    std::size_t at=0;while(at<encoded.bytes.size()){const auto io=opened.file->read(std::span(encoded.bytes).subspan(at),at);check(io.error);require(io.bytes!=0);at+=static_cast<std::size_t>(io.bytes);}
    const auto bytes=std::span(encoded.bytes);require(std::equal(bytes.begin(),bytes.begin()+8,"MWKSBOX1"));
    const auto names=word(bytes,8),payload=word(bytes,12);require(names<=4096 && payload<=limit && 48ULL+names+payload==bytes.size());
    const auto digest=hash(bytes.first(bytes.size()-32));require(CRYPTO_memcmp(digest.data(),bytes.data()+bytes.size()-32,32)==0);
    Record result;result.name.assign(reinterpret_cast<const char*>(bytes.data()+16),names);require(filename(result.name)==path.substr(path.find_last_of('/')+1));
    result.data.bytes.assign(bytes.begin()+16+names,bytes.end()-32);return result;
}
template<class F> std::uint32_t attempt(F&& body){try{body();return 0;}catch(const Failure& e){return e.code;}catch(const SecurityDescriptorError& e){return e.code;}catch(const std::bad_alloc&){return 8;}}
void commit(const PrivateKeyStoreContext& context,const std::string& parent,const std::string& name,bool machine,std::span<const std::uint8_t> bytes,bool create,PrivateKeyAccess access){
        require(bytes.size()<=limit,8);const auto path=parent+"/"+filename(name);const auto prior=context.files.information(path);
        if(create)require(prior.error!=0,exists);else check(prior.error);
        if(prior.error && prior.error!=2 && prior.error!=3)check(prior.error);
        if(!prior.error){require(!(prior.metadata.attributes&0x410U) && prior.metadata.links==1,5);check(context.files.security_access(path,0x40000000U));}
        Secret encoded;encoded.bytes={'M','W','K','S','B','O','X','1'};word(encoded.bytes,static_cast<std::uint32_t>(name.size()));word(encoded.bytes,static_cast<std::uint32_t>(bytes.size()));
        encoded.bytes.insert(encoded.bytes.end(),name.begin(),name.end());encoded.bytes.insert(encoded.bytes.end(),bytes.begin(),bytes.end());const auto digest=hash(encoded.bytes);encoded.bytes.insert(encoded.bytes.end(),digest.begin(),digest.end());
        std::array<std::uint8_t,16> random{};require(RAND_bytes(random.data(),static_cast<int>(random.size()))==1,8);const auto temporary=parent+"/.tmp-"+hex(random);
        const auto sd=prior.error?descriptor(context,machine && access==PrivateKeyAccess::LogonUser,false):security::relative_descriptor(security::file_descriptor(context.files.security_information(path,true),0x17));
        auto opened=context.files.open_host(temporary,0xc0010000U,1,0,7,[&]{return sd;});check(opened.error);
        try{
            require(::fchmod(opened.file->transfer_descriptor(),0600)==0,5);
            std::size_t at=0;while(at<encoded.bytes.size()){const auto io=opened.file->write(std::span(encoded.bytes).subspan(at),at);check(io.error);require(io.bytes!=0,29);at+=static_cast<std::size_t>(io.bytes);}
            check(opened.file->flush());opened.file.reset();check(context.files.rename(temporary,path,!create,false,true));
        }catch(...){opened.file.reset();(void)context.files.remove(temporary,false);throw;}
}
}
std::uint32_t private_key_store_read(const PrivateKeyStoreContext& context,std::uint32_t type,const std::string& name,bool machine,Bytes& output){
    return attempt([&]{const auto leaf=filename(name),parent=directory(context,type,machine,false);auto record=read(context,parent+"/"+leaf);require(record.name==name);output=std::move(record.data.bytes);});
}
std::uint32_t private_key_store_write(const PrivateKeyStoreContext& context,std::uint32_t type,const std::string& name,bool machine,std::span<const std::uint8_t> bytes,bool create,PrivateKeyAccess access){
    return attempt([&]{require(context.files.writable(),5);(void)filename(name);require(bytes.size()<=limit,8);const auto parent=directory(context,type,machine,true);Lock lock(context,parent,machine);commit(context,parent,name,machine,bytes,create,access);});
}
std::uint32_t private_key_store_modify(const PrivateKeyStoreContext& context,std::uint32_t type,const std::string& name,bool machine,const std::function<std::uint32_t(Bytes&)>& modify){
    return attempt([&]{require(context.files.writable(),5);const auto leaf=filename(name),parent=directory(context,type,machine,false);Lock lock(context,parent,machine);
        auto current=read(context,parent+"/"+leaf);require(current.name==name);check(modify(current.data.bytes));commit(context,parent,name,machine,current.data.bytes,false,PrivateKeyAccess::Application);
    });
}
std::uint32_t private_key_store_delete(const PrivateKeyStoreContext& context,std::uint32_t type,const std::string& name,bool machine){
    return attempt([&]{require(context.files.writable(),5);const auto leaf=filename(name),parent=directory(context,type,machine,false);Lock lock(context,parent,machine);check(context.files.remove(parent+"/"+leaf,false));});
}
std::uint32_t private_key_store_list(const PrivateKeyStoreContext& context,std::uint32_t type,bool machine,std::vector<std::string>& output){
    auto status=attempt([&]{const auto parent=directory(context,type,machine,false);const auto entries=context.files.list(parent);check(entries.error);std::vector<std::string> result;
        for(const auto& entry:entries.entries){if(!entry.name.ends_with(".key"))continue;require(result.size()<4096,8);try{auto value=read(context,parent+"/"+entry.name);result.push_back(std::move(value.name));}catch(const Failure& e){if(e.code!=5 && e.code!=missing)throw;}}
        std::sort(result.begin(),result.end());output=std::move(result);
    });if(status==missing){output.clear();return 0;}return status;
}
std::uint32_t private_key_store_security(const PrivateKeyStoreContext& context,std::uint32_t type,const std::string& name,bool machine,std::uint32_t access,const std::function<std::uint32_t(const HostFile&)>& operation){
    return attempt([&]{const auto parent=directory(context,type,machine,false),path=parent+"/"+filename(name);Lock lock(context,parent,machine);
        const auto info=context.files.information(path);check(info.error);require(!(info.metadata.attributes&0x410U) && info.metadata.links==1,5);
        check(context.files.security_access(path,access));const auto pinned=context.files.pin_security(path);check(pinned.error);
        require(pinned.metadata.device==info.metadata.device && pinned.metadata.id==info.metadata.id,1237);check(operation(*pinned.file));
    });
}
}
