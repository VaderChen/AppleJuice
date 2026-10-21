#include "x64arm/windows_unwind.hpp"
#include "x64arm/x87_fp.hpp"
#include "x64arm/host_services.hpp"
#include "x64arm/decoder.hpp"
#include "x64arm/startup_profile.hpp"
#include <algorithm>
#include <set>

namespace x64arm {
namespace {
std::uint64_t checked_add(std::uint64_t base, std::uint64_t offset) {
    if (offset > UINT64_MAX - base) throw HostCallFault("unwind 地址溢位");
    return base + offset;
}
std::uint64_t rva(const RuntimeImage& image, std::uint64_t offset, std::size_t size) {
    if (offset > image.size || size > image.size - offset) throw HostCallFault("unwind RVA 超出映像");
    return image.base + offset;
}
bool nonvolatile(unsigned reg) { return reg == 3 || reg == 5 || reg == 6 || reg == 7 || reg >= 12; }
struct Code { unsigned offset, operation, info; std::uint64_t argument; };
struct Info { unsigned flags, prolog, frame, frame_offset; std::vector<Code> codes; std::uint64_t tail; unsigned version{}, epilog_size{}; std::vector<unsigned> epilogs; };
class ImageMemoryView {
    std::vector<UnwindMappingView> mappings_;
    mutable const UnwindMappingView* recent_{};
    const UnwindMappingView* locate(GuestAddress address) const {
        const auto contains=[&](const auto& mapping){return address.value>=mapping.base.value && address.value-mapping.base.value<mapping.bytes.size();};
        if(recent_ && contains(*recent_))return recent_;
        auto found=std::upper_bound(mappings_.begin(),mappings_.end(),address.value,
            [](auto value,const auto& mapping){return value<mapping.base.value;});
        if(found==mappings_.begin() || !contains(*--found))return nullptr;
        return recent_=&*found;
    }
public:
    explicit ImageMemoryView(std::span<const UnwindMappingView> mappings):mappings_(mappings.begin(),mappings.end()){
        std::sort(mappings_.begin(),mappings_.end(),[](const auto& a,const auto& b){return a.base.value<b.base.value;});
        std::uint64_t end=0;
        for(const auto& mapping:mappings_){
            if(mapping.bytes.empty() || mapping.bytes.size()>UINT64_MAX-mapping.base.value || mapping.base.value<end || (mapping.permissions&~7U))
                throw HostCallFault("unwind 區段視圖範圍或權限不合法");
            end=mapping.base.value+mapping.bytes.size();
        }
    }
    bool mapped(GuestAddress address)const{return locate(address)!=nullptr;}
    void check(GuestAddress address,std::size_t size,Access access)const{
        if(size>UINT64_MAX-address.value)throw MemoryFault(address,access,size,"客體存取地址加法溢位");
        auto cursor=address;auto remaining=size;
        while(remaining){
            const auto* mapping=locate(cursor);
            if(!mapping)throw MemoryFault(cursor,access,size,"客體地址未映射");
            if(!(mapping->permissions&permission(access)))throw MemoryFault(cursor,access,size,"客體記憶體權限不足");
            const auto count=std::min(remaining,mapping->bytes.size()-static_cast<std::size_t>(cursor.value-mapping->base.value));
            cursor.value+=count;remaining-=count;
        }
    }
    std::uint64_t load(GuestAddress address,std::uint8_t width)const{
        const unsigned size=width/8U;check(address,size,Access::Read);
        std::uint64_t result=0;unsigned shift=0;
        for(unsigned remaining=size;remaining;){
            const auto* mapping=locate(address);
            const auto offset=static_cast<std::size_t>(address.value-mapping->base.value);
            const auto count=static_cast<unsigned>(std::min<std::size_t>(remaining,mapping->bytes.size()-offset));
            for(unsigned n=0;n<count;++n,shift+=8)result|=std::uint64_t{mapping->bytes[offset+n]}<<shift;
            address.value+=count;remaining-=count;
        }
        return result;
    }
};
template<class Memory>
Info parse(const Memory& memory, const RuntimeImage& image, std::uint64_t unwind, bool executing = true) {
    const auto at = rva(image, unwind, 4);
    const auto load = [&](std::uint64_t offset, std::uint8_t width) { return memory.load(GuestAddress{rva(image, unwind + offset, width / 8U)}, width); };
    const auto header = load(0, 8), count = load(2, 8), fp = load(3, 8);
    // 純 header／WORD codes 的手寫 assembly 紀錄可緊接前一紀錄，不必
    // 為不存在的 DWORD handler／chain 欄位補齊；有尾端欄位時仍要求 DWORD 對齊。
    if((unwind&1U) || ((unwind&2U) && (header>>3)))throw HostCallFault("UNWIND_INFO 未對齊");
    if (((header & 7) != 1 && (header & 7) != 2) || header >> 3 > 7 || ((header & 32) && (header & 24))) throw HostCallFault("UNWIND_INFO 只支援合法的版本 1／2 旗標");
    Info result{static_cast<unsigned>(header >> 3), static_cast<unsigned>(load(1, 8)), static_cast<unsigned>(fp & 15), static_cast<unsigned>(fp >> 4), {}, at + 4 + ((header>>3)?((count + 1) & ~UINT64_C(1)):count) * 2, 0, 0, {}};
    result.version=static_cast<unsigned>(header&7);
    // 手寫 leaf assembly 可使用 CONTEXT 中的 R10/R11 保存原 RSP；
    // 依四位元暫存器編碼還原，不把編譯器的非揮發保存慣例當作載入限制。
    if (!result.frame && result.frame_offset) throw HostCallFault("unwind frame register 不合法");
    unsigned previous = 256;bool saw_code=false;
    for (std::uint64_t i = 0; i < count;) {
        const auto offset = static_cast<unsigned>(load(4 + i * 2, 8)), opcode = static_cast<unsigned>(load(5 + i * 2, 8));
        const auto op = opcode & 15, info = opcode >> 4;
        if(op==6 && result.version==2 && !saw_code) {
            if(i==0) {
                if(!offset || info>1)throw HostCallFault("unwind v2 epilog 標頭不合法");
                result.epilog_size=offset;if(info)result.epilogs.push_back(offset);
            } else {
                const auto distance=(info<<8)|offset;
                if(distance)result.epilogs.push_back(distance);
            }
            ++i;continue;
        }
        if (offset > result.prolog || offset > previous) throw HostCallFault("unwind code 順序或 prolog offset 不合法");
        previous = offset;
        unsigned slots = 1; std::uint64_t argument = 0;
        switch (op) {
        // OpInfo 是完整的四位元暫存器編號；手寫 assembly 也會保存揮發暫存器。
        // 沿用共同還原流程，不能以編譯器的 callee-saved 慣例拒絕有效紀錄。
        case 0: break;
        case 1: if (info > 1) throw HostCallFault("ALLOC_LARGE OpInfo 不合法"); slots = info ? 3U : 2U; break;
        case 2: argument = info * 8U + 8U; break;
        case 3: if (!result.frame) throw HostCallFault("SET_FPREG 缺少 frame register"); break;
        case 4: case 5: slots = op == 4 ? 2U : 3U; break;
        case 8: case 9: slots = op == 8 ? 2U : 3U; break;
        case 10: if (info > 1) throw HostCallFault("PUSH_MACHFRAME 不合法"); break;
        default: throw HostCallFault("未支援的 unwind opcode");
        }
        if (slots > count - i) throw HostCallFault("unwind code 額外 slot 截斷");
        if (slots > 1) {
            argument = load(4 + (i + 1) * 2, slots == 2 ? 16 : 32);
            if (slots == 2) argument *= op == 8 ? 16U : 8U;
            if (argument % ((op == 8 || op == 9) ? 16U : 8U)) throw HostCallFault("unwind stack offset 未對齊");
        }
        // 載入驗證只需要完整檢查 opcode；真正還原堆疊時才保存執行清單。
        if(executing)result.codes.push_back({offset, op, info, argument});
        saw_code=true;i += slots;
    }
    // handler／chain 之前的 padding 必須存在；沒有尾端資料時只讀取實際 codes。
    memory.check(GuestAddress{rva(image, unwind, static_cast<std::size_t>(result.tail - at))}, static_cast<std::size_t>(result.tail - at), Access::Read);
    if (result.flags & 4) memory.check(GuestAddress{rva(image, result.tail - image.base, 12)}, 12, Access::Read);
    else if (result.flags & 3) {
        const auto handler = memory.load(GuestAddress{rva(image, result.tail - image.base, 4)}, 32);
        const GuestAddress target{rva(image, handler, 1)};
        // 載入只驗證 metadata 與映射；程式可在啟動後調整頁面保護。
        // 真正 unwind／派送時才要求 handler 可執行，不能因未使用的 handler 拒絕整個映像。
        if(executing)memory.check(target,1,Access::Execute);
        else if(!memory.mapped(target))throw HostCallFault("unwind handler 未映射");
    }
    return result;
}
template<class Memory>
RuntimeFunction chain(const Memory& memory, const RuntimeImage& image, std::uint64_t address) {
    const auto begin = memory.load(GuestAddress{address}, 32), end = memory.load(GuestAddress{address + 4}, 32), unwind = memory.load(GuestAddress{address + 8}, 32);
    if (begin >= end) throw HostCallFault("chained RUNTIME_FUNCTION 範圍不合法");
    memory.check(GuestAddress{rva(image, begin, static_cast<std::size_t>(end - begin))}, static_cast<std::size_t>(end - begin), Access::Execute);
    return {static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(end), static_cast<std::uint32_t>(unwind)};
}
RuntimeFunction primary_function(const GuestMemory& memory, const RuntimeImage& image, RuntimeFunction function) {
    std::array<std::uint32_t,32> visited{};std::size_t depth=0;
    for(;;) {
        if(depth==visited.size() || std::find(visited.begin(),visited.begin()+depth,function.unwind)!=visited.begin()+depth)
            throw HostCallFault("chained unwind 循環或過深");
        visited[depth++]=function.unwind;
        const auto info=parse(memory,image,function.unwind,false);
        if(!(info.flags&4))return function;
        function=chain(memory,image,info.tail);
    }
}
struct EpilogResult {GuestState caller;std::array<std::uint64_t,16> integer_sources{};};
std::optional<EpilogResult> epilog(const GuestMemory& memory, const RuntimeImage& image, const RuntimeFunction& function, const GuestState& context) {
    EpilogResult result{context};auto& next=result.caller;bool first = true;
    // x64 ABI epilog 僅允許 stack adjustment、POP 與 RET／向函式外 JMP。
    for (unsigned i = 0; i < 20; ++i) {
        const auto decoded = Decoder{}.decode_at(memory, next.rip);
        if (decoded.status != DecodeStatus::Ok) return std::nullopt;
        const auto& instruction = decoded.instruction;
        const auto& a = instruction.operands[0]; const auto& b = instruction.operands[1];
        const auto following = GuestAddress{checked_add(next.rip.value, instruction.length)};
        if (first && instruction.opcode == Opcode::Add && a.kind == OperandKind::Register && a.reg.index == Gpr::Rsp && a.width == 64 && b.kind == OperandKind::Immediate) {
            next.reg(Gpr::Rsp) = checked_add(next.reg(Gpr::Rsp), b.immediate);
        } else if (first && instruction.opcode == Opcode::Lea && a.kind == OperandKind::Register && a.reg.index == Gpr::Rsp && b.kind == OperandKind::Memory && b.memory.base && nonvolatile(static_cast<unsigned>(b.memory.base->index)) && !b.memory.index) {
            next.reg(Gpr::Rsp) = effective_address(b, instruction, next, following).value;
        } else if (instruction.opcode == Opcode::Pop && a.kind == OperandKind::Register && a.width == 64 && nonvolatile(static_cast<unsigned>(a.reg.index))) {
            result.integer_sources[static_cast<unsigned>(a.reg.index)]=next.reg(Gpr::Rsp);
            next.reg(a.reg.index) = memory.load(GuestAddress{next.reg(Gpr::Rsp)}, 64); next.reg(Gpr::Rsp) = checked_add(next.reg(Gpr::Rsp), 8);
        } else if (instruction.opcode == Opcode::Ret) {
            next.rip = GuestAddress{memory.load(GuestAddress{next.reg(Gpr::Rsp)}, 64)};
            next.reg(Gpr::Rsp) = checked_add(next.reg(Gpr::Rsp), 8 + (instruction.operand_count ? a.immediate : 0)); return result;
        } else if (instruction.opcode == Opcode::Jmp) {
            // 尾呼叫的 caller context 仍取 return slot，不執行跳躍目標。
            if (a.kind == OperandKind::Immediate) {
                const auto target = following.value + a.immediate;
                if (target >= image.base + function.begin && target < image.base + function.end) return std::nullopt;
                // MSVC 可將同一函式拆成多個 RUNTIME_FUNCTION；跨分段跳躍不是尾呼叫。
                // 比對完整 primary 身分，避免合併僅共用相同 xdata 的獨立函式。
                if(const auto* destination=lookup_function(image,target);destination &&
                   primary_function(memory,image,*destination)==primary_function(memory,image,function))return std::nullopt;
            } else {
                // ABI 以編碼區分尾呼叫與一般間接分支：記憶體 JMP 僅允許
                // ModRM.mod=00；暫存器 JMP 必須明確帶 REX.W。
                // 例如不帶 W 的 JMP R9 可用於函式內跳躍表，應改走 xdata，
                // 不能拒絕展開或把尚未釋放的 stack allocation 當成 return slot。
                std::size_t opcode=0;unsigned rex=0;
                if (instruction.bytes[0]>=0x40 && instruction.bytes[0]<=0x4f) {
                    rex=instruction.bytes[0];++opcode;
                }
                if (opcode+1>=instruction.length || instruction.bytes[opcode]!=0xff) return std::nullopt;
                const auto modrm=instruction.bytes[opcode+1];
                if (((modrm>>3)&7U)!=4U || a.width!=64) return std::nullopt;
                if (a.kind==OperandKind::Memory) {
                    if ((modrm&0xc0U)!=0) return std::nullopt;
                } else if (a.kind==OperandKind::Register) {
                    if ((modrm&0xc0U)!=0xc0U || !(rex&8U)) return std::nullopt;
                } else return std::nullopt;
            }
            next.rip = GuestAddress{memory.load(GuestAddress{next.reg(Gpr::Rsp)}, 64)};
            next.reg(Gpr::Rsp) = checked_add(next.reg(Gpr::Rsp), 8); return result;
        } else return std::nullopt;
        next.rip = following; first = false;
        if (next.rip.value >= image.base + function.end) return std::nullopt;
    }
    return std::nullopt;
}
}
const RuntimeFunction* lookup_function(const RuntimeImage& image, std::uint64_t pc, std::uint64_t function_entry) {
    if (pc < image.base || pc - image.base >= image.size) return nullptr;
    const auto offset = pc - image.base;
    if(function_entry){
        if(function_entry<image.function_table)return nullptr;
        const auto delta=function_entry-image.function_table;
        if(delta%12 || delta/12>=image.functions.size())return nullptr;
        const auto& row=image.functions[static_cast<std::size_t>(delta/12)];
        return row.begin<=offset && offset<row.end?&row:nullptr;
    }
    auto found = std::upper_bound(image.functions.begin(), image.functions.end(), offset,
        [](std::uint64_t value, const RuntimeFunction& function) { return value < function.begin; });
    while(found!=image.functions.begin()){
        const auto& function=*--found;
        if(function.begin==function.end)continue;
        return offset<function.end?&function:nullptr;
    }
    return nullptr;
}
namespace {
template<class Memory>
void validate_unwind_impl(const Memory& memory, const RuntimeImage& image) {
    for (const auto& function : image.functions) {
        if(function.begin==function.end)continue;
        auto current = function;std::array<std::uint32_t,32> visited{};std::size_t depth=0;
        for (;;) {
            if(depth==visited.size() || std::find(visited.begin(),visited.begin()+depth,current.unwind)!=visited.begin()+depth)throw HostCallFault("chained unwind 循環或過深");
            visited[depth++]=current.unwind;
            const auto info = parse(memory, image, current.unwind, false);
            if (info.prolog > current.end - current.begin) throw HostCallFault("unwind prolog 超出函式範圍");
            for(const auto distance:info.epilogs)if(distance<info.epilog_size || distance>current.end-current.begin-info.prolog)throw HostCallFault("unwind v2 epilog 超出函式範圍");
            if (!(info.flags & 4)) break;
            const auto linked = chain(memory, image, info.tail);
            const auto parent = parse(memory, image, linked.unwind, false);
            if (info.frame != parent.frame || info.frame_offset != parent.frame_offset) throw HostCallFault("chained unwind frame register 不一致");
            current = linked;
        }
    }
}
}
void validate_unwind(const GuestMemory& memory, const RuntimeImage& image) {
    startup_profile::Scope measurement("unwind","validate_memory",true);
    validate_unwind_impl(memory,image);
}
void validate_unwind(std::span<const UnwindMappingView> mappings, const RuntimeImage& image) {
    startup_profile::Scope measurement("unwind","validate_image",true);
    validate_unwind_impl(ImageMemoryView{mappings},image);
}
UnwindResult virtual_unwind(const GuestMemory& memory, const RuntimeImage& image, std::uint64_t pc, const GuestState& context, unsigned handler_type, std::uint64_t function_entry) {
    UnwindResult result{context, context.reg(Gpr::Rsp), 0, 0, 0};
    auto& next = result.caller;
    const auto* found = lookup_function(image, pc, function_entry);
    if(function_entry && !found)throw HostCallFault("RUNTIME_FUNCTION entry 不符合 ControlPc");
    if (!found) {
        next.rip = GuestAddress{memory.load(GuestAddress{next.reg(Gpr::Rsp)}, 64)};
        next.reg(Gpr::Rsp) = checked_add(next.reg(Gpr::Rsp), 8); return result;
    }
    result.function_entry = image.function_table + static_cast<std::uint64_t>(found - image.functions.data()) * 12;
    auto current = *found;
    const auto initial = parse(memory, image, current.unwind);
    const auto prolog_offset = pc - image.base - current.begin;
    bool frame_set = prolog_offset >= initial.prolog;
    for (const auto& code : initial.codes) if (code.operation == 3 && code.offset <= prolog_offset) frame_set = true;
    if (initial.frame && frame_set) result.establisher = context.gpr[initial.frame] - initial.frame_offset * 16U;
    const bool in_epilog=initial.version==1 || std::any_of(initial.epilogs.begin(),initial.epilogs.end(),[&](unsigned distance){const auto begin=std::uint64_t{current.end}-distance;return pc-image.base>=begin && pc-image.base-begin<initial.epilog_size;});
    if (prolog_offset >= initial.prolog && in_epilog) {
        auto at_pc = context; at_pc.rip = GuestAddress{pc};
        if (const auto caller = epilog(memory, image, current, at_pc)) { result.caller = caller->caller;result.integer_sources=caller->integer_sources;return result; }
    }
    std::set<std::uint32_t> visited;
    bool first = true;
    for (;;) {
        if (visited.size() >= 32 || !visited.insert(current.unwind).second) throw HostCallFault("chained unwind 循環或過深");
        const auto info = parse(memory, image, current.unwind);
        for (const auto& code : info.codes) {
            if (first && code.offset > prolog_offset) continue;
            auto& sp = next.reg(Gpr::Rsp);
            switch (code.operation) {
            case 0: result.integer_sources[code.info]=sp;next.gpr[code.info] = memory.load(GuestAddress{sp}, 64); sp = checked_add(sp, 8); break;
            case 1: case 2: sp = checked_add(sp, code.argument); break;
            case 3: sp = next.gpr[info.frame] - info.frame_offset * 16U; break;
            case 4: case 5: result.integer_sources[code.info]=checked_add(sp, code.argument);next.gpr[code.info] = memory.load(GuestAddress{result.integer_sources[code.info]}, 64); break;
            case 8: case 9: result.floating_sources[code.info]=checked_add(sp, code.argument);memory.read(GuestAddress{result.floating_sources[code.info]}, next.xmm[code.info]); break;
            case 10: {
                const auto frame = checked_add(sp, code.info * 8U);
                next.rip = GuestAddress{memory.load(GuestAddress{frame}, 64)};
                next.rflags = memory.load(GuestAddress{frame + 16}, 64);
                sp = memory.load(GuestAddress{frame + 24}, 64); return result;
            }
            default: throw HostCallFault("未支援的 unwind code");
            }
        }
        if (info.flags & 4) { current = chain(memory, image, info.tail); first = false; continue; }
        if ((info.flags & handler_type) && prolog_offset >= initial.prolog) {
            result.handler = image.base + memory.load(GuestAddress{info.tail}, 32); result.handler_data = info.tail + 4; result.handler_flags = info.flags;
        }
        break;
    }
    next.rip = GuestAddress{memory.load(GuestAddress{next.reg(Gpr::Rsp)}, 64)};
    next.reg(Gpr::Rsp) = checked_add(next.reg(Gpr::Rsp), 8); return result;
}
void write_context(GuestMemory& memory, std::uint64_t address, const GuestState& state) {
    write_context(memory,address,state,memory.execution_mode());
}
void write_context(GuestMemory& memory, std::uint64_t address, const GuestState& state, ExecutionMode mode) {
    if(mode==ExecutionMode::X86) {
        memory.write(GuestAddress{address},std::array<std::uint8_t,716>{});
        memory.store(GuestAddress{address},32,0x1002f); // i386 CONTROL／INTEGER／SEGMENTS／FLOAT／EXTENDED
        for(std::size_t i=0;i<6;++i)memory.store(GuestAddress{address+4+i*4},32,state.debug_registers[i]);
        constexpr std::array registers{Gpr::Rdi,Gpr::Rsi,Gpr::Rbx,Gpr::Rdx,Gpr::Rcx,Gpr::Rax,Gpr::Rbp};
        for(std::size_t i=0;i<registers.size();++i)memory.store(GuestAddress{address+156+i*4},32,state.reg(registers[i]));
        constexpr std::array<unsigned,6> segments{148,188,200,152,144,140};
        for(std::size_t i=0;i<segments.size();++i)memory.store(GuestAddress{address+segments[i]},32,state.segments[i]);
        memory.store(GuestAddress{address+184},32,state.rip.value);memory.store(GuestAddress{address+192},32,state.rflags);memory.store(GuestAddress{address+196},32,state.reg(Gpr::Rsp));
        memory.store(GuestAddress{address+28},32,state.x87_control);memory.store(GuestAddress{address+32},32,state.x87_status);memory.store(GuestAddress{address+36},32,state.x87_tag);
        memory.store(GuestAddress{address+40},32,state.x87_instruction_pointer);memory.store(GuestAddress{address+44},32,state.x87_code_selector|(std::uint32_t{state.x87_opcode}<<16));
        memory.store(GuestAddress{address+48},32,state.x87_data_pointer);memory.store(GuestAddress{address+52},32,state.x87_data_selector);
        const auto fx=address+204;
        memory.store(GuestAddress{fx},16,state.x87_control);memory.store(GuestAddress{fx+2},16,state.x87_status);memory.store(GuestAddress{fx+6},16,state.x87_opcode);
        memory.store(GuestAddress{fx+8},32,state.x87_instruction_pointer);memory.store(GuestAddress{fx+12},16,state.x87_code_selector);
        memory.store(GuestAddress{fx+16},32,state.x87_data_pointer);memory.store(GuestAddress{fx+20},16,state.x87_data_selector);
        memory.store(GuestAddress{fx+24},32,state.mxcsr);memory.store(GuestAddress{fx+28},32,0xffff);
        unsigned abridged=0;
        for(unsigned i=0;i<8;++i){
            if(((state.x87_tag>>(i*2))&3U)!=3)abridged|=1U<<i;
            const auto physical=(((state.x87_status>>11)&7U)+i)&7U;
            for(const auto at:{address+56+i*10,fx+32+i*16}){memory.store(GuestAddress{at},64,state.x87[physical].significand);memory.store(GuestAddress{at+8},16,state.x87[physical].sign_exponent);}
            memory.write(GuestAddress{fx+160+i*16},state.xmm[i]);
        }
        memory.store(GuestAddress{fx+4},8,abridged);return;
    }
    memory.write(GuestAddress{address}, std::array<std::uint8_t, 1232>{});
    memory.store(GuestAddress{address + 48}, 32, 0x10000b); // AMD64 CONTROL | INTEGER | FLOATING_POINT
    memory.store(GuestAddress{address + 52}, 32, state.mxcsr);
    constexpr std::array<unsigned,6> segment_offsets{60,56,66,58,62,64};
    for(std::size_t i=0;i<6;++i)memory.store(GuestAddress{address+segment_offsets[i]},16,state.segments[i]);
    memory.store(GuestAddress{address + 68}, 32, state.rflags);
    for(std::size_t i=0;i<6;++i)memory.store(GuestAddress{address+72+i*8},64,state.debug_registers[i]);
    for (std::size_t i = 0; i < 16; ++i) memory.store(GuestAddress{address + 120 + i * 8}, 64, state.gpr[i]);
    memory.store(GuestAddress{address + 248}, 64, state.rip.value);
    memory.store(GuestAddress{address + 256}, 16, state.x87_control); memory.store(GuestAddress{address + 258}, 16, state.x87_status);
    memory.store(GuestAddress{address + 262},16,state.x87_opcode);
    memory.store(GuestAddress{address + 264},32,state.x87_instruction_pointer);memory.store(GuestAddress{address + 268},16,state.x87_code_selector);
    memory.store(GuestAddress{address + 272},32,state.x87_data_pointer);memory.store(GuestAddress{address + 276},16,state.x87_data_selector);
    unsigned abridged = 0;
    for (unsigned i = 0; i < 8; ++i) {
        if (((state.x87_tag >> (i * 2)) & 3U) != 3) abridged |= 1U << i;
        const auto physical = (((state.x87_status >> 11) & 7U) + i) & 7U;
        memory.store(GuestAddress{address + 288 + i * 16}, 64, state.x87[physical].significand);
        memory.store(GuestAddress{address + 296 + i * 16}, 16, state.x87[physical].sign_exponent);
    }
    memory.store(GuestAddress{address + 260}, 8, abridged);
    memory.store(GuestAddress{address + 280}, 32, state.mxcsr); memory.store(GuestAddress{address + 284}, 32, 0xffff);
    for (std::size_t i = 0; i < 16; ++i) memory.write(GuestAddress{address + 416 + i * 16}, state.xmm[i]);
}
GuestState read_context(const GuestMemory& memory, std::uint64_t address, const GuestState& previous) {
    if(memory.execution_mode()==ExecutionMode::X86){
        memory.check(GuestAddress{address},716,Access::Read);auto state=previous;state.rep_compare={};
        const auto flags=memory.load(GuestAddress{address},32);if(!(flags&0x10000))throw HostCallFault("CONTEXT 缺少 i386 標記");
        if(flags&0x10){
            std::array<std::uint64_t,6> debug{};for(std::size_t i=0;i<6;++i)debug[i]=memory.load(GuestAddress{address+4+i*4},32);
            if(debug[5]&0x20ffU)throw HostCallFault("CPU 尚未支援啟用硬體 breakpoint comparator");
            debug[4]=(debug[4]&0xe00fU)|0xffff0ff0U;debug[5]=(debug[5]&0xffff0f00U)|0x400U;state.debug_registers=debug;
        }
        if(flags&2){constexpr std::array registers{Gpr::Rdi,Gpr::Rsi,Gpr::Rbx,Gpr::Rdx,Gpr::Rcx,Gpr::Rax};for(std::size_t i=0;i<registers.size();++i)state.reg(registers[i])=memory.load(GuestAddress{address+156+i*4},32);}
        if(flags&1){state.reg(Gpr::Rbp)=memory.load(GuestAddress{address+180},32);state.reg(Gpr::Rsp)=memory.load(GuestAddress{address+196},32);state.rip=GuestAddress{memory.load(GuestAddress{address+184},32)};state.rflags=(memory.load(GuestAddress{address+192},32)&0xcd5U)|2U;}
        // Selector 只能回復目前的平面區段；任意 LDT／TEB 基底重設不屬 CONTEXT 子集。
        constexpr std::array<unsigned,6> segments{148,188,200,152,144,140};
        for(std::size_t i=0;i<segments.size();++i)if((flags&((i==1 || i==2)?1U:4U)) && memory.load(GuestAddress{address+segments[i]},16)!=state.segments[i])throw HostCallFault("CONTEXT 要求未支援的區段 selector 切換");
        if(flags&0x20){
            const auto fx=address+204;state.x87_control=static_cast<std::uint16_t>(memory.load(GuestAddress{fx},16));state.x87_status=static_cast<std::uint16_t>(memory.load(GuestAddress{fx+2},16));
            const auto abridged=memory.load(GuestAddress{fx+4},8);state.x87_tag=0;
            state.x87_opcode=static_cast<std::uint16_t>(memory.load(GuestAddress{fx+6},16));state.x87_instruction_pointer=memory.load(GuestAddress{fx+8},32);state.x87_code_selector=static_cast<std::uint16_t>(memory.load(GuestAddress{fx+12},16));
            state.x87_data_pointer=memory.load(GuestAddress{fx+16},32);state.x87_data_selector=static_cast<std::uint16_t>(memory.load(GuestAddress{fx+20},16));
            for(unsigned i=0;i<8;++i){const auto physical=(((state.x87_status>>11)&7U)+i)&7U;auto& value=state.x87[physical];value.significand=memory.load(GuestAddress{fx+32+i*16},64);value.sign_exponent=static_cast<std::uint16_t>(memory.load(GuestAddress{fx+40+i*16},16));state.x87_tag=static_cast<std::uint16_t>(state.x87_tag|(((abridged&(1U<<physical))?x87_tag(value):3U)<<(physical*2)));memory.read(GuestAddress{fx+160+i*16},state.xmm[i]);}
            state.mxcsr=static_cast<std::uint32_t>(memory.load(GuestAddress{fx+24},32));if(state.mxcsr&0xffff0000U)throw HostCallFault("CONTEXT 包含無效 MXCSR 保留位元");
        }else if(flags&8){
            state.x87_control=static_cast<std::uint16_t>(memory.load(GuestAddress{address+28},32));state.x87_status=static_cast<std::uint16_t>(memory.load(GuestAddress{address+32},32));state.x87_tag=static_cast<std::uint16_t>(memory.load(GuestAddress{address+36},32));
            state.x87_instruction_pointer=memory.load(GuestAddress{address+40},32);state.x87_code_selector=static_cast<std::uint16_t>(memory.load(GuestAddress{address+44},16));state.x87_opcode=static_cast<std::uint16_t>(memory.load(GuestAddress{address+46},16));state.x87_data_pointer=memory.load(GuestAddress{address+48},32);state.x87_data_selector=static_cast<std::uint16_t>(memory.load(GuestAddress{address+52},16));
            for(unsigned i=0;i<8;++i){auto& value=state.x87[(((state.x87_status>>11)&7U)+i)&7U];value.significand=memory.load(GuestAddress{address+56+i*10},64);value.sign_exponent=static_cast<std::uint16_t>(memory.load(GuestAddress{address+64+i*10},16));}
        }
        return state;
    }
    // CONTEXT 只包含架構狀態；外部還原後重新開始 REP，不能沿用宿主切片的續跑資訊。
    memory.check(GuestAddress{address}, 1232, Access::Read); auto state = previous;state.rep_compare={};
    const auto flags_value = memory.load(GuestAddress{address + 48}, 32);
    if (!(flags_value & 0x100000)) throw HostCallFault("CONTEXT 缺少 AMD64 標記");
    if(flags_value&0x10){
        std::array<std::uint64_t,6> debug{};for(std::size_t i=0;i<6;++i)debug[i]=memory.load(GuestAddress{address+72+i*8},64);
        if(debug[5]&0x20ffU)throw HostCallFault("CPU 尚未支援啟用硬體 breakpoint comparator");
        debug[4]=(debug[4]&0xe00fU)|0xffff0ff0U;debug[5]=(debug[5]&0xffff0f00U)|0x400U;state.debug_registers=debug;
    }
    if (flags_value & 2) for (std::size_t i = 0; i < 16; ++i) if (i != 4) state.gpr[i] = memory.load(GuestAddress{address + 120 + i * 8}, 64);
    if (flags_value & 1) {
        state.reg(Gpr::Rsp) = memory.load(GuestAddress{address + 152}, 64);
        state.rip = GuestAddress{memory.load(GuestAddress{address + 248}, 64)};
        state.rflags = (memory.load(GuestAddress{address + 68}, 32) & UINT64_C(0x00000cd5)) | 2;
    }
    if (flags_value & 8) {
        state.x87_control = static_cast<std::uint16_t>(memory.load(GuestAddress{address + 256}, 16));
        state.x87_status = static_cast<std::uint16_t>(memory.load(GuestAddress{address + 258}, 16));
        state.x87_opcode=static_cast<std::uint16_t>(memory.load(GuestAddress{address + 262},16));
        state.x87_instruction_pointer=memory.load(GuestAddress{address + 264},32);state.x87_code_selector=static_cast<std::uint16_t>(memory.load(GuestAddress{address + 268},16));
        state.x87_data_pointer=memory.load(GuestAddress{address + 272},32);state.x87_data_selector=static_cast<std::uint16_t>(memory.load(GuestAddress{address + 276},16));
        const auto abridged = memory.load(GuestAddress{address + 260}, 8);
        state.x87_tag = 0;
        for (unsigned i = 0; i < 8; ++i) {
            const auto physical = (((state.x87_status >> 11) & 7U) + i) & 7U;
            auto& value = state.x87[physical];
            value.significand = memory.load(GuestAddress{address + 288 + i * 16}, 64);
            value.sign_exponent = static_cast<std::uint16_t>(memory.load(GuestAddress{address + 296 + i * 16}, 16));
            const auto tag = (abridged & (1U << physical)) ? x87_tag(value) : 3U;
            state.x87_tag = static_cast<std::uint16_t>(state.x87_tag | (tag << (physical * 2)));
        }
        state.mxcsr = static_cast<std::uint32_t>(memory.load(GuestAddress{address + 52}, 32));
        if (state.mxcsr & 0xffff0000U) throw HostCallFault("CONTEXT 包含無效 MXCSR 保留位元");
        for (std::size_t i = 0; i < 16; ++i) memory.read(GuestAddress{address + 416 + i * 16}, state.xmm[i]);
    }
    return state;
}
}
