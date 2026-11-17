// crypt32 的 OID 資訊與記憶體配置（由 windows_certificates.cpp 的 invoke_certificate 引入）：
// - CryptFindOIDInfo／CryptEnumOIDInfo／CertOIDToAlgId／CertAlgIdToOID：內建 OID 表（雜湊、加密、公鑰、簽章、RDN 屬性、延伸與屬性、增強金鑰用途、原則），
//   第一次使用時在本程序的執行階段區配置成 CRYPT_OID_INFO 陣列（含 CNG 欄位），回傳的指標在程序存續期間有效。依 OID、名稱（不分大小寫）、ALG_ID、
//   簽章（雜湊＋公鑰 ALG_ID）、CNG 演算法或 CNG 簽章查詢；群組 0 為全部，高位的金鑰長度與旗標位元不影響比對。
// - CryptFindLocalizedName：系統憑證存放區與存放區位置的顯示名稱。
// - CryptMemAlloc／CryptMemRealloc／CryptMemFree：與 crypt32 輸出（CRYPT_*_ALLOC_FLAG）相同使用 LocalAlloc 系列。
if(symbol=="CryptFindOIDInfo" || symbol=="CryptEnumOIDInfo" || symbol=="CertOIDToAlgId" || symbol=="CertAlgIdToOID" || symbol=="CryptFindLocalizedName" ||
   symbol=="CryptMemAlloc" || symbol=="CryptMemRealloc" || symbol=="CryptMemFree"){
    struct OidEntry {const char* oid;const char16_t* name;std::uint32_t group,value;std::array<std::uint32_t,3> extra;const char16_t* cng;const char16_t* cng_extra;};
    static constexpr std::uint32_t md2=0x8001,md4=0x8002,md5=0x8003,sha1=0x8004,sha256=0x800c,sha384=0x800d,sha512=0x800e,rsa_sign=0x2400,rsa_keyx=0xa400,dss=0x2200,ecdsa=0x2203;
    static constexpr OidEntry table[]{
        {"1.2.840.113549.2.2",u"md2",1,md2,{},u"MD2",nullptr},{"1.2.840.113549.2.4",u"md4",1,md4,{},u"MD4",nullptr},{"1.2.840.113549.2.5",u"md5",1,md5,{},u"MD5",nullptr},
        {"1.3.14.3.2.26",u"sha1",1,sha1,{},u"SHA1",nullptr},{"2.16.840.1.101.3.4.2.1",u"sha256",1,sha256,{},u"SHA256",nullptr},
        {"2.16.840.1.101.3.4.2.2",u"sha384",1,sha384,{},u"SHA384",nullptr},{"2.16.840.1.101.3.4.2.3",u"sha512",1,sha512,{},u"SHA512",nullptr},
        {"1.2.840.113549.3.2",u"rc2",2,0x6602,{},u"RC2",nullptr},{"1.2.840.113549.3.4",u"rc4",2,0x6801,{},u"RC4",nullptr},{"1.3.14.3.2.7",u"des",2,0x6601,{},u"DES",nullptr},
        {"1.2.840.113549.3.7",u"3des",2,0x6603,{},u"3DES",nullptr},{"2.16.840.1.101.3.4.1.2",u"aes128",2,0x660e,{},u"AES",nullptr},
        {"2.16.840.1.101.3.4.1.22",u"aes192",2,0x660f,{},u"AES",nullptr},{"2.16.840.1.101.3.4.1.42",u"aes256",2,0x6610,{},u"AES",nullptr},
        {"1.2.840.113549.1.1.1",u"RSA",3,rsa_keyx,{},u"RSA",nullptr},{"1.2.840.10040.4.1",u"DSA",3,dss,{},u"DSA",nullptr},
        {"1.2.840.10045.2.1",u"ECC",3,0xfffffffeU,{},u"CryptOIDInfoECCParameters",nullptr},{"1.2.840.113549.1.3.1",u"DH",3,0xaa01,{},u"DH",nullptr},
        {"1.2.840.113549.1.1.7",u"RSAES_OAEP",3,rsa_keyx,{},u"RSA",nullptr},
        {"1.2.840.113549.1.1.5",u"sha1RSA",4,sha1,{rsa_sign,0,0},u"SHA1",u"RSA"},{"1.2.840.113549.1.1.4",u"md5RSA",4,md5,{rsa_sign,0,0},u"MD5",u"RSA"},
        {"1.2.840.113549.1.1.2",u"md2RSA",4,md2,{rsa_sign,0,0},u"MD2",u"RSA"},{"1.2.840.113549.1.1.3",u"md4RSA",4,md4,{rsa_sign,0,0},u"MD4",u"RSA"},
        {"1.2.840.113549.1.1.11",u"sha256RSA",4,sha256,{rsa_sign,0,0},u"SHA256",u"RSA"},{"1.2.840.113549.1.1.12",u"sha384RSA",4,sha384,{rsa_sign,0,0},u"SHA384",u"RSA"},
        {"1.2.840.113549.1.1.13",u"sha512RSA",4,sha512,{rsa_sign,0,0},u"SHA512",u"RSA"},{"1.2.840.113549.1.1.10",u"RSASSA-PSS",4,0xfffffffeU,{rsa_sign,0,0},u"CryptOIDInfoParameters",u"RSA"},
        {"1.2.840.10040.4.3",u"sha1DSA",4,sha1,{dss,4,0},u"SHA1",u"DSA"},{"1.2.840.10045.4.1",u"sha1ECDSA",4,sha1,{ecdsa,4,0},u"SHA1",u"ECDSA"},
        {"1.2.840.10045.4.3.2",u"sha256ECDSA",4,sha256,{ecdsa,4,0},u"SHA256",u"ECDSA"},{"1.2.840.10045.4.3.3",u"sha384ECDSA",4,sha384,{ecdsa,4,0},u"SHA384",u"ECDSA"},
        {"1.2.840.10045.4.3.4",u"sha512ECDSA",4,sha512,{ecdsa,4,0},u"SHA512",u"ECDSA"},
        {"2.5.4.3",u"CN",5,0,{},nullptr,nullptr},{"2.5.4.7",u"L",5,0,{},nullptr,nullptr},{"2.5.4.10",u"O",5,0,{},nullptr,nullptr},{"2.5.4.11",u"OU",5,0,{},nullptr,nullptr},
        {"1.2.840.113549.1.9.1",u"E",5,0,{7,0,0},nullptr,nullptr},{"2.5.4.6",u"C",5,0,{4,0,0},nullptr,nullptr},{"2.5.4.8",u"S",5,0,{},nullptr,nullptr},
        {"2.5.4.9",u"STREET",5,0,{},nullptr,nullptr},{"2.5.4.12",u"T",5,0,{},nullptr,nullptr},{"2.5.4.42",u"G",5,0,{},nullptr,nullptr},{"2.5.4.43",u"I",5,0,{},nullptr,nullptr},
        {"2.5.4.4",u"SN",5,0,{},nullptr,nullptr},{"0.9.2342.19200300.100.1.25",u"DC",5,0,{7,0,0},nullptr,nullptr},{"2.5.4.13",u"Description",5,0,{},nullptr,nullptr},
        {"2.5.4.17",u"PostalCode",5,0,{},nullptr,nullptr},{"2.5.4.5",u"SERIALNUMBER",5,0,{4,0,0},nullptr,nullptr},
        {"2.5.29.35",u"Authority Key Identifier",6,0,{},nullptr,nullptr},{"2.5.29.15",u"Key Usage",6,0,{},nullptr,nullptr},{"2.5.29.17",u"Subject Alternative Name",6,0,{},nullptr,nullptr},
        {"2.5.29.18",u"Issuer Alternative Name",6,0,{},nullptr,nullptr},{"2.5.29.19",u"Basic Constraints",6,0,{},nullptr,nullptr},{"2.5.29.14",u"Subject Key Identifier",6,0,{},nullptr,nullptr},
        {"2.5.29.31",u"CRL Distribution Points",6,0,{},nullptr,nullptr},{"2.5.29.32",u"Certificate Policies",6,0,{},nullptr,nullptr},{"2.5.29.37",u"Enhanced Key Usage",6,0,{},nullptr,nullptr},
        {"2.5.29.30",u"Name Constraints",6,0,{},nullptr,nullptr},{"2.5.29.20",u"CRL Number",6,0,{},nullptr,nullptr},{"1.3.6.1.5.5.7.1.1",u"Authority Information Access",6,0,{},nullptr,nullptr},
        {"1.2.840.113549.1.9.3",u"Content Type",6,0,{},nullptr,nullptr},{"1.2.840.113549.1.9.4",u"Message Digest",6,0,{},nullptr,nullptr},
        {"1.2.840.113549.1.9.5",u"Signing Time",6,0,{},nullptr,nullptr},{"1.2.840.113549.1.9.6",u"Counter Signature",6,0,{},nullptr,nullptr},
        {"1.2.840.113549.1.9.15",u"SMIME Capabilities",6,0,{},nullptr,nullptr},
        {"1.3.6.1.5.5.7.3.1",u"Server Authentication",7,0,{},nullptr,nullptr},{"1.3.6.1.5.5.7.3.2",u"Client Authentication",7,0,{},nullptr,nullptr},
        {"1.3.6.1.5.5.7.3.3",u"Code Signing",7,0,{},nullptr,nullptr},{"1.3.6.1.5.5.7.3.4",u"Secure Email",7,0,{},nullptr,nullptr},
        {"1.3.6.1.5.5.7.3.8",u"Time Stamping",7,0,{},nullptr,nullptr},{"1.3.6.1.5.5.7.3.9",u"OCSP Signing",7,0,{},nullptr,nullptr},
        {"1.3.6.1.4.1.311.10.3.4",u"Encrypting File System",7,0,{},nullptr,nullptr},{"1.3.6.1.4.1.311.20.2.2",u"Smart Card Logon",7,0,{},nullptr,nullptr},
        {"1.3.6.1.4.1.311.10.3.12",u"Document Signing",7,0,{},nullptr,nullptr},{"2.5.29.37.0",u"Any Purpose",7,0,{},nullptr,nullptr},
        {"2.5.29.32.0",u"All issuance policies",8,0,{},nullptr,nullptr}};
    // 執行時的項目：CryptRegisterOIDInfo 登錄的項目（CRYPT_INSTALL_OID_INFO_BEFORE_FLAG 放在內建表之前，其餘在後）＋內建表。
    // 程序第一次使用時讀取登錄並建立，之後不再重新讀取：CryptRegisterOIDInfo 新登錄的項目由之後啟動的程序看到。
    const auto entries=[&]()->const std::vector<CryptOidInfoEntry>&{
        if(!crypt_oid_entries_.empty())return crypt_oid_entries_;
        std::vector<CryptOidInfoEntry> before,builtin,after;
        for(const auto& entry:table){CryptOidInfoEntry item{entry.oid,entry.name,entry.group,entry.value,{},entry.cng?entry.cng:u"",entry.cng_extra?entry.cng_extra:u""};
            for(const auto value:entry.extra)if(value)item.extra.push_back(value);builtin.push_back(std::move(item));}
        const auto text=[](const RegistryValue& value){std::u16string out;for(std::size_t i=0;i+1<value.bytes.size();i+=2){const auto c=static_cast<char16_t>(value.bytes[i]|(value.bytes[i+1]<<8));if(!c)break;out+=c;}return out;};
        const auto dword=[](const RegistryValue& value){std::uint32_t out=0;for(std::size_t i=0;i<4 && i<value.bytes.size();++i)out|=std::uint32_t{value.bytes[i]}<<(8*i);return out;};
        for(const auto& [key,values]:crypt_oid_registry_children(0,"CryptDllFindOIDInfo")){
            const auto bang=key.rfind(u'!');if(bang==std::u16string::npos || !bang)continue;CryptOidInfoEntry item;item.oid=utf16_to_utf8(key.substr(0,bang));
            try{item.group=static_cast<std::uint32_t>(std::stoul(utf16_to_utf8(key.substr(bang+1))));}catch(...){continue;}
            if(const auto v=values.find(u"name");v!=values.end())item.name=text(v->second);if(const auto v=values.find(u"algid");v!=values.end())item.value=dword(v->second);
            if(const auto v=values.find(u"extrainfo");v!=values.end())for(std::size_t i=0;i+3<v->second.bytes.size();i+=4)item.extra.push_back(static_cast<std::uint32_t>(v->second.bytes[i])|static_cast<std::uint32_t>(v->second.bytes[i+1])<<8|static_cast<std::uint32_t>(v->second.bytes[i+2])<<16|static_cast<std::uint32_t>(v->second.bytes[i+3])<<24);
            if(const auto v=values.find(u"cngalgid");v!=values.end())item.cng=text(v->second);if(const auto v=values.find(u"cngextraalgid");v!=values.end())item.cng_extra=text(v->second);
            const auto flags=values.find(u"flags");(flags!=values.end() && (dword(flags->second)&1U)?before:after).push_back(std::move(item));
        }
        crypt_oid_entries_=std::move(before);crypt_oid_entries_.insert(crypt_oid_entries_.end(),builtin.begin(),builtin.end());crypt_oid_entries_.insert(crypt_oid_entries_.end(),after.begin(),after.end());
        return crypt_oid_entries_;
    };
    const auto& list=entries();const std::size_t count=list.size();const std::uint32_t entry_size=x86_?36U:64U;
    // 第一次使用時寫入整張表：結構陣列、ExtraInfo 的 DWORD 陣列與字串放在同一塊。
    const auto table_base=[&]()->std::uint64_t{
        if(crypt_oid_table_)return crypt_oid_table_;
        std::vector<std::uint8_t> bytes(count*entry_size);std::vector<std::pair<std::size_t,std::size_t>> fixups;
        const auto put=[&](std::size_t at,unsigned width,std::uint64_t value){for(unsigned i=0;i<width;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(8*i));};
        const auto append=[&](std::span<const std::uint8_t> data){while(bytes.size()%4)bytes.push_back(0);const auto at=bytes.size();bytes.insert(bytes.end(),data.begin(),data.end());return at;};
        const auto wide=[&](const std::u16string& text){return append(utf16_bytes(text));};
        for(std::size_t i=0;i<count;++i){
            const auto& entry=list[i];const auto base=i*entry_size;const auto field=[&](unsigned x86_offset,unsigned x64_offset){return base+(x86_?x86_offset:x64_offset);};
            put(base,4,entry_size);std::vector<std::uint8_t> oid_bytes(entry.oid.begin(),entry.oid.end());oid_bytes.push_back(0);
            fixups.emplace_back(field(4,8),append(oid_bytes));fixups.emplace_back(field(8,16),wide(entry.name));
            put(field(12,24),4,entry.group);put(field(16,28),4,entry.value);
            if(!entry.extra.empty()){std::vector<std::uint8_t> data(entry.extra.size()*4);for(std::size_t j=0;j<entry.extra.size();++j)for(unsigned b=0;b<4;++b)data[j*4+b]=static_cast<std::uint8_t>(entry.extra[j]>>(8*b));
                put(field(20,32),4,data.size());fixups.emplace_back(field(24,40),append(data));}
            if(!entry.cng.empty())fixups.emplace_back(field(28,48),wide(entry.cng));if(!entry.cng_extra.empty())fixups.emplace_back(field(32,56),wide(entry.cng_extra));
        }
        const auto address=runtime_allocate(memory,bytes,bytes.size(),16);if(!address)throw CertificateError{8};
        for(const auto& [at,target]:fixups){std::vector<std::uint8_t> pointer(unit);for(unsigned b=0;b<unit;++b)pointer[b]=static_cast<std::uint8_t>((address+target)>>(8*b));memory.write(GuestAddress{address+at},pointer);}
        crypt_oid_table_=address;return address;
    };
    const auto lower=[](std::u16string text){for(auto& c:text)c=unicode_case_unit(c,false);return text;};
    if(symbol=="CryptFindOIDInfo"){
        // (dwKeyType, pvKey, dwGroupId)：找不到時回傳 NULL。
        const auto key_type=u32(0),group=u32(2)&0xffffU;result=0;
        if(!arg(1))throw CertificateError{87};
        std::string oid;std::u16string text,extra_text;std::uint32_t algid=0,pubkey=0;
        if(key_type==1)oid=read_ansi(memory,arg(1),1024);
        else if(key_type==2 || key_type==5)text=lower(read_utf16(memory,GuestAddress{arg(1)},1024));
        else if(key_type==3)algid=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)},32));
        else if(key_type==4){algid=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)},32));pubkey=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)+4},32));}
        else if(key_type==6){const auto first=memory.load(GuestAddress{arg(1)},bits),second=memory.load(GuestAddress{arg(1)+unit},bits);if(!first || !second)throw CertificateError{87};
            text=lower(read_utf16(memory,GuestAddress{first},256));extra_text=lower(read_utf16(memory,GuestAddress{second},256));}
        else throw CertificateError{87};
        for(std::size_t i=0;i<count;++i){
            const auto& entry=list[i];if(group && entry.group!=group)continue;bool match=false;
            switch(key_type){
            case 1:match=oid==entry.oid;break;case 2:match=text==lower(entry.name);break;case 3:match=entry.value==algid && entry.group<=4;break;
            case 4:match=entry.group==4 && entry.value==algid && !entry.extra.empty() && entry.extra[0]==pubkey;break;case 5:match=!entry.cng.empty() && text==lower(entry.cng) && entry.cng_extra.empty();break;
            case 6:match=!entry.cng.empty() && !entry.cng_extra.empty() && text==lower(entry.cng) && extra_text==lower(entry.cng_extra);break;
            }
            if(match){result=table_base()+i*entry_size;break;}
        }
    }else if(symbol=="CertOIDToAlgId"){
        result=0;if(arg(0)){const auto oid=read_ansi(memory,arg(0),1024);for(const auto& entry:list)if(entry.group<=4 && oid==entry.oid){result=entry.value;break;}}
    }else if(symbol=="CertAlgIdToOID"){
        result=0;for(std::size_t i=0;i<count;++i)if(list[i].group<=4 && list[i].value==u32(0)){result=memory.load(GuestAddress{table_base()+i*entry_size+(x86_?4U:8U)},bits);break;}
    }else if(symbol=="CryptFindLocalizedName"){
        // 系統存放區與位置的顯示名稱；不認得的名稱回傳 NULL。
        result=0;if(arg(0))if(const auto i=certificate_localized_name_index(read_utf16(memory,GuestAddress{arg(0)},256))){
            if(!crypt_localized_names_[*i]){crypt_localized_names_[*i]=runtime_allocate(memory,utf16_bytes(std::u16string(certificate_localized_names[*i].second)),0,2);if(!crypt_localized_names_[*i])throw CertificateError{8};}
            result=crypt_localized_names_[*i];}
    }else if(symbol=="CryptMemAlloc")result=nested_service(state,memory,"kernel32.dll","LocalAlloc",{0,arg(0)});
    else if(symbol=="CryptMemRealloc")result=arg(0)?nested_service(state,memory,"kernel32.dll","LocalReAlloc",{arg(0),arg(1),2}):nested_service(state,memory,"kernel32.dll","LocalAlloc",{0,arg(1)});
    else if(symbol=="CryptMemFree"){if(arg(0))(void)nested_service(state,memory,"kernel32.dll","LocalFree",{arg(0)});arg.return_to(state,0);return std::nullopt;}
    else{
        // CryptEnumOIDInfo(dwGroupId, dwFlags, pvArg, pfnEnumOIDInfo)：回呼傳回 FALSE 時停止並回傳 FALSE。
        if(u32(1) || !arg(3))throw CertificateError{87};const auto group=u32(0)&0xffffU;const auto base=table_base();
        std::vector<std::uint64_t> addresses;for(std::size_t i=0;i<count;++i)if(!group || list[i].group==group)addresses.push_back(base+i*entry_size);
        const auto callback=arg(3),context=arg(2);
        return nested_continuation(state,memory,service.symbol,[service,addresses,callback,context](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);std::uint64_t value=1;
            for(const auto entry:addresses)if(!(co_await service_callback::call_value(callback,{entry,context})&UINT32_MAX)){value=0;break;}
            call.return_to(s,value);co_return std::nullopt;
        });
    }
    arg.return_to(state,result);return std::nullopt;
}
