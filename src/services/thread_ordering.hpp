#pragma once

#include "x64arm/shared_event.hpp"
#include "x64arm/shared_wake.hpp"
#include "monotonic_clock.hpp"
#include "shared_object_lifetime.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <limits>
#include <memory>
#include <string>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace x64arm {
class ThreadOrderingContext {
public:
    using Guid = std::array<std::uint8_t,16>;
    struct Error {std::uint32_t code;};
    struct Turn {
        std::uint32_t error{};
        bool waiting{};
        std::uint64_t deadline{UINT64_MAX};
    };
    enum class Role : std::uint32_t { Before, Parent, After };

    static std::shared_ptr<ThreadOrderingContext> open(const std::string& session, const Guid& guid,
        std::shared_ptr<SharedWake> wake, std::uint64_t local_thread, std::uint32_t public_thread,
        Role role, std::uint64_t period=0, std::uint64_t timeout=0, int broker_priority=0);
    ~ThreadOrderingContext();
    std::uint64_t thread() const noexcept {return local_thread_;}
    bool parent() const noexcept {return role_==Role::Parent;}
    int broker_priority() const noexcept {return broker_priority_;}
    Turn turn(bool enter);
    std::uint32_t remove(bool exiting);

private:
    static constexpr std::uint64_t signature=UINT64_C(0x315244524f543458);
    static constexpr std::uint64_t liveness_interval=500000;
    static constexpr std::size_t page=4096;
    struct Header {
        std::uint64_t magic{},period{},timeout{},round{},sequence{},check_owners{},started{};
        std::uint32_t used{};
        std::int32_t broker_priority{};
        bool deleted{};
    };
    struct Member {
        std::uint64_t serial{},eligible{},granted{},completed{},expires{};
        std::uint32_t pid{},thread{};
        std::int32_t fd{};
        Role role{};
        bool waiting{};
    };

    ThreadOrderingContext()=default;
    static std::uint64_t add(std::uint64_t a, std::uint64_t b) {return b>UINT64_MAX-a?UINT64_MAX:a+b;}
    Header& header() const {return *static_cast<Header*>(view_);}
    Member* members() const {return reinterpret_cast<Member*>(static_cast<Header*>(view_)+1);}
    std::size_t capacity() const {return (length_-sizeof(Header))/sizeof(Member);}
    void map(std::size_t minimum, bool grow=false);
    bool alive(const Member&) const;
    bool advance(std::uint64_t now);
    void destroy();
    Member* member() const;

    std::shared_ptr<SharedWake> wake_;
    std::string name_;
    std::uint64_t local_thread_{},serial_{};
    std::uint32_t slot_{};
    Role role_{};
    int broker_priority_{};
    int directory_{-1},file_{-1};
#if !defined(_WIN32)
    struct stat identity_{};
#endif
    void* view_{};
    std::size_t length_{};
};

#if !defined(_WIN32)
inline ThreadOrderingContext::~ThreadOrderingContext() {
    if (view_) ::munmap(view_,length_);
    if (file_>=0) ::close(file_);
    if (directory_>=0) ::close(directory_);
}

inline void ThreadOrderingContext::map(std::size_t minimum, bool grow) {
    struct stat file_info{};
    if (::fstat(file_,&file_info) || !S_ISREG(file_info.st_mode) || file_info.st_uid!=::geteuid() ||
        (file_info.st_mode&077) || file_info.st_size<0) throw Error{5};
    auto length=static_cast<std::size_t>(file_info.st_size);
    if (length<minimum) {
        if (!grow || minimum>static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())-page) throw Error{8};
        length=(minimum+page-1)/page*page;
        if (::ftruncate(file_,static_cast<off_t>(length))) throw Error{8};
    }
    if (view_ && length_==length) return;
    void* const next=::mmap(nullptr,length,PROT_READ|PROT_WRITE,MAP_SHARED,file_,0);
    if (next==MAP_FAILED) throw Error{8};
    if (view_) ::munmap(view_,length_);
    view_=next;
    length_=length;
    identity_=file_info;
}

inline bool ThreadOrderingContext::alive(const Member& member) const {
    if (member.pid==static_cast<std::uint32_t>(::getpid())) {
        struct stat owner{};
        return !::fstat(member.fd,&owner) && owner.st_dev==identity_.st_dev && owner.st_ino==identity_.st_ino;
    }
#if defined(__APPLE__)
    return detail::named_object_fd_matches(static_cast<pid_t>(member.pid),member.fd,identity_);
#else
    struct stat owner{};
    const auto path="/proc/"+std::to_string(member.pid)+"/fd/"+std::to_string(member.fd);
    return !::stat(path.c_str(),&owner) && owner.st_dev==identity_.st_dev && owner.st_ino==identity_.st_ino;
#endif
}

