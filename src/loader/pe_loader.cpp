#include "x64arm/startup_profile.hpp"
#include "x64arm/pe_loader.hpp"
#include "x64arm/guest_address_policy.hpp"
#include "x64arm/windows_unwind.hpp"
#include "x64arm/smap.hpp"
#include <algorithm>
#include <array>
#include <set>
#include <sstream>
#include <functional>
#include <utility>

namespace x64arm {
namespace {
[[noreturn]] void reject(std::string_view message) { throw std::invalid_argument("PE 載入拒絕：" + std::string(message)); }
bool power2(std::uint32_t value) { return value != 0 && (value & (value - 1)) == 0; }
struct Reader {
    std::span<const std::uint8_t> bytes;
    void check(std::size_t at, std::size_t size) const {
        if (at > bytes.size() || size > bytes.size() - at) reject("檔案範圍越界或資料截斷");
    }
    std::uint64_t integer(std::size_t at, unsigned size) const {
        check(at, size); std::uint64_t result = 0;
        for (unsigned i = 0; i < size; ++i) result |= std::uint64_t{bytes[at + i]} << (i * 8U);
        return result;
    }
    std::uint16_t u16(std::size_t at) const { return static_cast<std::uint16_t>(integer(at, 2)); }
    std::uint32_t u32(std::size_t at) const { return static_cast<std::uint32_t>(integer(at, 4)); }
    std::uint64_t u64(std::size_t at) const { return integer(at, 8); }
};
struct Section {
    std::uint32_t rva, size, raw, raw_size; Permissions permissions;
    std::vector<std::uint8_t> bytes{};
};
struct Directory { std::uint32_t rva{}, size{}; };
struct Image {
    // 區段直接持有最終資料；不配置 SizeOfImage 大小的含間隙暫時映像。
    std::vector<std::uint8_t> header_bytes;
    std::vector<Section> sections;
    std::uint32_t headers, image_size;
    // 只排序索引，保留 section／mapping 的原始順序。
    std::vector<std::size_t> section_order{};
    mutable const Section* recent_section{};
    void index_sections() {
        if(sections.size()<=8)return;
        section_order.reserve(sections.size());
        for(std::size_t n=0;n<sections.size();++n)section_order.push_back(n);
        std::sort(section_order.begin(),section_order.end(),[&](auto a,auto b){return sections[a].rva<sections[b].rva;});
    }
    const Section* section_at(std::uint64_t rva) const {
        // 少量區段線性搜尋較便宜；大量區段才使用索引與局部性快取。
        if(sections.size()<=8){
            for(const auto& section:sections)if(rva>=section.rva && rva-section.rva<section.size)return &section;
            return nullptr;
        }
        // 名稱通常連續讀同一段；索引建立後 sections 不再變動。
        if(recent_section && rva>=recent_section->rva && rva-recent_section->rva<recent_section->size)return recent_section;
        auto found=std::upper_bound(section_order.begin(),section_order.end(),rva,
            [&](auto address,auto index){return address<sections[index].rva;});
        if(found==section_order.begin())return nullptr;
        const auto& section=sections[*--found];
        if(rva-section.rva>=section.size)return nullptr;
        recent_section=&section;return recent_section;
    }
    void check(std::uint64_t rva, std::uint64_t size) const {
        if (rva > image_size || size > image_size - rva) reject("RVA 範圍超出 SizeOfImage");
        auto at = rva;
        const auto end = rva + size;
        while (at < end) {
            if (at < headers) { at = std::min<std::uint64_t>(end, headers); continue; }
            const auto* section=section_at(at);
            if(!section)reject("RVA 指向未映射的映像間隙");
            at=std::min<std::uint64_t>(end,std::uint64_t{section->rva}+section->size);
        }
    }
    void executable(std::uint64_t rva, std::uint64_t size, std::string_view message) const {
        check(rva, size);
        for (auto at = rva; at < rva + size;) {
            const auto found = std::find_if(sections.begin(), sections.end(), [&](const Section& section) {
                return at >= section.rva && at - section.rva < section.size;
            });
            if (found == sections.end() || !(found->permissions & permission(Access::Execute))) reject(message);
            at = std::min<std::uint64_t>(rva + size, std::uint64_t{found->rva} + found->size);
        }
    }
    std::span<const std::uint8_t> prefix(std::uint64_t rva) const {
        if(rva>=image_size)reject("RVA 範圍超出 SizeOfImage");
        if(rva<headers)return std::span(header_bytes).subspan(static_cast<std::size_t>(rva),headers-static_cast<std::size_t>(rva));
        if(const auto* section=section_at(rva))return std::span(section->bytes).subspan(static_cast<std::size_t>(rva-section->rva));
        reject("RVA 指向未映射的映像間隙");
    }
    std::span<std::uint8_t> writable_prefix(std::uint64_t rva) {
        const auto bytes=std::as_const(*this).prefix(rva);
        // 所有區段與標頭資料都由此非 const Image 獨佔，prefix 不借用來源檔案。
        return {const_cast<std::uint8_t*>(bytes.data()),bytes.size()};
    }
    std::uint64_t integer(std::uint64_t rva, unsigned size) const {
        if(!size){check(rva,0);return 0;}
        const auto bytes=prefix(rva);
        if(size<=bytes.size())return Reader{bytes}.integer(0,size);
        check(rva,size);std::uint64_t value=0;unsigned read=0;
        while(read<size) {
            const auto part=prefix(rva+read);const auto count=std::min<std::size_t>(size-read,part.size());
            for(std::size_t n=0;n<count;++n)value|=std::uint64_t{part[n]}<<((read+n)*8U);
            read+=static_cast<unsigned>(count);
        }
        return value;
    }
    void store(std::uint64_t rva, std::uint64_t value, unsigned size) {
        check(rva, size);
        unsigned written=0;
        while(written<size) {
            const auto part=writable_prefix(rva+written);const auto count=std::min<std::size_t>(size-written,part.size());
            for(std::size_t n=0;n<count;++n)part[n]=static_cast<std::uint8_t>(value>>((written+n)*8U));
            written+=static_cast<unsigned>(count);
        }
    }
    void read(std::uint64_t rva,std::span<std::uint8_t> destination) const {
        check(rva,destination.size());std::size_t copied=0;
        while(copied<destination.size()) {
            const auto part=prefix(rva+copied);const auto count=std::min(part.size(),destination.size()-copied);
            std::copy_n(part.begin(),count,destination.begin()+static_cast<std::ptrdiff_t>(copied));copied+=count;
        }
    }
    std::string string(std::uint64_t rva, unsigned limit = 256) const {
        std::string result;
        // 每段連續映射只查一次；NUL 前停止，不探查字串後方的間隙。
        for(std::uint64_t at=rva;at-rva<limit;){
            const auto part=prefix(at);const auto count=std::min<std::uint64_t>(part.size(),limit-(at-rva));
            for(std::size_t n=0;n<count;++n,++at){
                const auto byte=part[n];
                if(byte==0){if(result.empty())reject("匯入名稱為空");return result;}
                if(byte<32 || byte>=127)reject("匯入名稱不是 ASCII");
                result.push_back(static_cast<char>(byte));
            }
        }
        reject("PE 名稱缺少終止字元或超過字串上限");
    }
};
void exports(const Image& image, Directory dir, RuntimeImage& runtime) {
    if (!dir.rva) return;
    if (dir.size < 40) reject("export directory 標頭截斷");
    const auto start=std::uint64_t{dir.rva}, end=start+dir.size;
    if(image.integer(start,4))reject("export directory 保留旗標非零");
    (void)image.string(image.integer(start+12,4),4096);
    const auto ordinal_base=image.integer(start+16,4), count=image.integer(start+20,4), names=image.integer(start+24,4);
    const auto addresses=image.integer(start+28,4), name_table=image.integer(start+32,4), ordinals=image.integer(start+36,4);
    // 表格大小以映像範圍為界，包含匿名匯出、ordinal base 與稀疏 EAT。
    if(count>image.image_size/4 || names>image.image_size/6 || (count && ordinal_base+count-1>UINT32_MAX))reject("export table 數量或 ordinal 溢位");
    if(count && !addresses)reject("export address table 缺少 RVA");
    if(names && (!name_table || !ordinals))reject("export name table 缺少 RVA");
    image.check(addresses,count*4);image.check(name_table,names*4);image.check(ordinals,names*2);
    for(std::uint64_t i=0;i<count;++i){
        const auto target=image.integer(addresses+i*4,4);
        if(!target)continue;
        if(target>=start && target<end){
            const auto forwarder=image.string(target,static_cast<unsigned>(std::min<std::uint64_t>(4096,end-target)));
            const auto dot=forwarder.rfind('.');
            if(dot==std::string::npos || dot==0 || dot+1==forwarder.size())reject("export forwarder 缺少模組或匯出名稱");
            if(forwarder[dot+1]=='#'){
                std::uint64_t ordinal=0;
                if(dot+2==forwarder.size())reject("export forwarder ordinal 為空");
                for(auto j=dot+2;j<forwarder.size();++j){
                    const auto ch=forwarder[j];
                    if(ch<'0' || ch>'9')reject("export forwarder ordinal 不是十進位整數");
                    ordinal=ordinal*10+static_cast<unsigned>(ch-'0');
                    if(ordinal>UINT16_MAX)reject("export forwarder ordinal 超過 16 bits");
                }
            }
            runtime.export_forwarders.emplace(static_cast<std::uint32_t>(ordinal_base+i),forwarder);
            continue;
        }
        image.check(target,1);
        runtime.export_ordinals.emplace(static_cast<std::uint32_t>(ordinal_base+i),static_cast<std::uint32_t>(target));
    }
    for(std::uint64_t i=0;i<names;++i){
        const auto index=image.integer(ordinals+i*2,2);if(index>=count)reject("export name ordinal 超出 address table");
        const auto name=image.string(image.integer(name_table+i*4,4),4096);
        if(!runtime.export_names.emplace(name,static_cast<std::uint32_t>(ordinal_base+index)).second)reject("export 名稱重複");
    }
}
// Debug metadata never executes. Validate both mapped RVA and file-only records,
// including modern zero-payload reproducible-build records, then leave it untouched.
void debug_directory(const Reader& file, const Image& image, Directory dir) {
    if (!dir.rva) return;
    if (dir.size % 28) reject("debug directory 項目截斷");
    for (std::uint64_t at = dir.rva; at < std::uint64_t{dir.rva} + dir.size; at += 28) {
        if (image.integer(at, 4)) reject("debug directory 保留欄位非零");
        const auto size = image.integer(at + 16, 4);
        const auto rva = image.integer(at + 20, 4), raw = image.integer(at + 24, 4);
        if (size) {
            if (!raw) reject("debug data 缺少檔案位置");
            file.check(static_cast<std::size_t>(raw), static_cast<std::size_t>(size));
            if (rva) image.check(rva, size);
        }
    }
}
void resources(const Image& image, Directory dir) {
    if (!dir.rva) return;
    const auto check = [&](std::uint64_t offset, std::uint64_t size) {
        if (offset > dir.size || size > dir.size - offset) reject("resource directory 內部 offset 越界");
        image.check(std::uint64_t{dir.rva} + offset, size);
    };
    std::set<std::uint64_t> active; std::size_t nodes = 0;
    std::function<void(std::uint64_t, unsigned)> visit = [&](std::uint64_t offset, unsigned depth) {
        if (depth > 16 || ++nodes > 65536 || !active.insert(offset).second) reject("resource directory 循環或超過深度／項目上限");
        check(offset, 16);
        const auto table = std::uint64_t{dir.rva} + offset;
        const auto named = image.integer(table + 12, 2), ids = image.integer(table + 14, 2);
        check(offset + 16, (named + ids) * 8);
        for (std::uint64_t i = 0; i < named + ids; ++i) {
            const auto name = image.integer(table + 16 + i * 8, 4), target = image.integer(table + 20 + i * 8, 4);
            if (name & 0x80000000U) {
                const auto at = name & 0x7fffffffU; check(at, 2);
                check(at + 2, image.integer(std::uint64_t{dir.rva} + at, 2) * 2);
            }
            const auto at = target & 0x7fffffffU;
            if (target & 0x80000000U) visit(at, depth + 1);
            else {
                check(at, 16); const auto leaf = std::uint64_t{dir.rva} + at;
                image.check(image.integer(leaf, 4), image.integer(leaf + 4, 4));
                if (image.integer(leaf + 12, 4)) reject("resource data entry 保留欄位非零");
            }
        }
        active.erase(offset);
    };
    visit(0, 0);
}
void certificate(const Reader& file, Directory dir, const Image& image) {
    if (!dir.rva) return;
    // Certificate Table 使用檔案 offset，不是 RVA；只驗證結構，未驗證 Authenticode 信任。
    file.check(dir.rva, dir.size);
    if (dir.rva % 8 || dir.rva < image.headers) reject("certificate table offset 未對齊或覆蓋標頭");
    const auto end = std::uint64_t{dir.rva} + dir.size;
    for (const auto& section : image.sections)
        if (section.raw_size && dir.rva < std::uint64_t{section.raw} + section.raw_size && section.raw < end) reject("certificate table 覆蓋 section 資料");
    for (std::uint64_t at = dir.rva; at < end;) {
        if (end - at < 8) reject("WIN_CERTIFICATE 標頭截斷");
        const auto size = file.u32(static_cast<std::size_t>(at));
        const auto rounded = (std::uint64_t{size} + 7) & ~UINT64_C(7);
        if (size < 8 || rounded > end - at) reject("WIN_CERTIFICATE 長度不合法");
        at += rounded;
    }
}
// Function Override 提供可選的硬體特化版本；未選用時保留 linker 的預設實作。
// 仍驗證整張表，其他動態 relocation 類型不能當成可忽略的提示。
void function_overrides(const Image& image,std::uint64_t at,std::uint64_t available) {
    if(available<8 || image.integer(at,4)!=1)reject("不支援的 DVRT 版本");
    const auto length=image.integer(at+4,4);
    if(length>available-8)reject("DVRT 超出 section");
    const auto end=at+8+length;at+=8;
    while(at<end){
        if(end-at<12 || image.integer(at,8)!=7)reject("不支援的動態 relocation 類型");
        const auto bytes=image.integer(at+8,4);at+=12;
        if(bytes<4 || bytes>end-at)reject("Function Override 資料截斷");
        const auto next=at+bytes,records=image.integer(at,4);at+=4;
        if(records>next-at)reject("Function Override 表格截斷");
        const auto records_end=at+records,bdd=records_end;
        while(at<records_end){
            if(records_end-at<16)reject("Function Override 記錄截斷");
            image.executable(image.integer(at,4),1,"Function Override 原始函式不是程式碼");
            const auto bdd_offset=image.integer(at+4,4),rvas=image.integer(at+8,4),relocs=image.integer(at+12,4);at+=16;
            if(rvas%4 || rvas>records_end-at || relocs>records_end-at-rvas)reject("Function Override 範圍無效");
            for(std::uint64_t n=0;n<rvas;n+=4)image.executable(image.integer(at+n,4),1,"Function Override 目標不是程式碼");
            at+=rvas;const auto reloc_end=at+relocs;
            while(at<reloc_end){
                if(reloc_end-at<8)reject("Function Override relocation 截斷");
                const auto page=image.integer(at,4),block=image.integer(at+4,4);
                if(block<8 || block%2 || block>reloc_end-at)reject("Function Override relocation 區塊無效");
                for(std::uint64_t n=8;n<block;n+=2){const auto fix=image.integer(at+n,2);if(fix)image.check(page+(fix&4095),4);}
                at+=block;
            }
            if(bdd_offset>next-bdd || next-bdd-bdd_offset<8)reject("Function Override BDD 標頭截斷");
            const auto info=bdd+bdd_offset,version=image.integer(info,4),bdd_size=image.integer(info+4,4);
            if(version!=1 || bdd_size%8 || bdd_size>next-info-8)reject("Function Override BDD 無效");
            image.check(info+8,bdd_size);
        }
        at=next;
    }
}
void load_config32(const Image& image,Directory dir,std::uint64_t base,RuntimeImage& runtime,bool guarded){
    if(!dir.rva){if(guarded)reject("PE32 GuardCF 缺少 Load Config");return;}
    if(dir.size<4)reject("PE32 Load Config 標頭截斷");
    const auto size=image.integer(dir.rva,4);
    // 舊版 linker 可保留 64-byte directory extent，而 Size 描述完整結構。
    if(size<4 || size>192 || dir.size>192)reject("尚未支援此 PE32 Load Config 大小");
    image.check(dir.rva,std::max<std::uint64_t>(size,dir.size));
    const auto value=[&](unsigned offset){return offset+4<=size?image.integer(std::uint64_t{dir.rva}+offset,4):0;};
    // 舊版 72-byte 結構只到 SafeSEH，並未提供 CFG instrumentation。
    // DLLCharacteristics 宣告不能讓不存在的 GuardFlags 變成作用中 policy。
    const bool legacy_config=size<=72 && dir.size<=72;
    const auto guard_flags=value(88);const bool cfg=guarded && !legacy_config;
    if(cfg && (size<92 || (guard_flags&0x500)!=0x500 || (guard_flags&~UINT64_C(0x10017d00))))reject("尚未支援此 PE32 GuardCF policy");
    if(!cfg && (guard_flags&~UINT64_C(0x100)))reject("PE32 GuardFlags 缺少完整 CFG 結構");
    if(cfg && value(76))reject("PE32 不提供 AMD64 CFG dispatch ABI");
    runtime.guard_cf=cfg;
    constexpr std::array pointers{60U,72U,76U,184U,188U};
    for(std::uint64_t offset=12;offset<std::max<std::uint64_t>(size,dir.size);++offset){
        bool accepted=false;
        for(auto field:pointers)if(field+4<=size && offset>=field && offset<field+4)accepted=true;
        if(cfg && ((offset>=80 && offset<92) || (offset>=104 && offset<120)))accepted=true;
        // DeCommit 閾值是宿主 heap 的回收調校提示；客體配置仍受既有容量與權限限制。
        for(auto field:{24U,28U,64U,68U,88U,160U})if(field+4<=size && offset>=field && offset<field+4)accepted=true;
        if(!accepted && image.integer(std::uint64_t{dir.rva}+offset,1))reject("PE32 Load Config 包含尚未支援的作用中欄位，offset="+std::to_string(offset));
    }
    for(auto field:pointers)if(const auto pointer=value(field)){if(pointer<base)reject("PE32 Load Config 指標低於 image base");image.check(pointer-base,4);}
    const auto handlers=value(64),count=value(68);
    if(count){if(handlers<base || count>image.image_size/4)reject("SafeSEH 表格範圍無效");image.check(handlers-base,count*4);
        std::uint64_t previous=0;
        for(std::uint64_t i=0;i<count;++i){const auto rva=image.integer(handlers-base+i*4,4);if(i && rva<=previous)reject("SafeSEH 表格必須遞增且不得重複");image.executable(rva,1,"SafeSEH handler 不在 executable section");previous=rva;}
    }
    if(cfg){
        const auto stride=4U+static_cast<unsigned>(guard_flags>>28);
        if(stride>5)reject("未知的 PE32 GFIDS metadata stride");
        const auto table=[&](unsigned pointer_offset,unsigned count_offset,bool code,bool metadata_flags,std::vector<std::uint32_t>* targets){
            const auto pointer=value(pointer_offset),entries=value(count_offset);
            if(entries>image.image_size/stride || (entries && pointer<base))reject("PE32 CFG table 指標或數量無效");
            if(!entries)return;
            image.check(pointer-base,entries*stride);std::uint64_t previous=0;
            for(std::uint64_t i=0;i<entries;++i){
                const auto at=pointer-base+i*stride,target=image.integer(at,4);
                if(i && target<=previous)reject("PE32 CFG table 必須遞增且不得重複");previous=target;
                if(code)image.executable(target,1,"PE32 CFG target 不在 executable section");else image.check(target,4);
                const auto metadata=stride>4?image.integer(at+4,1):0;
                if((metadata&~UINT64_C(3)) || (!metadata_flags && metadata))reject("PE32 CFG metadata 含未知旗標");
                if(targets && !(metadata&1))targets->push_back(static_cast<std::uint32_t>(target));
            }
        };
        table(80,84,true,true,&runtime.guard_targets);
        table(104,108,false,false,nullptr);
        runtime.guard_long_jump=bool(guard_flags&0x10000);
        table(112,116,true,false,&runtime.guard_long_jump_targets);
        runtime.guard_check_cell=value(72);
        if(!runtime.guard_check_cell)reject("PE32 CFG 缺少 check pointer cell");
        const auto cell=runtime.guard_check_cell;
        image.check(cell-base,4);
        const auto section=std::find_if(image.sections.begin(),image.sections.end(),[&](const Section& part){return cell-base>=part.rva && cell-base+4<=std::uint64_t{part.rva}+part.size;});
        if(section==image.sections.end() || section->permissions!=permission(Access::Read))reject("PE32 CFG pointer cell 必須位於唯讀資料 section");
    }
    // SafeSEH 表格與 CFG 表格分別驗證，不能以一種 metadata 取代另一種。
    if(const auto pointer=value(160)){
        if(pointer<base)reject("PE32 volatile metadata 指標低於 image base");const auto at=pointer-base,length=image.integer(at,4);
        if(length<24)reject("PE32 volatile metadata 標頭截斷");image.check(at,length);
        const auto accesses=image.integer(at+8,4),access_size=image.integer(at+12,4),ranges=image.integer(at+16,4),range_size=image.integer(at+20,4);
        if(access_size%4 || range_size%8 || (access_size && !accesses) || (range_size && !ranges))reject("PE32 volatile metadata 表格格式不合法");
        image.check(accesses,access_size);image.check(ranges,range_size);
        for(std::uint64_t n=0;n<access_size;n+=4)image.check(image.integer(accesses+n,4),1);
        for(std::uint64_t n=0;n<range_size;n+=8)image.check(image.integer(ranges+n,4),image.integer(ranges+n+4,4));
    }
}
void load_config(const Image& image, Directory dir, std::uint64_t base, RuntimeImage& runtime, bool guarded) {
    if (!dir.rva) { if(guarded)reject("GuardCF 缺少 Load Config");return; }
    if (dir.size < 4) reject("Load Config 標頭截斷");
    const auto size = image.integer(dir.rva, 4);
    if (size < 4 || size > dir.size || size > 320) reject("尚未支援此 Load Config 大小");
    // CFG 由客體載入器安裝檢查／dispatch 入口，不能保留 compiler 的 no-op stub。
    // XFG 映像可在未啟用 XFG 的程序中使用 CFG 相容模式；不宣告 hash enforcement。
    // export suppression 啟用與未知的動態 relocation 仍明確拒絕。
    const auto guard_flags=size>=148?image.integer(std::uint64_t{dir.rva}+144,4):0;
    if(guarded && ((guard_flags&0x500)!=0x500 || (guard_flags&~UINT64_C(0x10c17d00))))reject("尚未支援此 GuardCF policy");
    if((guard_flags&0x800000U) && (!guarded || size<304))reject("XFG Load Config 缺少完整的相容入口");
    runtime.guard_cf=guarded;
    if(size>=230){
        const auto offset=image.integer(std::uint64_t{dir.rva}+224,4),section=image.integer(std::uint64_t{dir.rva}+228,2);
        if(section || offset){
            if(!section || section>image.sections.size())reject("DVRT section 無效");
            const auto& source=image.sections[section-1];
            if(offset>source.size)reject("DVRT offset 超出 section");
            function_overrides(image,std::uint64_t{source.rva}+offset,source.size-offset);
        }
    }
    constexpr std::array pointer_offsets{88U, 112U, 120U, 256U, 280U, 288U, 296U, 304U, 312U};
    for (std::uint64_t offset = 12; offset < dir.size; ++offset) {
        bool accepted = false;
        for (const auto pointer : pointer_offsets) if (offset >= pointer && offset < pointer + 8 && pointer + 8 <= size) accepted = true;
        if (offset>=224 && offset<230 && size>=230)accepted=true;
        if (offset >= 144 && offset < 148 && size >= 148) accepted = true;
        if(guarded && ((offset>=128 && offset<144) || (offset>=160 && offset<192) || (offset>=264 && offset<280)))accepted=true;
        if (!accepted && image.integer(std::uint64_t{dir.rva} + offset, 1)) reject("Load Config 包含尚未支援的作用中欄位，offset=" + std::to_string(offset));
    }
    // Volatile metadata 描述編譯器的 volatile 存取；直譯器原有逐次記憶體語義
    // 已保留這些存取，不需 OS 修補。仍驗證完整標頭、RVA 表與範圍表。
    if (size >= 264) {
        const auto pointer=image.integer(std::uint64_t{dir.rva}+256,8);
        if(pointer){
            if(pointer<base)reject("volatile metadata 指標低於 image base");
            const auto at=pointer-base, length=image.integer(at,4);
            if(length<24)reject("volatile metadata 標頭截斷");
            image.check(at,length);
            const auto accesses=image.integer(at+8,4), access_size=image.integer(at+12,4);
            const auto ranges=image.integer(at+16,4), range_size=image.integer(at+20,4);
            if(access_size%4 || range_size%8 || (access_size && !accesses) || (range_size && !ranges))reject("volatile metadata 表格格式不合法");
            image.check(accesses,access_size);image.check(ranges,range_size);
            for(std::uint64_t n=0;n<access_size;n+=4)image.check(image.integer(accesses+n,4),1);
            for(std::uint64_t n=0;n<range_size;n+=8)image.check(image.integer(ranges+n,4),image.integer(ranges+n+4,4));
        }
    }
    if (!guarded && size >= 148 && (image.integer(std::uint64_t{dir.rva} + 144, 4) & ~UINT64_C(0x900))) reject("尚未支援作用中的 GuardFlags");
    for (const auto offset : pointer_offsets) if (offset + 8 <= size) {
        const auto pointer = image.integer(std::uint64_t{dir.rva} + offset, 8);
        if (pointer) { if (pointer < base) reject("Load Config 指標低於 image base"); image.check(pointer - base, 8); }
    }
    if(guarded){
        const auto stride=4U+static_cast<unsigned>(guard_flags>>28);
        if(stride>5)reject("未知的 GFIDS metadata stride");
        const auto table=[&](unsigned pointer_offset,unsigned count_offset,bool code,bool flags,std::vector<std::uint32_t>* targets,unsigned item_stride){
            if(count_offset+8>size)return;
            const auto pointer=image.integer(std::uint64_t{dir.rva}+pointer_offset,8),count=image.integer(std::uint64_t{dir.rva}+count_offset,8);
            if(count>image.image_size/item_stride || (count && pointer<base))reject("CFG table 指標或數量無效");
            if(!count)return;
            image.check(pointer-base,count*item_stride);std::uint64_t previous=0;
            for(std::uint64_t n=0;n<count;++n){
                const auto at=pointer-base+n*item_stride,rva=image.integer(at,4);
                if(n && rva<=previous)reject("CFG table 必須遞增且不得重複");previous=rva;
                if(code)image.executable(rva,1,"CFG target 不在 executable section");else image.check(rva,8);
                const auto metadata=item_stride>4?image.integer(at+4,1):0;
                if(metadata&~UINT64_C(11) || (!flags && metadata))reject("CFG metadata 含未知旗標");
                if(metadata&8U){if(rva<8)reject("XFG hash 位址無效");image.executable(rva-8,8,"XFG hash 不在 executable section");}
                if(targets && !(metadata&1))targets->push_back(static_cast<std::uint32_t>(rva));
            }
        };
        table(128,136,true,true,&runtime.guard_targets,stride);
        table(160,168,false,false,nullptr,stride);
        runtime.guard_long_jump=bool(guard_flags&0x10000);
        table(176,184,true,false,&runtime.guard_long_jump_targets,stride);
        // EH continuation metadata 屬於 CET；目前未宣告 CET 支援，但仍驗證表格。
        table(264,272,true,false,nullptr,stride);
        runtime.guard_check_cell=image.integer(std::uint64_t{dir.rva}+112,8);
        runtime.guard_dispatch_cell=image.integer(std::uint64_t{dir.rva}+120,8);
        if(guard_flags&0x800000U){
            runtime.guard_xfg_check_cell=image.integer(std::uint64_t{dir.rva}+280,8);
            runtime.guard_xfg_dispatch_cell=image.integer(std::uint64_t{dir.rva}+288,8);
            runtime.guard_xfg_table_dispatch_cell=image.integer(std::uint64_t{dir.rva}+296,8);
            if(!runtime.guard_xfg_check_cell || !runtime.guard_xfg_dispatch_cell || !runtime.guard_xfg_table_dispatch_cell)reject("XFG 相容入口不得為空");
        }
        for(const auto cell:{runtime.guard_check_cell,runtime.guard_dispatch_cell,runtime.guard_xfg_check_cell,runtime.guard_xfg_dispatch_cell,runtime.guard_xfg_table_dispatch_cell})if(cell){
            image.check(cell-base,8);
            const auto section=std::find_if(image.sections.begin(),image.sections.end(),[&](const Section& part){return cell-base>=part.rva && cell-base+8<=std::uint64_t{part.rva}+part.size;});
            if(section==image.sections.end() || section->permissions!=permission(Access::Read))reject("CFG pointer cell 必須位於唯讀資料 section");
        }
    }
}
void relocation(Image& image, Directory dir, std::uint64_t delta, PeMetadata& metadata) {
    const auto width = metadata.pointer_bytes();
    if (!dir.rva) { if (delta) reject("變更 image base 需要 relocation table"); return; }
    const auto end = std::uint64_t{dir.rva} + dir.size;
    auto at = std::uint64_t{dir.rva};
    // 目標互不重疊的判定：每個目標占 width 位元組，以映像位元組位圖 O(1) 檢查與標記。
    // 大型 DLL（nw.dll 約 135 萬項）原先逐項 std::set 查詢加配置約占整個解析 150 ms；位圖語意相同：
    // 任兩個目標的 [target, target+width) 相交即拒絕（含完全相同與反向次序）。
    // 目標 >= 映像大小者不可能與先前（皆在映像內）的目標相交，交給下方 image.integer 的界線檢查拒絕。
    const auto image_size = std::uint64_t{image.image_size};
    std::vector<std::uint64_t> covered((image_size + width + 63) / 64);
    // 先收集且檢查，避免重定位資料與目標重疊時改寫後續解析內容。
    std::vector<std::pair<std::uint64_t, std::uint64_t>> changes;
    changes.reserve(dir.size / 2);
    while (at < end) {
        if (end - at < 8) reject("重定位區塊標頭截斷");
        const auto page = image.integer(at, 4), size = image.integer(at + 4, 4);
        if (page % 4096 || size < 8 || size % 4 || size > end - at) reject("重定位區塊大小或頁地址不合法");
        for (std::uint64_t offset = 8; offset < size; offset += 2) {
            const auto item = image.integer(at + offset, 2);
            const auto type = item >> 12;
            if (type == 0) continue;
            if (type != (width == 4 ? 3 : 10)) reject("重定位型別與映像架構不符（HIGHLOW／DIR64）");
            const auto target = page + (item & 0xfffU);
            if (target < image_size) {
                bool overlap = false;
                for (std::uint64_t byte = target; byte < target + width; ++byte) {
                    auto& word = covered[byte >> 6];
                    const auto bit = UINT64_C(1) << (byte & 63U);
                    overlap |= (word & bit) != 0;
                    word |= bit;
                }
                if (overlap) reject("重定位目標重疊");
            }
            changes.emplace_back(target, image.integer(target, width) + delta);
        }
        at += size;
    }
    for (const auto& [target, value] : changes) image.store(target, value, width);
    metadata.relocations = static_cast<std::uint32_t>(changes.size());
}
std::vector<PeImport> imports(const Image& image, Directory directory, Directory iat, PeMetadata& metadata, bool inventory_only=false) {
    const auto width = metadata.pointer_bytes();
    const auto ordinal_bit = UINT64_C(1) << (width * 8 - 1);
    if (!directory.rva) return {};
    const auto end = std::uint64_t{directory.rva} + directory.size;
    bool terminated = false;
    std::vector<PeImport> result;
    std::set<std::uint64_t> slots;
    unsigned descriptors = 0;
    for (auto at = std::uint64_t{directory.rva}; end - at >= 20; at += 20) {
        if (++descriptors > 256) reject("匯入描述子超過 256 項上限");
        const auto original = image.integer(at, 4), timestamp = image.integer(at + 4, 4);
        const auto forward = image.integer(at + 8, 4), dll_rva = image.integer(at + 12, 4), first = image.integer(at + 16, 4);
        if (!(original | timestamp | forward | dll_rva | first)) { terminated = true; break; }
        if ((timestamp || forward) && (!inventory_only || !original)) reject("尚未支援 bound import 或 forwarder chain");
        if (!dll_rva || !first) reject("匯入描述子缺少名稱或 IAT");
        const auto dll = image.string(dll_rva);
        const auto lookup = original ? original : first;
        bool thunk_end = false;
        for (std::uint64_t n = 0; n < 4096; ++n) {
            const auto value = image.integer(lookup + n * width, width);
            const auto slot = first + n * width;
            image.check(slot, width);
            if (iat.rva && (slot < iat.rva || slot + width > std::uint64_t{iat.rva} + iat.size)) reject("IAT slot 超出資料目錄範圍");
            if (!value) { thunk_end = true; break; }
            PeImport imported;
            imported.dll=dll;imported.slot=static_cast<std::uint32_t>(slot);
            imported.by_ordinal=bool(value & ordinal_bit);
            if(imported.by_ordinal){
                if(value & ~(ordinal_bit | UINT64_C(0xffff)))reject("ordinal import 的保留位元非零");
                imported.ordinal=static_cast<std::uint16_t>(value);
            }else{
                if (value > UINT32_MAX) reject("import-by-name RVA 超過 32 bits");
                image.check(value, 2);
                imported.symbol=image.string(value + 2,4096);
            }
            const auto section = std::find_if(image.sections.begin(), image.sections.end(), [&](const Section& s) {
                return slot >= s.rva && slot + width <= std::uint64_t{s.rva} + s.size;
            });
            if (!inventory_only && (section == image.sections.end())) reject("IAT 不得位於標頭");
            const auto next = slots.lower_bound(slot);
            if ((next != slots.end() && *next < slot + width) || (next != slots.begin() && *std::prev(next) + width > slot) || result.size() >= 4096)
                reject("IAT slot 重疊或匯入項目超過上限");
            slots.insert(slot);
            metadata.imports.push_back(dll + "!" + (imported.by_ordinal ? "#" + std::to_string(imported.ordinal) : imported.symbol));
            result.push_back(std::move(imported));
        }
        if (!thunk_end) reject("匯入 thunk 缺少終止項目");
    }
    if (!terminated) reject("import directory 缺少終止描述子");
    return result;
}
std::vector<PeDelayImport> delay_imports(const Image& image,Directory directory,
    const std::vector<PeImport>& eager,std::uint64_t preferred,unsigned width,std::vector<Directory>& tables){
    const auto ordinal_bit = UINT64_C(1) << (width * 8 - 1);
    if(!directory.rva)return {};
    std::vector<PeDelayImport> result;
    std::set<std::uint64_t> slots;
    for(const auto& imported:eager)slots.insert(imported.slot);
    const auto data_slot=[&](std::uint64_t slot){
        image.check(slot,width);
        const auto section=std::find_if(image.sections.begin(),image.sections.end(),[&](const Section& s){
            return slot>=s.rva && slot+width<=std::uint64_t{s.rva}+s.size;
        });
        if(section==image.sections.end() || (section->permissions&permission(Access::Execute)))
            reject("delay import 儲存位置不得位於標頭或 executable section");
    };
    const auto original_thunk=[&](std::uint64_t slot){
        const auto target=image.integer(slot,width);
        if(target<preferred)reject("delay IAT 初始 thunk 不在映像內");
        image.executable(target-preferred,1,"delay IAT 初始 thunk 不在 executable section");
    };
    const auto end=std::uint64_t{directory.rva}+directory.size;
    bool terminated=false;unsigned descriptors=0;
    for(auto at=std::uint64_t{directory.rva};end-at>=32;at+=32){
        if(++descriptors>256)reject("delay import 描述子超過 256 項上限");
        std::array<std::uint64_t,8> fields{};
        for(unsigned n=0;n<fields.size();++n)fields[n]=image.integer(at+n*4,4);
        if(std::all_of(fields.begin(),fields.end(),[](auto value){return value==0;})){terminated=true;break;}
        const auto attributes=fields[0],name=fields[1],handle=fields[2],iat=fields[3],lookup=fields[4];
        const auto bound=fields[5],unload=fields[6],timestamp=fields[7];
        if(attributes!=1)reject("delay import 僅支援 RVA descriptor attributes=1");
        if(timestamp)reject("尚未支援帶時間戳的 bound delay import");
        if(!name || !handle || !iat || !lookup)reject("delay import 描述子缺少名稱、模組 handle 或 thunk table");
        const auto dll=image.string(name);data_slot(handle);
        bool thunk_end=false;
        for(std::uint64_t n=0;n<4096;++n){
            const auto value=image.integer(lookup+n*width,width),slot=iat+n*width;
            data_slot(slot);
            if(bound)image.check(bound+n*width,width);
            if(unload){image.check(unload+n*width,width);}
            if(!value){
                if(image.integer(slot,width) || (bound && image.integer(bound+n*width,width)) || (unload && image.integer(unload+n*width,width)))
                    reject("delay import 對應的 thunk table 缺少終止項目");
                tables.push_back({static_cast<std::uint32_t>(iat),static_cast<std::uint32_t>((n+1)*width)});
                thunk_end=true;break;
            }
            const auto next=slots.lower_bound(slot);
            if((next!=slots.end() && *next<slot+width) || (next!=slots.begin() && *std::prev(next)+width>slot) || slots.size()>=4096)
                reject("delay IAT slot 重疊或匯入項目超過上限");
            slots.insert(slot);
            original_thunk(slot);if(unload)original_thunk(unload+n*width);
            PeDelayImport imported;imported.dll=dll;imported.slot=static_cast<std::uint32_t>(slot);
            imported.module_handle_rva=static_cast<std::uint32_t>(handle);
            imported.by_ordinal=bool(value&ordinal_bit);
            if(imported.by_ordinal){
                if(value&~(ordinal_bit | UINT64_C(0xffff)))reject("delay ordinal import 的保留位元非零");
                imported.ordinal=static_cast<std::uint16_t>(value);
            }else{
                if(value>UINT32_MAX)reject("delay import-by-name RVA 超過 32 bits");
                image.check(value,2);imported.symbol=image.string(value+2,4096);
            }
            result.push_back(std::move(imported));
        }
        if(!thunk_end)reject("delay import thunk 缺少終止項目");
    }
    if(!terminated)reject("delay import directory 缺少終止描述子");
    return result;
}
ParsedPeImage parse_pe_image_impl(std::span<const std::uint8_t> bytes, std::optional<GuestAddress> image_base, bool dll, std::size_t memory_limit, bool resource_only, bool inventory_only, const PeImageBaseSelector& select_base = {}, bool native_subsystem = false) {
    if(!memory_limit || memory_limit>GuestMemory::maximum_capacity)reject("客體記憶體容量設定不合法");
    // 安裝器的 overlay 是檔案資料，不計入映像的客體記憶體額度。
    Reader file{bytes};
    file.check(0, 64);
    if (file.u16(0) != 0x5a4d) reject("缺少 MZ 簽章");
    const auto pe = std::size_t{file.u32(60)};
    if (pe < 64) reject("e_lfanew 指向 DOS 標頭內部");
    file.check(pe, 24);
    if (file.u32(pe) != 0x4550) reject("缺少 PE 簽章");
    const auto machine = file.u16(pe + 4);
    if (machine != 0x8664 && machine != 0x14c) reject("只接受 PE32 i386 或 PE32+ AMD64 映像");
    const bool x86 = machine == 0x14c;
    const unsigned pointer_size = x86 ? 4 : 8, directory_offset = x86 ? 96 : 112;
    const auto count = file.u16(pe + 6), optional_size = file.u16(pe + 20), characteristics = file.u16(pe + 22);
    if (count == 0 || count > 96 || !(characteristics & 2) || ((characteristics & 0x1000U) && !native_subsystem)) reject("不支援 system image 或不合法 section 數目");
    // 核心驅動是可執行映像（無 DLL 旗標），由驅動宿主以模組方式載入。
    if(!resource_only && !inventory_only && !native_subsystem && bool(characteristics & 0x2000U)!=dll)reject("PE 映像種類與要求的 EXE／DLL 不符");
    const auto opt = pe + 24;
    file.check(opt, optional_size);
    if (optional_size < directory_offset || file.u16(opt) != (x86 ? 0x10b : 0x20b)) reject("PE optional header 與 Machine 架構不符");
    const auto entry = file.u32(opt + 16), section_alignment = file.u32(opt + 32), file_alignment = file.u32(opt + 36);
    const auto preferred = file.integer(opt + (x86 ? 28 : 24), pointer_size);
    const auto image_size = file.u32(opt + 56), headers = file.u32(opt + 60), directory_count = file.u32(opt + directory_offset - 4);
    const auto stack_reserve = file.integer(opt + 72, pointer_size), stack_commit = file.integer(opt + 72 + pointer_size, pointer_size);
    auto stack_size = stack_reserve;
    if (!power2(section_alignment) || section_alignment < 4096 || !power2(file_alignment) || file_alignment < 512 ||
        file_alignment > 65536 || file_alignment > section_alignment) reject("不支援此 section/file alignment");
    if (image_size == 0 || image_size > memory_limit || image_size % section_alignment ||
        headers == 0 || headers > image_size || headers % file_alignment) reject("SizeOfImage 或 SizeOfHeaders 不合法");
    if (!inventory_only && !dll) {
        // PE 的保留量以 bytes 表示；Windows 向上取整至 64 KiB 配置粒度。
        // 使用同一個正規化大小建立映射、初始 SP 及 TEB，容量以程序設定為準。
        constexpr std::uint64_t stack_granularity = 65536;
        if (!stack_reserve || stack_reserve > UINT64_MAX - (stack_granularity - 1))
            reject("堆疊保留量為零或對齊時溢位");
        stack_size = (stack_reserve + stack_granularity - 1) & ~(stack_granularity - 1);
        if (stack_commit > stack_size || stack_size > memory_limit - image_size)
            reject("堆疊提交量、保留量或客體總容量超過支援範圍");
    }
    // native subsystem 只開放給核心驅動宿主載入的 DLL 型映像。
    if (!inventory_only && file.u16(opt + 68) != 3 && file.u16(opt + 68) != 2 && !(native_subsystem && dll && file.u16(opt + 68) == 1)) reject("只支援 Windows console／GUI subsystem");
    if (!resource_only && !inventory_only && (file.u16(opt + 70) & 0x1000U)) reject("尚未支援 AppContainer 映像");
    if (directory_count > 16 || std::uint64_t{directory_count} * 8 + directory_offset > optional_size) reject("data directory 數目超過 optional header");
    const auto table = opt + optional_size;
    file.check(0, headers); file.check(table, std::size_t{count} * 40);
    if (table + std::size_t{count} * 40 > headers) reject("section table 超過 SizeOfHeaders");
    const std::uint64_t user_limit = x86 ? UINT64_C(0x100000000) : UINT64_C(0x0000700000000000);
    if (!preferred || preferred % 65536 || preferred >= user_limit || image_size > user_limit - preferred)
        reject("image base 未對齊或超出客體地址範圍");
    // callback 僅選址；所有 section、重定位、匯入、TLS／unwind 等仍走同一次完整解析。
    const auto base = image_base ? image_base->value : select_base ?
        select_base(GuestAddress{preferred},image_size,x86?PeArchitecture::X86:PeArchitecture::X64).value : preferred;
    if (!base || base % 65536 || base >= user_limit || image_size > user_limit - base)
        reject("image base 未對齊或超出客體地址範圍");
    // Windows 映像標頭佔用完整客體頁；檔案 SizeOfHeaders 仍用於 RVA 驗證。
    const auto mapped_headers=(static_cast<std::size_t>(headers)+4095)&~std::size_t{4095};
    Image image{std::vector<std::uint8_t>(mapped_headers), {}, headers,image_size};
    image.sections.reserve(count);
    std::copy_n(bytes.begin(), headers, image.header_bytes.begin());
    for (unsigned n = 0; n < count; ++n) {
        const auto at = table + std::size_t{n} * 40;
        const auto virtual_size = file.u32(at + 8), rva = file.u32(at + 12), raw_size = file.u32(at + 16), raw = file.u32(at + 20);
        const auto flags_value = file.u32(at + 36);
        const auto extent = std::max(virtual_size, raw_size);
        if (extent == 0) continue;
        const auto rounded = (std::uint64_t{extent} + 4095) & ~UINT64_C(4095);
        if (rva % section_alignment || rva < headers || rva >= image_size || rounded > image_size - rva) reject("section RVA 或大小不合法");
        // Windows 可載入省略尾端 file-alignment padding 的映像；檔案來源仍須完整落在檔案內，
        // 映射尾端由已清零的 section 補足，不讀取宣告範圍外的位元組。
        if (raw_size && (raw < headers || raw % file_alignment)) reject("section raw 位址未對齊或覆蓋標頭");
        if (raw_size) file.check(raw, raw_size);
        Permissions permissions = 0;
        if (flags_value & 0x40000000U) permissions |= permission(Access::Read);
        if (flags_value & 0x80000000U) permissions |= permission(Access::Write);
        if (flags_value & 0x20000000U) permissions |= permission(Access::Execute);
        if(resource_only)permissions=permission(Access::Read);
        // 沒有 R/W/X 的 metadata 區段仍有合法的映像範圍與內容；以不可存取映射保留。
        // 客體 W+X 仍是資料映射；僅經 JIT 驗證的 ARM64 進入宿主可執行 arena。
        // 可執行來源寫入沿用 GuestMemory 世代／範圍撤銷，不能直接執行舊翻譯。
        else if (!inventory_only && (flags_value & 0x10000000U)) reject("尚未支援 shared section");
        for (const auto& other : image.sections) {
            if (rva < std::uint64_t{other.rva} + other.size && other.rva < std::uint64_t{rva} + rounded) reject("section 虛擬範圍重疊");
            if (raw_size && other.raw_size && raw < std::uint64_t{other.raw} + other.raw_size && other.raw < std::uint64_t{raw} + raw_size) reject("section raw 範圍重疊");
        }
        image.sections.push_back({rva, static_cast<std::uint32_t>(rounded), raw, raw_size, permissions,
                                  std::vector<std::uint8_t>(static_cast<std::size_t>(rounded))});
        if (raw_size) std::copy_n(bytes.begin() + raw, raw_size, image.sections.back().bytes.begin());
    }
    image.index_sections();
    std::array<Directory, 16> directories{};
    for (std::uint32_t i = 0; i < directory_count; ++i) {
        const Directory directory{file.u32(opt + directory_offset + i * 8), file.u32(opt + directory_offset + 4 + i * 8)};
        // 資源映射不執行映像；只驗證實際會使用的 resource directory。
        if(resource_only && i!=2)continue;
        if(inventory_only && i!=0 && i!=1 && i!=12 && i!=13)continue;
        if ((directory.rva == 0) != (directory.size == 0)) reject("data directory RVA/size 必須同時為零或非零");
        if (directory.rva) {
            if (i != 0 && i != 1 && i != 2 && i != 3 && i != 4 && i != 5 && i != 6 && i != 9 && i != 10 && i != 12 && i != 13)
                reject("尚未支援必要的 PE data directory，index=" + std::to_string(i) + "（例如 resource、CLR 或 load config）");
            if (i == 4) certificate(file, directory, image);
            else image.check(directory.rva, directory.size);
        }
        directories[i] = directory;
    }
    if (!resource_only && (characteristics & 1U) && base != preferred) reject("映像標示 RELOCS_STRIPPED，不能變更 image base");
    ParsedPeImage parsed;
    parsed.dll=inventory_only?bool(characteristics & 0x2000U):dll;parsed.stack_size=stack_size;
    auto& runtime=parsed.runtime;
    runtime.large_address_aware=bool(characteristics & 0x20U);
    if(resource_only) {
        resources(image,directories[2]);
        parsed.metadata={GuestAddress{preferred},GuestAddress{base},GuestAddress{0},image_size,0,{}};
        parsed.metadata.architecture = x86 ? PeArchitecture::X86 : PeArchitecture::X64;
        runtime.base=base;runtime.size=image_size;
        parsed.mappings.push_back({0,permission(Access::Read),std::move(image.header_bytes)});
        for(auto& section:image.sections)parsed.mappings.push_back({section.rva,permission(Access::Read),std::move(section.bytes)});
        return parsed;
    }
    exports(image,directories[0],runtime);
    parsed.metadata = {GuestAddress{preferred}, GuestAddress{base}, GuestAddress{entry ? base + entry : 0}, image_size, 0, {}};
    parsed.metadata.architecture = x86 ? PeArchitecture::X86 : PeArchitecture::X64;
    // 先解析匯入，避免重定位或 IAT patch 改變之後的解析內容。
    parsed.imports=imports(image,directories[1],directories[12],parsed.metadata,inventory_only);
    std::vector<Directory> delay_tables;
    parsed.delay_imports=delay_imports(image,directories[13],parsed.imports,preferred,pointer_size,delay_tables);
    if(inventory_only)return parsed;
    resources(image,directories[2]);
    debug_directory(file,image,directories[6]);
    relocation(image,directories[5],base-preferred,parsed.metadata);
    if(x86)load_config32(image,directories[10],base,runtime,bool(file.u16(opt+70)&0x4000));
    else load_config(image,directories[10],base,runtime,bool(file.u16(opt+70)&0x4000));
    std::set<std::uint32_t> protected_delay_pages;
    if(runtime.guard_cf && !delay_tables.empty()){
        // 保留原始 helper thunk 與 metadata；由客體 delay helper 透過
        // VirtualProtect 更新並還原 IAT。保護範圍包含 NULL 終止項目，
        // 以 Windows 的 4 KiB 頁面為單位，不把相鄰資料頁一併設成唯讀。
        // IN_ITS_OWN_SECTION 描述 linker 配置；真正啟用保護的是 0x1000。
        const auto flags_offset=x86?88U:144U;
        if(image.integer(std::uint64_t{directories[10].rva}+flags_offset,4)&0x1000U)
            for(const auto delay_table:delay_tables){
                const auto end=(std::uint64_t{delay_table.rva}+delay_table.size+4095)&~UINT64_C(4095);
                for(auto page=std::uint64_t{delay_table.rva}&~UINT64_C(4095);page<end;page+=4096)
                    protected_delay_pages.insert(static_cast<std::uint32_t>(page));
            }
    }
    if ((!dll && entry == 0) || entry >= image_size) reject("entry point RVA 不合法");
    if(entry)image.executable(entry,1,"entry point 不在可執行 section");
    runtime.base=base;runtime.size=image_size;runtime.entry=parsed.metadata.entry.value;
    const auto exception = directories[3];
    runtime.function_table = exception.rva ? base + exception.rva : 0;
    if (x86 && exception.rva) reject("PE32 不支援 AMD64 RUNTIME_FUNCTION table");
    if (exception.rva) {
        if (exception.rva % 4 || exception.size % 12) reject("RUNTIME_FUNCTION table 格式不合法");
        std::uint64_t previous_begin = 0, previous_end = 0;
        for (std::uint64_t at = exception.rva; at < std::uint64_t{exception.rva} + exception.size; at += 12) {
            const auto begin = image.integer(at, 4), end = image.integer(at + 4, 4), unwind = image.integer(at + 8, 4);
            // Linker 可保留零長度函式的 .pdata。它不覆蓋任何 PC，但實體索引仍須保存。
            // ICF 可讓不同符號共用同一段程式碼，並各自保留 unwind 記錄。
            // 保留所有實體索引；只容許完全相同的範圍，不接受部分相交或亂序。
            const bool folded = !runtime.functions.empty() && begin == previous_begin && end == previous_end;
            if (begin > end || begin < previous_begin || (begin < end && begin < previous_end && !folded)) reject("RUNTIME_FUNCTION 範圍未排序或重疊");
            image.check(begin, end - begin); image.check(unwind, 4);
            image.executable(begin,end-begin,"RUNTIME_FUNCTION 不在 executable section");
            const auto header = image.integer(unwind, 1), count_codes = image.integer(unwind + 2, 1);
            if((unwind&1U) || ((unwind&2U) && (header>>3)))reject("UNWIND_INFO 未對齊");
            if (((header & 7) != 1 && (header & 7) != 2) || (header >> 3) > 7 || ((header & 32) && (header & 24))) reject("尚未支援此 UNWIND_INFO 版本或旗標");
            const auto slots=(header>>3)?((count_codes + 1) & ~UINT64_C(1)):count_codes;
            image.check(unwind + 4, slots * 2);
            const auto tail = unwind + 4 + slots * 2;
            if (header & 32) image.check(tail, 12);
            else if (header & 24) { const auto handler = image.integer(tail, 4); image.check(handler, 1); }
            runtime.functions.push_back({static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(end), static_cast<std::uint32_t>(unwind)});
            previous_begin = begin;
            if(begin<end)previous_end = end;
        }
    }
    const auto tls = directories[9];
    if (tls.rva) {
        if (tls.size < pointer_size * 4 + 8) reject("TLS directory 截斷");
        const auto start = image.integer(tls.rva, pointer_size), end = image.integer(tls.rva + pointer_size, pointer_size);
        const auto index = image.integer(tls.rva + pointer_size * 2, pointer_size), callbacks = image.integer(tls.rva + pointer_size * 3, pointer_size);
        const auto zero = image.integer(tls.rva + pointer_size * 4, 4), characteristics_tls = image.integer(tls.rva + pointer_size * 4 + 4, 4);
        const bool empty_template=start==0 && end==0;
        if ((!empty_template && start < base) || end < start || end - start > memory_limit || zero > memory_limit - (end - start) || index < base || index - base > UINT32_MAX)
            reject("TLS template／index 範圍不合法");
        if ((characteristics_tls & ~UINT64_C(0x00f00000)) || (characteristics_tls >> 20) == 15) reject("TLS characteristics 包含未支援旗標");
        if(!empty_template)image.check(start-base,end-start);
        image.check(index-base,4);
        runtime.tls_index = index; runtime.tls_zero = static_cast<std::size_t>(zero);
        runtime.tls_alignment = characteristics_tls ? std::size_t{1} << ((characteristics_tls >> 20) - 1) : 16;
        if(!empty_template){runtime.tls_data.resize(static_cast<std::size_t>(end-start));image.read(start-base,runtime.tls_data);}
        if (callbacks) {
            if (callbacks < base || callbacks - base >= image_size) reject("TLS callback table 不在映像內");
            bool terminated = false;
            for (std::uint64_t i = 0; i < 1024; ++i) {
                const auto callback = image.integer(callbacks - base + i * pointer_size, pointer_size);
                if (!callback) { terminated = true; break; }
                if(callback<base)reject("TLS callback 不在 executable mapping");
                image.executable(callback-base,1,"TLS callback 不在 executable mapping");
                runtime.tls_callbacks.push_back(callback);
            }
            if (!terminated) reject("TLS callback table 缺少終止項目");
        }
    }
    parsed.mappings.push_back({0,permission(Access::Read),std::move(image.header_bytes)});
    for(auto& section:image.sections){
        const auto end=std::uint64_t{section.rva}+section.size;
        for(auto start=std::uint64_t{section.rva};start<end;){
            const auto protected_page=[&](std::uint64_t at){return protected_delay_pages.contains(static_cast<std::uint32_t>(at));};
            const auto permissions=protected_page(start)?permission(Access::Read):section.permissions;
            auto next=start+4096;
            while(next<end && (protected_page(next)?permission(Access::Read):section.permissions)==permissions)next+=4096;
            // 一般 section 直接交接所有權；只有延遲 IAT 的保護切割需要複製 slice。
            if(start==section.rva && next==end)
                parsed.mappings.push_back({section.rva,permissions,std::move(section.bytes)});
            else parsed.mappings.push_back({static_cast<std::uint32_t>(start),permissions,
                {section.bytes.begin()+static_cast<std::ptrdiff_t>(start-section.rva),
                 section.bytes.begin()+static_cast<std::ptrdiff_t>(next-section.rva)}});
            start=next;
        }
    }
    // 借用最終區段的唯讀資料，共用完整 opcode／chain 驗證；不再配置／複製整份暫時客體映像。
    if(!runtime.functions.empty()){
        try {
            std::vector<UnwindMappingView> views;views.reserve(parsed.mappings.size());
            for(const auto& mapping:parsed.mappings)
                views.push_back({GuestAddress{base+mapping.rva},mapping.bytes,mapping.permissions});
            validate_unwind(views,runtime);
        } catch(const HostCallFault& error){reject(error.what());}
        catch(const MemoryFault& error){reject(error.what());}
    }
    return parsed;
}
} // namespace
ParsedPeImage parse_pe_image(std::span<const std::uint8_t> bytes, std::optional<GuestAddress> image_base, bool dll, std::size_t memory_limit, bool resource_only, const PeImageBaseSelector& select_base, bool native_subsystem) {
    startup_profile::Scope measurement("loader","parse_pe_image",true);
    return parse_pe_image_impl(bytes,image_base,dll,memory_limit,resource_only,false,select_base,native_subsystem);
}
PeInventory inspect_pe_image(std::span<const std::uint8_t> bytes) {
    auto parsed=parse_pe_image_impl(bytes,{},false,GuestMemory::capacity_limit,false,true);
    return {std::move(parsed.metadata),std::move(parsed.imports),std::move(parsed.delay_imports),
        std::move(parsed.runtime.export_names),std::move(parsed.runtime.export_ordinals),
        std::move(parsed.runtime.export_forwarders),parsed.dll};
}
std::optional<PeResource> read_pe_resource(std::span<const std::uint8_t> bytes,
    std::u16string_view type,std::optional<std::uint16_t> id,std::uint16_t language){
    const auto parsed=parse_pe_image(bytes,{},false,256U*1024U*1024U,true);
    const Reader file{bytes};const auto opt=std::size_t{file.u32(60)}+24;
    const bool small=file.u16(opt)==0x10b;const auto directories=opt+(small?96:112);
    if(file.u32(opt+(small?92:108))<=2)return {};
    const auto root=file.u32(directories+16),length=file.u32(directories+20);if(!root)return {};
    const auto data=[&](std::uint64_t rva,std::size_t size)->std::span<const std::uint8_t>{
        for(const auto& mapping:parsed.mappings)if(rva>=mapping.rva && rva-mapping.rva<=mapping.bytes.size() && size<=mapping.bytes.size()-(rva-mapping.rva))
            return std::span{mapping.bytes}.subspan(static_cast<std::size_t>(rva-mapping.rva),size);
        reject("資源資料未完整落在映射區段");
    };
    const auto integer=[&](std::uint64_t offset,unsigned size){
        if(offset>length || size>length-offset)reject("資源目錄超出範圍");return Reader{data(std::uint64_t{root}+offset,size)}.integer(0,size);
    };
    struct Entry{std::uint32_t name,target;};
    const auto entries=[&](std::uint32_t at){
        const auto count=integer(at+12,2)+integer(at+14,2);std::vector<Entry> result;
        for(std::uint64_t i=0;i<count;++i)result.push_back({static_cast<std::uint32_t>(integer(at+16+i*8,4)),static_cast<std::uint32_t>(integer(at+20+i*8,4))});return result;
    };
    const auto child=[](Entry entry){if(!(entry.target&0x80000000U))reject("資源目錄層級缺少子目錄");return entry.target&0x7fffffffU;};
    std::optional<Entry> selected;
    std::optional<std::uint16_t> type_id;
    if(type.size()>1 && type.front()==u'#'){
        std::uint32_t number=0;bool numeric=true;
        for(const auto c:type.substr(1)){if(c<u'0' || c>u'9' || number>6553){numeric=false;break;}number=number*10+static_cast<unsigned>(c-u'0');}
        if(numeric && number<=0xffffU)type_id=static_cast<std::uint16_t>(number);
    }
    for(const auto entry:entries(0)){
      if(type_id && entry.name==*type_id){selected=entry;break;}
      if(entry.name&0x80000000U){
        const auto at=entry.name&0x7fffffffU;const auto count=integer(at,2);if(count!=type.size())continue;
        bool equal=true;for(std::size_t i=0;i<type.size();++i)if(integer(at+2+i*2,2)!=type[i]){equal=false;break;}
        if(equal){selected=entry;break;}
      }
    }
    if(!selected)return {};const auto names=entries(child(*selected));selected.reset();
    for(const auto entry:names)if(!id || entry.name==*id){selected=entry;break;}if(!selected)return {};
    const auto resource=*selected;const auto languages=entries(child(resource));if(languages.empty())return {};
    selected=languages.front();
    for(const auto desired:{std::uint16_t{0},language,static_cast<std::uint16_t>(language&0x3ffU)}){
        const auto found=std::find_if(languages.begin(),languages.end(),[&](Entry e){return e.name==desired;});
        if(found!=languages.end()){selected=*found;break;}
    }
    if(selected->target&0x80000000U)reject("資源語言層級不是資料項目");
    const auto rva=integer(selected->target,4),size=integer(std::uint64_t{selected->target}+4,4);
    const auto payload=data(rva,static_cast<std::size_t>(size));PeResource result;
    if(!(resource.name&0x80000000U)){if(resource.name>0xffff)reject("資源編號超過 WORD");result.id=static_cast<std::uint16_t>(resource.name);}
    result.bytes.assign(payload.begin(),payload.end());return result;
}
void map_pe_image(GuestMemory& memory,const ParsedPeImage& image){
    startup_profile::Scope measurement("loader","map_pe_image",true);
    std::size_t total=0;
    std::map<std::uint64_t,std::uint64_t> ranges;
    for(const auto& mapping:image.mappings){
        const auto base=image.metadata.loaded_base.value;
        const auto size=mapping.bytes.size();
        if(!size || mapping.rva>image.metadata.image_size || size>image.metadata.image_size-mapping.rva ||
            mapping.rva>UINT64_MAX-base || size>UINT64_MAX-base-mapping.rva ||
            (mapping.permissions&~7U))
            reject("解析後映像的映射範圍或權限不合法");
        const auto at=base+mapping.rva;
        const auto next=ranges.lower_bound(at);
        if((next!=ranges.end() && next->first<at+size) || (next!=ranges.begin() && std::prev(next)->second>at) ||
            !memory.range_available(GuestAddress{at},size))reject("PE 映射與既有範圍重疊");
        ranges.emplace(at,at+size);
        if(size>memory.remaining_capacity()-total)reject("PE 映射超過客體剩餘容量");
        total+=size;
    }
    std::vector<GuestAddress> mapped;
    mapped.reserve(image.mappings.size());
    try{
        for(const auto& mapping:image.mappings){
            const GuestAddress base{image.metadata.loaded_base.value+mapping.rva};
            memory.map(base,mapping.bytes.size(),mapping.permissions,mapping.bytes);
            mapped.push_back(base);
        }
    }catch(...){for(const auto base:mapped)memory.unmap(base);throw;}
}
void bind_pe_imports(GuestMemory& memory,const ParsedPeImage& image,const PeImportResolver& resolver){
    startup_profile::Scope measurement("loader","bind_pe_imports",true);
    const auto width = image.metadata.pointer_bytes();
    if(!resolver && !image.imports.empty())reject("匯入解析器未設定");
    struct Patch {GuestAddress address;std::array<std::uint8_t,8> bytes;GuestMemory::Mapping mapping;};
    std::vector<Patch> patches;patches.reserve(image.imports.size());
    for(const auto& imported:image.imports){
        if(imported.slot>image.metadata.image_size || image.metadata.image_size-imported.slot<width ||
            imported.slot>UINT64_MAX-image.metadata.loaded_base.value)reject("IAT slot 超出解析後映像");
        const GuestAddress address{image.metadata.loaded_base.value+imported.slot};
        const auto mapping=memory.query(address);
        if(!mapping ||
            mapping->size-(address.value-mapping->base.value)<width ||
            (!(mapping->permissions&permission(Access::Write)) && !(mapping->permissions&permission(Access::Read))))
            reject("IAT slot 不在可更新的資料映射");
        patches.push_back({address,{},*mapping});
    }
    // resolver 失敗時不留下部分 IAT 綁定；綁定後保留客體原始區段權限。
    for(std::size_t i=0;i<image.imports.size();++i){
        const auto target=resolver(image.imports[i]).value;
        if(width == 4 && target > UINT32_MAX) reject("PE32 匯入目標超過 32-bit 位址空間");
        if(!target)reject("匯入解析器回傳空函式地址");
        for(unsigned n=0;n<width;++n)patches[i].bytes[n]=static_cast<std::uint8_t>(target>>(n*8U));
    }
    for(const auto& patch:patches){
        const auto bytes=std::span{patch.bytes}.first(width);
        if(patch.mapping.permissions&permission(Access::Write))memory.write(patch.address,bytes);
        else if(!(patch.mapping.permissions&permission(Access::Execute)))memory.initialize_readonly(patch.address,bytes);
        else {
            // loader 在客體 worker 停止期間更新 RX IAT。走共用撤銷路徑，禁止靜默改寫舊翻譯。
            memory.protect(patch.mapping.base,patch.mapping.size,static_cast<Permissions>(patch.mapping.permissions|permission(Access::Write)));
            try{memory.write(patch.address,bytes);}catch(...){memory.protect(patch.mapping.base,patch.mapping.size,patch.mapping.permissions);throw;}
            memory.protect(patch.mapping.base,patch.mapping.size,patch.mapping.permissions);
        }
    }
}
PeProgram load_pe(std::span<const std::uint8_t> bytes,PeOptions options){
    startup_profile::Scope measurement("loader","load_pe",true);
    auto parsed=parse_pe_image(bytes,options.image_base,false,options.memory_limit);
    const bool x86 = parsed.metadata.architecture == PeArchitecture::X86;
    startup_profile::mark("pe_program_construct_begin");
    PeProgram program;
    startup_profile::mark("pe_program_construct_end");
    program.metadata=parsed.metadata;program.memory=GuestMemory{options.memory_limit};
    program.memory.set_execution_mode(x86 ? ExecutionMode::X86 : ExecutionMode::X64);
    map_pe_image(program.memory,parsed);
    GuestAddress stack_base{x86 ? UINT64_C(0x78000000) : UINT64_C(0x3000000000)}; // x64 主堆疊位於 MMU 視窗內。
    if(!x86 && prefer_low_guest_stack())
        if(const auto low=find_guest_address_gap(program.memory,UINT64_C(0x78000000),parsed.stack_size,UINT64_C(0x100000000)))stack_base=*low;
    {startup_profile::Scope phase("loader","map_stack",true);
        program.memory.map(stack_base,static_cast<std::size_t>(parsed.stack_size),read_write);}
    program.services.initialize(program.memory,options.command_line,options.filesystem_root,options.c_drive,options.writable_files,options.install_root,options.drive_mappings,options.certificate_roots_file,options.resident_translation);
    if(options.primary_token_descriptor>=0)program.services.import_primary_token(options.primary_token_descriptor);
    else if(!options.executable_path.empty())program.services.apply_manifest_elevation(options.executable_path);
    auto& runtime=parsed.runtime;
    if(!options.executable_path.empty())runtime.path=program.services.map_executable(options.executable_path);
    program.state.rip=program.metadata.entry;
    program.state.reg(Gpr::Rsp)=stack_base.value+parsed.stack_size-(x86 ? 4 : 40);
    program.memory.store(GuestAddress{program.state.reg(Gpr::Rsp)},x86 ? 32 : 64,x86 ? UINT64_C(0x7ffffff0) : return_sentinel.value);
    runtime.stack_base=stack_base.value;runtime.stack_size=parsed.stack_size;
    // parsed 僅由本次完整解析建立，映射後尚未執行客體；直接交接驗證結果。
    try{startup_profile::Scope phase("loader","configure_image",true);
        program.services.configure_image_after_unwind_validation(program.state,program.memory,std::move(runtime));}
    catch(const HostCallFault& error){reject(error.what());}
    catch(const MemoryFault& error){
        std::ostringstream detail;detail<<error.what()<<"，address=0x"<<std::hex<<error.address.value<<" access="<<access_name(error.access)<<" size="<<std::dec<<error.size;
        reject(detail.str());
    }
    program.services.configure_loader(options.executable_path,options.trap_unresolved_imports);
#ifdef X64ARM_HOST_SERVICES_TRANSLATION_IMAGE
    // 主程式與 DLL 同樣以內容雜湊作為翻譯身分；沒有宿主來源路徑（記憶體載入）時不提供。
    if(!options.executable_path.empty())program.services.set_main_translation(TranslationImage{options.executable_path,
        SmapCache::source_identity(bytes),parsed.metadata.loaded_base,parsed.metadata.preferred_base,parsed.metadata.image_size});
#endif
    parsed.mappings.clear();program.pending_imports=std::move(parsed);
    if(!options.defer_imports)complete_pe_load(program);
    return program;
}
void complete_pe_load(PeProgram& program){
    startup_profile::Scope measurement("loader","complete_pe_load",true);
    if(!program.pending_imports)return;
    bind_pe_imports(program.memory,*program.pending_imports,[&](const PeImport& imported){return program.services.bind_image_import(program.memory,imported,program.metadata.loaded_base.value);});
    program.services.prepare_image_startup(program.state);program.pending_imports.reset();
}
std::string format_pe(const PeMetadata& metadata) {
    std::ostringstream out;
    out << (metadata.architecture == PeArchitecture::X86 ? "PE32 i386" : "PE32+ AMD64") << " image_base=0x" << std::hex << metadata.loaded_base.value
        << " preferred_base=0x" << metadata.preferred_base.value << " entry=0x" << metadata.entry.value
        << " image_size=" << std::dec << metadata.image_size << " relocations=" << metadata.relocations << '\n';
    for (const auto& imported : metadata.imports) out << "import=" << imported << '\n';
    return out.str();
}
}
