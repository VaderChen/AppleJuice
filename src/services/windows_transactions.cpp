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
    enum class Kind {Delete,Move,CreateDirectory,RemoveDirectory,HardLink};
    // staged：CreateFileTransacted 的暫存檔（from），提交時以取代模式改名為 to；交易放棄時刪除。
    Kind kind{};std::string from,to;std::uint32_t flags{};std::vector<std::uint8_t> security;std::string source;bool staged{};
};
// 交易內的名稱視圖：source 為目前內容所在的實際路徑（未變更即原路徑；交易中建立的目錄為空），name 為原始大小寫的檔名。
struct KtmNameView {bool exists{},directory{};std::string source,name;};
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
// 同目錄的暫存名：提交時的刪除／取代與 CreateFileTransacted 的寫入時複製共用。
std::string staging_name(const std::string& path,std::uint32_t pid,std::uint64_t& sequence){
    auto [parent,leaf]=split_path(path);const auto separator=parent.empty() || parent.back()=='\\' || parent.back()=='/'?std::string{}:std::string{"\\"};
    return parent+separator+leaf+".~ktm-"+std::to_string(pid)+"-"+std::to_string(++sequence);
}
bool staging_leaf(std::string_view leaf){return leaf.find(".~ktm-")!=std::string_view::npos;}
// 交易放棄（回滾、逾時、最後一個 handle 關閉、提交失敗）：刪除尚未套用的暫存檔並丟棄作業。
void discard_operations(ReadOnlyFiles& files,KtmTransaction& transaction){
    for(const auto& operation:transaction.operations)if(operation.staged)(void)files.remove(operation.from,false);
    transaction.operations.clear();transaction.names.clear();
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
        {"kernel32.dll","RemoveDirectoryTransactedA",2},{"kernel32.dll","RemoveDirectoryTransactedW",2},
        {"kernel32.dll","GetFileAttributesTransactedA",4},{"kernel32.dll","GetFileAttributesTransactedW",4},
        {"kernel32.dll","FindFirstFileTransactedA",7},{"kernel32.dll","FindFirstFileTransactedW",7},
        {"kernel32.dll","CreateFileTransactedA",10},{"kernel32.dll","CreateFileTransactedW",10},
        {"kernel32.dll","CreateHardLinkTransactedA",4},{"kernel32.dll","CreateHardLinkTransactedW",4},
        {"ntdll.dll","NtCreateTransaction",10},{"ntdll.dll","NtCommitTransaction",2},{"ntdll.dll","NtRollbackTransaction",2}
    };return table;
}
TransactionState& WindowsServices::transaction_state(){if(!transactions_)transactions_=std::make_shared<TransactionState>();return *transactions_;}
bool WindowsServices::transactions_open()const{return transactions_ && !transactions_->handles.empty();}
void WindowsServices::append_transaction_handles(std::vector<std::uint64_t>& handles)const{
    if(transactions_)for(const auto& [handle,item]:transactions_->handles){(void)item;handles.push_back(handle);}
}
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
    if(last && transaction->outcome==undetermined){transaction->outcome=aborted;discard_operations(filesystem_,*transaction);}
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
            out->outcome=aborted;discard_operations(filesystem_,*out); // 逾時即由 KTM 回滾。
        }
        return out->outcome==undetermined?0U:out->outcome==committed?already_committed:already_aborted;
    };
    try{
        if(symbol=="NtCreateTransaction" || symbol=="NtCommitTransaction" || symbol=="NtRollbackTransaction"){
            // 原生 API 與 ktmw32 共用同一份交易：轉呼叫 CreateTransaction／CommitTransaction／RollbackTransaction，Win32 錯誤轉回 NTSTATUS，不改動 LastError。
            const auto status_of=[](std::uint32_t code)->std::uint32_t{switch(code){case 0:return 0;case 2:return 0xc0000034U;case 3:return 0xc000003aU;case 5:return 0xc0000022U;case 6:return 0xc0000008U;
                case 8:return 0xc0000017U;case 32:return 0xc0000043U;case 87:return 0xc000000dU;case 145:return 0xc0000101U;case 183:return 0xc0000035U;case 267:return 0xc0000103U;case 998:return 0xc0000005U;
                case 6701:return 0xc0190003U;case 6704:return 0xc0190015U;case 6705:return 0xc0190016U;default:return 0xc0000001U;}};
            const auto saved=last_error_;const auto done=[&](std::uint32_t status){last_error_=saved;arg.return_to(state,status);return std::optional<std::uint32_t>{};};
            if(symbol!="NtCreateTransaction"){
                // (TransactionHandle, Wait)：本後端的提交為同步，Wait 為 FALSE 時同樣在返回前完成。
                const auto ok=nested_service(state,memory,"ktmw32.dll",symbol=="NtCommitTransaction"?"CommitTransaction":"RollbackTransaction",{arg(0)});
                return done(ok?0U:status_of(last_error_));
            }
            // (TransactionHandle*, DesiredAccess, ObjectAttributes, Uow, TmHandle, CreateOptions, IsolationLevel, IsolationFlags, Timeout, Description)
            const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
            if(!arg(0))return done(0xc0000005U);memory.check(GuestAddress{arg(0)},unit,Access::Write);
            if(arg(4))return done(0xc0000008U); // 沒有資源管理員（TM）物件
            GuestScratch temporary(*this,memory,state);std::uint64_t attributes=0,description=0;
            if(arg(2)){
                // OBJECT_ATTRIBUTES 的 SecurityDescriptor 與 OBJ_INHERIT 轉成 SECURITY_ATTRIBUTES。
                const auto flags=memory.load(GuestAddress{arg(2)+unit*3},32),descriptor=memory.load(GuestAddress{arg(2)+unit*4},bits);
                attributes=temporary.allocate(unit*3);if(!attributes)return done(0xc0000017U);memory.fill(GuestAddress{attributes},0,unit*3);
                memory.store(GuestAddress{attributes},32,unit*3);memory.store(GuestAddress{attributes+unit},bits,descriptor);memory.store(GuestAddress{attributes+unit*2},32,(flags&2U)?1U:0U);
            }
            std::uint32_t timeout=0;
            if(arg(8)){
                // 負值為相對時間、正值為絕對 FILETIME（100 奈秒）；0 表示立即逾時，以 1 毫秒表達。
                const auto value=static_cast<std::int64_t>(memory.load(GuestAddress{arg(8)},64));const auto now=static_cast<std::int64_t>(runtime_.system_filetime());
                const auto relative=value<0?-value:value-now;timeout=relative<=0?1U:static_cast<std::uint32_t>(std::min<std::int64_t>((relative+9999)/10000,0xfffffffeLL));
            }
            if(arg(9)){
                const auto length=memory.load(GuestAddress{arg(9)},16),buffer=memory.load(GuestAddress{arg(9)+unit},bits);
                if(length%2 || (length && !buffer))return done(0xc000000dU);
                std::u16string text(length/2,u'\0');for(std::size_t i=0;i<text.size();++i)text[i]=static_cast<char16_t>(memory.load(GuestAddress{buffer+i*2},16));
                description=temporary.text(text);if(!description)return done(0xc0000017U);
            }
            const auto handle=nested_service(state,memory,"ktmw32.dll","CreateTransaction",{attributes,arg(3),arg(5),arg(6),arg(7),timeout,description});
            if(handle==invalid_handle)return done(status_of(last_error_));
            memory.store(GuestAddress{arg(0)},bits,handle);return done(0);
        }
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
            else {transaction->outcome=aborted;discard_operations(filesystem_,*transaction);result=1;}
            return finish();
        }
        if(symbol=="CommitTransaction" || symbol=="CommitTransactionAsync"){
            const auto error=active(arg(0),transaction);
            if(error){fail(error);return finish();}
            if(filesystem_.writable() && !runtime_.network.external_io)throw HostCallFault("比較模式不允許宿主檔案交易提交");
            const auto commit_error=commit_transaction(*transaction);
            // 失敗時各步已復原，暫存檔回到暫存名，一併刪除。
            if(commit_error)discard_operations(filesystem_,*transaction);else {transaction->operations.clear();transaction->names.clear();}
            if(commit_error){transaction->outcome=aborted;fail(commit_error);}else {transaction->outcome=committed;result=1;}
            return finish();
        }
        // 以下為 TxF 檔案作業：只驗證並排入交易，提交前不改動磁碟。
        const bool wide=symbol.back()=='W';
        const auto path=[&](unsigned i){return wide?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(i)})):read_ansi(memory,arg(i),32767);};
        const bool moving=symbol.starts_with("MoveFileTransacted"),creating=symbol.starts_with("CreateDirectoryTransacted");
        const bool attributes=symbol.starts_with("GetFileAttributesTransacted"),finding=symbol.starts_with("FindFirstFileTransacted"),
            opening=symbol.starts_with("CreateFileTransacted"),linking=symbol.starts_with("CreateHardLinkTransacted");
        const auto transaction_argument=moving?5U:creating || attributes || linking?3U:finding?6U:opening?7U:1U;
        if(finding || opening)result=invalid_handle;
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
            return {info.error==0,info.error==0 && (info.metadata.attributes&0x10)!=0,info.error==0?value:std::string{},split_path(value).second};
        };
        const auto shared_open=[&](const std::string& value){
            const auto info=filesystem_.information(value);
            return !info.error && std::any_of(files_.begin(),files_.end(),[&](const auto& entry){return !entry.second.native && entry.second.key==info.key && !(entry.second.sharing&4U);});
        };
        std::uint32_t failure=0;KtmOperation operation;
        GuestScratch temporary(*this,memory,state);
        const auto wide_path=[&](const std::string& value)->std::uint64_t{const auto at=temporary.text(utf8_to_utf16_lossy(value));if(!at)throw std::bad_alloc{};return at;};
        // 交易視圖中的目錄項目：內容在 source（交易中建立的目錄沒有實體，時間取現在）。
        const auto entry_of=[&](const std::string& name,const KtmNameView& item){DirectoryEntry entry;entry.name=item.name.empty()?name:item.name;
            if(item.source.empty()){const auto now=runtime_.system_filetime();entry.metadata.attributes=0x10;entry.metadata.created=entry.metadata.accessed=entry.metadata.modified=entry.metadata.changed=now;}
            else entry.metadata=filesystem_.information(item.source).metadata;return entry;};
        if(attributes){
            // (lpFileName, fInfoLevelId＝GetFileExInfoStandard, lpFileInformation, hTransaction)：依交易視圖；未變更的名稱與 GetFileAttributesEx 相同。
            if(!arg(0) || u32(1) || !arg(2)){fail(87);return finish();}
            const auto target=path(0);const auto key=name_key(target,failure);if(failure){fail(failure);return finish();}
            const auto current=view(target,key);if(!current.exists){fail(2);return finish();}
            if(current.source.empty()){
                memory.check(GuestAddress{arg(2)},36,Access::Write);memory.fill(GuestAddress{arg(2)},0,36);memory.store(GuestAddress{arg(2)},32,0x10);
                const auto now=runtime_.system_filetime();for(unsigned i=0;i<3;++i)memory.store(GuestAddress{arg(2)+4+i*8},64,now);result=1;last_error_=0;return finish();
            }
            result=current.source==target?nested_service(state,memory,"kernel32.dll",wide?"GetFileAttributesExW":"GetFileAttributesExA",{arg(0),0,arg(2)})
                :nested_service(state,memory,"kernel32.dll","GetFileAttributesExW",{wide_path(current.source),0,arg(2)});
            return finish();
        }
        if(finding){
            // 與 FindFirstFileEx 相同的參數檢查；交易外的目錄快照扣除交易中刪除／移走的名稱、加入交易中建立的名稱，暫存檔不列出。
            if(!arg(0) || !arg(2) || u32(1)>1 || u32(3)>1 || arg(4) || (u32(5)&~7U)){fail(87);return finish();}
            const auto pattern=path(0);const auto [parent,leaf]=split_path(pattern);if(leaf.empty()){fail(2);return finish();}
            const auto directory=filesystem_.information(parent.empty()?std::string{"."}:parent);if(directory.error){fail(directory.error==2?3:directory.error);return finish();}
            auto [search_error,search]=search_directory(pattern);if(search_error && search_error!=2){fail(search_error);return finish();}
            const auto prefix=directory.key+'\x01';DirectorySearch merged;
            for(const auto& entry:search.entries){if(staging_leaf(entry.name))continue;const auto found=transaction->names.find(prefix+fold_leaf(entry.name));
                if(found==transaction->names.end())merged.entries.push_back(entry);else if(found->second.exists)merged.entries.push_back(entry_of(entry.name,found->second));}
            const auto expression=utf8_to_utf16_lossy(leaf);
            for(const auto& [key,item]:transaction->names){
                if(!item.exists || !key.starts_with(prefix) || item.name.empty())continue;const auto folded=key.substr(prefix.size());
                if(std::any_of(search.entries.begin(),search.entries.end(),[&](const auto& entry){return fold_leaf(entry.name)==folded;}))continue;
                if(directory_name_matches(expression,utf8_to_utf16_lossy(item.name)))merged.entries.push_back(entry_of(item.name,item));
            }
            if(merged.entries.empty()){fail(2);return finish();}if(runtime_.directory_searches.size()>=1024){fail(8);return finish();}
            const auto handle=runtime_.allocate_wait_handle();runtime_.directory_searches.emplace(handle,std::move(merged));
            if(!nested_service(state,memory,"kernel32.dll",wide?"FindNextFileW":"FindNextFileA",{handle,arg(2)})){runtime_.directory_searches.erase(handle);return finish();}
            result=handle;last_error_=0;return finish();
        }
        if(opening){
            // (lpFileName, access, share, sa, disposition, flags, template, hTransaction, pusMiniVersion, lpExtendedParameter)：讀取依交易視圖開啟實際來源；
            // 寫入、建立或截斷時在同目錄建立暫存檔（開啟既有檔案寫入時先複製內容），提交時以取代模式改名到原名，交易外看到的仍是原檔。
            if(!arg(0) || arg(8) || arg(9)){fail(87);return finish();}
            const auto access=u32(1),disposition=u32(4);if(disposition<1 || disposition>5){fail(87);return finish();}
            const auto target=path(0);const auto key=name_key(target,failure);if(failure){fail(failure);return finish();}
            const auto current=view(target,key);const bool existed=current.exists;
            const bool write=(access&0x50000000U) || (access&0x000d0116U),truncate=disposition==2 || disposition==5;
            if(existed && current.directory){
                if(write || truncate || disposition==1){fail(5);return finish();}if(current.source.empty()){fail(5);return finish();}
                result=nested_service(state,memory,"kernel32.dll","CreateFileW",{wide_path(current.source),arg(1),arg(2),arg(3),3,arg(5),0});return finish();
            }
            if(disposition==1 && existed){fail(80);return finish();}
            if((disposition==3 || disposition==5) && !existed){fail(2);return finish();}
            if(disposition==5 && !write){fail(87);return finish();}
            const auto saved_error=[&](std::uint64_t handle){if(handle!=invalid_handle && existed && (disposition==2 || disposition==4))last_error_=183;else if(handle!=invalid_handle)last_error_=0;};
            if(existed && !write && !truncate){
                const auto handle=current.source==target?nested_service(state,memory,"kernel32.dll",wide?"CreateFileW":"CreateFileA",{arg(0),arg(1),arg(2),arg(3),3,arg(5),arg(6)})
                    :nested_service(state,memory,"kernel32.dll","CreateFileW",{wide_path(current.source),arg(1),arg(2),arg(3),3,arg(5),arg(6)});
                result=handle;saved_error(handle);return finish();
            }
            const bool staged=existed && std::any_of(transaction->operations.begin(),transaction->operations.end(),[&](const auto& item){return item.staged && item.from==current.source;});
            if(staged){
                result=nested_service(state,memory,"kernel32.dll","CreateFileW",{wide_path(current.source),arg(1),arg(2),arg(3),truncate?5U:3U,arg(5),arg(6)});saved_error(result);return finish();
            }
            if(transaction->operations.size()>=65536){fail(8);return finish();}
            const auto copy=staging_name(target,process_id(),transaction->sequence);
            if(existed && !truncate)if(const auto copied=filesystem_.copy(current.source,copy,true)){fail(copied);return finish();}
            const auto handle=nested_service(state,memory,"kernel32.dll","CreateFileW",{wide_path(copy),arg(1),arg(2),arg(3),existed && !truncate?3U:1U,arg(5),arg(6)});
            if(handle==invalid_handle){const auto saved=last_error_;(void)filesystem_.remove(copy,false);last_error_=saved;return finish();}
            operation.kind=KtmOperation::Kind::Move;operation.from=copy;operation.to=target;operation.flags=1;operation.staged=true;
            transaction->operations.push_back(std::move(operation));transaction->names[key]={true,false,copy,split_path(target).second};
            result=handle;saved_error(handle);return finish();
        }
        if(linking){
            // (lpFileName 新連結, lpExistingFileName, lpSecurityAttributes 保留, hTransaction)：交易中建立的名稱也可作為來源；提交時建立實際硬連結。
            if(!arg(0) || !arg(1)){fail(87);return finish();}
            operation.kind=KtmOperation::Kind::HardLink;operation.to=path(0);operation.from=path(1);
            const auto link_key=name_key(operation.to,failure);if(failure){fail(failure);return finish();}
            const auto existing_key=name_key(operation.from,failure);if(failure){fail(failure);return finish();}
            const auto source=view(operation.from,existing_key);
            if(!source.exists){fail(2);return finish();}if(source.directory){fail(5);return finish();}if(view(operation.to,link_key).exists){fail(183);return finish();}
            transaction->names[link_key]={true,false,source.source,split_path(operation.to).second};
        }else
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
            if(from_key!=to_key){transaction->names[from_key]={};transaction->names[to_key]={true,source.directory,source.source,split_path(operation.to).second};}
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
            transaction->names[key]={true,true,{},split_path(operation.to).second};
        }else throw HostCallFault("未知的交易式檔案入口");
        if(transaction->operations.size()>=65536){fail(8);return finish();}
        transaction->operations.push_back(std::move(operation));result=1;last_error_=0;
    }catch(const MemoryFault&){fail(998);result=symbol=="CreateTransaction" || symbol.starts_with("FindFirstFileTransacted") || symbol.starts_with("CreateFileTransacted")?invalid_handle:0;}
    catch(const std::bad_alloc&){fail(8);result=symbol=="CreateTransaction" || symbol.starts_with("FindFirstFileTransacted") || symbol.starts_with("CreateFileTransacted")?invalid_handle:0;}
    return finish();
}
std::uint32_t WindowsServices::commit_transaction(KtmTransaction& transaction){
    // 每一步都可逆：刪除與被取代的目標先改名到同目錄的暫存名，全部成功後才實際刪除。
    struct Undo {enum class Kind {Rename,RemoveDirectory,RemoveFile} kind;std::string from,to;};
    std::vector<Undo> undo;std::vector<std::pair<std::string,bool>> staged;
    const auto staging=[&](const std::string& path){return staging_name(path,process_id(),transaction.sequence);};
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
        }else if(operation.kind==Kind::HardLink){
            error=filesystem_.hard_link(operation.from,operation.to);if(error)break;
            undo.push_back({Undo::Kind::RemoveFile,operation.to,{}});
        }else {
            error=filesystem_.create_directory(operation.to,[&]{return operation.security;},operation.source);if(error)break;
            undo.push_back({Undo::Kind::RemoveDirectory,operation.to,{}});
        }
    }
    if(error){
        for(auto it=undo.rbegin();it!=undo.rend();++it){
            if(it->kind==Undo::Kind::Rename)(void)filesystem_.rename(it->from,it->to,false);
            else (void)filesystem_.remove(it->from,it->kind==Undo::Kind::RemoveDirectory);
        }
        return error;
    }
    // 提交點之後只清除暫存名；清除失敗不改變已提交的結果。
    for(const auto& [hidden,directory]:staged)(void)filesystem_.remove(hidden,directory);
    return 0;
}
}
