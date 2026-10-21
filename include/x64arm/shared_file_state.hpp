#pragma once
#include "x64arm/readonly_files.hpp"
namespace x64arm {
class SharedFileLease;
class SharedFileState : public std::enable_shared_from_this<SharedFileState> {
    struct Storage;std::unique_ptr<Storage> storage_;
    explicit SharedFileState(std::unique_ptr<Storage>);
    friend class SharedFileLease;
    void release(std::uint64_t);
public:
    ~SharedFileState();
    static std::pair<std::shared_ptr<SharedFileState>,std::uint32_t> open(const std::string&,const FileMetadata&,bool create);
    std::uint32_t sharing(std::uint32_t,std::uint32_t,std::uint64_t ignore=0,bool allow_pending=false);
    bool pending();
    std::pair<std::shared_ptr<SharedFileLease>,std::uint32_t> acquire(std::uint64_t,std::uint32_t,std::uint32_t,bool existing=false,std::uint32_t close_target=0);
    std::pair<std::uint32_t,std::uint32_t> target(int,std::string_view,const FileMetadata&,bool directory);
    std::uint32_t disposition(std::uint32_t,bool,std::uint64_t object=0);
    std::uint32_t close_target(std::uint64_t,std::uint32_t);
    std::uint32_t mutate_size(const std::function<std::uint32_t()>&);
    // 同一 Sandbox 的 FILE_OBJECT 共用範圍鎖，不取決於宿主描述元的讀寫權限。
    std::uint32_t range_conflict(std::uint64_t object,std::uint64_t offset,std::uint64_t length,bool writing);
    std::uint32_t range_lock(std::uint64_t object,std::uint64_t offset,std::uint64_t length,bool exclusive,bool unlock,const std::function<std::uint32_t()>& native,bool adopt=false);
};
class SharedFileLease {
    std::shared_ptr<SharedFileState> state_;std::uint64_t identity_{};
    SharedFileLease(std::shared_ptr<SharedFileState> state,std::uint64_t identity):state_(std::move(state)),identity_(identity){}
    friend class SharedFileState;
public:
    ~SharedFileLease();
    std::uint64_t identity() const noexcept{return identity_;}
};
}
