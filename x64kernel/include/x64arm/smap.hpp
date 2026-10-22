#pragma once
#include "x64arm/ir.hpp"
#include <filesystem>
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <tuple>

namespace x64arm {
// 磁碟格式不包含 C++ 物件、宿主指標或返回堆疊。
enum class CompilationBackend : std::uint8_t { Jit, Aot, AotLlvm, ResidentAot };
class LlvmRegion;
class AotModule;
class SmapCache {
public:
    enum class FileKind : std::uint8_t { Runtime, PreparedAot };
    // 模式及視窗寬度屬於生成碼契約；0 表示沿用受檢 helper，不能與視窗版本混用。
    void set_context(MemoryOrdering ordering,unsigned window_bits=0) { ordering_=ordering;window_bits_=window_bits; }
    bool open(const std::filesystem::path& source, bool primary = true, bool load_existing = true,bool require_authentication=false,
              FileKind kind=FileKind::Runtime,std::uint64_t image_base=0,
              const std::array<std::uint8_t,32>* expected_source=nullptr);
    // 限制單一快取的索引成本；來源身分仍須與載入器實際讀到的內容相符。
    void set_index_budget(std::size_t bytes) {index_budget_=bytes;}
    // 以追加段寫出本程序新翻譯；同一物件可重複呼叫。磁碟已由另一程序更新時先合併再追加。
    // append_only：只追加，不壓實（執行中的定期保存不重寫整個檔案；壓實留給正常退出或下次啟動）。
    bool save(bool append_only=false);
    bool enabled() const noexcept { return !source_.empty(); }
    bool dirty() const noexcept { return dirty_; }
    // 尚未寫出磁碟的本程序翻譯數；定期保存只在有新內容時進行。
    std::size_t pending() const noexcept { return compiled_count_; }
    struct Restored { IrBlock ir; std::vector<std::uint32_t> words; Arm64Recovery recovery; CompilationBackend backend{CompilationBackend::Jit}; };
    std::optional<Restored> restore(std::uint64_t rip, bool fragment, const GuestMemory& memory,
                                  GuestAddress sentinel, const HostServices* services) const;
    void remember(const IrBlock&, const std::vector<std::uint32_t>&, bool fragment, const GuestMemory&, const Arm64Recovery* recovery=nullptr, CompilationBackend backend=CompilationBackend::Jit);
    bool remember_llvm(const std::shared_ptr<const LlvmRegion>&,const GuestMemory&);
    std::shared_ptr<const LlvmRegion> restore_llvm(std::uint64_t,const GuestMemory&,GuestAddress sentinel,const HostServices*)const;
    using Policy=std::array<std::uint8_t,32>;
    static Policy source_identity(std::span<const std::uint8_t>);
    // 執行中引擎的產碼身分（CodeDirectory 與宿主產碼能力）；記憶體快取以此區隔不同引擎版本。
    static Policy engine_identity();
    // 冷邊界綁定產碼環境與呼叫端政策；不保存原始環境字串或程序位址。
    static Policy aot_policy(std::span<const std::uint64_t>);
    struct RestoredAot {IrBlock ir;std::shared_ptr<const AotModule> module;};
    // 離線準備維持已選入的前綴，不能讓後續掃描擠掉先前準備的入口。
    bool remember_aot(const IrBlock&,const std::shared_ptr<const AotModule>&,const GuestMemory&,const Policy&,bool precise,bool preserve_prepared=false);
    std::optional<RestoredAot> restore_aot(std::uint64_t,const GuestMemory&,GuestAddress,const HostServices*,const Policy&,bool precise)const;
    std::size_t size() const noexcept { return records_.size(); }
    struct IoStats {
        std::uint64_t index_bytes_read{},payload_bytes_read{},payload_reads{},read_failures{};
        std::uint64_t appends{},compactions{},bytes_written{};
    };
    const IoStats& io_stats() const noexcept { return io_stats_; }
    // 不計執行器已發布的機器碼；磁碟 payload 不常駐，索引成本依筆數成長。
    std::size_t resident_payload_bytes() const noexcept;
    std::size_t index_memory_bytes() const noexcept;
private:
    struct Node { std::uint64_t rip{}; std::vector<std::uint8_t> bytes; bool operator==(const Node&)const=default; };
    struct Payload { std::vector<Node> nodes; std::vector<std::uint32_t> words;Arm64Recovery recovery;
        std::vector<std::uint8_t> llvm_payload;
        Policy policy{};
    };
    struct File;
    struct Record {
        std::shared_ptr<Payload> payload;
        CompilationBackend backend{CompilationBackend::Jit};
        // 持有已開啟的 inode；原子替換不影響舊 reader。按需 pread 一筆，不映射或保留整檔。
        std::shared_ptr<File> encoded;std::size_t offset{},length{};bool primary_source{true},authenticated{};
        // stored：本程序已追加寫入磁碟的本地記錄，不再重複追加；used：本次執行曾還原，壓實時優先保留。
        bool stored{};mutable bool used{};
        std::array<std::uint8_t,32> mac{};std::array<std::uint8_t,12> identity{};
        std::size_t size() const { if(encoded)return length;
            if(!payload)return 0;
            const auto& p=*payload;
            if(backend==CompilationBackend::AotLlvm){std::size_t bytes=22+p.llvm_payload.size();for(const auto& node:p.nodes)bytes+=9+node.bytes.size();return bytes;}
            if(backend==CompilationBackend::ResidentAot){std::size_t bytes=56+p.llvm_payload.size();for(const auto& node:p.nodes)bytes+=9+node.bytes.size();return bytes;}
            std::size_t bytes=17+p.words.size()*4+p.nodes.size()*2+p.recovery.sites.size()*3;
            if(p.recovery.explicit_ids)bytes+=5+p.recovery.rips.size()*14;
            if(p.recovery.loop_limit)bytes+=7;
            for(const auto& node:p.nodes)bytes+=9+node.bytes.size();return bytes; }
    };
    std::optional<Record> materialize(const Record&) const;
    std::vector<std::uint8_t> read_record(const Record&) const;
    // 視窗欄位低 6 bits 保存原寬度，bit 6 區分直接 MMU，bit 7 區分 x86。
    // 診斷模式不載入直接存取產物；舊受檢記錄仍保留原格式。
    // 0：基本區塊，1：原生串接 fragment，2：LLVM，3/4：resident AOT 普通／精確本體。
    using Key=std::tuple<std::uint64_t,std::uint8_t,MemoryOrdering,unsigned>;
    struct LoadedFile {
        std::map<Key,Record> records;std::size_t bytes{},size{},segments{},entries{};
        std::shared_ptr<File> file;bool complete{};
    };
    bool load_file(const std::filesystem::path&,bool primary,const std::vector<std::uint8_t>& digest,std::size_t budget,LoadedFile&) const;
    enum class SaveOutcome { Saved, Conflict, Failed };
    SaveOutcome write_journal(bool append_only);
    bool reload_primary();
    std::map<Key,Record> records_;
    MemoryOrdering ordering_{MemoryOrdering::Tso};
    unsigned window_bits_{};
    std::filesystem::path source_;
    FileKind file_kind_{FileKind::Runtime};
    std::uint64_t image_base_{};
    std::size_t index_budget_{512U*1024U*1024U};
    std::vector<std::uint8_t> source_hash_;
    bool dirty_{},verify_{};
    std::shared_ptr<File> primary_file_;
    bool primary_complete_{};
    std::size_t primary_size_{},segment_count_{},journal_entries_{};
    std::size_t stored_bytes_{},compiled_bytes_{},compiled_count_{};
    mutable IoStats io_stats_;
};
// 只重定位 emitter 的 MOVZ/MOVK helper 序列，不為持久化增加熱路徑指令。
bool relocate_smap_helpers(std::vector<std::uint32_t>& words, bool storing);
}
