#include "x64arm/registry_store.hpp"
#include "x64arm/security_descriptor.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <span>
#include <stdexcept>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace x64arm {
namespace {
constexpr std::size_t limit=32U*1024U*1024U;
struct Descriptor {int value{-1};~Descriptor(){if(value>=0)::close(value);}};
struct Reader {
    std::span<const std::uint8_t> data;std::size_t offset{};
    std::uint64_t number(unsigned size){if(size>data.size()-offset)throw std::runtime_error("登錄檔截斷");std::uint64_t value=0;for(unsigned i=0;i<size;++i)value|=std::uint64_t{data[offset++]}<<(i*8);return value;}
    std::u16string text(){const auto size=number(4);if(size>32768 || size>(data.size()-offset)/2)throw std::runtime_error("登錄字串超過範圍");std::u16string result;for(std::uint64_t i=0;i<size;++i)result.push_back(static_cast<char16_t>(number(2)));return result;}
};
void number(std::vector<std::uint8_t>& data,std::uint64_t value,unsigned size){for(unsigned i=0;i<size;++i)data.push_back(static_cast<std::uint8_t>(value>>(i*8)));}
void text(std::vector<std::uint8_t>& data,const std::u16string& value){number(data,value.size(),4);for(auto c:value)number(data,c,2);}
std::uint32_t io_error(){return errno==EACCES || errno==EPERM || errno==ELOOP?5:errno==ENOENT?3:29;}
}
RegistryTransaction::RegistryTransaction(RegistryState& state,const std::filesystem::path& path,RegistryStoreCache* cache,bool readonly):state_(state),path_(path),cache_(cache),readonly_(readonly){
    if(path.empty())return;
    lock_=::open((path.string()+".lock").c_str(),O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
    if(lock_<0){error=io_error();return;}
    if(::flock(lock_,LOCK_EX)){error=io_error();return;}
    Descriptor file{::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW)};
    if(file.value<0){if(errno!=ENOENT)error=io_error();return;}
    struct stat status{};if(::fstat(file.value,&status) || !S_ISREG(status.st_mode) || status.st_size<16 || static_cast<std::uint64_t>(status.st_size)>limit){error=13;return;}
    // 在鎖內對已開啟的檔案核對身分、大小與奈秒時間，原子替換及原地更新都會重載。
#if defined(__APPLE__)
    const auto modified=status.st_mtimespec,changed=status.st_ctimespec;
#else
    const auto modified=status.st_mtim,changed=status.st_ctim;
#endif
    const std::array<std::uint64_t,7> stamp{static_cast<std::uint64_t>(status.st_dev),static_cast<std::uint64_t>(status.st_ino),static_cast<std::uint64_t>(status.st_size),static_cast<std::uint64_t>(modified.tv_sec),static_cast<std::uint64_t>(modified.tv_nsec),static_cast<std::uint64_t>(changed.tv_sec),static_cast<std::uint64_t>(changed.tv_nsec)};
    if(cache_ && cache_->valid && cache_->owner==&state && cache_->path==path && cache_->stamp==stamp){++cache_->hits;return;}
    if(cache_)cache_->valid=false;
    std::vector<std::uint8_t> data(static_cast<std::size_t>(status.st_size));std::size_t read=0;
    while(read<data.size()){const auto n=::read(file.value,data.data()+read,data.size()-read);if(n<0 && errno==EINTR)continue;if(n<=0){error=13;return;}read+=static_cast<std::size_t>(n);}
    try {
        Reader reader{data};if(reader.number(8)!=UINT64_C(0x314745524d524158))throw std::runtime_error("未知登錄格式");
        const auto version=reader.number(4);if(version<1 || version>3)throw std::runtime_error("未知登錄版本");
        RegistryState loaded;const auto count=reader.number(4);if(count>4096)throw std::runtime_error("登錄索引過多");
        for(std::uint64_t i=0;i<count;++i){
            const auto key_path=reader.text();if(key_path.empty() || key_path[0]>6 || key_path[0]==4 || loaded.keys.contains(key_path))throw std::runtime_error("無效登錄索引");
            loaded.display_names[key_path]=reader.text();loaded.modified_times[key_path]=reader.number(8);auto& values=loaded.keys[key_path];const auto value_count=reader.number(4);
            if(value_count>4096-loaded.values)throw std::runtime_error("登錄值過多");loaded.values+=value_count;
            for(std::uint64_t j=0;j<value_count;++j){const auto original=reader.text();auto name=original;for(auto& c:name)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);if(values.contains(name))throw std::runtime_error("重複登錄值");RegistryValue value;if(original!=name)value.display_name=original;value.type=static_cast<std::uint32_t>(reader.number(4));const auto size=reader.number(4);
                if(size>16U*1024U*1024U-loaded.bytes || size>data.size()-reader.offset)throw std::runtime_error("登錄值超過範圍");loaded.bytes+=size;
                value.bytes.assign(data.begin()+static_cast<std::ptrdiff_t>(reader.offset),data.begin()+static_cast<std::ptrdiff_t>(reader.offset+size));reader.offset+=static_cast<std::size_t>(size);values.emplace(name,std::move(value));
            }
            if(version>=2){const auto size=reader.number(4);if(size>262144 || size>data.size()-reader.offset)throw std::runtime_error("安全描述元超過範圍");
                if(size){std::vector<std::uint8_t> descriptor(data.begin()+static_cast<std::ptrdiff_t>(reader.offset),data.begin()+static_cast<std::ptrdiff_t>(reader.offset+size));(void)security::parse_relative_descriptor(descriptor);loaded.security_descriptors.emplace(key_path,std::move(descriptor));}
                reader.offset+=static_cast<std::size_t>(size);}
            if(version>=3){auto class_name=reader.text();if(!class_name.empty())loaded.classes.emplace(key_path,std::move(class_name));const auto options=reader.number(4);if(options>1)throw std::runtime_error("登錄選項無效");if(options)loaded.volatile_keys.insert(key_path);}
        }
        if(version>=3){const auto mounts=reader.number(4);if(mounts>128)throw std::runtime_error("掛載 hive 過多");
            for(std::uint64_t i=0;i<mounts;++i){const auto mount_root=reader.text();RegistryHiveMount mount;mount.source=reader.text();mount.device=reader.number(8);mount.id=reader.number(8);mount.minor=static_cast<std::uint32_t>(reader.number(4));
                if(!loaded.keys.contains(mount_root) || loaded.hives.contains(mount_root) || mount.source.empty() || !mount.id || (mount.minor!=3 && mount.minor!=5))throw std::runtime_error("掛載 hive 紀錄無效");loaded.hives.emplace(mount_root,std::move(mount));}}
        if(reader.offset!=data.size())throw std::runtime_error("登錄檔有未解析資料");
        loaded.handles=std::move(state.handles);loaded.next_handle=state.next_handle;state=std::move(loaded);
        if(cache_){cache_->owner=&state;cache_->path=path;cache_->stamp=stamp;cache_->valid=true;++cache_->loads;}
    }catch(const SecurityDescriptorError&){error=13;}catch(const std::exception&){error=13;}
}
RegistryTransaction::~RegistryTransaction(){
    // 可寫操作即使失敗也可能改過暫存狀態；下一次必須以持久資料重新建立快照。
    if(cache_ && (!readonly_ || error))cache_->valid=false;
    if(lock_>=0){::flock(lock_,LOCK_UN);::close(lock_);}
}
std::uint32_t RegistryTransaction::commit(){
    if(error)return error;if(path_.empty())return 0;
    const unsigned version=state_.classes.empty() && state_.volatile_keys.empty() && state_.hives.empty()?2U:3U;
    const auto generated=[&](const std::u16string& path){return std::any_of(state_.generated_keys.begin(),state_.generated_keys.end(),[&](const auto& root){return path==root || path.starts_with(root+u"\\");});};
    const auto key_count=static_cast<std::uint64_t>(std::count_if(state_.keys.begin(),state_.keys.end(),[&](const auto& item){return !generated(item.first);}));
    std::vector<std::uint8_t> data;number(data,UINT64_C(0x314745524d524158),8);number(data,version,4);number(data,key_count,4);
    for(const auto& [path,values]:state_.keys){if(generated(path))continue;text(data,path);const auto display=state_.display_names.find(path);text(data,display==state_.display_names.end()?path.substr(path.find_last_of(u'\\')+1):display->second);const auto modified=state_.modified_times.find(path);number(data,modified==state_.modified_times.end()?0:modified->second,8);number(data,values.size(),4);
        for(const auto& [name,value]:values){text(data,value.display_name.empty()?name:value.display_name);number(data,value.type,4);number(data,value.bytes.size(),4);data.insert(data.end(),value.bytes.begin(),value.bytes.end());}
        const auto descriptor=state_.security_descriptors.find(path);const auto size=descriptor==state_.security_descriptors.end()?0:descriptor->second.size();
        if(size>262144)return 13;
        if(size){try{(void)security::parse_relative_descriptor(descriptor->second);}catch(const SecurityDescriptorError&){return 13;}}
        number(data,size,4);if(size)data.insert(data.end(),descriptor->second.begin(),descriptor->second.end());
        if(version>=3){const auto item=state_.classes.find(path);text(data,item==state_.classes.end()?std::u16string{}:item->second);number(data,state_.volatile_keys.contains(path)?1:0,4);}
        if(data.size()>limit)return 8;
    }
    if(version>=3){if(state_.hives.size()>128)return 8;number(data,state_.hives.size(),4);for(const auto& [path,mount]:state_.hives){if(!state_.keys.contains(path) || mount.source.empty() || !mount.id || (mount.minor!=3 && mount.minor!=5))return 13;text(data,path);text(data,mount.source);number(data,mount.device,8);number(data,mount.id,8);number(data,mount.minor,4);}}
    if(data.size()>limit)return 8;
    auto pattern=path_.string()+".tmp-XXXXXX";std::vector<char> temporary(pattern.begin(),pattern.end());temporary.push_back(0);Descriptor file{::mkstemp(temporary.data())};if(file.value<0)return io_error();
    std::uint32_t result=0;std::size_t written=0;
    while(written<data.size()){const auto n=::write(file.value,data.data()+written,data.size()-written);if(n<0 && errno==EINTR)continue;if(n<=0){result=io_error();break;}written+=static_cast<std::size_t>(n);}
    if(!result && ::fsync(file.value))result=io_error();
    if(!result && ::rename(temporary.data(),path_.c_str()))result=io_error();
    if(!result){Descriptor directory{::open(path_.parent_path().empty()?".":path_.parent_path().c_str(),O_RDONLY|O_CLOEXEC)};
        if(directory.value<0 || ::fsync(directory.value))result=io_error();}
    if(result)::unlink(temporary.data());return result;
}
}
