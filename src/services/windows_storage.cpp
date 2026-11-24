#include "ansi_codepage.hpp"
#include "windows_storage_state.hpp"
#include "property_set_dictionary.hpp"
#include "service_callback_task.hpp"
#include "x64arm/windows_paths.hpp"
#include <algorithm>

namespace x64arm {
namespace {
using PropertyGuid=std::array<std::uint8_t,16>;
struct PropertyName {PropertyGuid guid;std::u16string_view name;};
constexpr PropertyName property_names[]{
    {{0xe0,0x85,0x9f,0xf2,0xf9,0x4f,0x68,0x10,0xab,0x91,8,0,0x2b,0x27,0xb3,0xd9},u"SummaryInformation"},
    {{2,0xd5,0xcd,0xd5,0x9c,0x2e,0x1b,0x10,0x93,0x97,8,0,0x2b,0x2c,0xf9,0xae},u"DocumentSummaryInformation"},
    // 使用者自訂區段共用文件摘要串流，反查時取前一筆的 FMTID。
    {{5,0xd5,0xcd,0xd5,0x9c,0x2e,0x1b,0x10,0x93,0x97,8,0,0x2b,0x2c,0xf9,0xae},u"DocumentSummaryInformation"},
    {{0,0x6f,0x61,0x56,0x54,0xc1,0xce,0x11,0x85,0x53,0,0xaa,0,0xa1,0xf9,0x5b},u"Global Info"},
    {{0,0x64,0x61,0x56,0x54,0xc1,0xce,0x11,0x85,0x53,0,0xaa,0,0xa1,0xf9,0x5b},u"Image Contents"},
    {{0,0x65,0x61,0x56,0x54,0xc1,0xce,0x11,0x85,0x53,0,0xaa,0,0xa1,0xf9,0x5b},u"Image Info"}
};
constexpr std::u16string_view property_alphabet=u"abcdefghijklmnopqrstuvwxyz012345";
char16_t property_lower(char16_t c){return c>=u'A' && c<=u'Z'?static_cast<char16_t>(c+32):c;}
std::u16string property_set_name(const PropertyGuid& guid){
    std::u16string text(1,5);
    for(const auto& entry:property_names)if(entry.guid==guid)return text.append(entry.name);
    // 每五個位元產生一個字元；從位元組邊界開始的字母使用大寫。
    for(unsigned bit=0;bit<128;bit+=5){
        unsigned digit=0;
        for(unsigned k=0;k<5 && bit+k<128;++k)digit|=((guid[(bit+k)/8]>>((bit+k)%8))&1U)<<k;
        auto c=property_alphabet[digit];
        if(bit%8==0 && c>=u'a' && c<=u'z')c=static_cast<char16_t>(c-32);
        text.push_back(c);
    }
    return text;
}
}
std::span<const WindowsServiceDescriptor> storage_service_descriptors(){
    static constexpr WindowsServiceDescriptor descriptors[]{
        {"ole32.dll","StgCreateDocfileOnILockBytes",4},{"ole32.dll","StgOpenStorageOnILockBytes",6},{"ole32.dll","StgIsStorageILockBytes",1},
        {"ole32.dll","StgCreateDocfile",4,0,0,0x6},{"ole32.dll","StgOpenStorage",6,0,0,0x14},{"ole32.dll","StgIsStorageFile",1},
        {"ole32.dll","StgCreateStorageEx",8,0,0,0xe},{"ole32.dll","StgOpenStorageEx",8,0,0,0xe},{"ole32.dll","StgSetTimes",4},
        {"ntdll.dll","RtlGuidToPropertySetName",2},{"ntdll.dll","RtlPropertySetNameToGuid",3,0,0,1},
        {"ole32.dll","ReadClassStg",2},{"ole32.dll","WriteClassStg",2},{"ole32.dll","FmtIdToPropStgName",2},{"ole32.dll","PropStgNameToFmtId",2},
        {"ole32.dll","StgCreatePropSetStg",3,0,0,0x2},
        {"ole32.dll","StgCreatePropStg",6,0,0,0x18},
        {"ole32.dll","StgOpenPropStg",5,0,0,0xc},
        {"internal-prop-enum","QueryInterface",3},
        {"internal-prop-enum","AddRef",1},
        {"internal-prop-enum","Release",1},
        {"internal-prop-enum","Next",4,0,0,2},
        {"internal-prop-enum","Skip",2,0,0,2},
        {"internal-prop-enum","Reset",1},
        {"internal-prop-enum","Clone",2},
        {"internal-prop-set-enum","QueryInterface",3},
        {"internal-prop-set-enum","AddRef",1},
        {"internal-prop-set-enum","Release",1},
        {"internal-prop-set-enum","Next",4,0,0,2},
        {"internal-prop-set-enum","Skip",2,0,0,2},
        {"internal-prop-set-enum","Reset",1},
        {"internal-prop-set-enum","Clone",2},
        {"internal-prop-set","QueryInterface",3},
        {"internal-prop-set","AddRef",1},
        {"internal-prop-set","Release",1},
        {"internal-prop-set","Create",6,0,0,0x18},
        {"internal-prop-set","Open",4,0,0,0x4},
        {"internal-prop-set","Delete",2},
        {"internal-prop-set","Enum",2},
        {"internal-prop-storage","QueryInterface",3},
        {"internal-prop-storage","AddRef",1},
        {"internal-prop-storage","Release",1},
        {"internal-prop-storage","ReadMultiple",4,0,0,0x2},
        {"internal-prop-storage","WriteMultiple",5,0,0,0x12},
        {"internal-prop-storage","DeleteMultiple",3,0,0,0x2},
        {"internal-prop-storage","ReadPropertyNames",4,0,0,0x2},
        {"internal-prop-storage","WritePropertyNames",4,0,0,0x2},
        {"internal-prop-storage","DeletePropertyNames",3,0,0,0x2},
        {"internal-prop-storage","Commit",2,0,0,0x2},
        {"internal-prop-storage","Revert",1},
        {"internal-prop-storage","Enum",2},
        {"internal-prop-storage","SetTimes",4},
        {"internal-prop-storage","SetClass",2},
        {"internal-prop-storage","Stat",2},
        {"internal-storage","QueryInterface",3},{"internal-storage","AddRef",1},{"internal-storage","Release",1},
        {"internal-storage","CreateStream",6,0,0,0x1c},{"internal-storage","OpenStream",6,0,0,0x18},{"internal-storage","CreateStorage",6,0,0,0x1c},{"internal-storage","OpenStorage",7,0,0,0x28},
        {"internal-storage","CopyTo",5},{"internal-storage","MoveElementTo",5},{"internal-storage","Commit",2},{"internal-storage","Revert",1},
        {"internal-storage","EnumElements",5,0,0,0xa},{"internal-storage","DestroyElement",2},{"internal-storage","RenameElement",3},
        {"internal-storage","SetElementTimes",5},{"internal-storage","SetClass",2},{"internal-storage","SetStateBits",3},{"internal-storage","Stat",3},
        {"internal-storage-stream","QueryInterface",3},{"internal-storage-stream","AddRef",1},{"internal-storage-stream","Release",1},
        {"internal-storage-stream","Read",4},{"internal-storage-stream","Write",4},{"internal-storage-stream","Seek",4,2},{"internal-storage-stream","SetSize",2,2},
        {"internal-storage-stream","CopyTo",5,4},{"internal-storage-stream","Commit",2},{"internal-storage-stream","Revert",1},
        {"internal-storage-stream","LockRegion",4,6},{"internal-storage-stream","UnlockRegion",4,6},{"internal-storage-stream","Stat",3},{"internal-storage-stream","Clone",2},
        {"internal-storage-enum","QueryInterface",3},{"internal-storage-enum","AddRef",1},{"internal-storage-enum","Release",1},
        {"internal-storage-enum","Next",4},{"internal-storage-enum","Skip",2},{"internal-storage-enum","Reset",1},{"internal-storage-enum","Clone",2}
    };return descriptors;
}
StorageState& WindowsServices::storage_bridge(){if(!storage_)storage_=std::make_shared<StorageState>();else if(storage_.use_count()>1)storage_=std::make_shared<StorageState>(*storage_);return *storage_;}
bool WindowsServices::same_storage(const WindowsServices& other)const{return !storage_ || !other.storage_?storage_==other.storage_:*storage_==*other.storage_;}
std::uint64_t WindowsServices::create_storage_object(GuestMemory& memory,unsigned type,std::uint64_t view,std::uint32_t node,std::uint32_t mode){
    auto& bridge=storage_bridge();if(type>=bridge.tables.size() || bridge.objects.size()>=65536)throw storage::Error{storage::no_memory};
    const auto unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);auto& table=bridge.tables[type];
    if(!table){constexpr std::array families{"internal-storage","internal-storage-stream","internal-storage-enum"};std::vector<WindowsServiceDescriptor> methods;
        for(const auto& entry:storage_service_descriptors())if(entry.family==families[type])methods.push_back(entry);
        const auto address=runtime_allocate(memory,{},methods.size()*unit);for(std::size_t i=0;i<methods.size();++i)memory.store(GuestAddress{address+i*unit},bits,resolve(methods[i].family,methods[i].symbol).value);table=address;}
    const auto handle=allocate_heap(memory,unit);if(!handle)throw storage::Error{storage::no_memory};
    try{memory.store(GuestAddress{handle},bits,table);StorageState::Object object;object.type=static_cast<StorageState::Type>(type);object.view=view;object.generation=bridge.views.at(view).generation;object.node=node;object.mode=mode&~(storage::create|storage::convert);bridge.objects.emplace(handle,std::move(object));}
    catch(...){GuestState unused;(void)free_heap(memory,handle,unused);throw;}return handle;
}
std::vector<GuestCallback> WindowsServices::release_storage_object(GuestState& state,GuestMemory& memory,std::uint64_t handle){
    auto& bridge=storage_bridge();const auto found=bridge.objects.find(handle);if(found==bridge.objects.end())throw storage::Error{storage::pointer_error};
    auto& object=found->second;if(--object.references)return {};
    const auto document=bridge.views.at(object.view).document;auto& record=bridge.documents.at(document);std::vector<GuestCallback> callbacks;
    if(object.type==StorageState::Type::Storage){std::vector<std::uint64_t> parents{handle};for(std::size_t i=0;i<parents.size();++i)for(auto& [child,value]:bridge.objects)if(child!=handle && value.parent==parents[i] && bridge.views.at(value.view).document==document && !value.invalidated){value.invalidated=true;parents.push_back(child);}
        if(!object.parent && record.owns_backend){callbacks.push_back({storage::method(memory,record.backend,2),{record.backend}});record.owns_backend=false;}}
    bridge.objects.erase(found);(void)free_heap(memory,handle,state);
    if(std::any_of(bridge.objects.begin(),bridge.objects.end(),[&](const auto& entry){return bridge.views.at(entry.second.view).document==document;}))return callbacks;
    if(record.owns_backend)callbacks.push_back({storage::method(memory,record.backend,2),{record.backend}});bridge.documents.erase(document);
    std::erase_if(bridge.views,[&](const auto& entry){return entry.second.document==document;});return callbacks;
}
void WindowsServices::storage_stat(GuestState& state,GuestMemory& memory,std::uint64_t address,std::uint32_t flags,const compound::Node& node,std::uint32_t mode,bool root){
    if(!address)throw storage::Error{storage::pointer_error};if(flags>2)throw storage::Error{storage::invalid_flag};
    const auto unit=x86_?4U:8U,extent=x86_?72U:80U,base=x86_?8U:16U;const auto bits=static_cast<std::uint8_t>(unit*8);
    memory.check(GuestAddress{address},extent,Access::Write);std::uint64_t name=0;
    if(!root && !(flags&1)){const auto bytes=utf16_bytes(node.name);name=allocate_heap(memory,bytes.size());if(!name)throw storage::Error{storage::no_memory};try{memory.write(GuestAddress{name},bytes);}catch(...){(void)free_heap(memory,name,state);throw;}}
    try{memory.write(GuestAddress{address},std::vector<std::uint8_t>(extent));memory.store(GuestAddress{address},bits,name);
        memory.store(GuestAddress{address+unit},32,node.kind==compound::Kind::Stream?2:1);memory.store(GuestAddress{address+base},64,node.data.size());
        if(node.kind!=compound::Kind::Stream){memory.store(GuestAddress{address+base+8},64,node.modified);memory.store(GuestAddress{address+base+16},64,root?0:node.created);memory.write(GuestAddress{address+base+40},node.clsid);}
        memory.store(GuestAddress{address+base+32},32,mode);memory.store(GuestAddress{address+base+56},32,node.state_bits);
    }catch(...){if(name)(void)free_heap(memory,name,state);throw;}
}
std::optional<std::uint32_t> WindowsServices::invoke_storage(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
#include "windows_property_storage.inc"
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const auto bits=static_cast<std::uint8_t>(x86_?32:64);
    const auto done=[&](std::uint64_t result){arg.return_to(state,result);return std::optional<std::uint32_t>{};};
    const auto d=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    try{
        const bool rtl=service.family=="ntdll.dll";
        const bool encode=name=="FmtIdToPropStgName" || name=="RtlGuidToPropertySetName";
        if(encode || name=="PropStgNameToFmtId" || name=="RtlPropertySetNameToGuid"){
            constexpr std::uint32_t invalid_argument=0x80070057U,invalid_nt_parameter=0xc000000dU;
            if(encode){
                if(!arg(0) || !arg(1))return done(rtl?0U:invalid_argument);
                PropertyGuid guid{};
                memory.read(GuestAddress{arg(0)},guid);
                const auto text=property_set_name(guid);
                memory.write(GuestAddress{arg(1)},utf16_bytes(text));
                return done(rtl?text.size():0);
            }
            const auto input=arg(rtl?1:0),output=arg(rtl?2:1);
            if(!output)return done(rtl?invalid_nt_parameter:invalid_argument);
            const auto invalid_name=rtl?invalid_nt_parameter:storage::invalid_name;
            if(!input || (rtl && (!d(0) || d(0)>27)))return done(invalid_name);
            std::u16string text;
            if(rtl)text=read_utf16_counted(memory,GuestAddress{input},d(0));
            else{
                try{text=read_utf16(memory,GuestAddress{input},27);}
                catch(const std::invalid_argument&){return done(invalid_name);}
            }
            if(text.empty() || text.front()!=5)return done(invalid_name);
            const std::u16string_view suffix=std::u16string_view{text}.substr(1);
            for(const auto& entry:property_names){
                if(suffix.size()==entry.name.size() && std::equal(suffix.begin(),suffix.end(),entry.name.begin(),
                    [](char16_t a,char16_t b){return property_lower(a)==property_lower(b);})){
                    memory.write(GuestAddress{output},entry.guid);
                    return done(0);
                }
            }
            if(suffix.size()!=26)return done(invalid_name);
            PropertyGuid guid{};
            for(unsigned i=0;i<26;++i){
                const auto digit=property_alphabet.find(property_lower(suffix[i]));
                if(digit==property_alphabet.npos || (i==25 && digit>7)){
                    memory.write(GuestAddress{output},guid);
                    return done(invalid_name);
                }
                for(unsigned k=0;k<5 && i*5+k<128;++k){
                    const auto bit=i*5+k;
                    if(digit&(1U<<k))guid[bit/8]|=static_cast<std::uint8_t>(1U<<(bit%8));
                }
            }
            memory.write(GuestAddress{output},guid);
            return done(0);
        }
        auto& bridge=storage_bridge();
        if(name=="WriteClassStg"){
            if(!arg(0))return done(0x80070057U);
            if(!arg(1))return done(storage::pointer_error);
            state.rip=GuestAddress{storage::method(memory,arg(0),15)}; // IStorage::SetClass 共用原呼叫框架。
            return std::nullopt;
        }
        if(name=="ReadClassStg"){
            // IStorage::Stat(STATFLAG_NONAME) 的 clsid（x86 位移 48、x64 位移 56）。
            if(!arg(0) || !arg(1))return done(0x80070057U);
            return nested_continuation(state,memory,"ReadClassStg",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall call(s,m,service);GuestScratch scratch(*this,m,s);const auto stat=scratch.allocate(96);if(!stat){call.return_to(s,std::uint64_t{storage::no_memory});co_return std::nullopt;}
                const auto status=static_cast<std::uint32_t>(co_await service_callback::call(storage::method(m,call(0),17),{call(0),stat,1}));
                if(!(status&0x80000000U)){std::array<std::uint8_t,16> clsid{};m.read(GuestAddress{stat+(x86_?48U:56U)},clsid);m.write(GuestAddress{call(1)},clsid);}
                call.return_to(s,std::uint64_t{status});co_return std::nullopt;});
        }
        if(name=="StgSetTimes"){
            // 以 FILE_WRITE_ATTRIBUTES 開啟後設定建立／存取／修改時間；NULL 表示不變。
            if(!arg(0))return done(storage::invalid_parameter);
            const auto handle=nested_service(state,memory,"kernel32.dll","CreateFileW",{arg(0),0x100,7,0,3,0x02000000,0});const auto invalid=x86_?UINT32_MAX:UINT64_MAX;
            if((x86_?handle&UINT32_MAX:handle)==invalid)return done(0x80030000U|(last_error_&0xffffU));
            const auto set=nested_service(state,memory,"kernel32.dll","SetFileTime",{handle,arg(1),arg(2),arg(3)})&UINT32_MAX;const auto error=last_error_;
            (void)nested_service(state,memory,"kernel32.dll","CloseHandle",{handle});return done(set?0U:0x80030000U|(error&0xffffU));
        }
        if(name=="StgCreateDocfile" || name=="StgOpenStorage" || name=="StgIsStorageFile" || name=="StgCreateStorageEx" || name=="StgOpenStorageEx"){
            // 檔案式複合文件：以同一套檔案開啟流程建立檔案 ILockBytes，交給與 *OnILockBytes 相同的作業；作業結束後釋放建立時的參照。
            // StgCreateDocfile 的名稱為 NULL 時在暫存目錄建立檔案並加上 STGM_DELETEONRELEASE。*Ex 支援 STGFMT_STORAGE（0）、ANY（4，僅開啟）、DOCFILE（5），介面須為 IStorage。
            const bool ex=name.ends_with("Ex"),checking=name=="StgIsStorageFile",creating=name.starts_with("StgCreate");
            const auto output=checking?0:arg(ex?7:creating?3:5);if(!checking && !output)return done(storage::pointer_error);if(output)memory.store(GuestAddress{output},bits,0);
            auto mode=checking?0x40U:d(1+(!creating && !ex?1:0)); // StgIsStorageFile：唯讀、共用
            if(ex){const auto format=d(2);if(format!=0 && format!=5 && !(format==4 && !creating))return done(storage::invalid_parameter);
                if(d(3) || (!creating && arg(5)))return done(storage::invalid_parameter);
                if(arg(4)){const auto sector=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(4)+4},32));if(sector && sector!=512)return done(storage::invalid_parameter);}
                std::array<std::uint8_t,16> iid{};if(!arg(6))return done(storage::invalid_parameter);memory.read(GuestAddress{arg(6)},iid);
                constexpr std::array<std::uint8_t,16> unknown{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};auto storage_iid=unknown;storage_iid[0]=11;if(iid!=unknown && iid!=storage_iid)return done(0x80004002U);}
            if(!checking && !creating && !ex && (arg(1) || arg(3)))return done(storage::invalid_parameter);
            const auto storage_mode=mode&~0x04000000U; // STGM_DELETEONRELEASE 只影響檔案生命週期
            if(!checking)storage::mode(storage_mode,true,false,creating);
            std::u16string path;auto file_mode=mode&0x04001073U;
            if(arg(0))path=read_utf16(memory,GuestAddress{arg(0)},32767);
            else if(creating){GuestScratch temporary(*this,memory,state);const auto folder=temporary.allocate(2*261),file=temporary.allocate(2*261),prefix=temporary.text(u"STG");if(!folder || !file || !prefix)return done(storage::no_memory);
                if(!(nested_service(state,memory,"kernel32.dll","GetTempPathW",{261,folder})&UINT32_MAX) || !(nested_service(state,memory,"kernel32.dll","GetTempFileNameW",{folder,prefix,0,file})&UINT32_MAX))return done(0x80030000U|(last_error_&0xffffU));
                path=read_utf16(memory,GuestAddress{file},260);file_mode|=0x04000000U|0x1000U;} // DELETEONRELEASE｜CREATE（GetTempFileName 已建立空檔）
            else return done(storage::invalid_name);
            path=normalized_windows_path(std::move(path),runtime_.current_directory);
            std::uint64_t backend=0;const auto status=open_file_object(memory,path,file_mode,0,creating,true,backend);
            if(status){const auto code=status&0xffffU;if((status&0xffff0000U)==0x80070000U && (code==2 || code==3 || code==5 || code==32 || code==33 || code==80))return done(0x80030000U|code);return done(status);}
            if(bridge.operations.size()>=4096)return done(storage::no_memory);
            StorageState::Operation operation;operation.kind=checking?StorageState::Kind::IsStorage:creating?StorageState::Kind::Create:StorageState::Kind::Open;
            operation.backend=backend;operation.output=output;operation.mode=checking?0:(creating?storage_mode|storage::create:storage_mode);operation.release_backend=true;
            const auto token=bridge.next_operation++;bridge.operations.emplace(token,std::move(operation));return resume_storage(state,memory,token,0);
        }
        if(service.family=="ole32.dll"){
            const bool creating=name=="StgCreateDocfileOnILockBytes",checking=name=="StgIsStorageILockBytes";const auto output=checking?0:arg(creating?3:5);
            if(!checking && !output)return done(storage::pointer_error);if(output)memory.store(GuestAddress{output},bits,0);
            if(!arg(0))return done(storage::pointer_error);if(!checking){if(arg(creating?2:4) || (!creating && (arg(1) || arg(3))))return done(storage::invalid_parameter);storage::mode(d(creating?1:2),true,false,creating);}
            if(creating && !(d(1)&(storage::create|storage::convert)))return done(storage::exists);
            if(bridge.operations.size()>=4096)return done(storage::no_memory);
            StorageState::Operation operation;operation.kind=checking?StorageState::Kind::IsStorage:creating?StorageState::Kind::Create:StorageState::Kind::Open;
            operation.backend=arg(0);operation.output=output;operation.mode=checking?0:d(creating?1:2);
            const auto token=bridge.next_operation++;bridge.operations.emplace(token,std::move(operation));return resume_storage(state,memory,token,0);
        }
        const auto raw=bridge.objects.find(arg(0));if(raw==bridge.objects.end())return done(storage::pointer_error);
        constexpr std::array families{"internal-storage","internal-storage-stream","internal-storage-enum"};
        if(service.family!=families[static_cast<unsigned>(raw->second.type)])return done(storage::pointer_error);
        if(name=="Release"){
            const auto references=raw->second.references-1;auto callbacks=release_storage_object(state,memory,arg(0));
            if(!callbacks.empty()){start_callbacks(state,memory,std::move(callbacks),Continuation::Return,references);return std::nullopt;}return done(references);
        }
        auto& object=bridge.object(arg(0));
        if(name=="AddRef")return done(++object.references);
        if(name=="QueryInterface"){
            if(!arg(2))return done(0x80004003U);memory.store(GuestAddress{arg(2)},bits,0);if(!arg(1))return done(0x80004003U);std::array<std::uint8_t,16> iid{};memory.read(GuestAddress{arg(1)},iid);
            constexpr std::array<std::uint8_t,16> unknown{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
            auto expected=unknown;expected[0]=object.type==StorageState::Type::Storage?11:object.type==StorageState::Type::Stream?12:13;
            if(iid!=unknown && iid!=expected)return done(0x80004002U);
            ++object.references;memory.store(GuestAddress{arg(2)},bits,arg(0));return done(0);
        }
        if(object.type==StorageState::Type::Stream)return invoke_storage_stream(state,memory,service);
        if(object.type==StorageState::Type::Enumerator)return invoke_storage_enumerator(state,memory,service);
        auto& view=bridge.views.at(object.view);auto& document=bridge.documents.at(view.document);
        if(name=="Stat"){storage_stat(state,memory,arg(1),d(2),view.tree.nodes.at(object.node),object.mode,object.node==0);return done(0);}
        if(name=="CopyTo" || name=="MoveElementTo")return begin_storage_copy(state,memory,service);
        if(name=="EnumElements"){
            if(!arg(4))return done(storage::pointer_error);memory.store(GuestAddress{arg(4)},bits,0);if(arg(1) || arg(2) || arg(3))return done(storage::invalid_parameter);
            const auto handle=create_storage_object(memory,2,object.view,object.node,0);bridge.objects.at(handle).parent=arg(0);auto& enumeration=bridge.objects.at(handle).enumeration;
            try{for(auto child:view.tree.children(object.node))enumeration.push_back(view.tree.nodes.at(child));memory.store(GuestAddress{arg(4)},bits,handle);}
            catch(...){(void)release_storage_object(state,memory,handle);throw;}return done(0);
        }
        if(document.busy)return done(storage::lock_violation);
        if(name=="Revert"){
            if(!(object.mode&storage::transacted))return done(0);
            auto restored=view.committed;restored.next_node=document.next_node;view.tree=std::move(restored);++view.generation;object.generation=view.generation;return done(0);
        }
        if(name=="Commit"){
            if(d(1)&~15U)return done(storage::invalid_flag);if((d(1)&8) && (object.node || !(object.mode&storage::transacted)))return done(storage::invalid_flag);
            if(!storage::writable(object.mode))return done(storage::access_denied);
            if(object.node!=view.root || !(object.mode&storage::transacted)){if(object.node || view.parent)return done(0);return persist_storage(state,memory,object.view,arg(0),{},0,0,d(1));}
            if(!view.parent)return persist_storage(state,memory,object.view,arg(0),{},0,0,d(1),{object.view});
            auto& parent=bridge.views.at(view.parent);const auto before=parent.tree;auto merged=parent.tree;
            const auto parent_id=merged.nodes.at(view.root).parent;std::vector<std::uint32_t> descendants{view.root};
            for(std::size_t i=0;i<descendants.size();++i){const auto children=view.tree.children(descendants[i]);descendants.insert(descendants.end(),children.begin(),children.end());}
            merged.erase(view.root);for(auto id:descendants)merged.nodes.emplace(id,view.tree.nodes.at(id));merged.nodes.at(view.root).parent=parent_id;merged.next_node=document.next_node;merged.validate();parent.tree=std::move(merged);
            return persist_storage(state,memory,view.parent,arg(0),{{view.parent,before}},0,0,d(1),{object.view});
        }
        if(!storage::writable(object.mode) && name!="OpenStream" && name!="OpenStorage")return done(storage::access_denied);
        const auto before=view.tree;
        const auto changed=[&](std::uint64_t output=0,std::uint64_t child=0,std::vector<std::uint64_t> views=std::vector<std::uint64_t>{}){return persist_storage(state,memory,object.view,arg(0),{{object.view,before}},output,child,0,{},std::move(views));};
        const auto element_name=[&](std::uint64_t address){return storage::name(memory,address);};
        if(name=="CreateStream" || name=="OpenStream" || name=="CreateStorage" || name=="OpenStorage"){
            const bool stream=name=="CreateStream" || name=="OpenStream",creating=name=="CreateStream" || name=="CreateStorage";
            const auto output=arg(name=="OpenStorage"?6:5);if(!output)return done(storage::pointer_error);memory.store(GuestAddress{output},bits,0);
            const auto text=element_name(arg(1));const auto mode=d(name=="OpenStream"?3:name=="OpenStorage"?3:2);
            if((name=="OpenStorage" && (arg(2) || arg(4) || arg(5))) || (name=="OpenStream" && (arg(2) || arg(4))) || (creating && (arg(3) || arg(4))))return done(storage::invalid_parameter);
            storage::mode(mode,false,stream,creating);if(!storage::child_access(object.mode,mode))return done(storage::access_denied);
            auto existing=view.tree.find(object.node,text);const auto type=stream?compound::Kind::Stream:compound::Kind::Storage;
            if(existing && !creating && view.tree.nodes.at(*existing).kind!=type)return done(storage::missing);
            if(existing && std::any_of(bridge.objects.begin(),bridge.objects.end(),[&](const auto& pair){const auto& value=pair.second;return !value.invalidated && value.type!=StorageState::Type::Enumerator && value.node==*existing && bridge.views.at(value.view).document==view.document && bridge.valid_view(value.view) && value.generation==bridge.views.at(value.view).generation;}))return done(storage::access_denied);
            if(creating && existing && !(mode&storage::create))return done(storage::exists);if(!creating && !existing)return done(storage::missing);
            auto tree=view.tree;
            if(creating){if(existing)tree.erase(*existing);tree.next_node=document.next_node;existing=tree.add(object.node,text,type);document.next_node=tree.next_node;}
            auto target_view=object.view;std::vector<std::uint64_t> created_views;
            if(!stream && (mode&storage::transacted)){target_view=bridge.next_view++;StorageState::View child;child.tree=tree;child.committed=tree;child.document=view.document;child.parent=object.view;child.parent_generation=view.generation;child.root=*existing;child.mode=mode&~storage::create;bridge.views.emplace(target_view,std::move(child));created_views.push_back(target_view);}
            std::uint64_t handle=0;
            try{handle=create_storage_object(memory,stream?1:0,target_view,*existing,mode);bridge.objects.at(handle).parent=arg(0);if(creating){view.tree=std::move(tree);return changed(output,handle,std::move(created_views));}memory.store(GuestAddress{output},bits,handle);return done(0);}
            catch(...){if(handle)(void)release_storage_object(state,memory,handle);for(auto id:created_views)bridge.views.erase(id);view.tree=before;throw;}
        }
        auto tree=view.tree;
        if(name=="SetClass"){if(!arg(1))return done(storage::pointer_error);memory.read(GuestAddress{arg(1)},tree.nodes.at(object.node).clsid);}
        else if(name=="SetStateBits"){auto& value=tree.nodes.at(object.node).state_bits;value=(value&~d(2))|(d(1)&d(2));}
        else if(name=="DestroyElement" || name=="RenameElement"){
            const auto child=tree.find(object.node,element_name(arg(1)));if(!child)return done(storage::missing);
            if(name=="DestroyElement")tree.erase(*child);else tree.rename(*child,element_name(arg(2)));
        }else if(name=="SetElementTimes"){
            const auto child=arg(1)?tree.find(object.node,element_name(arg(1))):std::optional<std::uint32_t>{object.node};if(!child)return done(storage::missing);
            std::array<std::uint64_t,3> times{};for(unsigned i=0;i<3;++i)if(arg(2+i))times[i]=memory.load(GuestAddress{arg(2+i)},64);
            auto& node=tree.nodes.at(*child);if(node.kind!=compound::Kind::Stream){if(arg(2) && *child)node.created=times[0];if(arg(4))node.modified=times[2];}
        }else throw HostCallFault("未分類的 IStorage 方法");
        view.tree=std::move(tree);return changed();
    }catch(const storage::Error& error){return done(error.status);}catch(const compound::Error& error){return done(storage::error(error));}
    catch(const MemoryFault&){return done(storage::pointer_error);}catch(const std::bad_alloc&){return done(storage::no_memory);}
}
}
