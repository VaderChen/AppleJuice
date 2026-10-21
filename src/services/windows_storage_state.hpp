#pragma once
#include "x64arm/compound_file.hpp"
#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"

namespace x64arm {
namespace storage {
constexpr std::uint32_t pointer_error=0x80030009U,invalid_flag=0x800300ffU,invalid_parameter=0x80030057U,invalid_name=0x800300fcU;
constexpr std::uint32_t access_denied=0x80030005U,missing=0x80030002U,exists=0x80030050U,reverted=0x80030102U;
constexpr std::uint32_t no_memory=0x80030008U,medium_full=0x80030070U,lock_violation=0x80030021U,invalid_function=0x80030001U;
constexpr std::uint32_t invalid_header=0x800300fbU,read_fault=0x8003001eU,write_fault=0x8003001dU,not_current=0x80030101U;
constexpr std::uint32_t create=0x1000,transacted=0x10000,convert=0x20000;
struct Error {std::uint32_t status;};
inline std::uint32_t error(const compound::Error& value){
    switch(value.failure){case compound::Failure::InvalidName:return invalid_name;case compound::Failure::Duplicate:return exists;case compound::Failure::Missing:return missing;case compound::Failure::TooLarge:return medium_full;default:return invalid_header;}
}
inline bool writable(std::uint32_t mode){return (mode&3)!=0;}
inline bool readable(std::uint32_t mode){return (mode&3)!=1;}
inline bool child_access(std::uint32_t parent,std::uint32_t child){return (!writable(child)||writable(parent)) && (!readable(child)||readable(parent));}
inline std::u16string name(const GuestMemory& memory,std::uint64_t address){
    if(!address)throw Error{invalid_name};std::u16string result;
    for(unsigned i=0;i<32;++i){const auto value=static_cast<char16_t>(memory.load(GuestAddress{address+i*2ULL},16));if(!value){if(!compound::valid_name(result))throw Error{invalid_name};return result;}result.push_back(value);}
    throw Error{invalid_name};
}
inline void mode(std::uint32_t value,bool root,bool stream,bool creating){
    const auto allowed=3U|0x70U|(creating?create:0U)|(stream?0U:transacted)|(root && creating?convert:0U);
    if((value&~allowed) || (value&3)==3 || (value&0x70)>0x40 || ((value&create) && (value&convert)) ||
       (!root && (value&0x70)!=0x10) || (root && !(value&transacted) && (value&0x70)!=0x10) || (root && creating && !writable(value)))throw Error{invalid_flag};
}
inline std::uint64_t method(const GuestMemory& memory,std::uint64_t object,unsigned slot){
    if(!object)throw Error{pointer_error};const auto unit=memory.execution_mode()==ExecutionMode::X86?4U:8U;
    const auto bits=static_cast<std::uint8_t>(unit*8);const auto table=memory.load(GuestAddress{object},bits);
    if(!table || slot>(UINT64_MAX-table)/unit)throw Error{pointer_error};const auto target=memory.load(GuestAddress{table+slot*unit},bits);if(!target)throw Error{pointer_error};return target;
}
}
struct StorageState {
    enum class Type {Storage,Stream,Enumerator};
    struct View {
        compound::Document tree,committed;std::uint64_t document{},parent{},parent_generation{},generation{1};
        std::uint32_t root{},mode{};bool operator==(const View&)const=default;
    };
    struct Document {
        std::uint64_t backend{},root_view{},busy{};std::uint32_t revision{},next_node{1},locks{};
        bool owns_backend{true};
        bool operator==(const Document&)const=default;
    };
    struct Object {
        Type type{Type::Storage};std::uint64_t view{},generation{},position{},parent{};std::uint32_t node{},mode{},references{1};bool invalidated{};
        std::vector<compound::Node> enumeration{};bool operator==(const Object&)const=default;
    };
    enum class Kind {Create,Open,IsStorage,Persist};
    enum class Phase {Start,AddRef,Stat,Lock,Read,Resize,WriteBody,FlushBody,WriteHeader,Flush,Trim,RestoreSize,Restore,RestoreFlush,Unlock,Release};
    struct Operation {
        Kind kind{Kind::Persist};Phase phase{Phase::Start};std::uint64_t backend{},document{},view{},caller{},output{},new_object{},scratch{},buffer{},offset{},backend_size{};
        std::uint32_t mode{},flags{},status{},locks{},requested{},success{},count{};std::uint64_t count_output{},previous_position{};
        bool held{},locked{},modified{},published{},append{},commit{},position_changed{};
        std::vector<std::uint8_t> previous{},bytes{};std::map<std::uint64_t,compound::Document> rollback{},checkpoints{};
        std::vector<std::uint64_t> commit_views{},new_views{};compound::Document initial;
        bool operator==(const Operation&)const=default;
    };
    struct Copy {
        struct Frame {compound::Node node;std::uint64_t parent{},object{};bool root{},opened{},stream{};bool operator==(const Frame&)const=default;};
        enum class Phase {AddRef,Create,Open,Class,State,Times,Write,ReleaseElement,ReleaseOwned,ReleaseDestination,Finish};
        Phase phase{Phase::AddRef};std::uint64_t source{},destination{},scratch{},buffer{},move_node{},offset{};std::uint32_t status{},flags{},requested{};
        std::vector<Frame> frames{};std::vector<std::uint64_t> owned{};std::size_t index{};bool held{},move{},merge{};
        bool operator==(const Copy&)const=default;
    };
    std::map<std::uint64_t,Document> documents;std::map<std::uint64_t,View> views;std::map<std::uint64_t,Object> objects;
    std::map<std::uint64_t,Operation> operations;std::map<std::uint64_t,Copy> copies;
    std::array<std::uint64_t,3> tables{};std::uint64_t next_document{1},next_view{1},next_operation{1};
    bool operator==(const StorageState&)const=default;
    bool valid_view(std::uint64_t id)const{
        const auto found=views.find(id);if(found==views.end())return false;const auto* view=&found->second;
        std::set<std::uint64_t> seen;
        while(view->parent){if(!seen.insert(id).second)return false;const auto parent=views.find(view->parent);
            if(parent==views.end() || parent->second.generation!=view->parent_generation || !parent->second.tree.nodes.contains(view->root))return false;
            id=view->parent;view=&parent->second;}
        return documents.contains(view->document);
    }
    Object& object(std::uint64_t handle){const auto found=objects.find(handle);if(found==objects.end())throw storage::Error{storage::pointer_error};auto& value=found->second;
        if(value.invalidated || !valid_view(value.view) || value.generation!=views.at(value.view).generation || !views.at(value.view).tree.nodes.contains(value.node))throw storage::Error{storage::reverted};return value;}
};
}
