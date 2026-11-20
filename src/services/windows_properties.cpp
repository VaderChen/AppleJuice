#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/shared_job.hpp"
#include <algorithm>
#include <bit>
#include <new>

namespace x64arm {
std::span<const WindowsServiceDescriptor> property_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"win32u.dll","NtUserBuildPropList",4},
        {"user32.dll","GetPropA",2},{"user32.dll","GetPropW",2},
        {"user32.dll","SetPropA",3},{"user32.dll","SetPropW",3},
        {"user32.dll","RemovePropA",2},{"user32.dll","RemovePropW",2},
        {"kernel32.dll","GlobalAddAtomA",1},{"kernel32.dll","GlobalAddAtomW",1},
        {"kernel32.dll","GlobalFindAtomA",1},{"kernel32.dll","GlobalFindAtomW",1},
        {"kernel32.dll","GlobalDeleteAtom",1},
        {"kernel32.dll","GlobalGetAtomNameA",3},{"kernel32.dll","GlobalGetAtomNameW",3},
        // 本機（程序內）Atom 表：與全域表相同的整數／字串 atom 規則，表格只屬於本程序。
        {"kernel32.dll","AddAtomA",1},{"kernel32.dll","AddAtomW",1},{"kernel32.dll","FindAtomA",1},{"kernel32.dll","FindAtomW",1},
        {"kernel32.dll","DeleteAtom",1,0,0,0x1},{"kernel32.dll","GetAtomNameA",3,0,0,0x5},{"kernel32.dll","GetAtomNameW",3,0,0,0x5},
        {"kernel32.dll","InitAtomTable",1,0,0,0x1}
    };return table;
}
void WindowsServices::release_gui_atom(std::uint16_t atom,const std::shared_ptr<SharedJob>& job){
    if(job){(void)job->delete_global_atom(atom);return;}
    const auto found=gui_.global_atoms.find(atom);if(found==gui_.global_atoms.end())return;
    if(--found->second.references)return;
    // 清理不得再配置字串；即使正在處理配置失敗，也能釋放完整鍵的生命週期。
    const auto name=std::find_if(gui_.global_atom_names.begin(),gui_.global_atom_names.end(),[&](const auto& item){return item.second==atom;});
    if(name!=gui_.global_atom_names.end())gui_.global_atom_names.erase(name);
    gui_.global_atoms.erase(found);
}
std::uint16_t gui_atom_table_add(std::map<std::uint16_t,GuiAtom>& atoms,std::map<std::u16string,std::uint16_t>& names,std::u16string text,bool add,std::uint32_t& error){
    error=0;
        auto key=text;for(auto& ch:key)ch=unicode_case_unit(ch,true);
        if(const auto found=names.find(key);found!=names.end()){
            if(add){auto& references=atoms.at(found->second).references;if(references==UINT32_MAX){error=8;return 0;};++references;}
            return found->second;
        }
        if(!add)return 0;
        std::uint32_t id=0xc000;while(id<=65535 && atoms.contains(static_cast<std::uint16_t>(id)))++id;
        if(id>65535){error=8;return 0;};const auto atom=static_cast<std::uint16_t>(id);
        atoms.emplace(atom,GuiAtom{std::move(text),1});
        try{names.emplace(std::move(key),atom);}catch(...){atoms.erase(atom);throw;}
        return atom;
}
std::optional<std::uint32_t> WindowsServices::invoke_properties(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;
    if(symbol=="NtUserBuildPropList"){
        const auto capacity=static_cast<std::uint32_t>(arg(1));
        const auto buffer=arg(2),count=arg(3);
        const auto window=gui_.windows.find(arg(0));
        std::uint32_t status=0;
        if(!buffer || !count)status=0xc000000dU;
        else if(window==gui_.windows.end())status=0xc0010000U | 1400U;
        else {
            const auto& properties=window->second.properties;
            std::uint64_t index=0;
            for(const auto& [key,entry]:properties){
                if(index==capacity)break;
                // 兩種 ABI 共用 16 位元組記錄；尾端填充保留呼叫端的內容。
                const auto address=buffer+16*index++;
                memory.store(GuestAddress{address},64,entry.value);
                memory.store(GuestAddress{address+8},16,key.second);
                memory.store(GuestAddress{address+10},8,entry.string_name);
            }
            memory.store(GuestAddress{count},32,properties.size());
            if(properties.size()>capacity)status=0xc0000023U;
        }
        arg.return_to(state,status);return std::nullopt;
    }
    const bool wide=symbol.ends_with('W'),property=service.family=="user32.dll";
    std::uint64_t result=0;
    // 整數 atom 以低位指標表示；值零直接回零，不能把它當作空字串加入表中。
    if(!property && !arg(0) && (symbol.starts_with("AddAtom") || symbol.starts_with("FindAtom") ||
       symbol.starts_with("GlobalAddAtom") || symbol.starts_with("GlobalFindAtom"))){
        arg.return_to(state,0);return std::nullopt;
    }
    const auto atom_job=job_atom_table();const auto atom_namespace=atom_job?atom_job->snapshot().identity:0;
    struct Failure {std::uint32_t error;};
    // 名稱解析共用：整數 atom（指標值 ≤ 0xFFFF 或 "#十進位"）回 {atom, 空字串}，字串 atom 回 {0, 名稱}。
    const auto parse_atom=[&](std::uint64_t pointer,bool property_key)->std::pair<std::uint16_t,std::u16string> {
        if(pointer<=65535){
            if(property_key){if(!pointer)throw Failure{87};return {static_cast<std::uint16_t>(pointer),{}};}
            if(pointer>=0xc000)throw Failure{87};return {static_cast<std::uint16_t>(pointer),{}};
        }
        std::u16string text;
        if(wide)text=read_utf16(memory,GuestAddress{pointer},255);
        else {
            std::string bytes;for(std::uint64_t i=0;;++i){
                if(i>UINT64_MAX-pointer)throw MemoryFault(GuestAddress{pointer},Access::Read,1,"atom 字串地址溢位");
                const auto ch=static_cast<char>(memory.load(GuestAddress{pointer+i},8));if(!ch)break;
                if(i==1020)throw Failure{87};bytes.push_back(ch);
            }
            text=utf8_to_utf16_lossy(bytes);
        }
        if(text.empty() || text.size()>255)throw Failure{87};
        if(text.front()==u'#' && text.size()>1 && std::all_of(text.begin()+1,text.end(),[](char16_t ch){return ch>=u'0' && ch<=u'9';})){
            std::uint32_t number=0;for(std::size_t i=1;i<text.size();++i){number=number*10U+static_cast<std::uint32_t>(text[i]-u'0');if(number>=0xc000)throw Failure{87};}
            if(!number)throw Failure{87};return {static_cast<std::uint16_t>(number),{}};
        }
        return {0,std::move(text)};
    };
    const auto table_atom=[&](std::map<std::uint16_t,GuiAtom>& atoms,std::map<std::u16string,std::uint16_t>& names,std::u16string text,bool add)->std::uint16_t{
        std::uint32_t error=0;const auto atom=gui_atom_table_add(atoms,names,std::move(text),add,error);if(error)throw Failure{error};return atom;};
    const auto atom_for=[&](std::uint64_t pointer,bool add,bool property_key)->std::uint16_t {
        auto [number,text]=parse_atom(pointer,property_key);if(number)return number;
        if(atom_job){std::uint32_t error=0;const auto atom=atom_job->global_atom(text,add,error);if(error && (add || error!=2))throw Failure{error};return atom;}
        return table_atom(gui_.global_atoms,gui_.global_atom_names,std::move(text),add);
    };
    try {
        if(service.family=="kernel32.dll" && !symbol.starts_with("Global")){
            // 本機 Atom：AddAtom／FindAtom／DeleteAtom／GetAtomName／InitAtomTable。
            auto& atoms=local_atoms_.atoms;auto& names=local_atoms_.names;
            if(symbol=="InitAtomTable"){result=1;}
            else if(symbol=="DeleteAtom"){
                // 成功回 0；整數 atom 不在表中，刪除一律成功。失敗時回原 atom。
                const auto atom=static_cast<std::uint16_t>(arg(0));result=0;
                if(atom>=0xc000){
                    const auto found=atoms.find(atom);
                    if(found==atoms.end()){last_error_=6;result=atom;}
                    else if(!--found->second.references){
                        const auto name=std::find_if(names.begin(),names.end(),[&](const auto& item){return item.second==atom;});
                        if(name!=names.end())names.erase(name);atoms.erase(found);
                    }
                }
            }else if(symbol.starts_with("GetAtomName")){
                const auto atom=static_cast<std::uint16_t>(arg(0));const auto capacity=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(2)));
                if(!atom)throw Failure{87};if(capacity<=0)throw Failure{234};
                std::u16string name;
                if(atom<0xc000)name=utf8_to_utf16("#"+std::to_string(atom));
                else {const auto found=atoms.find(atom);if(found==atoms.end())throw Failure{6};name=found->second.name;}
                const auto limit=static_cast<std::uint32_t>(capacity)-1U;
                if(wide){result=std::min<std::uint64_t>(name.size(),limit);memory.write(GuestAddress{arg(1)},utf16_bytes(name.substr(0,result)));}
                else {const auto bytes=utf16_to_utf8(name);const auto count=std::min<std::size_t>(bytes.size(),limit);
                    std::vector<std::uint8_t> output(bytes.begin(),bytes.begin()+static_cast<std::ptrdiff_t>(count));output.push_back(0);memory.write(GuestAddress{arg(1)},output);result=count;}
            }else {
                auto [number,text]=parse_atom(arg(0),false);
                result=number?number:table_atom(atoms,names,std::move(text),symbol.starts_with("Add"));
                if(!result)throw Failure{2};
            }
        }else if(property){
            const auto window=gui_.windows.find(arg(0));if(window==gui_.windows.end())throw Failure{1400};
            const bool set=symbol.starts_with("Set"),remove=symbol.starts_with("Remove");
            const auto atom=atom_for(arg(1),set,true);auto& properties=window->second.properties;
            const bool acquired=set && arg(1)>65535 && atom>=0xc000;
            const auto key=std::pair{atom>=0xc000?atom_namespace:UINT64_C(0),atom};const auto found=properties.find(key);
            if(set){
                if(found!=properties.end()){
                    found->second.value=arg(2);
                    found->second.string_name=arg(1)>65535;
                    if(acquired){if(found->second.owns_atom)release_gui_atom(atom,atom_job);else {found->second.owns_atom=true;found->second.atom_job=atom_job;}}
                }else {
                    try{properties.emplace(key,GuiProperty{arg(2),acquired,arg(1)>65535,atom_job});}
                    catch(...){if(acquired)release_gui_atom(atom,atom_job);throw;}
                }
                result=1;
            }else if(found!=properties.end()){
                result=found->second.value;
                if(remove){if(found->second.owns_atom)release_gui_atom(atom,found->second.atom_job);properties.erase(found);}
            }
        }else if(symbol=="GlobalDeleteAtom"){
            const auto atom=static_cast<std::uint16_t>(arg(0));
            if(atom_job){if(const auto error=atom_job->delete_global_atom(atom))throw Failure{error};}
            else {if(!atom || (atom>=0xc000 && !gui_.global_atoms.contains(atom)))throw Failure{87};release_gui_atom(atom);}
        }else if(symbol.starts_with("GlobalGetAtomName")){
            const auto atom=static_cast<std::uint16_t>(arg(0));
            const auto capacity=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(2)));
            if(!atom)throw Failure{87};if(capacity<=0)throw Failure{234};
            std::u16string name;
            if(atom<0xc000)name=utf8_to_utf16("#"+std::to_string(atom));
            else if(atom_job){std::uint32_t error=0;name=atom_job->global_atom_name(atom,error);if(error)throw Failure{error};}
            else {const auto found=gui_.global_atoms.find(atom);if(found==gui_.global_atoms.end())throw Failure{6};name=found->second.name;}
            if(wide){
                // W 的 nSize 剛好等於名稱長度時，Windows 回完整長度但沒有空間寫 NUL。
                result=std::min<std::uint64_t>(name.size(),static_cast<std::uint32_t>(capacity));
                auto bytes=utf16_bytes(name.substr(0,result));if(result==static_cast<std::uint32_t>(capacity))bytes.resize(bytes.size()-2);
                memory.write(GuestAddress{arg(1)},bytes);
            }else {
                auto bytes=utf16_to_utf8(name);const auto count=std::min<std::size_t>(bytes.size(),static_cast<std::uint32_t>(capacity)-1U);
                std::vector<std::uint8_t> output(bytes.begin(),bytes.begin()+static_cast<std::ptrdiff_t>(count));output.push_back(0);memory.write(GuestAddress{arg(1)},output);
                if(count<bytes.size())throw Failure{234};result=count;
            }
        }else {
            result=atom_for(arg(0),symbol.starts_with("GlobalAdd"),false);
            if(!result && arg(0))throw Failure{2};
        }
    }catch(const Failure& error){last_error_=error.error;result=0;}
    catch(const std::bad_alloc&){last_error_=8;result=0;}
    catch(const std::invalid_argument&){last_error_=87;result=0;}
    arg.return_to(state,result);return std::nullopt;
}
}
