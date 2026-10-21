#pragma once
#include <utility>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <span>
#include <vector>
#include <memory>
#include <map>
#include <set>
#include <algorithm>
#include "x64arm/jit_recovery.hpp"
#include "x64arm/llvm_region.hpp"
#include "x64arm/host_features.hpp"

namespace x64arm {
struct GuestState;
class GuestMemory;
void* jit_checked_memory(GuestMemory* memory, std::uint64_t address, unsigned size, unsigned permissions) noexcept;
void* jit_checked_memory_page(GuestMemory* memory, std::uint64_t address, unsigned size, unsigned permissions) noexcept;
unsigned host_physical_processors();
std::uint64_t host_physical_memory();
class JitEngine;
class Arm64CodeDescription;
class AotModule;
namespace resident_abi {struct DispatchEntry;}
struct NativeChainNode;
struct Arm64LinkSite;
class PlatformUnavailable : public std::runtime_error { using std::runtime_error::runtime_error; };
enum class ProbeStatus { Ok, Unavailable, Failed };
struct JitProbe { ProbeStatus status; std::uint32_t value{}; std::string message; };
class JitMemory {
public:
    static constexpr std::size_t capacity_limit=std::size_t{1024}*1024*1024;
    // 只能由私有 factory 建立，完整定義與不可變內容留在平台發布器。
    class PreparedBlock;
    explicit JitMemory(std::size_t capacity);
    ~JitMemory();
    JitMemory(const JitMemory&) = delete;
    JitMemory& operator=(const JitMemory&) = delete;
    JitMemory(JitMemory&&) = delete;
    JitMemory& operator=(JitMemory&&) = delete;
    void write_return_constant(std::uint32_t value);
    std::uint32_t invoke() const;
    std::size_t capacity() const noexcept { return capacity_; }
private:
    friend class JitEngine;
    friend class AotEngine;
    friend class NativeMemoryScope;
    friend struct JitMemoryTestAccess;
    static std::shared_ptr<const PreparedBlock> prepare_llvm_block(const std::shared_ptr<const LlvmRegion>&);
    static std::shared_ptr<const PreparedBlock> prepare_aot_module(const std::shared_ptr<const AotModule>&);
    void (*aot_module_entry(std::size_t offset,std::uint32_t word_offset) const)();
    void (*aot_module_entry(std::size_t offset,GuestAddress rip) const)();
    LlvmRegion::Function llvm_entry(std::size_t offset) const;
    // share_recovery：呼叫端不保留 canonical 恢復表（例如未啟用 SMAP）時，直接把同一份表交給發布重定位，省去兩次複製；
    // 呼叫端之後持有的即為已重定位的表，restore／execution_limit 只依 id 與完成數，不受字位置影響。
    static std::shared_ptr<const PreparedBlock> prepare_unscoped_block(std::vector<std::uint32_t>& words,bool chained,std::shared_ptr<Arm64Recovery>& recovery,const std::shared_ptr<const Arm64CodeDescription>& description={},bool share_recovery=false);
    static std::shared_ptr<const PreparedBlock> finish_prepared(std::shared_ptr<const Arm64CodeDescription> description,bool chained,std::shared_ptr<Arm64Recovery> recovery,bool fold=true);
    static std::shared_ptr<const PreparedBlock> prepare_block(std::span<const std::uint32_t> words,bool chained,std::shared_ptr<const Arm64Recovery> recovery,const std::shared_ptr<const Arm64CodeDescription>& description={});
    std::size_t publish_prepared(const std::shared_ptr<const PreparedBlock>& block,
                                std::span<const resident_abi::DispatchEntry> direct_targets={});
    // 包含作用域轉換及 IBTC 的最終碼量；容量決策不得使用 canonical words 的大小。
    static std::size_t prepared_bytes(const PreparedBlock& block) noexcept;
    std::vector<std::size_t> publish_prepared_batch(std::span<const std::shared_ptr<const PreparedBlock>> blocks);
    std::size_t publish_block(std::span<const std::uint32_t> words,bool chained=false,std::shared_ptr<const Arm64Recovery> recovery={});
    std::optional<JitRecoveryLocation> recovery_at(std::uintptr_t pc) const noexcept;
    bool guard_hardware_reads(std::uintptr_t fault_pc);
    using ChainBody = void (*)();
    ChainBody chain_entry(std::size_t offset) const;
    using BlockFunction = std::uint64_t (*)(GuestState*, GuestMemory*);
    BlockFunction block_entry(std::size_t offset) const;
    void invoke_block(std::size_t offset, GuestState& state) const;
    void clear_blocks();
    // 安全點呼叫；任何目標被撤銷／替換前，清空此 arena 的所有呼叫點快取。
    void invalidate_aot_site_caches() noexcept;
    std::span<const resident_abi::DispatchEntry> aot_site_cache_entries(std::size_t offset) const;
    void discard_block(std::size_t offset);
    // 直接連結：把已發布串接區塊中條件出口樁的一字改為 B thunk；thunk 為固定模板，
    // 執行期核對框架、預算、目標 body 與作用域後直接進入目標節點，否則退回原出口路徑。
    // elide=true 從樁開頭連結，略過 RIP／旗標物化（呼叫端須證明目標入口不讀舊旗標）。
    // 修補與撤銷只能在沒有任何執行緒執行原生碼的安全點進行。
    struct ExitLink {std::size_t thunk{},site{};std::uint32_t original{},linked{};};
    std::vector<Arm64LinkSite> link_sites(std::size_t offset) const;
    // check_rip 非 0 時 thunk 先比對已物化的客體 RIP（動態出口的單態快取；elide 不可與之併用）。
    std::optional<ExitLink> link_exit(std::size_t offset,std::size_t index,const NativeChainNode* target,std::uint32_t source_flags,bool elide,std::uint64_t check_rip=0);
    // 還原連結點並釋放 thunk；來源區塊已撤銷時只釋放 thunk。
    void unlink_exit(const ExitLink&);
    std::size_t remaining() const noexcept { return std::max(capacity_-used_,free_sizes_.empty()?0:free_sizes_.rbegin()->first); }
    std::size_t live_bytes() const noexcept { return live_bytes_; }
    void add_free(std::size_t offset,std::size_t bytes);
    void erase_free(std::map<std::size_t,std::size_t>::iterator);
    [[maybe_unused]] void* address_{};
    std::size_t capacity_{};
    [[maybe_unused]] bool ready_{};
    std::size_t used_{};
    std::size_t live_bytes_{};
    std::map<std::size_t,std::size_t> free_ranges_;
    // 大小與位址共同作鍵；同尺寸空洞很多時，合併／移除仍為 O(log n)。
    std::set<std::pair<std::size_t,std::size_t>> free_sizes_;
    // 發布序號跨 arena 回收保持遞增，供取樣排除同位址已換成另一段程式碼的情況。
    std::uint64_t publication_serial_{};
    struct PublishedRecovery {std::size_t bytes;bool chained;std::uint8_t body_words;std::shared_ptr<const Arm64Recovery> table;std::uint64_t serial;std::shared_ptr<const PreparedBlock> read_fallback;bool llvm_native{};std::shared_ptr<const LlvmRegion> llvm{};std::shared_ptr<const AotModule> aot_module{};std::shared_ptr<std::vector<resident_abi::DispatchEntry>> aot_sites{};};
    std::map<std::size_t,PublishedRecovery> recovery_;
    // 帶模組 AOT 站點快取的已發布區塊位移；撤銷任一區塊時只需清這些快取，不必走訪全部 recovery_。
    // JIT 基本區塊沒有站點快取，集合為空時撤銷成本與區塊總數無關。
    std::set<std::size_t> aot_site_offsets_;
    static constexpr std::size_t aot_runtime_pool_max_bytes=4096;
    // 池不隸屬任何客體模組；同 arena 撤銷全部程式碼時才回收，單一模組失效不破壞其他呼叫。
    std::vector<std::size_t> aot_runtime_pools_;
    std::uint64_t aot_runtime_pools_created_{},aot_runtime_linked_modules_{},aot_runtime_calls_{};
    std::uint64_t aot_runtime_saved_words_{},aot_runtime_pool_bytes_{},aot_runtime_padding_words_{},aot_runtime_alignment_bytes_{},aot_published_words_{};
    std::uint64_t aot_direct_edges_{},aot_direct_far_edges_{};
public:
    // 本次發布中尚未綁定的後發布回補點（客體 RIP、宿主字位址）；引擎取走後自行登錄。
    std::vector<std::pair<std::uint64_t,std::uintptr_t>> take_late_exits(){return std::exchange(aot_late_exits_,{});}
    // 把回補點的 B .+4 改為 B target：核對字形、目標為同 arena 存活模組的精確公開入口且在 ±128 MiB 內。
    bool link_late_exit(std::uintptr_t site,std::uint64_t rip,std::uintptr_t target);
private:
    std::vector<std::pair<std::uint64_t,std::uintptr_t>> aot_late_exits_;
};
std::size_t host_page_size();
JitProbe probe_jit();
std::string doctor_report();
}
