#include "directory_watch.hpp"
#include "directory_watch_index.hpp"
#include "fast_poll.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <set>
#if defined(__APPLE__)
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#endif

namespace x64arm {
#if defined(__APPLE__)
namespace {
struct Descriptor {
    int value{-1};
    ~Descriptor(){if(value>=0)::close(value);}
};
std::uint32_t watch_error() {
    switch(errno){case EACCES:case EPERM:return 5;case ENOMEM:return 8;
    case EMFILE:case ENFILE:return 4;case ENOENT:return 2;case ENOTDIR:return 267;default:return 1117;}
}
struct Node {
    std::shared_ptr<HostFile> file;
    FileMetadata info;
    std::uint32_t mode{},uid{},gid{};
    std::vector<std::uint8_t> acl;
};
class MacDirectoryWatch final : public DirectoryWatch {
    Descriptor queue_;
    std::shared_ptr<HostFile> root_;
    std::map<std::string,Node> nodes_;
    std::vector<std::uint8_t> pending_;
    std::size_t last_record_{};
    std::uint32_t filter_,capacity_,notification_error_{};
    unsigned information_class_;
    bool subtree_,overflow_{},rescan_{};
    std::chrono::steady_clock::time_point next_scan_{},next_poll_{};
    using Identity=std::pair<std::uint64_t,std::uint64_t>;
    static Identity identity(const Node& n){return {n.info.device,n.info.id};}
    struct Watched {
        Descriptor descriptor;
        std::shared_ptr<HostFile> file;
        std::string path;
    };
    std::map<std::string,std::shared_ptr<Watched>> watched_;
    bool collect(int directory,const std::string& prefix,std::map<std::string,Node>& output,
                 std::map<std::string,std::shared_ptr<Watched>>& watches,std::uint32_t& error,unsigned depth) {
        if(depth>128){error=8;return false;}
        const int copy=::openat(directory,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        if(copy<0){error=watch_error();return false;}
        struct Stream {DIR* value;~Stream(){if(value)::closedir(value);}} stream{::fdopendir(copy)};
        if(!stream.value){::close(copy);error=watch_error();return false;}
        for(;;) {
            errno=0;const auto* entry=::readdir(stream.value);
            if(!entry){if(errno){error=watch_error();return false;}break;}
            const std::string leaf=entry->d_name;if(leaf=="." || leaf=="..")continue;
            if(output.size()>=4096){error=8;return false;}
            const auto path=prefix+leaf;
            struct stat info{};
            if(::fstatat(directory,leaf.c_str(),&info,AT_SYMLINK_NOFOLLOW)){error=watch_error();return false;}
            // 不追蹤 symlink 目標，也不開啟 device／FIFO。名稱本身仍可通知。
            if(!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode) && !S_ISLNK(info.st_mode))continue;
            auto old=nodes_.find(path);std::shared_ptr<Watched> watched;
            if(old!=nodes_.end() && old->second.info.id==info.st_ino && old->second.info.device==static_cast<std::uint64_t>(info.st_dev))watched=watched_.at(path);
            else {
                Descriptor opened{::openat(directory,leaf.c_str(),O_RDONLY|O_CLOEXEC|O_NONBLOCK|(S_ISLNK(info.st_mode)?O_SYMLINK:O_NOFOLLOW))};
                if(opened.value<0){error=watch_error();return false;}
                auto file=std::make_shared<HostFile>(opened.value);opened.value=-1;
                watched=std::make_shared<Watched>();watched->file=std::move(file);watched->path=path;
                watched->descriptor.value=watched->file->duplicate_descriptor();
                if(watched->descriptor.value<0){error=watch_error();return false;}
                struct kevent change{};EV_SET(&change,static_cast<uintptr_t>(watched->descriptor.value),EVFILT_VNODE,EV_ADD|EV_CLEAR,
                    NOTE_WRITE|NOTE_EXTEND|NOTE_ATTRIB|NOTE_DELETE|NOTE_RENAME|NOTE_LINK|NOTE_REVOKE,0,nullptr);
                if(::kevent(queue_.value,&change,1,nullptr,0,nullptr)<0){error=watch_error();return false;}
            }
            const auto data=watched->file->information();
            if(data.error){error=data.error;return false;}
            Node node;node.file=watched->file;node.info=data.metadata;node.mode=info.st_mode;node.uid=info.st_uid;node.gid=info.st_gid;
            if(filter_&0x100) {
                const auto count=::fgetxattr(watched->descriptor.value,"com.apple.system.Security",nullptr,0,0,0);
                if(count>0 && count<=65536){node.acl.resize(static_cast<std::size_t>(count));if(::fgetxattr(watched->descriptor.value,"com.apple.system.Security",node.acl.data(),node.acl.size(),0,0)!=count){error=1022;return false;}}
                else if(count>65536){error=8;return false;}
            }
            output.emplace(path,std::move(node));watches.emplace(path,watched);
            if(subtree_ && (data.metadata.attributes&0x10) && !collect(watched->descriptor.value,path+"\\",output,watches,error,depth+1))return false;
        }
        return true;
    }
    void lose_details(std::uint32_t error=1022){pending_.clear();overflow_=true;notification_error_=error;}
    void append(std::uint32_t action,const std::string& path,const Node& node,const std::map<std::string,Node>& snapshot) {
        if(overflow_)return;
        const auto name=utf8_to_utf16(path);const auto header=information_class_==1?12U:84U;
        if(information_class_==3 && name.size()>UINT16_MAX/2){lose_details();return;}
        const auto size=(header+name.size()*2+3)&~std::size_t{3};
        if(size>capacity_ || pending_.size()>capacity_-size){lose_details(0);return;}
        const auto offset=pending_.size();
        // vector 的幾何擴張不得跨過首次要求的佇列容量。
        if(offset+size>pending_.capacity())pending_.reserve(std::min<std::size_t>(capacity_,std::max(offset+size,pending_.capacity()*2)));
        pending_.resize(offset+size);
        const auto store=[&](std::size_t at,std::uint32_t value){for(unsigned i=0;i<4;++i)pending_[at+i]=static_cast<std::uint8_t>(value>>(8*i));};
        const auto wide=[&](std::size_t at,std::uint64_t value){store(offset+at,static_cast<std::uint32_t>(value));store(offset+at+4,static_cast<std::uint32_t>(value>>32));};
        if(offset)store(last_record_,static_cast<std::uint32_t>(offset-last_record_));
        last_record_=offset;store(offset+4,action);
        if(information_class_==1)store(offset+8,static_cast<std::uint32_t>(name.size()*2));
        else {
            const auto& info=node.info;
            wide(8,info.created);wide(16,info.modified);wide(24,info.changed);wide(32,info.accessed);
            wide(40,info.allocation_size);wide(48,info.size);store(offset+56,info.attributes);store(offset+60,info.reparse_tag);wide(64,info.id);
            const auto split=path.rfind('\\');const auto parent=split==path.npos?snapshot.end():snapshot.find(path.substr(0,split));
            wide(72,parent==snapshot.end()?root_->information().metadata.id:parent->second.info.id);
            // Full 的長度為 USHORT；未提供 DOS 別名，名稱類型旗標與 Reserved 維持零。
            store(offset+80,static_cast<std::uint32_t>(name.size()*2));
        }
        for(std::size_t i=0;i<name.size();++i){pending_[offset+header+i*2]=static_cast<std::uint8_t>(name[i]);pending_[offset+header+1+i*2]=static_cast<std::uint8_t>(name[i]>>8);}
    }
public:
    MacDirectoryWatch(bool subtree,std::uint32_t filter,std::uint32_t capacity,unsigned information_class):filter_(filter),capacity_(capacity),information_class_(information_class),subtree_(subtree){}
    bool initialize(const HostFile& file,std::uint32_t& error) {
        queue_.value=::kqueue();if(queue_.value<0){error=watch_error();return false;}
        (void)::fcntl(queue_.value,F_SETFD,FD_CLOEXEC);
        Descriptor fd{file.duplicate_descriptor()};if(fd.value<0){error=watch_error();return false;}
        root_=std::make_shared<HostFile>(fd.value);fd.value=-1;
        auto root=std::make_shared<Watched>();root->file=root_;root->descriptor.value=root_->duplicate_descriptor();
        if(root->descriptor.value<0){error=watch_error();return false;}
        struct kevent change{};EV_SET(&change,static_cast<uintptr_t>(root->descriptor.value),EVFILT_VNODE,EV_ADD|EV_CLEAR,NOTE_WRITE|NOTE_DELETE|NOTE_REVOKE,0,nullptr);
        if(::kevent(queue_.value,&change,1,nullptr,0,nullptr)<0){error=watch_error();return false;}
        watched_.emplace("",root);
        return collect(root->descriptor.value,"",nodes_,watched_,error,0);
    }
    bool matches(bool subtree,std::uint32_t filter,unsigned information_class)const override{return subtree_==subtree && filter_==filter && information_class_==information_class;}
    std::size_t pending_bytes()const override{return pending_.size();}
    void poll() override try {
        // vnode 事件在 kqueue 內保留；同一毫秒的排程切片共用輪詢結果。
        const auto now=std::chrono::steady_clock::now();if(now<next_poll_)return;
        next_poll_=now+std::chrono::milliseconds(1);
        std::map<std::string,std::uint32_t> flags;bool lost=false;
        directory_detail::EventPaths paths(watched_,[](const auto& item){return static_cast<uintptr_t>(item->descriptor.value);});
        std::array<struct kevent,128> events;const timespec zero{}; // 只讀取核心回報的筆數，不需先清零 4 KB
        for(;;){
            // 沒有待處理的 vnode 事件時，select 約 0.2 µs；零逾時 kevent 約 13 µs。可讀或無法判定才取回事件。
            if(fast_poll::readable_now(queue_.value)==0)break;
            const int count=::kevent(queue_.value,nullptr,0,events.data(),static_cast<int>(events.size()),&zero);
            if(count<0){if(errno==EINTR)continue;lose_details();return;}if(!count)break;
            paths.visit(std::span(events.data(),static_cast<std::size_t>(count)),[](const auto& event){return event.ident;},[&](const auto& event,const auto* path){
                if(event.flags&EV_ERROR){lost=true;return;}
                if(path)flags[*path]|=event.fflags;
                if(event.fflags&NOTE_REVOKE)lost=true;
            });
        }
        // LAST_ACCESS 沒有 vnode 事件，仍定期比較原生時間；其他事件立即處理。
        if(!lost && flags.empty() && ((!rescan_ && !(filter_&0x20)) || now<next_scan_))return;
        next_scan_=now+std::chrono::milliseconds(20);
        std::map<std::string,Node> current;
        std::map<std::string,std::shared_ptr<Watched>> watches{{"",watched_.at("")}};
        std::uint32_t error=0;
        if(!collect(watched_.at("")->descriptor.value,"",current,watches,error,0)){lose_details();rescan_=true;return;}
        rescan_=false;
        const auto changes=directory_detail::snapshot_delta(nodes_,current);
        const auto& removed=changes.removed;const auto& added=changes.added;
        std::set<std::string_view> changed_directories;
        const auto parent=[](std::string_view path){const auto split=path.rfind('\\');return split==path.npos?std::string_view{}:path.substr(0,split);};
        const auto names=[&](const Node& node){return (filter_&((node.info.attributes&0x10)?2U:1U))!=0;};
        for(const auto& [id,items]:removed)for(const auto item:items){const auto& [path,node]=*item;
            changed_directories.insert(parent(path));const auto target=added.find(id);
            if(items.size()==1 && target!=added.end() && target->second.size()==1){
                if(names(node)){append(4,path,node,nodes_);const auto& moved=*target->second.front();append(5,moved.first,moved.second,current);}
            }else if(names(node))append(2,path,node,nodes_);
        }
        for(const auto& [id,items]:added)for(const auto item:items){const auto& [path,node]=*item;
            changed_directories.insert(parent(path));const auto source=removed.find(id);
            if(!(items.size()==1 && source!=removed.end() && source->second.size()==1) && names(node))append(1,path,node,current);
        }
        for(const auto& [path,node]:current){
            const auto found=nodes_.find(path);if(found==nodes_.end() || identity(node)!=identity(found->second))continue;
            const auto& old=found->second;const auto event=flags.find(path);const auto bits=event==flags.end()?0U:event->second;
            const bool modified=((filter_&4) && node.info.attributes!=old.info.attributes) ||
                ((filter_&8) && node.info.size!=old.info.size) ||
                ((filter_&0x10) && (node.info.modified!=old.info.modified || ((bits&NOTE_WRITE) && !(node.info.attributes&0x10)))) ||
                ((filter_&0x20) && node.info.accessed!=old.info.accessed) ||
                ((filter_&0x40) && node.info.created!=old.info.created) ||
                ((filter_&0x100) && (node.mode!=old.mode || node.uid!=old.uid || node.gid!=old.gid || node.acl!=old.acl));
            if(modified)append(3,path,node,current);
        }
        // kqueue 會合併事件。create+delete 等無法重建名稱時，要求重新列舉，不能吞掉事件。
        for(const auto& [path,bits]:flags){const auto node=nodes_.find(path);const bool directory=path.empty() || (node!=nodes_.end() && (node->second.info.attributes&0x10));
            if((filter_&3) && directory && (path.empty() || subtree_) && (bits&NOTE_WRITE) && !changed_directories.contains(path))lost=true;
            if(path.empty() && (bits&NOTE_DELETE))lost=true;
        }
        if(lost)lose_details();nodes_=std::move(current);watched_=std::move(watches);
    }catch(const std::bad_alloc&){lose_details();}catch(const std::invalid_argument&){lose_details();}
    std::optional<FileIoResult> take(std::span<std::uint8_t> output) override {
        if(overflow_){overflow_=false;return FileIoResult{0,notification_error_};}
        if(pending_.empty())return std::nullopt;
        const auto count=pending_.size();if(count<=output.size())std::copy(pending_.begin(),pending_.end(),output.begin());
        pending_.clear();return FileIoResult{count<=output.size()?count:0,0};
    }
};
}
#endif
std::shared_ptr<DirectoryWatch> DirectoryWatch::create(const HostFile& file,bool subtree,std::uint32_t filter,std::uint32_t capacity,std::uint32_t& error,unsigned information_class) try {
    if(information_class<1 || information_class>3){error=87;return {};}
#if defined(__APPLE__)
    auto result=std::make_shared<MacDirectoryWatch>(subtree,filter,capacity,information_class);
    if(result->initialize(file,error))return result;
    return {};
#else
    (void)file;(void)subtree;(void)filter;(void)capacity;error=50;return {};
#endif
}catch(const std::bad_alloc&){error=8;return {};}
}
