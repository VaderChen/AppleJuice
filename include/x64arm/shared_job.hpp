#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <functional>
namespace x64arm {
struct SharedJobStorage;
struct JobNotification {std::uint64_t sequence{};std::uint32_t message{},pid{};};
struct JobSnapshot {
    std::uint64_t identity{};
    std::uint32_t flags{},active_limit{},total{},terminated{};
    std::uint32_t ui_restrictions{};
    std::uint64_t process_memory_limit{},peak_process_memory{},peak_job_memory{};
    std::uint64_t notification_sequence{},completion_generation{};
    std::vector<std::uint32_t> processes;
};
struct JobMemberPoll {
    std::uint32_t flags{};
    std::uint64_t process_memory_limit{};
    std::optional<std::uint32_t> termination_code;
};
class SharedJob {
    std::shared_ptr<SharedJobStorage> storage_;
public:
    explicit SharedJob(std::shared_ptr<SharedJobStorage> storage):storage_(std::move(storage)){}
    int descriptor() const;
    int lock_descriptor() const;
    JobSnapshot snapshot() const;
    std::u16string object_name() const;
    std::uint32_t associate_completion_port(std::uint64_t port,std::uint64_t key,JobSnapshot& snapshot);
    // 同一把鎖內核對關聯及取出通知；nullopt 代表關聯已失效。
    std::optional<std::vector<JobNotification>> notifications_since(std::uint64_t& cursor,std::uint64_t generation) const;
    void record_exit_code(std::uint32_t code);
    std::uint32_t set_limits(std::uint32_t flags,std::uint32_t active_limit,std::uint64_t process_memory_limit=0);
    std::uint32_t set_ui_restrictions(std::uint32_t flags);
    std::uint16_t global_atom(std::u16string_view name,bool add,std::uint32_t& error);
    std::u16string global_atom_name(std::uint16_t atom,std::uint32_t& error);
    std::uint32_t delete_global_atom(std::uint16_t atom);
    std::uint32_t grant_user_handle(std::uint64_t handle,std::uint32_t owner,bool grant);
    bool user_handle_granted(std::uint64_t handle,std::uint32_t owner) const;
    void record_memory(std::uint64_t current,std::uint64_t peak);
    // 控制狀態版本；不加鎖讀取，只用來判斷是否需要立即完整輪詢。
    std::uint64_t revision() const noexcept;
    // 成員的限制、記憶體記帳與終止狀態在同一次持鎖掃描完成。
    JobMemberPoll poll_member(std::uint64_t current,std::uint64_t peak);
    std::uint32_t join();
    void terminate(std::uint32_t code);
    std::optional<std::uint32_t> termination_code() const;
};
struct OpenSharedJob {std::shared_ptr<SharedJob> job;std::uint32_t error{};bool existed{};};
OpenSharedJob open_shared_job(const std::string& session,std::u16string name,bool create,const std::function<std::vector<std::uint8_t>()>& security={});
OpenSharedJob import_shared_job(const std::string& session,int descriptor,bool handle);
}