inline void ThreadOrderingContext::destroy() {
    header().deleted=true;
    struct stat current{};
    if (!::fstatat(directory_,name_.c_str(),&current,AT_SYMLINK_NOFOLLOW) &&
        current.st_dev==identity_.st_dev && current.st_ino==identity_.st_ino)
        (void)::unlinkat(directory_,name_.c_str(),0);
}

inline bool ThreadOrderingContext::advance(std::uint64_t now) {
    auto& group=header();
    if (group.deleted) return false;
    if (group.used>capacity()) map(sizeof(Header)+static_cast<std::size_t>(group.used)*sizeof(Member));
    auto& current=header();
    auto* data=members();
    bool changed=false;
    const bool check_owners=now>=current.check_owners;
    if (check_owners) current.check_owners=add(now,liveness_interval);
    const auto next=add(current.round,current.period);
    const auto expiry=add(next,current.timeout);
    bool finished=true;
    for (std::uint32_t index=0;index<current.used;++index) {
        auto& entry=data[index];
        if (!entry.serial) continue;
        const bool incomplete=entry.eligible<=current.round && entry.completed!=current.round;
        const auto limit=entry.granted==current.round?entry.expires:expiry;
        if ((check_owners && !alive(entry)) || (incomplete && !entry.waiting && now>=limit)) {
            if (entry.role==Role::Parent) {
                destroy();
                return true;
            }
            entry.serial=0;
            changed=true;
        } else if (incomplete) {
            finished=false;
        }
    }
    if (finished && now>=next && next!=UINT64_MAX) {
        current.round+=(now-current.round)/current.period*current.period;
        changed=true;
    }
    return changed;
}

inline ThreadOrderingContext::Member* ThreadOrderingContext::member() const {
    if (header().deleted || slot_>=header().used || slot_>=capacity()) return nullptr;
    auto& entry=members()[slot_];
    return entry.serial==serial_?&entry:nullptr;
}

