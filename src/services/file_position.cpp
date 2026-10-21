#include "x64arm/file_position.hpp"
#include "x64arm/host_services.hpp"
#include <array>
#include <algorithm>
#include <cerrno>
#include <mutex>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace x64arm {
namespace {
constexpr std::uint64_t signature=UINT64_C(0x32534f5046523458);
struct PositionHeader {
    std::uint64_t magic{signature},identity{},device{},inode{},position{};
    std::uint32_t access{},sharing{},mode{},readonly{},path_size{};
    std::array<char,131072> path{};
    std::uint64_t object{};
};
std::string record_name(std::uint64_t identity){return "file-position-"+std::to_string(identity);}
}
struct SharedFilePosition {
    int directory{-1},descriptor{-1};PositionHeader* data{};
    std::recursive_mutex mutex;unsigned depth{};
    ~SharedFilePosition(){
#if !defined(_WIN32)
        if(data)::munmap(data,sizeof(PositionHeader));
        if(descriptor>=0)::close(descriptor);if(directory>=0)::close(directory);
#endif
    }
};
namespace {
struct PositionLock {
    std::shared_ptr<SharedFilePosition> storage;
    std::unique_lock<std::recursive_mutex> local;
    explicit PositionLock(std::shared_ptr<SharedFilePosition> value):storage(std::move(value)),local(storage->mutex){
#if !defined(_WIN32)
        if(!storage->depth)while(::flock(storage->descriptor,LOCK_EX)<0)if(errno!=EINTR)throw HostCallFault("無法鎖定共用檔案游標");
#endif
        ++storage->depth;
    }
    ~PositionLock(){
#if !defined(_WIN32)
        if(!--storage->depth)(void)::flock(storage->descriptor,LOCK_UN);
#endif
    }
};
#if !defined(_WIN32)
std::uint32_t open_directory(SharedFilePosition& storage,const std::string& session){
    storage.directory=::open(session.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);struct stat info{};
    if(storage.directory<0 || ::fstat(storage.directory,&info) || info.st_uid!=::geteuid() || (info.st_mode&077))return 5;
    return 0;
}
std::uint32_t map_record(SharedFilePosition& storage){
    struct stat info{};if(::fstat(storage.descriptor,&info) || !S_ISREG(info.st_mode) || info.st_uid!=::geteuid() || (info.st_mode&077) || info.st_size!=sizeof(PositionHeader))return 6;
    const auto data=::mmap(nullptr,sizeof(PositionHeader),PROT_READ|PROT_WRITE,MAP_SHARED,storage.descriptor,0);if(data==MAP_FAILED)return 8;
    storage.data=static_cast<PositionHeader*>(data);
    if(storage.data->magic!=signature || !storage.data->identity || storage.data->position>INT64_MAX || storage.data->sharing>7 || storage.data->readonly>3 || storage.data->path_size>=storage.data->path.size())return 6;
    return 0;
}
#endif
}
std::shared_ptr<void> FilePosition::lock()const{return shared_?std::make_shared<PositionLock>(shared_):std::shared_ptr<void>{};}
FilePosition::operator std::uint64_t()const{const auto guard=lock();return shared_?shared_->data->position:local_;}
FilePosition& FilePosition::operator=(std::uint64_t value){const auto guard=lock();if(shared_)shared_->data->position=value;else local_=value;return *this;}
std::uint64_t FilePosition::identity()const{return shared_?shared_->data->identity:0;}
std::uint32_t FilePosition::publish(const std::string& session,std::uint64_t identity,const FileTransferMetadata& metadata){
#if defined(_WIN32)
    (void)session;(void)identity;(void)metadata;return 50;
#else
    if(shared_){
        const auto guard=lock();auto& data=*shared_->data;
        if(data.device!=metadata.device || data.inode!=metadata.inode || data.sharing!=metadata.sharing || data.mode!=metadata.mode || data.readonly!=(static_cast<std::uint32_t>(metadata.readonly)|(metadata.manage_volume_at_open?2U:0U)) || data.object!=metadata.object)return 6;
        // 窄權限 handle 可先輸出；同 FILE_OBJECT 的後續合法 handle 擴充能力上限。
        // 接收端每個 handle 仍保存自身 rights，不取得其他 handle 的權限。
        data.access|=metadata.access;return 0;
    }
    if(!identity || metadata.path.size()>=PositionHeader{}.path.size())return 206;
    auto storage=std::make_shared<SharedFilePosition>();if(const auto error=open_directory(*storage,session))return error;
    const auto name=record_name(identity);storage->descriptor=::openat(storage->directory,name.c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);if(storage->descriptor<0)return errno==EACCES?5U:8U;
    auto header=std::make_unique<PositionHeader>();header->identity=identity;header->device=metadata.device;header->inode=metadata.inode;header->position=local_;header->access=metadata.access;header->sharing=metadata.sharing;header->mode=metadata.mode;header->readonly=static_cast<std::uint32_t>(metadata.readonly)|(metadata.manage_volume_at_open?2U:0U);header->object=metadata.object;header->path_size=static_cast<std::uint32_t>(metadata.path.size());std::copy(metadata.path.begin(),metadata.path.end(),header->path.begin());
    if(::ftruncate(storage->descriptor,sizeof(PositionHeader)) || ::pwrite(storage->descriptor,header.get(),sizeof(PositionHeader),0)!=sizeof(PositionHeader)){(void)::unlinkat(storage->directory,name.c_str(),0);return 8;}
    if(const auto error=map_record(*storage)){(void)::unlinkat(storage->directory,name.c_str(),0);return error;}
    shared_=std::move(storage);return 0;
#endif
}
std::pair<FilePosition,std::uint32_t> FilePosition::inherit(const std::string& session,std::uint64_t identity,int file_descriptor,FileTransferMetadata& metadata){
#if defined(_WIN32)
    (void)session;(void)identity;(void)file_descriptor;(void)metadata;return {FilePosition{},50};
#else
    auto storage=std::make_shared<SharedFilePosition>();if(const auto error=open_directory(*storage,session))return {FilePosition{},error};
    storage->descriptor=::openat(storage->directory,record_name(identity).c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW);if(storage->descriptor<0)return {FilePosition{},6};
    if(const auto error=map_record(*storage))return {FilePosition{},error};
    PositionLock guard(storage);const auto& data=*storage->data;struct stat info{};
    if(data.identity!=identity || ::fstat(file_descriptor,&info) || !S_ISREG(info.st_mode) || data.device!=static_cast<std::uint64_t>(info.st_dev) || data.inode!=static_cast<std::uint64_t>(info.st_ino))return {FilePosition{},6};
    metadata={data.device,data.inode,data.access,data.sharing,data.mode,(data.readonly&1)!=0,std::string(data.path.data(),data.path_size),data.object,(data.readonly&2)!=0};
    FilePosition result;result.shared_=std::move(storage);return {std::move(result),0};
#endif
}
}
