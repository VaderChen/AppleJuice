#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/security_descriptor.hpp"
#include "x64arm/windows_security_attributes.hpp"
#include <algorithm>
#include <chrono>

namespace x64arm {
// KTM 交易與 TxF 檔案作業：交易內的變更延到 CommitTransaction 才套用到 Windows 磁碟，
// 交易外的讀取看到的仍是原狀；提交以可復原的改名日誌執行，任一步失敗即全部還原。
struct KtmOperation {
    enum class Kind {Delete,Move,CreateDirectory,RemoveDirectory};
    Kind kind{};std::string from,to;std::uint32_t flags{};std::vector<std::uint8_t> security;std::string source;
};
struct KtmNameView {bool exists{},directory{};};
struct KtmTransaction {
    std::array<std::uint8_t,16> id{};std::u16string description;
    std::uint32_t options{},isolation_level{},isolation_flags{},timeout{},outcome{1};
    std::chrono::steady_clock::time_point created{std::chrono::steady_clock::now()};
    std::vector<KtmOperation> operations;std::map<std::string,KtmNameView> names;std::uint64_t sequence{};
};
struct TransactionState {std::map<std::uint64_t,std::shared_ptr<KtmTransaction>> handles;};
namespace {
constexpr std::uint32_t not_active=6701,already_aborted=6704,already_committed=6705;
constexpr std::uint32_t undetermined=1,committed=2,aborted=3;
std::string fold_leaf(std::string_view leaf){
    std::string result(leaf);for(auto& c:result)if(c>='A' && c<='Z')c=static_cast<char>(c+32);return result;
}
std::pair<std::string,std::string> split_path(const std::string& path){
    const auto at=path.find_last_of("\\/");
    if(at==std::string::npos)return {std::string{},path};
    if(at==0 || (at==2 && path.size()>1 && path[1]==':'))return {path.substr(0,at+1),path.substr(at+1)};
    return {path.substr(0,at),path.substr(at+1)};
}
}
std::span<const WindowsServiceDescriptor> transaction_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"ktmw32.dll","CreateTransaction",7},{"ktmw32.dll","CommitTransaction",1},{"ktmw32.dll","CommitTransactionAsync",1},
        {"ktmw32.dll","RollbackTransaction",1},{"ktmw32.dll","RollbackTransactionAsync",1},{"ktmw32.dll","GetTransactionId",2},
        {"ktmw32.dll","GetTransactionInformation",7},{"ktmw32.dll","SetTransactionInformation",5},
        {"kernel32.dll","DeleteFileTransactedA",2},{"kernel32.dll","DeleteFileTransactedW",2},
        {"kernel32.dll","MoveFileTransactedA",6},{"kernel32.dll","MoveFileTransactedW",6},
        {"kernel32.dll","CreateDirectoryTransactedA",4},{"kernel32.dll","CreateDirectoryTransactedW",4},
        {"kernel32.dll","RemoveDirectoryTransactedA",2},{"kernel32.dll","RemoveDirectoryTransactedW",2}
    };return table;
}
TransactionState& WindowsServices::transaction_state(){if(!transactions_)transactions_=std::make_shared<TransactionState>();return *transactions_;}
bool WindowsServices::transactions_open()const{return transactions_ && !transactions_->handles.empty();}
bool WindowsServices::transaction_handle(std::uint64_t handle)const{return transactions_ && transactions_->handles.contains(handle);}
std::optional<std::pair<std::uint64_t,std::uint32_t>> WindowsServices::transaction_identity(std::uint64_t handle)const{
    if(!transaction_handle(handle))return std::nullopt;const auto& target=transactions_->handles.at(handle);
    const auto count=static_cast<std::uint32_t>(std::count_if(transactions_->handles.begin(),transactions_->handles.end(),[&](const auto& entry){return entry.second==target;}));
    return std::pair{static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(target.get())),count};
}
bool WindowsServices::close_transaction(std::uint64_t handle){
    if(!transactions_)return false;const auto found=transactions_->handles.find(handle);if(found==transactions_->handles.end())return false;
    auto transaction=found->second;transactions_->handles.erase(found);
    // 最後一個 handle 關閉而未提交時，KTM 回滾交易；尚未套用的作業直接丟棄。
    const bool last=std::none_of(transactions_->handles.begin(),transactions_->handles.end(),[&](const auto& entry){return entry.second==transaction;});
    if(last && transaction->outcome==undetermined){transaction->outcome=aborted;transaction->operations.clear();transaction->names.clear();}
    return true;
}
std::uint64_t WindowsServices::duplicate_transaction(std::uint64_t handle){
    if(!transaction_handle(handle))return 0;const auto copy=runtime_.allocate_wait_handle();
    transactions_->handles.emplace(copy,transactions_->handles.at(handle));return copy;
}
std::optional<std::uint32_t> WindowsServices::invoke_transactions(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;
    const auto u32=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    const auto invalid_handle=x86_?UINT64_C(0xffffffff):UINT64_MAX;
    std::uint64_t result=0;
    const auto fail=[&](std::uint32_t error){last_error_=error;};
    const auto finish=[&]{arg.return_to(state,result);return std::optional<std::uint32_t>{};};
    auto& ktm=transaction_state();
    const auto active=[&](std::uint64_t handle,std::shared_ptr<KtmTransaction>& out)->std::uint32_t{
        const auto found=ktm.handles.find(handle);if(found==ktm.handles.end())return 6;out=found->second;
        if(out->outcome==undetermined && out->timeout &&
           std::chrono::steady_clock::now()-out->created>std::chrono::milliseconds(out->timeout)){
            out->outcome=aborted;out->operations.clear();out->names.clear(); // 逾時即由 KTM 回滾。
        }
        return out->outcome==undetermined?0U:out->outcome==committed?already_committed:already_aborted;
    };
    try{
        if(symbol=="CreateTransaction"){
            // UOW 保留給 DTC，必須為 NULL；isolation 參數保留且必須為 0。
            result=invalid_handle;
            if(arg(1) || (u32(2)&~1U) || u32(3) || u32(4))fail(87);
            else {
                const auto sa=synchronization_security(memory,arg(0),x86_);
                if(sa.error)fail(sa.error);
                else if(ktm.handles.size()>=4096)fail(8);
                else {
                    auto transaction=std::make_shared<KtmTransaction>();
                    if(!random_bytes(transaction->id)){fail(8);return finish();}
                    transaction->id[7]=static_cast<std::uint8_t>((transaction->id[7]&15U)|64U);transaction->id[8]=static_cast<std::uint8_t>((transaction->id[8]&63U)|128U);
                    transaction->options=u32(2);transaction->timeout=u32(5);
                    if(arg(6))transaction->description=read_utf16(memory,GuestAddress{arg(6)},64);
                    result=runtime_.allocate_wait_handle();ktm.handles.emplace(result,std::move(transaction));
                    runtime_.handle_flags[result]=sa.inherit?1U:0U;last_error_=0;
                }
            }
            return finish();
        }
        std::shared_ptr<KtmTransaction> transaction;
        if(symbol=="GetTransactionId"){
            const auto error=active(arg(0),transaction);
            if(error==6)fail(6);else if(!arg(1))fail(87);else {memory.write(GuestAddress{arg(1)},transaction->id);result=1;}
            return finish();
        }
        if(symbol=="GetTransactionInformation"){
            const auto error=active(arg(0),transaction);
            if(error==6){fail(6);return finish();}
            for(unsigned i=1;i<=4;++i)if(arg(i))memory.check(GuestAddress{arg(i)},4,Access::Write);
            if(arg(1))memory.store(GuestAddress{arg(1)},32,transaction->outcome);
            if(arg(2))memory.store(GuestAddress{arg(2)},32,transaction->isolation_level);
            if(arg(3))memory.store(GuestAddress{arg(3)},32,transaction->isolation_flags);
            if(arg(4))memory.store(GuestAddress{arg(4)},32,transaction->timeout);
            if(arg(6)){
                const auto bytes=utf16_bytes(transaction->description);
                if(u32(5)<bytes.size()){fail(122);return finish();}
                memory.write(GuestAddress{arg(6)},bytes);
            }
            result=1;return finish();
        }
        if(symbol=="SetTransactionInformation"){
            const auto error=active(arg(0),transaction);
            if(error)fail(error);else if(u32(1) || u32(2))fail(87);
            else {transaction->timeout=u32(3);transaction->created=std::chrono::steady_clock::now();
                if(arg(4))transaction->description=read_utf16(memory,GuestAddress{arg(4)},64);result=1;}
            return finish();
        }
        if(symbol=="RollbackTransaction" || symbol=="RollbackTransactionAsync"){
            const auto error=active(arg(0),transaction);
            if(error==6 || error==already_committed)fail(error==6?6:not_active);
            else {transaction->outcome=aborted;transaction->operations.clear();transaction->names.clear();result=1;}
            return finish();
        }
        if(symbol=="CommitTransaction" || symbol=="CommitTransactionAsync"){
            const auto error=active(arg(0),transaction);
            if(error){fail(error);return finish();}
            if(filesystem_.writable() && !runtime_.network.external_io)throw HostCallFault("比較模式不允許宿主檔案交易提交");
            const auto commit_error=commit_transaction(*transaction);
            transaction->operations.clear();transaction->names.clear();
            if(commit_error){transaction->outcome=aborted;fail(commit_error);}else {transaction->outcome=committed;result=1;}
            return finish();
        }
        // 以下為 TxF 檔案作業：只驗證並排入交易，提交前不改動磁碟。
        const bool wide=symbol.back()=='W';
        const auto path=[&](unsigned i){return wide?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(i)})):read_ansi(memory,arg(i),32767);};
        const bool moving=symbol.starts_with("MoveFileTransacted"),creating=symbol.starts_with("CreateDirectoryTransacted");
        const auto transaction_argument=moving?5U:creating?3U:1U;
        const auto error=active(arg(transaction_argument),transaction);
        if(error){fail(error==6?6:error==already_committed?not_active:error);return finish();}
        const auto name_key=[&](const std::string& value,std::uint32_t& failure)->std::string{
            auto [parent,leaf]=split_path(value);
            if(leaf.empty() || leaf=="." || leaf==".."){failure=parent.empty()?3:123;return {};}
            const auto directory=filesystem_.information(parent.empty()?std::string{"."}:parent);
            if(directory.error){failure=directory.error==2?3:directory.error;return {};}
            if(!(directory.metadata.attributes&0x10)){failure=267;return {};}
            return directory.key+'\x01'+fold_leaf(leaf);
        };
        const auto view=[&](const std::string& value,const std::string& key)->KtmNameView{
            if(const auto found=transaction->names.find(key);found!=transaction->names.end())return found->second;
            const auto info=filesystem_.information(value);
            return {info.error==0,info.error==0 && (info.metadata.attributes&0x10)!=0};
        };
        const auto shared_open=[&](const std::string& value){
            const auto info=filesystem_.information(value);
            return !info.error && std::any_of(files_.begin(),files_.end(),[&](const auto& entry){return !entry.second.native && entry.second.key==info.key && !(entry.second.sharing&4U);});
        };
        std::uint32_t failure=0;KtmOperation operation;
        if(symbol.starts_with("DeleteFileTransacted") || symbol.starts_with("RemoveDirectoryTransacted")){
            const bool directory=symbol.starts_with("RemoveDirectory");
            if(!arg(0)){fail(87);return finish();}
            operation.kind=directory?KtmOperation::Kind::RemoveDirectory:KtmOperation::Kind::Delete;operation.from=path(0);
            const auto key=name_key(operation.from,failure);if(failure){fail(failure);return finish();}
            const auto current=view(operation.from,key);
            if(!current.exists)failure=directory?2:2;
            else if(current.directory!=directory)failure=directory?267:5;
            else if(shared_open(operation.from))failure=32;
            if(failure){fail(failure);return finish();}
            transaction->names[key]={};
        }else if(moving){
            const auto flags=u32(4);
            // 交易內不支援重開機延後移動，也不提供跨磁碟複製進度回呼。
            if(!arg(0) || !arg(1) || (flags&~11U) || (flags&4U)){fail(87);return finish();}
            if(arg(2) && (flags&2U)){fail(50);return finish();}
            operation.kind=KtmOperation::Kind::Move;operation.from=path(0);operation.to=path(1);operation.flags=flags;
            const auto from_key=name_key(operation.from,failure);if(failure){fail(failure);return finish();}
            const auto to_key=name_key(operation.to,failure);if(failure){fail(failure);return finish();}
            const auto source=view(operation.from,from_key),target=view(operation.to,to_key);
            if(!source.exists)failure=2;
            else if(from_key==to_key)failure=0;
            else if(target.exists && (!(flags&1U) || target.directory || source.directory))failure=183;
            else if(shared_open(operation.from) || (target.exists && shared_open(operation.to)))failure=32;
            if(failure){fail(failure);return finish();}
            if(from_key!=to_key){transaction->names[from_key]={};transaction->names[to_key]={true,source.directory};}
        }else if(creating){
            if(!arg(1)){fail(87);return finish();}
            operation.kind=KtmOperation::Kind::CreateDirectory;operation.to=path(1);
            if(arg(0))operation.source=path(0);
            const auto key=name_key(operation.to,failure);if(failure){fail(failure);return finish();}
            if(view(operation.to,key).exists){fail(183);return finish();}
            if(!operation.source.empty()){const auto source=filesystem_.information(operation.source);
                if(source.error){fail(source.error);return finish();}if(!(source.metadata.attributes&0x10)){fail(267);return finish();}}
            try{(void)read_security_attributes(memory,arg(2),x86_,false);operation.security=file_creation_descriptor(memory,arg(2));}
            catch(const SecurityDescriptorError& error){fail(error.code);return finish();}
            transaction->names[key]={true,true};
        }else throw HostCallFault("未知的交易式檔案入口");
        if(transaction->operations.size()>=65536){fail(8);return finish();}
        transaction->operations.push_back(std::move(operation));result=1;last_error_=0;
    }catch(const MemoryFault&){fail(998);result=symbol=="CreateTransaction"?invalid_handle:0;}
    catch(const std::bad_alloc&){fail(8);result=symbol=="CreateTransaction"?invalid_handle:0;}
    return finish();
}
std::uint32_t WindowsServices::commit_transaction(KtmTransaction& transaction){
    // 每一步都可逆：刪除與被取代的目標先改名到同目錄的暫存名，全部成功後才實際刪除。
    struct Undo {enum class Kind {Rename,RemoveDirectory} kind;std::string from,to;};
    std::vector<Undo> undo;std::vector<std::pair<std::string,bool>> staged;
    const auto staging=[&](const std::string& path){
        auto [parent,leaf]=split_path(path);const auto separator=parent.empty() || parent.back()=='\\' || parent.back()=='/'?std::string{}:std::string{"\\"};
        return parent+separator+leaf+".~ktm-"+std::to_string(process_id())+"-"+std::to_string(++transaction.sequence);
    };
    const auto shared_open=[&](const std::string& value){
        const auto info=filesystem_.information(value);
        return !info.error && std::any_of(files_.begin(),files_.end(),[&](const auto& entry){return !entry.second.native && entry.second.key==info.key && !(entry.second.sharing&4U);});
    };
    std::uint32_t error=0;
    for(const auto& operation:transaction.operations){
        using Kind=KtmOperation::Kind;
        if(operation.kind==Kind::Delete || operation.kind==Kind::RemoveDirectory){
            const bool directory=operation.kind==Kind::RemoveDirectory;
            const auto info=filesystem_.information(operation.from);
            if(info.error){error=info.error;break;}
            if(((info.metadata.attributes&0x10)!=0)!=directory){error=directory?267:5;break;}
            if(directory){const auto entries=filesystem_.list(operation.from);
                if(entries.error){error=entries.error;break;}
                if(std::any_of(entries.entries.begin(),entries.entries.end(),[](const auto& entry){return entry.name!="." && entry.name!="..";})){error=145;break;}}
            if(shared_open(operation.from)){error=32;break;}
            const auto hidden=staging(operation.from);
            error=filesystem_.rename(operation.from,hidden,false);if(error)break;
            undo.push_back({Undo::Kind::Rename,hidden,operation.from});staged.emplace_back(hidden,directory);
        }else if(operation.kind==Kind::Move){
            if(shared_open(operation.from)){error=32;break;}
            const auto target=filesystem_.information(operation.to);
            std::string replaced;
            if(!target.error && filesystem_.information(operation.from).key!=target.key){
                if(!(operation.flags&1U)){error=183;break;}
                if(shared_open(operation.to)){error=32;break;}
                replaced=staging(operation.to);error=filesystem_.rename(operation.to,replaced,false);if(error)break;
                undo.push_back({Undo::Kind::Rename,replaced,operation.to});staged.emplace_back(replaced,(target.metadata.attributes&0x10)!=0);
            }
            error=filesystem_.rename(operation.from,operation.to,false,(operation.flags&2U)!=0,(operation.flags&8U)!=0);if(error)break;
            undo.push_back({Undo::Kind::Rename,operation.to,operation.from});
        }else {
            error=filesystem_.create_directory(operation.to,[&]{return operation.security;},operation.source);if(error)break;
            undo.push_back({Undo::Kind::RemoveDirectory,operation.to,{}});
        }
    }
    if(error){
        for(auto it=undo.rbegin();it!=undo.rend();++it){
            if(it->kind==Undo::Kind::Rename)(void)filesystem_.rename(it->from,it->to,false);
            else (void)filesystem_.remove(it->from,true);
        }
        return error;
    }
    // 提交點之後只清除暫存名；清除失敗不改變已提交的結果。
    for(const auto& [hidden,directory]:staged)(void)filesystem_.remove(hidden,directory);
    return 0;
}
}
