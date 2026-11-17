// Safer（軟體限制原則）層級：由 windows_security.cpp 的 invoke_token 引入，沿用權杖查詢、新 handle 與 CreateRestrictedToken 的限制規則。
// 層級 handle 只在本程序有效。原則值讀取沙箱登錄 HKLM\SOFTWARE\Policies\Microsoft\Windows\Safer\CodeIdentifiers（DefaultLevel、TransparentEnabled、
// PolicyScope、AuthenticodeEnabled、ExecutableTypes），沒有設定時為 Windows 預設（Unrestricted、啟用、所有使用者、不檢查 Authenticode、內建檔案類型）。
// SaferIdentifyLevel 只套用預設層級，尚未評估路徑／雜湊／憑證／區域規則。
{
    constexpr std::uint32_t disallowed=0,untrusted=0x1000,constrained=0x10000,normal=0x20000,fully_trusted=0x40000;
    const auto valid_level=[](std::uint32_t level){return level==disallowed || level==untrusted || level==constrained || level==normal || level==fully_trusted;};
    // 值名稱以摺疊後的小寫查詢（登錄值的鍵）。
    const auto policy_dword=[&](std::u16string_view name,std::uint32_t fallback)->std::uint32_t{
        if(!registry_file_.empty() && !runtime_.network.external_io)return fallback;
        RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)return fallback;
        const auto key=runtime_.registry.keys.find(std::u16string(1,2)+u"\\software\\policies\\microsoft\\windows\\safer\\codeidentifiers");if(key==runtime_.registry.keys.end())return fallback;
        const auto value=key->second.find(std::u16string(name));if(value==key->second.end() || value->second.type!=4 || value->second.bytes.size()!=4)return fallback;
        return static_cast<std::uint32_t>(value->second.bytes[0])|static_cast<std::uint32_t>(value->second.bytes[1])<<8|static_cast<std::uint32_t>(value->second.bytes[2])<<16|static_cast<std::uint32_t>(value->second.bytes[3])<<24;
    };
    const auto level_of=[&](std::uint64_t handle)->SaferLevel&{const auto found=safer_levels_.find(handle);if(found==safer_levels_.end())throw SddlError{6};return found->second;};
    const auto new_level=[&](std::uint32_t scope,std::uint32_t level,std::uint64_t output){
        if(!output)throw SddlError{87};memory.check(GuestAddress{output},unit,Access::Write);if(safer_levels_.size()>=1024)throw SddlError{8};
        const auto handle=next_safer_level_++;safer_levels_.emplace(handle,SaferLevel{scope,level,{},{}});memory.store(GuestAddress{output},bits,handle);result=1;
    };
    const auto write_output=[&](std::span<const std::uint8_t> bytes,std::uint64_t buffer,std::uint32_t capacity,std::uint64_t returned){
        if(returned){memory.check(GuestAddress{returned},4,Access::Write);memory.store(GuestAddress{returned},32,bytes.size());}
        if(capacity<bytes.size() || !buffer)throw SddlError{122};memory.write(GuestAddress{buffer},bytes);result=1;
    };
    const auto dword_bytes=[](std::uint32_t value){return std::vector<std::uint8_t>{static_cast<std::uint8_t>(value),static_cast<std::uint8_t>(value>>8),static_cast<std::uint8_t>(value>>16),static_cast<std::uint8_t>(value>>24)};};
    if(symbol=="SaferCreateLevel"){
        // (dwScopeId, dwLevelId, OpenFlags, pLevelHandle, lpReserved)
        if((u32(0)!=1 && u32(0)!=2) || !valid_level(u32(1)) || (u32(2)&~1U) || arg(4))throw SddlError{87};new_level(u32(0),u32(1),arg(3));
    }else if(symbol=="SaferCloseLevel"){
        (void)level_of(arg(0));safer_levels_.erase(arg(0));result=1;
    }else if(symbol=="SaferIdentifyLevel"){
        // (dwNumProperties, pCodeProperties, pLevelHandle, lpReserved)：SAFER_CODE_PROPERTIES 至少要有 V1 的大小。
        if(arg(3) || (u32(0) && !arg(1)) || u32(0)>256)throw SddlError{87};const std::uint32_t minimum=x86_?112U:136U;
        for(std::uint32_t i=0,offset=0;i<u32(0);++i){const auto size=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)+offset},32));if(size<minimum)throw SddlError{87};offset+=size;}
        auto level=policy_dword(u"defaultlevel",fully_trusted);if(!valid_level(level))level=fully_trusted;new_level(1,level,arg(2));
    }else if(symbol=="SaferGetPolicyInformation"){
        // (dwScopeId, SaferPolicyInfoClass, InfoBufferSize, InfoBuffer, InfoBufferRetSize, lpReserved)
        if((u32(0)!=1 && u32(0)!=2) || arg(5))throw SddlError{87};std::vector<std::uint8_t> bytes;
        switch(u32(1)){
        case 1:for(const auto level:{disallowed,untrusted,constrained,normal,fully_trusted}){const auto item=dword_bytes(level);bytes.insert(bytes.end(),item.begin(),item.end());}break;
        case 2:bytes=dword_bytes(policy_dword(u"transparentenabled",1));break;
        case 3:{auto level=policy_dword(u"defaultlevel",fully_trusted);bytes=dword_bytes(valid_level(level)?level:fully_trusted);break;}
        case 4:bytes=dword_bytes(policy_dword(u"policyscope",0)==0?1U:0U);break;
        case 5:bytes=dword_bytes(policy_dword(u"policyscope",0));break;
        case 6:bytes=dword_bytes(0);break;
        case 7:bytes=dword_bytes(policy_dword(u"authenticodeenabled",0));break;
        default:throw SddlError{87};
        }
        write_output(bytes,arg(3),u32(2),arg(4));
    }else if(symbol=="SaferGetLevelInformation" || symbol=="SaferSetLevelInformation"){
        // Get(LevelHandle, dwInfoType, lpQueryBuffer, dwInBufferSize, lpdwOutBufferSize)／Set(LevelHandle, dwInfoType, lpQueryBuffer, dwInBufferSize)
        auto& item=level_of(arg(0));const auto type=u32(1);
        const auto default_name=[&]{switch(item.level){case disallowed:return u"Disallowed";case untrusted:return u"Untrusted";case constrained:return u"Restricted";case normal:return u"Basic User";default:return u"Unrestricted";}};
        const auto default_description=[&]{switch(item.level){
            case disallowed:return u"Software will not run.";case untrusted:return u"Software runs with access only to resources granted to Everyone.";
            case constrained:return u"Software runs with access only to resources granted to well-known groups.";case normal:return u"Software runs without administrator rights.";
            default:return u"Software runs with the access rights of the user.";}};
        if(symbol=="SaferSetLevelInformation"){
            if(type!=3 && type!=4)throw SddlError{87};if(!arg(2) || u32(3)<2 || u32(3)%2 || u32(3)>2048)throw SddlError{87};
            std::u16string text=read_utf16(memory,GuestAddress{arg(2)},u32(3)/2);(type==3?item.name:item.description)=std::move(text);result=1;
        }else{
            std::vector<std::uint8_t> bytes;
            switch(type){
            case 1:bytes=dword_bytes(item.level);break;case 2:bytes=dword_bytes(item.scope);break;
            case 3:bytes=utf16_bytes(item.name.empty()?std::u16string(default_name()):item.name);break;
            case 4:bytes=utf16_bytes(item.description.empty()?std::u16string(default_description()):item.description);break;
            case 5:bytes=dword_bytes(1);break;case 6:bytes=dword_bytes(item.level==disallowed?1U:0U);break;
            case 7:bytes=dword_bytes(item.level!=fully_trusted && item.level!=disallowed?1U:0U);break;case 16:bytes=dword_bytes(0);break;
            default:throw SddlError{87};
            }
            write_output(bytes,arg(2),u32(3),arg(4));
        }
    }else if(symbol=="SaferiIsExecutableFileType"){
        // (szFullPathname, bFromShellExecute)：只看副檔名、不分大小寫；bFromShellExecute 為 TRUE 時不把 .exe 視為執行檔類型。
        const auto path=arg(0)?read_utf16(memory,GuestAddress{arg(0)},32768):std::u16string{};
        const auto dot=path.find_last_of(u'.'),slash=path.find_last_of(u"\\/");
        if(path.empty() || dot==path.npos || (slash!=path.npos && dot<slash) || dot+1==path.size())throw SddlError{87};
        std::u16string extension=path.substr(dot+1);for(auto& c:extension)c=unicode_case_unit(c,true);
        std::vector<std::u16string> types;
        if(registry_file_.empty() || runtime_.network.external_io){
            RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);
            if(!transaction.error)if(const auto key=runtime_.registry.keys.find(std::u16string(1,2)+u"\\software\\policies\\microsoft\\windows\\safer\\codeidentifiers");key!=runtime_.registry.keys.end())
                if(const auto value=key->second.find(u"executabletypes");value!=key->second.end() && value->second.type==7){
                    std::u16string current;const auto& raw=value->second.bytes;
                    for(std::size_t i=0;i+1<raw.size();i+=2){const auto c=static_cast<char16_t>(raw[i]|(raw[i+1]<<8));if(c){current+=unicode_case_unit(c,true);continue;}if(current.empty())break;types.push_back(std::move(current));current.clear();}
                }
        }
        if(types.empty())for(const auto* name:{u"ADE",u"ADP",u"BAS",u"BAT",u"CHM",u"CMD",u"COM",u"CPL",u"CRT",u"EXE",u"HLP",u"HTA",u"INF",u"INS",u"ISP",u"JS",u"JSE",u"LNK",u"MDB",u"MDE",u"MSC",u"MSI",u"MSP",u"MST",
            u"OCX",u"PCD",u"PIF",u"PL",u"REG",u"SCR",u"SHS",u"URL",u"VB",u"VBE",u"VBS",u"WSC",u"WSF",u"WSH"})types.emplace_back(name);
        result=(std::find(types.begin(),types.end(),extension)!=types.end() && !(u32(1)&0xffU && extension==u"EXE"))?1U:0U;if(!result)last_error_=0;
    }else{
        // SaferComputeTokenFromLevel(LevelHandle, InAccessToken, OutAccessToken, dwFlags, lpReserved)
        const auto& item=level_of(arg(0));const auto flags=u32(3);if(flags&~15U)throw SddlError{87};
        if(item.level==disallowed)throw SddlError{1260};
        const auto source=arg(1)?token_for(arg(1),0xaU):runtime_.token_objects.at(effective_token_id());auto token=source;
        if(item.level!=fully_trusted){
            // 一般使用者：停用（只作拒絕用）系統管理群組、只保留基本特權、完整性不高於中；受限／不信任再加上限制 SID。
            for(const auto* text:{"S-1-5-32-544","S-1-5-32-547","S-1-5-32-548","S-1-5-32-549","S-1-5-32-550","S-1-5-32-551","S-1-5-32-552","S-1-5-32-556"}){
                const auto sid=sddl_sid(text);for(auto& group:token.groups)if(group.sid==sid)group.attributes=(group.attributes&~6U)|0x10U;}
            std::erase_if(token.privileges,[](const auto& privilege){return privilege.luid!=19 && privilege.luid!=23 && privilege.luid!=25 && privilege.luid!=33 && privilege.luid!=34;});
            token.integrity=std::min(token.integrity,item.level==untrusted?0x1000U:0x2000U);
            if(item.level==constrained || item.level==untrusted){
                std::vector<TokenSid> restricted;
                const auto texts=item.level==constrained?std::vector<const char*>{"S-1-1-0","S-1-5-32-545","S-1-5-4","S-1-5-11","S-1-5-12"}:std::vector<const char*>{"S-1-1-0","S-1-5-12"};
                for(const auto* text:texts)restricted.push_back({sddl_sid(text),7});
                for(const auto& group:token.groups)if(group.attributes&0xc0000000U)restricted.push_back({group.sid,7}); // 登入 SID
                token.restricted=std::move(restricted);token.restriction_check=true;
            }
        }
        if(flags&4U)token.sandbox_inert=true;
        const bool same=token==source;
        if(flags&2U){if(!arg(4))throw SddlError{87};memory.check(GuestAddress{arg(4)},4,Access::Write);memory.store(GuestAddress{arg(4)},32,same?0U:0xffffffffU);result=1;}
        else{
            if(flags&8U){if(!arg(4))throw SddlError{87};memory.check(GuestAddress{arg(4)},4,Access::Write);memory.store(GuestAddress{arg(4)},32,(item.level!=fully_trusted?4U:0U)|(flags&4U?2U:0U));}
            if(!arg(2))throw SddlError{87};memory.check(GuestAddress{arg(2)},unit,Access::Write);
            if(same && (flags&1U)){memory.store(GuestAddress{arg(2)},bits,0);result=1;}
            else{token.type=1;token.level=2;collect_tokens();const auto id=runtime_.next_token_object++;runtime_.token_objects.emplace(id,std::move(token));new_handle(id,token_all_access,arg(2));}
        }
    }
}