inline std::shared_ptr<ThreadOrderingContext> ThreadOrderingContext::open(const std::string& session,
    const Guid& guid, std::shared_ptr<SharedWake> wake, std::uint64_t local_thread,
    std::uint32_t public_thread, Role role, std::uint64_t period, std::uint64_t timeout, int broker_priority) {
    auto context=std::shared_ptr<ThreadOrderingContext>(new ThreadOrderingContext);
    context->local_thread_=local_thread;
    context->role_=role;
    context->wake_=std::move(wake);
    context->name_="thread-order-";
    static constexpr std::string_view hex="0123456789abcdef";
    for (const auto byte:guid) {
        context->name_+=hex[byte>>4];
        context->name_+=hex[byte&15];
    }
    context->directory_=::open(session.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if (context->directory_<0) throw Error{5};
    const SharedEventLock lock(context->directory_);
    bool created=false;
    const bool create=role==Role::Parent;
    const auto now=detail::monotonic_ticks();
    const auto erase_stale=[&] {
        // 只回收目前 GUID 的物件，且必須在共用命名空間鎖下重新核對檔案身分。
        if (context->view_) {
            context->destroy();
            (void)::munmap(context->view_,context->length_);
        } else {
            (void)::unlinkat(context->directory_,context->name_.c_str(),0);
        }
        context->view_=nullptr;
        context->length_=0;
        ::close(context->file_);
        context->file_=-1;
    };
    context->file_=::openat(context->directory_,context->name_.c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    if (context->file_>=0) {
        struct stat info{};
        if (::fstat(context->file_,&info) || !S_ISREG(info.st_mode) || info.st_uid!=::geteuid() ||
            (info.st_mode&077) || info.st_nlink!=1) throw Error{5};
        if (!info.st_size) erase_stale();  // 建立者可能在初始化前就異常結束。
        else {
            context->map(page);
            const auto& head=context->header();
            if (head.used && (head.magic!=signature || !head.period || head.used>context->capacity())) throw Error{13};
            if (!head.used || !context->members()[0].serial) erase_stale();
            else {
                if (context->advance(now) && context->wake_) context->wake_->notify();
                if (context->header().deleted || !context->alive(context->members()[0])) erase_stale();
                else if (create) throw Error{183};
            }
        }
    } else if (errno!=ENOENT) {
        throw Error{5};
    }
    if (context->file_<0) {
        if (!create) throw Error{2};
        context->file_=::openat(context->directory_,context->name_.c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
        if (context->file_<0) throw Error{errno==ENOSPC || errno==ENOMEM?8U:5U};
        created=true;
    }
    try {
        if (created) {
            context->map(page,true);
            context->header()={signature,period,timeout,now,0,add(now,liveness_interval),0,0,broker_priority,false};
        }
        const auto pid=static_cast<std::uint32_t>(::getpid());
        auto* data=context->members();
        const auto used=context->header().used;
        auto slot=used;
        for (std::uint32_t index=0;index<used;++index) {
            const auto& entry=data[index];
            if (!entry.serial) {slot=std::min(slot,index);continue;}
            if (entry.pid==pid && entry.thread==public_thread) throw Error{183};
        }
        if (slot==UINT32_MAX || context->header().sequence==UINT64_MAX) throw Error{8};
        if (slot>=context->capacity()) context->map(sizeof(Header)+(static_cast<std::size_t>(slot)+1)*sizeof(Member),true);
        auto& head=context->header();
        auto& entry=context->members()[slot];
        const auto eligible=head.started==head.round?add(head.round,head.period):head.round;
        entry={0,eligible,0,0,0,pid,public_thread,context->file_,role,false};
        if (slot==used) ++head.used;
        entry.serial=++head.sequence;
        context->slot_=slot;
        context->serial_=entry.serial;
        context->broker_priority_=head.broker_priority;
    } catch (...) {
        if (created) (void)::unlinkat(context->directory_,context->name_.c_str(),0);
        throw;
    }
    if (context->wake_) context->wake_->notify();
    return context;
}

inline ThreadOrderingContext::Turn ThreadOrderingContext::turn(bool enter) {
    const SharedEventLock lock(directory_);
    const auto now=detail::monotonic_ticks();
    bool changed=advance(now);
    const auto notify=[&] {if (changed && wake_) wake_->notify();};
    auto* current=member();
    if (!current) {notify();return {5};}
    auto& group=header();
    if (enter) {
        // 再次來到 Wait 即交還本週期的工作權並預先登記下一輪。
        if (current->granted==group.round) current->completed=group.round;
        current->waiting=true;
        changed=true;
        changed=advance(now) || changed;
    }
    const auto& parent=members()[0];
    const bool started=group.started==group.round || parent.waiting;
    Member* first=nullptr;
    for (std::uint32_t index=0;index<group.used;++index) {
        auto& entry=members()[index];
        if (!entry.serial || entry.eligible>group.round || entry.completed==group.round) continue;
        if (!first || entry.role<first->role || (entry.role==first->role && entry.serial<first->serial)) first=&entry;
    }
    if (started && first==current && current->waiting) {
        group.started=group.round;
        current->granted=group.round;
        current->expires=add(std::max(add(group.round,group.period),now),group.timeout);
        current->waiting=false;
        changed=true;  // 其他等待者須能依新持有者的逾時設定等待期限。
        notify();
        return {};
    }
    // 下一輪開始、目前執行者逾時與遠端程序存活都用同一單調時基。
    auto deadline=group.check_owners;
    if (!first) deadline=std::min(deadline,add(group.round,group.period));
    else if (!started) deadline=std::min(deadline,add(add(group.round,group.period),group.timeout));
    else if (!first->waiting) deadline=std::min(deadline,first->granted==group.round?first->expires:add(add(group.round,group.period),group.timeout));
    notify();
    return {0,true,deadline};
}

inline std::uint32_t ThreadOrderingContext::remove(bool exiting) {
    const SharedEventLock lock(directory_);
    const auto changed=advance(detail::monotonic_ticks());
    auto* entry=member();
    if (entry) {
        if (parent()) destroy();
        else entry->serial=0;
    }
    if ((entry || changed) && wake_) wake_->notify();
    return entry || exiting?0U:87U;
}
#else
inline ThreadOrderingContext::~ThreadOrderingContext()=default;
inline std::shared_ptr<ThreadOrderingContext> ThreadOrderingContext::open(const std::string&, const Guid&,
    std::shared_ptr<SharedWake>, std::uint64_t, std::uint32_t, Role, std::uint64_t, std::uint64_t, int) {throw Error{50};}
inline ThreadOrderingContext::Turn ThreadOrderingContext::turn(bool) {return {50};}
inline std::uint32_t ThreadOrderingContext::remove(bool) {return 50;}
#endif
}
