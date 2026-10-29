#pragma once
#include <map>
#include "x64arm/interpreter.hpp"
#include "x64arm/windows_services.hpp"
#include <functional>

namespace x64arm {
// 可診斷的客體 loader 失敗與宿主執行器錯誤分開，保留完整 Windows exit status。
class PeDependencyError : public std::invalid_argument {
public:
    PeDependencyError(std::uint32_t code,std::string message):std::invalid_argument(std::move(message)),win32_error(code){}
    std::uint32_t win32_error;
    std::uint32_t exit_status() const noexcept {
        switch(win32_error){case 126:return 0xc0000135U;case 127:return 0xc0000139U;
            case 193:return 0xc000007bU;case 5:return 0xc0000022U;case 8:return 0xc0000017U;default:return 0xc0000001U;}
    }
};

struct PeOptions {
    std::optional<GuestAddress> image_base;
    std::u16string command_line{u"x64arm.exe"};
    std::filesystem::path filesystem_root{};
    bool trap_unresolved_imports{};
    std::filesystem::path registry_file{};
    std::filesystem::path c_drive{};
    std::map<char,std::filesystem::path> drive_mappings{};
    std::filesystem::path install_root{};
    std::filesystem::path executable_path{};
    std::u16string guest_image_path{}; // 已持有映像快照時，名稱不需要重新開啟來源檔案。
    std::filesystem::path loader_directory{};
    bool writable_files{};
    std::size_t memory_limit{GuestMemory::capacity_limit};
    int primary_token_descriptor{-1}; // 在載入相依 DLL 及執行任何客體程式之前安裝。
    bool defer_imports{}; // 暫停程序在 ResumeThread 後以初始執行緒身分完成 loader。
    bool no_initial_thread{}; // Native 程序只建立位址空間；TEB、堆疊及 TLS 等待真正的執行緒。
    std::string certificate_roots_file{}; // 相對於 Sandbox C 槽；空值只使用內建公開 CA。
    bool resident_translation{}; // 呼叫端已選定常駐執行器，載入時建立對應的原生控制流入口。
};
enum class PeArchitecture : std::uint16_t { X86 = 0x14c, X64 = 0x8664 };
struct PeMetadata {
    GuestAddress preferred_base{}, loaded_base{}, entry{};
    std::uint32_t image_size{};
    std::uint32_t relocations{};
    std::vector<std::string> imports;
    PeArchitecture architecture{PeArchitecture::X64};
    unsigned pointer_bytes() const noexcept { return architecture == PeArchitecture::X86 ? 4 : 8; }
};
struct PeImport {
    std::string dll, symbol;
    std::uint16_t ordinal{};
    bool by_ordinal{};
    std::uint32_t slot{}; // 映像內的 IAT RVA。
};
struct PeMapping {
    std::uint32_t rva{};
    Permissions permissions{};
    std::vector<std::uint8_t> bytes; // 已完成重定位，尚未綁定匯入。
};
struct PeDelayImport : PeImport {
    std::uint32_t module_handle_rva{};
};
struct ParsedPeImage {
    PeMetadata metadata;
    RuntimeImage runtime;
    std::vector<PeImport> imports;
    std::vector<PeDelayImport> delay_imports; // 保留 guest helper 的原始 thunk，不提前綁定。
    std::vector<PeMapping> mappings;
    std::uint64_t stack_size{};
    bool dll{};
};
// 唯讀匯入盤點：沒有可映射的區段，不可交給 loader 執行。
struct PeInventory {
    PeMetadata metadata;
    std::vector<PeImport> imports;
    std::vector<PeDelayImport> delay_imports;
    std::map<std::string, std::uint32_t> export_names;
    std::map<std::uint32_t, std::uint32_t> export_ordinals;
    std::map<std::uint32_t, std::string> export_forwarders;
    bool dll{};
};
PeInventory inspect_pe_image(std::span<const std::uint8_t> bytes);
struct PeResource {
    std::optional<std::uint16_t> id;
    std::vector<std::uint8_t> bytes;
};
// 僅讀取並驗證資源，不執行映像；省略 id 時使用該類型的第一份資源。
std::optional<PeResource> read_pe_resource(std::span<const std::uint8_t> bytes,
    std::u16string_view type, std::optional<std::uint16_t> id = {}, std::uint16_t language = 0x409);
// 讀取並驗證映像標頭後選址，避免 preferred base 衝突時重新解析全部映像。
using PeImageBaseSelector = std::function<GuestAddress(GuestAddress,std::uint32_t,PeArchitecture)>;
// 純解析不建立程序、堆疊或服務；dll 表示呼叫端要求的映像種類。
ParsedPeImage parse_pe_image(std::span<const std::uint8_t> bytes,
    std::optional<GuestAddress> base = {}, bool dll = false,
    std::size_t memory_limit = GuestMemory::capacity_limit, bool resource_only = false,
    const PeImageBaseSelector& select_base = {}, bool native_subsystem = false);
// 分離映射與綁定，讓模組管理員可先登錄互相依賴的 DLL。
void map_pe_image(GuestMemory& memory, const ParsedPeImage& image);
using PeImportResolver = std::function<GuestAddress(const PeImport&)>;
void bind_pe_imports(GuestMemory& memory, const ParsedPeImage& image, const PeImportResolver& resolver);
struct PeProgram { GuestState state; GuestMemory memory; WindowsServices services; PeMetadata metadata; std::optional<ParsedPeImage> pending_imports; };
PeProgram load_pe(std::span<const std::uint8_t> bytes, PeOptions options = {});
void complete_pe_load(PeProgram& program);
std::string format_pe(const PeMetadata& metadata);
}
