// 依型別存取檢查與稽核警示（由 windows_lsa.inc 的分支引入，status 為 NTSTATUS；Win32 版由 LSA 區塊尾端轉成 BOOL 與 LastError）。
// - AccessCheckByTypeResultList、NtAccessCheckByType(ResultList)：依物件型別樹逐節點裁決（security::access_check_list）。
// - AccessCheck*AndAuditAlarm（含 ByType、ResultList、ByHandle 與 Nt 版）：產生稽核需要呼叫端「主要權杖」已啟用 SeAuditPrivilege；
//   ByType 系列的 AUDIT_ALLOW_NO_PRIVILEGE 在沒有權限時只做存取檢查不產生稽核。非 ByHandle 版以執行緒的模擬權杖為用戶端，
//   沒有模擬回 STATUS_NO_IMPERSONATION_TOKEN；DesiredAccess 的泛用權限依 GenericMapping 展開。
// - 產生稽核的條件：系統稽核原則（AuditSetSystemPolicy 的「其他物件存取事件」、「控制代碼操作」、「敏感權限使用」子類別）啟用對應的
//   成功／失敗，且（物件存取時）SACL 有符合用戶端 SID、成功／失敗旗標與存取遮罩的稽核 ACE；事件寫入 Security 記錄
//   （4656 要求控制代碼、4658 關閉、4660 刪除、4673 呼叫特殊權限服務、4674 對特殊權限物件的操作）。
//   GenerateOnClose 在產生成功稽核時為 TRUE，呼叫端關閉時應呼叫 ObjectCloseAuditAlarm。
{
    const bool nt=symbol.starts_with("Nt") || symbol.starts_with("Zw");
    std::string api(nt?symbol.substr(2):symbol);
    const bool ansi=!nt && api.ends_with('A') && api.find("AuditAlarm")!=std::string::npos;
    if(!nt && api.find("AuditAlarm")!=std::string::npos && (api.ends_with('A') || api.ends_with('W')))api.pop_back();
    if(nt){for(const auto& [from,to]:std::initializer_list<std::pair<std::string_view,std::string_view>>{{"OpenObjectAuditAlarm","ObjectOpenAuditAlarm"},
        {"CloseObjectAuditAlarm","ObjectCloseAuditAlarm"},{"DeleteObjectAuditAlarm","ObjectDeleteAuditAlarm"},{"PrivilegeObjectAuditAlarm","ObjectPrivilegeAuditAlarm"}})if(api==from)api=to;}
    constexpr std::uint32_t status_privilege_not_held=0xc0000061U,status_no_impersonation=0xc000005cU,status_bad_impersonation=0xc00000a5U,
        status_invalid_descriptor=0xc0000079U,status_generic_not_mapped=0xc000008eU,status_buffer_small=0xc0000023U,status_invalid_sid=0xc0000078U;
    const auto text=[&](std::uint64_t address)->std::u16string{
        if(!address)return {};
        if(nt)return read_string(address);
        return ansi?ansi_to_utf16(read_ansi(memory,address,0x10000)):read_utf16(memory,GuestAddress{address},0x8000);};
    const auto required_text=[&](std::uint64_t address){if(!address)throw LsaStatus{status_invalid_parameter};return text(address);};
    const auto audit_privilege=[&]{return runtime_.token_objects.at(1).has_privilege(21,true);};
    const auto token_handle=[&](std::uint64_t handle)->const VirtualToken&{
        try{return token_handle_object(handle,8);}catch(const SecurityDescriptorError& error){throw LsaStatus{error.code==5?status_access_denied:error.code==6?status_invalid_handle:status_invalid_parameter};}};
    const auto client=[&](const VirtualToken& token)->const VirtualToken&{
        if(token.type!=2)throw LsaStatus{status_no_impersonation};if(token.level<1)throw LsaStatus{status_bad_impersonation};return token;};
    const auto impersonated=[&]()->const VirtualToken&{
        const auto id=threads_.items.at(threads_.current).impersonation_token;if(!id)throw LsaStatus{status_no_impersonation};
        return client(runtime_.token_objects.at(id));};
    const auto descriptor=[&](std::uint64_t address){
        if(!address)throw LsaStatus{status_invalid_descriptor};
        auto sd=security::read_descriptor(memory,address,x86_,true);if(sd.data[0].empty() || sd.data[1].empty())throw LsaStatus{status_invalid_descriptor};return sd;};
    const auto object_types=[&](std::uint64_t address,std::uint32_t count){
        std::vector<security::ObjectTypeNode> types;const unsigned stride=x86_?8U:16U;
        if(count>4096 || (count && !address))throw LsaStatus{status_invalid_parameter};
        for(std::uint32_t i=0;i<count;++i){
            const auto at=address+std::uint64_t{i}*stride;security::ObjectTypeNode node;node.level=static_cast<std::uint16_t>(memory.load(GuestAddress{at},16));
            const auto guid=memory.load(GuestAddress{at+unit},bits);if(!guid)throw LsaStatus{status_invalid_parameter};memory.read(GuestAddress{guid},std::span(node.type));
            if(node.level>4 || (i==0)!=(node.level==0) || (i && node.level>types.back().level+1))throw LsaStatus{status_invalid_parameter};
            types.push_back(node);
        }
        return types;};
    const auto self_sid=[&](std::uint64_t address){std::vector<std::uint8_t> sid;if(address){try{sid=read_sid(memory,address);}catch(const SecurityDescriptorError&){throw LsaStatus{status_invalid_sid};}}return sid;};
    const auto hex=[](std::uint64_t value){char buffer[24];std::snprintf(buffer,sizeof buffer,"0x%llx",static_cast<unsigned long long>(value));return utf8_to_utf16(buffer);};
    // 系統稽核原則：子類別 {0CCE9xxx-69AE-11D9-...} 的成功（1）／失敗（2）位元。
    const auto policy_enabled=[&](std::uint16_t subcategory,bool success){
        const auto words=load_audit();
        for(std::size_t i=0;i<audit_subcategories.size();++i)if(audit_subcategories[i].first==subcategory)return (words[i+1]&(success?1U:2U))!=0;
        return false;};
    const auto sacl_matches=[&](const security::SecurityDescriptor& sd,const VirtualToken& token,std::uint32_t mask,bool success,const std::array<std::uint32_t,4>& mapping){
        if(!(sd.control&0x10U) || sd.data[2].size()<8)return false;
        const auto acl=security::parse_acl(sd.data[2]);
        for(std::size_t i=0;i<acl.offsets.size();++i){
            const auto at=acl.offsets[i],end=i+1<acl.offsets.size()?acl.offsets[i+1]:acl.used;const auto bytes=std::span<const std::uint8_t>(acl.bytes).subspan(at,end-at);
            if(bytes.size()<8 || (bytes[0]!=2 && bytes[0]!=7) || (bytes[1]&8U) || !(bytes[1]&(success?0x40U:0x80U)))continue;
            const auto ace=bytes[0]==2?security::basic_ace(bytes):security::read_ace(bytes);
            if(security::token_sid_matches(token,ace.sid,false) && (security::mapped_access(ace.mask,mapping)&mask))return true;
        }
        return false;};
    const auto privilege_list=[&](std::uint64_t address){
        std::vector<std::uint64_t> luids;if(!address)return luids;
        const auto count=memory.load(GuestAddress{address},32);if(count>64)throw LsaStatus{status_invalid_parameter};
        for(std::uint64_t i=0;i<count;++i)luids.push_back(memory.load(GuestAddress{address+8+i*12},64));
        return luids;};
    const auto privilege_text=[&](const std::vector<std::uint64_t>& luids){
        std::u16string out;for(const auto luid:luids){if(!out.empty())out+=u" ";out+=luid>=2 && luid<2+privilege_names.size()?utf8_to_utf16(std::string(privilege_names[luid-2])):hex(luid);}return out;};
    const auto user_sid_text=[&](const VirtualToken& token){return utf8_to_utf16(sid_string(token.user.sid));};
    const auto store_bool=[&](std::uint64_t address,bool value){if(!address)throw LsaStatus{status_invalid_parameter};memory.store(GuestAddress{address},nt?8:32,value?1U:0U);};
    // 依型別（含 ResultList）存取檢查的共用輸出：Win32 單筆 AccessStatus 為 BOOL、清單為 Win32 錯誤碼；Nt 版為 NTSTATUS。
    const auto write_results=[&](const std::vector<security::AccessCheckResult>& results,bool list,std::uint64_t granted_out,std::uint64_t status_out,bool win32_bool){
        if(!granted_out || !status_out)throw LsaStatus{status_invalid_parameter};
        const std::size_t count=list?results.size():1;
        memory.check(GuestAddress{granted_out},count*4,Access::Write);memory.check(GuestAddress{status_out},count*4,Access::Write);
        for(std::size_t i=0;i<count;++i){
            const auto& item=results[i];memory.store(GuestAddress{granted_out+i*4},32,item.granted);
            const std::uint32_t code=item.error==0?0U:item.error==1314?(nt?status_privilege_not_held:1314U):(nt?status_access_denied:5U);
            memory.store(GuestAddress{status_out+i*4},32,win32_bool?(item.error?0U:1U):code);
        }};
    if(api=="AccessCheckByType" || api=="AccessCheckByTypeResultList"){
        // (SecurityDescriptor, PrincipalSelfSid, ClientToken, DesiredAccess, ObjectTypeList, ObjectTypeListLength, GenericMapping, PrivilegeSet, PrivilegeSetLength*, GrantedAccess, AccessStatus)
        const bool list=api=="AccessCheckByTypeResultList";
        const auto& token=client(token_handle(arg(2)));const auto desired=u32(3);if(desired&0xf0000000U)throw LsaStatus{status_generic_not_mapped};
        const auto mapping=security::read_mapping(memory,arg(6));const auto types=object_types(arg(4),u32(5));const auto self=self_sid(arg(1));
        const auto sd=descriptor(arg(0));
        if(!arg(8))throw LsaStatus{status_invalid_parameter};
        const auto results=security::access_check_list(token,sd,desired,mapping,types,self);
        const auto& privileges=results.front().privileges;const auto needed=static_cast<std::uint32_t>(8+privileges.size()*12);const auto size=memory.load(GuestAddress{arg(8)},32);
        memory.store(GuestAddress{arg(8)},32,needed);
        if(size<needed || !arg(7))throw LsaStatus{nt?status_buffer_small:0xc0000023U};
        memory.check(GuestAddress{arg(7)},needed,Access::Write);memory.store(GuestAddress{arg(7)},32,privileges.size());memory.store(GuestAddress{arg(7)+4},32,0);
        for(std::size_t i=0;i<privileges.size();++i){memory.store(GuestAddress{arg(7)+8+i*12},64,privileges[i].luid);memory.store(GuestAddress{arg(7)+16+i*12},32,privileges[i].attributes);}
        write_results(results,list,arg(9),arg(10),false);
        status=0;
    }else if(api=="AccessCheckAndAuditAlarm" || api=="AccessCheckByTypeAndAuditAlarm" || api=="AccessCheckByTypeResultListAndAuditAlarm" || api=="AccessCheckByTypeResultListAndAuditAlarmByHandle"){
        const bool by_type=api!="AccessCheckAndAuditAlarm",list=api.find("ResultList")!=std::string::npos,by_handle=api.ends_with("ByHandle");
        // 引數位置：ByHandle 在 HandleId 後多一個 ClientToken；ByType 在 SecurityDescriptor 後多 PrincipalSelfSid，DesiredAccess 後多 AuditType、Flags、ObjectTypeList、長度。
        const unsigned h=by_handle?1U:0U;
        const unsigned type_name=2+h,object_name=3+h,sd_at=4+h,self_at=5+h,desired_at=by_type?6+h:5,audit_type=7+h,flags_at=8+h,list_at=9+h,length_at=10+h,
            mapping_at=by_type?11+h:6,creation_at=by_type?12+h:7,granted_at=by_type?13+h:8,status_at=by_type?14+h:9,close_at=by_type?15+h:10;
        const auto subsystem=required_text(arg(0));
        const auto flags=by_type?u32(flags_at):0U;
        if(by_type && (u32(audit_type)>1 || (flags&~1U)))throw LsaStatus{status_invalid_parameter};
        const bool privileged=audit_privilege();
        if(!privileged && !(flags&1U))throw LsaStatus{status_privilege_not_held};
        const auto& token=by_handle?client(token_handle(arg(2))):impersonated();
        const auto mapping=security::read_mapping(memory,arg(mapping_at));
        const auto desired=security::mapped_access(u32(desired_at),mapping);
        const auto types=by_type?object_types(arg(list_at),u32(length_at)):std::vector<security::ObjectTypeNode>{};
        const auto self=by_type?self_sid(arg(self_at)):std::vector<std::uint8_t>{};
        const auto sd=descriptor(arg(sd_at));
        const auto results=list?security::access_check_list(token,sd,desired,mapping,types,self):std::vector<security::AccessCheckResult>{security::access_check(token,sd,desired,mapping,types,self)};
        const bool granted=std::all_of(results.begin(),results.end(),[](const auto& item){return !item.error;});
        bool generated=false;
        if(privileged){
            const auto mask=granted?results.front().granted:desired;
            if(policy_enabled(0x9227,granted) && sacl_matches(sd,token,mask,granted,mapping)){
                const std::u16string values[]{user_sid_text(token),subsystem,text(arg(type_name)),text(arg(object_name)),hex(arg(1)),hex(desired),hex(results.front().granted),
                    (u32(creation_at)&0xffU)?u"1":u"0"};
                (void)security_audit_event(4656,granted,12804,values,token.user.sid);generated=granted;
            }
        }
        write_results(results,list,arg(granted_at),arg(status_at),!nt && !list);
        store_bool(arg(close_at),generated);
        if(!list)access_audit_error=results.front().error; // 與 AccessCheck 相同：拒絕時函式成功、AccessStatus 為 FALSE 並設定 LastError。
        status=0;
    }else if(api=="ObjectOpenAuditAlarm"){
        // (SubsystemName, HandleId, ObjectTypeName, ObjectName, SecurityDescriptor, ClientToken, DesiredAccess, GrantedAccess, Privileges, ObjectCreation, AccessGranted, GenerateOnClose*)
        const auto subsystem=required_text(arg(0));
        if(!audit_privilege())throw LsaStatus{status_privilege_not_held};
        const auto& token=token_handle(arg(5));
        const bool granted=(u32(10)&0xffU)!=0;bool generated=false;
        if(arg(4)){
            const auto sd=descriptor(arg(4));const std::array<std::uint32_t,4> identity{0,0,0,0};
            if(policy_enabled(0x9227,granted) && sacl_matches(sd,token,granted?u32(7):u32(6),granted,identity)){
                const std::u16string values[]{user_sid_text(token),subsystem,text(arg(2)),text(arg(3)),hex(arg(1)),hex(u32(6)),hex(u32(7)),privilege_text(privilege_list(arg(8))),(u32(9)&0xffU)?u"1":u"0"};
                (void)security_audit_event(4656,granted,12804,values,token.user.sid);generated=granted;
            }
        }
        store_bool(arg(11),generated);
        status=0;
    }else if(api=="ObjectCloseAuditAlarm" || api=="ObjectDeleteAuditAlarm"){
        // (SubsystemName, HandleId, GenerateOnClose)：開啟時產生過稽核才寫入關閉／刪除事件。
        const auto subsystem=required_text(arg(0));
        if(!audit_privilege())throw LsaStatus{status_privilege_not_held};
        if((u32(2)&0xffU) && policy_enabled(0x9223,true)){
            const auto& token=runtime_.token_objects.at(effective_token_id());
            const std::u16string values[]{user_sid_text(token),subsystem,hex(arg(1))};
            (void)security_audit_event(api=="ObjectCloseAuditAlarm"?4658:4660,true,12807,values,token.user.sid);
        }
        status=0;
    }else if(api=="ObjectPrivilegeAuditAlarm" || api=="PrivilegedServiceAuditAlarm"){
        // ObjectPrivilegeAuditAlarm(SubsystemName, HandleId, ClientToken, DesiredAccess, Privileges, AccessGranted)
        // PrivilegedServiceAuditAlarm(SubsystemName, ServiceName, ClientToken, Privileges, AccessGranted)
        const bool privileged_service=api=="PrivilegedServiceAuditAlarm";
        const auto subsystem=required_text(arg(0));const auto service_name=privileged_service?required_text(arg(1)):std::u16string{};
        if(!audit_privilege())throw LsaStatus{status_privilege_not_held};
        const auto& token=token_handle(arg(2));const auto privileges=privilege_list(arg(privileged_service?3:4));
        if(privileges.empty())throw LsaStatus{status_invalid_parameter};
        const bool granted=(u32(privileged_service?4:5)&0xffU)!=0;
        if(policy_enabled(0x9228,granted)){
            const std::u16string values[]{user_sid_text(token),subsystem,privileged_service?service_name:hex(arg(1)),privileged_service?privilege_text(privileges):hex(u32(3)),privileged_service?u"":privilege_text(privileges)};
            (void)security_audit_event(privileged_service?4673:4674,granted,13056,std::span(values).first(privileged_service?4:5),token.user.sid);
        }
        status=0;
    }else status=status_not_implemented;
}
