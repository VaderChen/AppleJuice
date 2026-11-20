// advapi32 LSA 原則（由 windows_security.cpp 的安全性服務引入，位於 symbol 以 "Lsa" 開頭的分支）：函式回傳 NTSTATUS，不寫 LastError。
// 原則 handle 依存取權開啟：任何使用者都可取得 POLICY_VIEW_LOCAL_INFORMATION、POLICY_LOOKUP_NAMES 與讀取控制，系統管理員權杖才有 POLICY_ALL_ACCESS；
// 泛用權限依 LSA 原則的對應表展開，MAXIMUM_ALLOWED 取得可用的全部權限。查詢結果以程序堆積配置，一次 LsaFreeMemory 釋放整份輸出。
// 帳戶權限存於沙箱登錄（見 with_rights）。帳戶與 SID 互查共用 LookupAccountName／LookupAccountSid 的帳戶表；網域 SID 為帳戶 SID 去掉最後一個子授權。本機不屬於任何網域（工作群組 WORKGROUP），沒有信任網域。
{
    constexpr std::uint32_t status_success=0,status_some_not_mapped=0x107,status_none_mapped=0xc0000073U,status_access_denied=0xc0000022U,status_invalid_handle=0xc0000008U,
        status_invalid_parameter=0xc000000dU,status_no_memory=0xc0000017U,status_no_such_privilege=0xc0000060U,status_no_more_entries=0x8000001aU,status_invalid_info_class=0xc0000003U,
        status_not_implemented=0xc0000002U,status_unsuccessful=0xc0000001U,status_not_supported=0xc00000bbU;
    constexpr std::uint32_t view_local=0x1,view_audit=0x2,lookup_names=0x800,create_account=0x10,all_access=0xf0fff,read_control=0x20000;
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);const unsigned string_size=x86_?8U:16U;
    initialize_tokens();
    const auto& primary=runtime_.token_objects.at(1).user.sid;
    // 輸出區塊：結構、字串與 SID 依序放在同一塊堆積記憶體，指標於配置後換成絕對位址。
    struct Block {std::vector<std::uint8_t> bytes;std::vector<std::pair<std::size_t,std::size_t>> pointers;
        std::size_t reserve(std::size_t size,std::size_t align){bytes.resize((bytes.size()+align-1)/align*align);const auto at=bytes.size();bytes.resize(at+size);return at;}
        void set(std::size_t at,std::uint64_t value,unsigned size){for(unsigned i=0;i<size;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(8*i));}};
    const auto data=[&](Block& block,std::span<const std::uint8_t> value){const auto at=block.reserve(value.size(),4);std::copy(value.begin(),value.end(),block.bytes.begin()+static_cast<std::ptrdiff_t>(at));return at;};
    const auto string=[&](Block& block,std::size_t at,std::u16string_view text){
        if(text.empty())return;const auto bytes=utf16_bytes(std::u16string(text));
        block.set(at,text.size()*2,2);block.set(at+2,bytes.size(),2);block.pointers.push_back({at+(x86_?4U:8U),data(block,bytes)});};
    const auto sid_pointer=[&](Block& block,std::size_t at,std::span<const std::uint8_t> sid){if(!sid.empty())block.pointers.push_back({at,data(block,sid)});};
    const auto commit=[&](Block& block)->std::uint64_t{
        if(block.bytes.empty())block.bytes.resize(unit);const auto base=allocate_heap(memory,block.bytes.size());if(!base)throw std::bad_alloc{};
        for(const auto& [at,target]:block.pointers)block.set(at,base+target,unit);memory.write(GuestAddress{base},block.bytes);return base;};
    const auto store_pointer=[&](std::uint64_t address,std::uint64_t value){memory.store(GuestAddress{address},bits,value);};
    const auto read_string=[&](std::uint64_t address){
        const auto length=memory.load(GuestAddress{address},16),capacity=memory.load(GuestAddress{address+2},16),buffer=memory.load(GuestAddress{address+(x86_?4U:8U)},bits);
        if((length&1) || length>capacity || (length && !buffer))throw SecurityDescriptorError{87};return read_utf16_counted(memory,GuestAddress{buffer},static_cast<std::size_t>(length/2));};
    const auto policy=[&](std::uint64_t handle,std::uint32_t required)->std::optional<std::uint32_t>{
        const auto found=lsa_policies_.find(handle);if(found==lsa_policies_.end())return status_invalid_handle;if((found->second&required)!=required)return status_access_denied;return std::nullopt;};
    // 參照網域清單：LSA_REFERENCED_DOMAIN_LIST {ULONG Entries; PLSA_TRUST_INFORMATION Domains}，LSA_TRUST_INFORMATION {LSA_UNICODE_STRING Name; PSID Sid}。
    struct Domain {std::u16string name;security::Bytes sid;};std::vector<Domain> domains;
    const auto domain_index=[&](std::u16string_view name,std::span<const std::uint8_t> sid)->std::int32_t{
        for(std::size_t i=0;i<domains.size();++i)if(same_name(utf16_to_utf8(domains[i].name),utf16_to_utf8(name)) && domains[i].sid.size()==sid.size() && std::equal(sid.begin(),sid.end(),domains[i].sid.begin()))return static_cast<std::int32_t>(i);
        domains.push_back({std::u16string(name),{sid.begin(),sid.end()}});return static_cast<std::int32_t>(domains.size()-1);};
    const auto domain_of=[&](const security::AccountIdentity& account)->std::int32_t{
        // SidTypeDomain（3）本身就是網域；其他帳戶的網域 SID 去掉最後一個子授權。
        auto sid=account.sid;if(account.type!=3 && sid.size()>=12 && sid[1]){--sid[1];sid.resize(sid.size()-4);}
        return domain_index(utf8_to_utf16(std::string(account.type==3?account.name:account.domain)),sid);};
    const auto domain_list=[&]{
        Block block;const auto header=block.reserve(unit*2,unit);block.set(header,domains.size(),4);
        if(!domains.empty()){const auto entry=string_size+unit;const auto array=block.reserve(domains.size()*entry,unit);block.pointers.push_back({header+unit,array});
            for(std::size_t i=0;i<domains.size();++i){string(block,array+i*entry,domains[i].name);sid_pointer(block,array+i*entry+string_size,domains[i].sid);}}
        return commit(block);};
    const auto mapped_status=[](std::size_t mapped,std::size_t count){return mapped==count?status_success:mapped?status_some_not_mapped:status_none_mapped;};
    // 帳戶權限：沙箱登錄 HKLM\SECURITY\Policy\Accounts\<SID> 的 Rights（REG_MULTI_SZ）。尚未修改過時使用 Windows 的預設指派，
    // 第一次修改時把整份指派寫入；讀取與修改都在同一個登錄交易內完成，其他程序立即看到結果。權限變更在下次登入的權杖生效，不改變現有權杖。
    struct LsaStatus {std::uint32_t code;};
    using Rights=std::map<std::string,std::set<std::string>>;
    const std::u16string accounts_root=std::u16string(1,u'\2')+u"\\security\\policy\\accounts";
    constexpr std::array<std::string_view,10> logon_rights{"SeInteractiveLogonRight","SeNetworkLogonRight","SeBatchLogonRight","SeServiceLogonRight","SeRemoteInteractiveLogonRight",
        "SeDenyInteractiveLogonRight","SeDenyNetworkLogonRight","SeDenyBatchLogonRight","SeDenyServiceLogonRight","SeDenyRemoteInteractiveLogonRight"};
    const auto canonical_right=[&](std::string_view name)->std::optional<std::string>{
        for(const auto candidate:privilege_names)if(same_name(name,candidate))return std::string(candidate);
        for(const auto candidate:logon_rights)if(same_name(name,candidate))return std::string(candidate);
        return std::nullopt;};
    const auto default_rights=[]{
        static constexpr std::pair<std::string_view,std::string_view> table[]{
            {"S-1-1-0","SeChangeNotifyPrivilege SeNetworkLogonRight"},
            {"S-1-5-19","SeAssignPrimaryTokenPrivilege SeAuditPrivilege SeChangeNotifyPrivilege SeCreateGlobalPrivilege SeImpersonatePrivilege SeIncreaseQuotaPrivilege SeSystemtimePrivilege SeTimeZonePrivilege"},
            {"S-1-5-20","SeAssignPrimaryTokenPrivilege SeAuditPrivilege SeChangeNotifyPrivilege SeCreateGlobalPrivilege SeImpersonatePrivilege SeIncreaseQuotaPrivilege"},
            {"S-1-5-6","SeCreateGlobalPrivilege SeImpersonatePrivilege"},
            {"S-1-5-32-544","SeBackupPrivilege SeBatchLogonRight SeChangeNotifyPrivilege SeCreateGlobalPrivilege SeCreatePagefilePrivilege SeCreateSymbolicLinkPrivilege SeDebugPrivilege "
             "SeDelegateSessionUserImpersonatePrivilege SeImpersonatePrivilege SeIncreaseBasePriorityPrivilege SeIncreaseQuotaPrivilege SeIncreaseWorkingSetPrivilege SeInteractiveLogonRight "
             "SeLoadDriverPrivilege SeManageVolumePrivilege SeNetworkLogonRight SeProfileSingleProcessPrivilege SeRemoteInteractiveLogonRight SeRemoteShutdownPrivilege SeRestorePrivilege "
             "SeSecurityPrivilege SeShutdownPrivilege SeSystemEnvironmentPrivilege SeSystemProfilePrivilege SeSystemtimePrivilege SeTakeOwnershipPrivilege SeTimeZonePrivilege SeUndockPrivilege"},
            {"S-1-5-32-545","SeChangeNotifyPrivilege SeIncreaseWorkingSetPrivilege SeInteractiveLogonRight SeNetworkLogonRight SeShutdownPrivilege SeTimeZonePrivilege SeUndockPrivilege"},
            {"S-1-5-32-551","SeBackupPrivilege SeBatchLogonRight SeChangeNotifyPrivilege SeInteractiveLogonRight SeNetworkLogonRight SeRestorePrivilege SeShutdownPrivilege"},
            {"S-1-5-32-555","SeRemoteInteractiveLogonRight"},{"S-1-5-32-559","SeBatchLogonRight"},{"S-1-5-80-0","SeServiceLogonRight"}};
        Rights rights;for(const auto& [sid,list]:table){auto& set=rights[std::string(sid)];std::size_t at=0;
            while(at<list.size()){const auto end=list.find(' ',at);set.insert(std::string(list.substr(at,end==list.npos?list.npos:end-at)));if(end==list.npos)break;at=end+1;}}
        return rights;};
    // 在登錄交易內讀出指派；modify 回傳 true 時寫回並提交。
    const auto with_rights=[&](bool writing,const auto& modify){
        if(!registry_file_.empty() && !runtime_.network.external_io)throw HostCallFault("比較模式不允許持久登錄檔 I/O");
        RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,!writing);if(transaction.error)throw LsaStatus{status_unsuccessful};
        auto& keys=runtime_.registry.keys;Rights rights;
        if(!keys.contains(accounts_root))rights=default_rights();
        else for(const auto& [key,values]:keys){
            if(key.size()<=accounts_root.size()+1 || !key.starts_with(accounts_root+u"\\") || key.find(u'\\',accounts_root.size()+1)!=key.npos)continue;
            auto sid=utf16_to_utf8(key.substr(accounts_root.size()+1));if(!sid.empty())sid[0]='S';
            const auto value=values.find(u"rights");if(value==values.end() || value->second.type!=7)continue;
            std::set<std::string> set;std::u16string item;const auto& bytes=value->second.bytes;
            for(std::size_t i=0;i+1<bytes.size();i+=2){const auto c=static_cast<char16_t>(bytes[i]|(bytes[i+1]<<8));if(c){item+=c;continue;}if(item.empty())break;set.insert(utf16_to_utf8(item));item.clear();}
            if(!set.empty())rights[sid]=std::move(set);
        }
        if(!modify(rights) || !writing){if(writing)transaction.unchanged();return;}
        const auto before=runtime_.registry;
        try{
            for(auto it=keys.begin();it!=keys.end();)it=it->first==accounts_root || it->first.starts_with(accounts_root+u"\\")?keys.erase(it):std::next(it);
            for(const auto& parent:{std::u16string(1,u'\2')+u"\\security",std::u16string(1,u'\2')+u"\\security\\policy",accounts_root})keys.try_emplace(parent);
            for(const auto& [sid,set]:rights){if(set.empty())continue;std::vector<std::uint8_t> bytes;
                for(const auto& right:set){const auto text=utf16_bytes(utf8_to_utf16(right));bytes.insert(bytes.end(),text.begin(),text.end());}bytes.push_back(0);bytes.push_back(0);
                auto name=utf8_to_utf16(sid);for(auto& c:name)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);
                keys[accounts_root+u"\\"+name][u"rights"]=RegistryValue{7,std::move(bytes),u"Rights"};}
            if(transaction.commit()){runtime_.registry=before;throw LsaStatus{status_unsuccessful};}
        }catch(...){runtime_.registry=before;throw;}};
    // 原則設定存於沙箱登錄 HKLM\SECURITY\Policy 的值：稽核（x64arm_auditpolicy：模式＋59 個子類別）、主要網域（polprdmn／polprdms）、帳戶網域（polacdmn／polacdms）、
    // DNS 網域（poldnddn、poldndmn、poldntrn、poldndmg、poldndms）。沒有寫過的值沿用工作群組預設。
    const std::u16string policy_root=std::u16string(1,u'\2')+u"\\security\\policy";
    const auto read_policy=[&](std::u16string_view name)->std::optional<RegistryValue>{
        RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)throw LsaStatus{status_unsuccessful};
        const auto key=runtime_.registry.keys.find(policy_root);if(key==runtime_.registry.keys.end())return std::nullopt;
        const auto value=key->second.find(std::u16string(name));if(value==key->second.end())return std::nullopt;return value->second;};
    const auto write_policy=[&](const std::vector<std::pair<std::u16string,RegistryValue>>& values){
        if(!registry_file_.empty() && !runtime_.network.external_io)throw HostCallFault("比較模式不允許持久登錄檔 I/O");
        RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,false);if(transaction.error)throw LsaStatus{status_unsuccessful};
        const auto before=runtime_.registry;
        try{
            for(const auto& parent:{std::u16string(1,u'\2')+u"\\security",policy_root})runtime_.registry.keys.try_emplace(parent);
            for(const auto& [name,value]:values){if(value.type)runtime_.registry.keys[policy_root][name]=value;else runtime_.registry.keys[policy_root].erase(name);}
            if(transaction.commit()){runtime_.registry=before;throw LsaStatus{status_unsuccessful};}
        }catch(...){runtime_.registry=before;throw;}};
    const auto text_value=[&](std::u16string_view name,std::u16string_view fallback){
        const auto value=read_policy(name);if(!value || value->type!=1)return std::u16string(fallback);std::u16string out;
        for(std::size_t i=0;i+1<value->bytes.size();i+=2){const auto c=static_cast<char16_t>(static_cast<unsigned>(value->bytes[i])|static_cast<unsigned>(value->bytes[i+1])<<8);if(!c)break;out.push_back(c);}return out;};
    const auto binary_value=[&](std::u16string_view name,std::span<const std::uint8_t> fallback){const auto value=read_policy(name);return value && value->type==3?value->bytes:security::Bytes(fallback.begin(),fallback.end());};
    const auto sz=[&](std::u16string_view display,std::u16string_view text){return RegistryValue{1,utf16_bytes(std::u16string(text)),std::u16string(display)};};
    const auto binary=[&](std::u16string_view display,std::span<const std::uint8_t> bytes){return RegistryValue{bytes.empty()?0U:3U,security::Bytes(bytes.begin(),bytes.end()),std::u16string(display)};};
    // 稽核子類別：{0CCE9xxx-69AE-11D9-BED3-505054503030}，類別 {6997984x-797A-11D9-BED3-505054503030}（系統、登入／登出、物件存取、特殊權限使用、詳細追蹤、
    // 原則變更、帳戶管理、目錄服務存取、帳戶登入，與舊式 POLICY_AUDIT_EVENT_TYPE 同序）。
    static constexpr std::array<std::pair<std::uint16_t,std::uint8_t>,59> audit_subcategories{{
        {0x9210,0},{0x9211,0},{0x9212,0},{0x9213,0},{0x9214,0},
        {0x9215,1},{0x9216,1},{0x9217,1},{0x9218,1},{0x9219,1},{0x921a,1},{0x921b,1},{0x921c,1},{0x9243,1},{0x9247,1},{0x9249,1},
        {0x921d,2},{0x921e,2},{0x921f,2},{0x9220,2},{0x9221,2},{0x9222,2},{0x9223,2},{0x9224,2},{0x9225,2},{0x9226,2},{0x9227,2},{0x9244,2},{0x9245,2},{0x9246,2},
        {0x9228,3},{0x9229,3},{0x922a,3},{0x922b,4},{0x922c,4},{0x922d,4},{0x922e,4},{0x9248,4},{0x924a,4},
        {0x922f,5},{0x9230,5},{0x9231,5},{0x9232,5},{0x9233,5},{0x9234,5},{0x9235,6},{0x9236,6},{0x9237,6},{0x9238,6},{0x9239,6},{0x923a,6},
        {0x923b,7},{0x923c,7},{0x923d,7},{0x923e,7},{0x923f,8},{0x9240,8},{0x9241,8},{0x9242,8}}};
    const auto audit_guid=[&](std::uint32_t data1,std::uint16_t data2){std::array<std::uint8_t,16> id{};for(unsigned i=0;i<4;++i)id[i]=static_cast<std::uint8_t>(data1>>(8*i));
        id[4]=static_cast<std::uint8_t>(data2);id[5]=static_cast<std::uint8_t>(data2>>8);id[6]=0xd9;id[7]=0x11;id[8]=0xbe;id[9]=0xd3;id[10]=0x50;id[11]=0x50;id[12]=0x54;id[13]=0x50;id[14]=0x30;id[15]=0x30;return id;};
    const auto load_audit=[&]{std::array<std::uint32_t,60> words{};const auto bytes=binary_value(u"x64arm_auditpolicy",{});
        if(bytes.size()==words.size()*4)for(std::size_t i=0;i<words.size();++i)words[i]=static_cast<std::uint32_t>(bytes[i*4])|static_cast<std::uint32_t>(bytes[i*4+1])<<8|static_cast<std::uint32_t>(bytes[i*4+2])<<16|static_cast<std::uint32_t>(bytes[i*4+3])<<24;return words;};
    const auto save_audit=[&](const std::array<std::uint32_t,60>& words){security::Bytes bytes(words.size()*4);for(std::size_t i=0;i<words.size();++i)for(unsigned b=0;b<4;++b)bytes[i*4+b]=static_cast<std::uint8_t>(words[i]>>(8*b));
        write_policy({{u"x64arm_auditpolicy",binary(u"X64ARM_AuditPolicy",bytes)}});};
    // 原則變更通知：LsaRegisterPolicyChangeNotification 的事件依類別（1 稽核、2 帳戶網域、4 DNS 網域）設定為有訊號。
    const auto notify_policy=[&](std::uint32_t information){for(const auto& [kind,event]:lsa_notifications_)if(kind==information)(void)nested_service(state,memory,"kernel32.dll","SetEvent",{event});};
    std::uint32_t audit_error=0;
    std::uint32_t access_audit_error=0;
    const auto read_rights=[&](std::uint64_t address,std::uint32_t count){
        std::vector<std::string> list;for(std::uint32_t i=0;i<count;++i){const auto name=utf16_to_utf8(read_string(address+i*string_size));const auto right=canonical_right(name);if(!right)throw LsaStatus{status_no_such_privilege};list.push_back(*right);}
        return list;};
    // LSA_ENUMERATION_INFORMATION {PSID Sid} 陣列。
    const auto sid_array=[&](const std::vector<std::string>& sids){Block block;const auto array=block.reserve(std::max<std::size_t>(sids.size(),1)*unit,unit);
        for(std::size_t i=0;i<sids.size();++i)sid_pointer(block,array+i*unit,sddl_sid(sids[i]));return commit(block);};
    std::uint32_t status=status_success;
    try {
    if(symbol=="LsaNtStatusToWinError")status=lsa_winerror(u32(0));
    else if(symbol=="LsaOpenPolicy"){
        // (SystemName, ObjectAttributes, DesiredAccess, PolicyHandle)：只接受本機名稱。
        if(!arg(1) || !arg(3))status=status_invalid_parameter;
        else {
            memory.check(GuestAddress{arg(1)},x86_?24:48,Access::Read);memory.check(GuestAddress{arg(3)},unit,Access::Write);
            const auto system=arg(0)?read_string(arg(0)):std::u16string{};const auto name=utf16_to_utf8(system);
            if(!system.empty() && name!="." && !same_name(name,"X64ARM") && !same_name(name,"\\\\X64ARM"))status=status_not_supported;
            else {
                const auto& token=runtime_.token_objects.at(effective_token_id());const auto administrators=sddl_sid("BA");
                const bool admin=token_sid_matches(token,administrators,false) && (!token.restriction_check || token_sid_matches(token,administrators,false,true));
                const std::uint32_t allowed=admin?all_access:(view_local|lookup_names|read_control);
                auto requested=u32(2);
                if(requested&0x80000000U)requested=(requested&~0x80000000U)|0x20006U;if(requested&0x40000000U)requested=(requested&~0x40000000U)|0x207f8U;
                if(requested&0x20000000U)requested=(requested&~0x20000000U)|0x20801U;if(requested&0x10000000U)requested=(requested&~0x10000000U)|all_access;
                const bool maximum=(requested&0x02000000U)!=0;requested&=~0x02000000U;
                if(requested&~allowed)status=status_access_denied;
                else {if(lsa_policies_.size()>=4096)throw std::bad_alloc{};const auto handle=next_lsa_policy_++;lsa_policies_[handle]=maximum?allowed:requested;store_pointer(arg(3),handle);}
            }
        }
    }else if(symbol=="LsaClose")status=lsa_policies_.erase(arg(0))?status_success:status_invalid_handle;
    else if(symbol=="LsaFreeMemory")status=!arg(0) || free_heap(memory,arg(0),state)?status_success:status_unsuccessful;
    else if(symbol=="LsaAddAccountRights" || symbol=="LsaRemoveAccountRights"){
        // LsaAddAccountRights(Policy, AccountSid, UserRights, Count)；LsaRemoveAccountRights(Policy, AccountSid, AllRights, UserRights, Count)。
        // 修改需要 POLICY_LOOKUP_NAMES 與 POLICY_CREATE_ACCOUNT（只有系統管理員權杖取得）；未知權限名稱整批不變並回傳 STATUS_NO_SUCH_PRIVILEGE。
        const bool add=symbol=="LsaAddAccountRights";const auto rights_at=arg(add?2:3);const auto count=u32(add?3:4);const bool all=!add && (arg(2)&0xff)!=0;
        if(const auto denied=policy(arg(0),lookup_names|create_account))status=*denied;
        else if(!arg(1) || (!all && count && !rights_at) || count>1024)status=status_invalid_parameter;
        else {
            const auto sid=sid_string(read_sid(memory,arg(1)));const auto list=all?std::vector<std::string>{}:read_rights(rights_at,count);
            with_rights(true,[&](Rights& rights){
                if(add){if(list.empty())return false;auto& set=rights[sid];for(const auto& right:list)set.insert(right);return true;}
                const auto found=rights.find(sid);if(found==rights.end())throw LsaStatus{0xc0000034U};
                if(all)rights.erase(found);else{for(const auto& right:list)found->second.erase(right);if(found->second.empty())rights.erase(found);}
                return true;});
        }
    }else if(symbol=="LsaEnumerateAccountRights"){
        // (Policy, AccountSid, PLSA_UNICODE_STRING* UserRights, PULONG Count)：只列直接指派給此 SID 的權限；沒有任何權限時回傳 STATUS_OBJECT_NAME_NOT_FOUND。
        if(const auto denied=policy(arg(0),lookup_names))status=*denied;
        else if(!arg(1) || !arg(2) || !arg(3))status=status_invalid_parameter;
        else {
            memory.check(GuestAddress{arg(2)},unit,Access::Write);memory.check(GuestAddress{arg(3)},4,Access::Write);store_pointer(arg(2),0);memory.store(GuestAddress{arg(3)},32,0);
            const auto sid=sid_string(read_sid(memory,arg(1)));std::set<std::string> assigned;
            with_rights(false,[&](Rights& rights){if(const auto found=rights.find(sid);found!=rights.end())assigned=found->second;return false;});
            if(assigned.empty())status=0xc0000034U;
            else {Block block;const auto array=block.reserve(assigned.size()*string_size,unit);std::size_t i=0;for(const auto& right:assigned)string(block,array+(i++)*string_size,utf8_to_utf16(right));
                store_pointer(arg(2),commit(block));memory.store(GuestAddress{arg(3)},32,assigned.size());}
        }
    }else if(symbol=="LsaEnumerateAccountsWithUserRight" || symbol=="LsaEnumerateAccounts"){
        // LsaEnumerateAccountsWithUserRight(Policy, UserRight（NULL 為任一權限）, Buffer**, Count*)；LsaEnumerateAccounts(Policy, EnumerationContext*, Buffer**, PreferedMaximumLength, Count*)。
        // 輸出 LSA_ENUMERATION_INFORMATION 陣列；沒有符合的帳戶時回傳 STATUS_NO_MORE_ENTRIES。
        const bool with_right=symbol=="LsaEnumerateAccountsWithUserRight";const auto buffer=arg(2),count_out=arg(with_right?3:4);
        if(const auto denied=policy(arg(0),with_right?(lookup_names|view_local):view_local))status=*denied;
        else if(!buffer || !count_out || (!with_right && !arg(1)))status=status_invalid_parameter;
        else {
            memory.check(GuestAddress{buffer},unit,Access::Write);memory.check(GuestAddress{count_out},4,Access::Write);store_pointer(buffer,0);memory.store(GuestAddress{count_out},32,0);
            std::optional<std::string> right;if(with_right && arg(1)){right=canonical_right(utf16_to_utf8(read_string(arg(1))));if(!right)throw LsaStatus{status_no_such_privilege};}
            std::vector<std::string> sids;with_rights(false,[&](Rights& rights){for(const auto& [sid,set]:rights)if(!right || set.contains(*right))sids.push_back(sid);return false;});
            // LsaEnumerateAccounts 依列舉內容續傳；本實作一次傳回其餘全部帳戶。
            std::size_t start=0;if(!with_right){start=static_cast<std::size_t>(memory.load(GuestAddress{arg(1)},32));if(start<sids.size())sids.erase(sids.begin(),sids.begin()+static_cast<std::ptrdiff_t>(start));else sids.clear();}
            if(sids.empty())status=status_no_more_entries;
            else {store_pointer(buffer,sid_array(sids));memory.store(GuestAddress{count_out},32,sids.size());if(!with_right)memory.store(GuestAddress{arg(1)},32,start+sids.size());}
        }
    }else if(symbol=="LsaLookupNames" || symbol=="LsaLookupNames2"){
        // LsaLookupNames(Policy, Count, Names, ReferencedDomains**, LSA_TRANSLATED_SID**)；LsaLookupNames2 多一個 Flags 參數並回傳 LSA_TRANSLATED_SID2（含完整 SID）。
        const bool second=symbol.ends_with('2');const unsigned shift=second?1U:0U;const std::uint32_t count=u32(1+shift);const auto names=arg(2+shift),domains_out=arg(3+shift),sids_out=arg(4+shift);
        if(const auto denied=policy(arg(0),lookup_names))status=*denied;
        else if(!domains_out || !sids_out || (count && !names) || count>20480)status=status_invalid_parameter;
        else {
            memory.check(GuestAddress{domains_out},unit,Access::Write);memory.check(GuestAddress{sids_out},unit,Access::Write);store_pointer(domains_out,0);store_pointer(sids_out,0);
            Block block;const std::size_t entry=second?(x86_?16U:24U):12U;const auto array=block.reserve(std::max<std::size_t>(count,1)*entry,unit);std::size_t mapped=0;
            for(std::uint32_t i=0;i<count;++i){
                const auto at=array+i*entry;const auto name=utf16_to_utf8(read_string(names+i*string_size));const auto account=resolve_account(name,primary);
                if(!account){block.set(at,8,4);block.set(at+(second?unit*2:8),0xffffffffU,4);continue;}
                ++mapped;const auto index=domain_of(*account);block.set(at,account->type,4);
                if(second){sid_pointer(block,at+unit,account->sid);block.set(at+unit*2,static_cast<std::uint32_t>(index),4);}
                else{const auto& sid=account->sid;std::uint32_t rid=0xffffffffU;if(account->type!=3 && sid[1]){rid=0;for(unsigned k=0;k<4;++k)rid|=std::uint32_t{sid[sid.size()-4+k]}<<(8*k);}block.set(at+4,rid,4);block.set(at+8,static_cast<std::uint32_t>(index),4);}
            }
            const auto sids=commit(block),list=domain_list();store_pointer(sids_out,sids);store_pointer(domains_out,list);status=mapped_status(mapped,count);
        }
    }else if(symbol=="LsaLookupSids"){
        // (Policy, Count, PSID* Sids, ReferencedDomains**, LSA_TRANSLATED_NAME** Names)：無法對應的 SID 以 SID 字串為名稱、SidTypeUnknown、DomainIndex -1。
        const auto count=u32(1);
        if(const auto denied=policy(arg(0),lookup_names))status=*denied;
        else if(!arg(3) || !arg(4) || (count && !arg(2)) || count>20480)status=status_invalid_parameter;
        else {
            memory.check(GuestAddress{arg(3)},unit,Access::Write);memory.check(GuestAddress{arg(4)},unit,Access::Write);store_pointer(arg(3),0);store_pointer(arg(4),0);
            Block block;const std::size_t entry=x86_?16U:32U;const auto array=block.reserve(std::max<std::size_t>(count,1)*entry,unit);std::size_t mapped=0;
            for(std::uint32_t i=0;i<count;++i){
                const auto at=array+i*entry;const auto sid=read_sid(memory,memory.load(GuestAddress{arg(2)+i*unit},bits));const auto account=resolve_account(sid,primary);
                if(!account){block.set(at,8,4);string(block,at+unit,utf8_to_utf16(sid_string(sid)));block.set(at+unit+string_size,0xffffffffU,4);continue;}
                ++mapped;block.set(at,account->type,4);string(block,at+unit,utf8_to_utf16(std::string(account->name)));block.set(at+unit+string_size,static_cast<std::uint32_t>(domain_of(*account)),4);
            }
            const auto names=commit(block),list=domain_list();store_pointer(arg(4),names);store_pointer(arg(3),list);status=mapped_status(mapped,count);
        }
    }else if(symbol=="LsaLookupPrivilegeName" || symbol=="LsaLookupPrivilegeDisplayName"){
        // LsaLookupPrivilegeName(Policy, PLUID, PLSA_UNICODE_STRING*)；LsaLookupPrivilegeDisplayName(Policy, PLSA_UNICODE_STRING Name, PLSA_UNICODE_STRING*, PSHORT LanguageReturned)。
        const bool display=symbol.ends_with("DisplayName");
        if(const auto denied=policy(arg(0),lookup_names))status=*denied;
        else if(!arg(1) || !arg(2) || (display && !arg(3)))status=status_invalid_parameter;
        else {
            std::size_t index=privilege_names.size();
            if(display){const auto name=utf16_to_utf8(read_string(arg(1)));index=static_cast<std::size_t>(std::find_if(privilege_names.begin(),privilege_names.end(),[&](const auto candidate){return same_name(name,candidate);})-privilege_names.begin());}
            else{const auto luid=memory.load(GuestAddress{arg(1)},64);if(luid>=2 && luid<2+privilege_names.size())index=static_cast<std::size_t>(luid-2);}
            if(index>=privilege_names.size())status=status_no_such_privilege;
            else {Block block;const auto at=block.reserve(string_size,unit);string(block,at,utf8_to_utf16(std::string(display?privilege_display_names[index]:privilege_names[index])));
                memory.check(GuestAddress{arg(2)},unit,Access::Write);store_pointer(arg(2),commit(block));if(display)memory.store(GuestAddress{arg(3)},16,0x409);}
        }
    }else if(symbol=="LsaQueryInformationPolicy"){
        // 稽核事件（2，需 POLICY_VIEW_AUDIT_INFORMATION）、主要網域（3）、帳戶網域（5）、伺服器角色（6）、DNS 網域（12）、本機帳戶網域（14）。
        const auto information=u32(1);const std::uint32_t required=information==2?view_audit:view_local;
        if(const auto denied=policy(arg(0),required))status=*denied;
        else if(!arg(2))status=status_invalid_parameter;
        else {
            memory.check(GuestAddress{arg(2)},unit,Access::Write);Block block;auto machine=primary;if(machine.size()>=12 && machine[1]){--machine[1];machine.resize(machine.size()-4);}
            switch(information){
            case 2:{
                // AuditingMode＋各類別選項（子類別設定的聯集，POLICY_AUDIT_EVENT_SUCCESS 1／FAILURE 2）。
                const auto words=load_audit();const auto at=block.reserve(unit*3,unit);const auto options=block.reserve(9*4,4);block.pointers.push_back({at+unit,options});block.set(at,words[0]?1:0,1);block.set(at+unit*2,9,4);
                for(std::size_t i=0;i<audit_subcategories.size();++i){const auto slot=options+audit_subcategories[i].second*4U;block.set(slot,(block.bytes[slot]|words[i+1])&3U,4);}
                break;}
            case 3:{const auto at=block.reserve(string_size+unit,unit);string(block,at,text_value(u"polprdmn",u"WORKGROUP"));const auto sid=binary_value(u"polprdms",{});sid_pointer(block,at+string_size,sid);break;}
            case 5:{const auto at=block.reserve(string_size+unit,unit);string(block,at,text_value(u"polacdmn",u"X64ARM"));const auto sid=binary_value(u"polacdms",machine);sid_pointer(block,at+string_size,sid);break;}
            case 14:{const auto at=block.reserve(string_size+unit,unit);string(block,at,u"X64ARM");sid_pointer(block,at+string_size,machine);break;}
            case 6:{const auto at=block.reserve(4,4);block.set(at,3,4);break;}
            case 12:{const auto at=block.reserve(string_size*3+16+unit,unit);string(block,at,text_value(u"poldnddn",u"WORKGROUP"));string(block,at+string_size,text_value(u"poldndmn",u""));
                string(block,at+string_size*2,text_value(u"poldntrn",u""));const auto guid=binary_value(u"poldndmg",{});if(guid.size()==16)std::copy(guid.begin(),guid.end(),block.bytes.begin()+static_cast<std::ptrdiff_t>(at+string_size*3));
                const auto sid=binary_value(u"poldndms",{});sid_pointer(block,at+string_size*3+16,sid);break;}
            default:status=information>=1 && information<=16?status_invalid_parameter:status_invalid_info_class;break;}
            if(status==status_success)store_pointer(arg(2),commit(block));
        }
    }else if(symbol=="LsaSetInformationPolicy"){
        // (PolicyHandle, InformationClass, Buffer)：稽核事件（2，需 POLICY_SET_AUDIT_REQUIREMENTS）、主要網域（3）、帳戶網域（5）、DNS 網域（12）（需 POLICY_TRUST_ADMIN）。
        // 舊式稽核選項套用到該類別的全部子類別：POLICY_AUDIT_EVENT_NONE（4）清除，SUCCESS／FAILURE 設定，0 不變。寫入後觸發對應的原則變更通知。
        const auto information=u32(1);const std::uint32_t required=information==2?0x100U:0x8U;
        if(information!=2 && information!=3 && information!=5 && information!=12)status=information>=1 && information<=16?status_invalid_parameter:status_invalid_info_class;
        else if(const auto denied=policy(arg(0),required))status=*denied;
        else if(!arg(2))status=status_invalid_parameter;
        else if(information==2){
            const auto mode=memory.load(GuestAddress{arg(2)},8),options=memory.load(GuestAddress{arg(2)+unit},bits),count=memory.load(GuestAddress{arg(2)+unit*2},32);
            if(count>9 || (count && !options))status=status_invalid_parameter;
            else {auto words=load_audit();words[0]=mode?1U:0U;
                for(std::uint32_t category=0;category<count;++category){const auto value=static_cast<std::uint32_t>(memory.load(GuestAddress{options+category*4},32));if(!value)continue;
                    for(std::size_t i=0;i<audit_subcategories.size();++i)if(audit_subcategories[i].second==category)words[i+1]=(value&4U)?0U:(value&3U);}
                save_audit(words);notify_policy(1);}
        }else {
            const auto sid_at=[&](std::uint64_t pointer)->security::Bytes{if(!pointer)return {};const auto count=memory.load(GuestAddress{pointer+1},8);if(memory.load(GuestAddress{pointer},8)!=1 || count>15)throw SecurityDescriptorError{87};
                security::Bytes sid(8+count*4);memory.read(GuestAddress{pointer},sid);return sid;};
            if(information==3)write_policy({{u"polprdmn",sz(u"PolPrDmN",read_string(arg(2)))},{u"polprdms",binary(u"PolPrDmS",sid_at(memory.load(GuestAddress{arg(2)+string_size},bits)))}});
            else if(information==5){const auto name=read_string(arg(2));if(name.empty())throw LsaStatus{status_invalid_parameter};
                write_policy({{u"polacdmn",sz(u"PolAcDmN",name)},{u"polacdms",binary(u"PolAcDmS",sid_at(memory.load(GuestAddress{arg(2)+string_size},bits)))}});}
            else {security::Bytes guid(16);memory.read(GuestAddress{arg(2)+string_size*3},guid);
                write_policy({{u"poldnddn",sz(u"PolDnDDN",read_string(arg(2)))},{u"poldndmn",sz(u"PolDnDmN",read_string(arg(2)+string_size))},{u"poldntrn",sz(u"PolDnTrN",read_string(arg(2)+string_size*2))},
                    {u"poldndmg",binary(u"PolDnDmG",guid)},{u"poldndms",binary(u"PolDnDmS",sid_at(memory.load(GuestAddress{arg(2)+string_size*3+16},bits)))}});}
            notify_policy(information==5?2U:4U);
        }
    }else if(symbol=="AuditQuerySystemPolicy" || symbol=="AuditSetSystemPolicy"){
        // AuditQuerySystemPolicy(pSubCategoryGuids, dwPolicyCount, ppAuditPolicy)／AuditSetSystemPolicy(pAuditPolicy, dwPolicyCount)：需啟用 SeSecurityPrivilege。
        // AUDIT_POLICY_INFORMATION {子類別 GUID, AuditingInformation, 類別 GUID}（36 位元組）；設定時 NONE（4）清除、0 不變、SUCCESS／FAILURE 設定。輸出以 AuditFree 釋放。
        const bool query=symbol=="AuditQuerySystemPolicy";const auto count=u32(1);
        const auto index_of=[&](std::span<const std::uint8_t> id)->std::size_t{for(std::size_t i=0;i<audit_subcategories.size();++i){const auto expected=audit_guid(0x0cce0000U|audit_subcategories[i].first,0x69ae);
            if(std::equal(expected.begin(),expected.end(),id.begin()))return i;}return audit_subcategories.size();};
        if(!runtime_.token_objects.at(effective_token_id()).has_privilege(8,true))audit_error=1314;
        else if(!arg(0) || !count || (query && !arg(2)))audit_error=87;
        else {
            auto words=load_audit();const unsigned stride=query?16U:36U;std::vector<std::size_t> indices;
            for(std::uint32_t i=0;i<count && !audit_error;++i){std::array<std::uint8_t,16> id{};memory.read(GuestAddress{arg(0)+i*stride},id);const auto index=index_of(id);if(index>=audit_subcategories.size())audit_error=87;else indices.push_back(index);}
            if(!audit_error && query){
                Block block;const auto at=block.reserve(36*indices.size(),4);
                for(std::size_t i=0;i<indices.size();++i){const auto index=indices[i];const auto sub=audit_guid(0x0cce0000U|audit_subcategories[index].first,0x69ae),category=audit_guid(0x69979848U+audit_subcategories[index].second,0x797a);
                    std::copy(sub.begin(),sub.end(),block.bytes.begin()+static_cast<std::ptrdiff_t>(at+i*36));block.set(at+i*36+16,words[index+1],4);std::copy(category.begin(),category.end(),block.bytes.begin()+static_cast<std::ptrdiff_t>(at+i*36+20));}
                memory.check(GuestAddress{arg(2)},unit,Access::Write);store_pointer(arg(2),commit(block));
            }else if(!audit_error){
                for(std::uint32_t i=0;i<count;++i){const auto value=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)+i*36+16},32));if(value&~7U){audit_error=87;break;}if(value)words[indices[i]+1]=(value&4U)?0U:(value&3U);}
                if(!audit_error){save_audit(words);notify_policy(1);}
            }
        }
    }else if(symbol=="AuditFree"){if(arg(0))(void)free_heap(memory,arg(0),state);
    }else if(symbol=="LsaGetUserName"){
        // (PUNICODE_STRING* UserName, PUNICODE_STRING* DomainName)：目前有效權杖的使用者。
        if(!arg(0))status=status_invalid_parameter;
        else {
            const auto& sid=runtime_.token_objects.at(effective_token_id()).user.sid;const auto account=resolve_account(sid,primary);
            const auto user=account?utf8_to_utf16(std::string(account->name)):utf8_to_utf16(sid_string(sid)),domain=account?utf8_to_utf16(std::string(account->domain)):std::u16string{};
            const auto one=[&](std::u16string_view text){Block block;const auto at=block.reserve(string_size,unit);string(block,at,text);return commit(block);};
            memory.check(GuestAddress{arg(0)},unit,Access::Write);if(arg(1))memory.check(GuestAddress{arg(1)},unit,Access::Write);
            store_pointer(arg(0),one(user));if(arg(1))store_pointer(arg(1),one(domain));
        }
    }else if(symbol=="LsaEnumerateTrustedDomains" || symbol=="LsaEnumerateTrustedDomainsEx"){
        // (Policy, EnumerationContext*, Buffer**, PreferedMaximumLength, CountReturned*)：工作群組電腦沒有信任網域。
        if(const auto denied=policy(arg(0),view_local))status=*denied;
        else if(!arg(1) || !arg(2) || !arg(4))status=status_invalid_parameter;
        else {store_pointer(arg(2),0);memory.store(GuestAddress{arg(4)},32,0);status=status_no_more_entries;}
    }else if(symbol=="LsaRegisterPolicyChangeNotification" || symbol=="LsaUnregisterPolicyChangeNotification"){
        // (InformationClass 1～8, NotificationEventHandle)：本程序登記；原則變更由 LsaSetInformationPolicy 等寫入觸發。
        const auto information=u32(0);const auto key=std::pair<std::uint32_t,std::uint64_t>{information,arg(1)};
        if(information<1 || information>8)status=status_invalid_parameter;
        else if(symbol.starts_with("LsaRegister")){const auto event=runtime_.wait_objects.find(arg(1));
            if(event==runtime_.wait_objects.end() || event->second.kind!=WaitObject::Kind::Event)status=status_invalid_handle;else lsa_notifications_.insert(key);}
        else status=lsa_notifications_.erase(key)?status_success:status_invalid_handle;
    }else if(access_audit_service(symbol)){
#include "windows_access_audit.inc"
    }else status=status_not_implemented;
    }catch(const LsaStatus& failure){status=failure.code;}catch(const SecurityDescriptorError&){status=status_invalid_parameter;}catch(const MemoryFault&){status=0xc0000005U;}catch(const std::bad_alloc&){status=status_no_memory;}
    // Audit* 回傳 BOOLEAN 並設定 LastError；例外對應：記憶體存取錯誤 998、記憶體不足 8、參數錯誤 87、其他 1。
    if(symbol.starts_with("Audit")){
        if(status!=status_success && !audit_error)audit_error=status==0xc0000005U?998U:status==status_no_memory?8U:status==status_invalid_parameter?87U:1U;
        if(audit_error)last_error_=audit_error;result=symbol=="AuditFree"?0U:audit_error?0U:1U;}
    else if(access_audit_service(symbol) && !symbol.starts_with("Nt") && !symbol.starts_with("Zw")){
        // 存取檢查／稽核警示的 Win32 版回 BOOL：失敗時 LastError 為 NTSTATUS 對應的 Win32 錯誤。
        if(status){last_error_=static_cast<std::uint32_t>(nested_service(state,memory,"ntdll.dll","RtlNtStatusToDosError",{status}));result=0;}
        else{if(access_audit_error)last_error_=access_audit_error;result=1;}}
    else result=status;
}
