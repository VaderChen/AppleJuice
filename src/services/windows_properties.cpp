#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/shared_job.hpp"
#include <algorithm>
#include <bit>
#include <new>

namespace x64arm {
std::span<const WindowsServiceDescriptor> property_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"user32.dll","GetPropA",2},{"user32.dll","GetPropW",2},
        {"user32.dll","SetPropA",3},{"user32.dll","SetPropW",3},
        {"user32.dll","RemovePropA",2},{"user32.dll","RemovePropW",2},
        {"kernel32.dll","GlobalAddAtomA",1},{"kernel32.dll","GlobalAddAtomW",1},
        {"kernel32.dll","GlobalFindAtomA",1},{"kernel32.dll","GlobalFindAtomW",1},
        {"kernel32.dll","GlobalDeleteAtom",1},
        {"kernel32.dll","GlobalGetAtomNameA",3},{"kernel32.dll","GlobalGetAtomNameW",3}
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
std::optional<std::uint32_t> WindowsServices::invoke_properties(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;
    const bool wide=symbol.ends_with('W'),property=service.family=="user32.dll";
    std::uint64_t result=0;
    const auto atom_job=job_atom_table();const auto atom_namespace=atom_job?atom_job->snapshot().identity:0;
    struct Failure {std::uint32_t error;};
    const auto atom_for=[&](std::uint64_t pointer,bool add,bool property_key)->std::uint16_t {
        if(pointer<=65535){
            if(property_key){if(!pointer)throw Failure{87};return static_cast<std::uint16_t>(pointer);}
            if(pointer>=0xc000)throw Failure{87};return static_cast<std::uint16_t>(pointer);
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
            if(!number)throw Failure{87};return static_cast<std::uint16_t>(number);
        }
        if(atom_job){std::uint32_t error=0;const auto atom=atom_job->global_atom(text,add,error);if(error && (add || error!=2))throw Failure{error};return atom;}
        auto key=text;for(auto& ch:key)ch=unicode_case_unit(ch,true);
        if(const auto found=gui_.global_atom_names.find(key);found!=gui_.global_atom_names.end()){
            if(add){auto& references=gui_.global_atoms.at(found->second).references;if(references==UINT32_MAX)throw Failure{8};++references;}
            return found->second;
        }
        if(!add)return 0;
        std::uint32_t id=0xc000;while(id<=65535 && gui_.global_atoms.contains(static_cast<std::uint16_t>(id)))++id;
        if(id>65535)throw Failure{8};const auto atom=static_cast<std::uint16_t>(id);
        gui_.global_atoms.emplace(atom,GuiAtom{std::move(text),1});
        try{gui_.global_atom_names.emplace(std::move(key),atom);}catch(...){gui_.global_atoms.erase(atom);throw;}
        return atom;
    };
    try {
        if(property){
            const auto window=gui_.windows.find(arg(0));if(window==gui_.windows.end())throw Failure{1400};
            const bool set=symbol.starts_with("Set"),remove=symbol.starts_with("Remove");
            const auto atom=atom_for(arg(1),set,true);auto& properties=window->second.properties;
            const bool acquired=set && arg(1)>65535 && atom>=0xc000;
            const auto key=std::pair{atom>=0xc000?atom_namespace:UINT64_C(0),atom};const auto found=properties.find(key);
            if(set){
                if(found!=properties.end()){
                    found->second.value=arg(2);
                    if(acquired){if(found->second.owns_atom)release_gui_atom(atom,atom_job);else {found->second.owns_atom=true;found->second.atom_job=atom_job;}}
                }else {
                    try{properties.emplace(key,GuiProperty{arg(2),acquired,atom_job});}
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
