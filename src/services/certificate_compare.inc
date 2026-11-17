// 憑證比較與查詢輔助（由 windows_certificates.cpp 的 invoke_certificate 引入，沿用同一組 blob、公鑰解碼與 store 搜尋）：
// - CertCompareIntegerBlob：小端序整數，先去掉正數的前導 0x00、負數的前導 0xFF（不改變正負號）再逐位元組比較。
// - CertCompareCertificate：簽發者名稱（DER）相同且序號相等。CertComparePublicKeyInfo：兩把公鑰解碼後比較（OpenSSL），無法解碼時比較位元字串。
// - CertGetPublicKeyLength：公鑰位元數。CertVerifyValidityNesting：主體有效期間在簽發者期間內。
// - CertFindRDNAttr／CertFindAttribute：在呼叫端提供的 CERT_NAME_INFO／CRYPT_ATTRIBUTE 陣列中找 OID，回傳該項指標。
// - CertRDNValueToStrA／W：字串類 RDN 值轉成字串（8 位元類型為位元組、BMP／UTF8 為 UTF-16），回傳含結尾 NUL 的字元數；非字串類型為空字串。
// - CertIsRDNAttrsInCertificateName：編碼名稱是否含有 CERT_RDN 的全部屬性（OID、值類型、值；可指定 Unicode 值與不分大小寫）。
// - CertGetSubjectCertificateFromStore：以 CERT_INFO 的簽發者與序號在 store 中找憑證（與 CERT_FIND_SUBJECT_CERT 同一路徑）。
if(symbol=="CertCompareIntegerBlob" || symbol=="CertCompareCertificate" || symbol=="CertComparePublicKeyInfo" || symbol=="CertGetPublicKeyLength" ||
   symbol=="CertVerifyValidityNesting" || symbol=="CertFindRDNAttr" || symbol=="CertFindAttribute" || symbol.starts_with("CertRDNValueToStr") ||
   symbol=="CertIsRDNAttrsInCertificateName" || symbol=="CertGetSubjectCertificateFromStore"){
    const auto trimmed=[](std::vector<std::uint8_t> value){
        while(value.size()>1 && ((value.back()==0 && !(value[value.size()-2]&0x80)) || (value.back()==0xff && (value[value.size()-2]&0x80))))value.pop_back();return value;};
    const auto integer=[&](std::uint64_t address){if(!address)throw CertificateError{87};const auto size=memory.load(GuestAddress{address},32),pointer=memory.load(GuestAddress{address+unit},bits);
        if(size>output_limit || (size && !pointer))throw CertificateError{87};std::vector<std::uint8_t> value(static_cast<std::size_t>(size));if(size)memory.read(GuestAddress{pointer},value);return value;};
    // CERT_INFO：序號在 unit、簽發者在 6*unit、NotBefore／NotAfter 在 8*unit 起。
    const auto validity=[&](std::uint64_t info){const auto at=info+8*unit;return std::pair{memory.load(GuestAddress{at},64),memory.load(GuestAddress{at+8},64)};};
    if(symbol=="CertCompareIntegerBlob")result=trimmed(integer(arg(0)))==trimmed(integer(arg(1)))?1:0;
    else if(symbol=="CertCompareCertificate"){
        if(!arg(1) || !arg(2))throw CertificateError{87};
        result=integer(arg(1)+6*unit)==integer(arg(2)+6*unit) && trimmed(integer(arg(1)+unit))==trimmed(integer(arg(2)+unit))?1:0;
    }else if(symbol=="CertComparePublicKeyInfo"){
        if(!arg(1) || !arg(2))throw CertificateError{87};
        try{auto first=public_key(arg(1));auto second=public_key(arg(2));result=EVP_PKEY_eq(first.get(),second.get())==1?1:0;}
        catch(const CertificateError&){
            const auto key=[&](std::uint64_t info){auto bytes=integer(info+3*unit);bytes.push_back(static_cast<std::uint8_t>(memory.load(GuestAddress{info+5*unit},32)));return bytes;};
            result=key(arg(1))==key(arg(2))?1:0;}
    }else if(symbol=="CertGetPublicKeyLength"){
        if((u32(0)&0xffffU)!=1)throw CertificateError{0x80070057};auto key=public_key(arg(1));result=static_cast<std::uint32_t>(EVP_PKEY_get_bits(key.get()));
        if(!result)throw CertificateError{0x80090005};
    }else if(symbol=="CertVerifyValidityNesting"){
        if(!arg(0) || !arg(1))throw CertificateError{87};const auto subject=validity(arg(0)),issuer=validity(arg(1));
        result=subject.first>=issuer.first && subject.second<=issuer.second?1:0;
    }else if(symbol=="CertFindRDNAttr" || symbol=="CertFindAttribute"){
        // CertFindRDNAttr(pszObjId, pName)／CertFindAttribute(pszObjId, cAttr, rgAttr)：找不到為 NULL。
        result=0;if(!arg(0))throw CertificateError{87};const auto oid=read_ansi(memory,arg(0),1024);
        const auto matches=[&](std::uint64_t entry){const auto pointer=memory.load(GuestAddress{entry},bits);return pointer && read_ansi(memory,pointer,1024)==oid;};
        if(symbol=="CertFindAttribute"){const std::uint32_t size=unit==4?12U:24U;for(std::uint32_t i=0;i<u32(1) && i<65536 && !result;++i)if(matches(arg(2)+std::uint64_t{i}*size))result=arg(2)+std::uint64_t{i}*size;}
        else if(arg(1)){
            const auto rdns=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)},32));const auto table=memory.load(GuestAddress{arg(1)+unit},bits);const std::uint32_t attribute_size=unit==4?16U:32U;
            for(std::uint32_t i=0;i<rdns && i<65536 && !result;++i){const auto rdn=table+std::uint64_t{i}*2*unit;const auto attributes=static_cast<std::uint32_t>(memory.load(GuestAddress{rdn},32));const auto list=memory.load(GuestAddress{rdn+unit},bits);
                for(std::uint32_t j=0;j<attributes && j<65536;++j)if(matches(list+std::uint64_t{j}*attribute_size)){result=list+std::uint64_t{j}*attribute_size;break;}}
        }
    }else if(symbol.starts_with("CertRDNValueToStr")){
        // (dwValueType, pValue, psz, csz)
        const bool wide=symbol.ends_with('W');const auto type=u32(0);std::u16string text;
        if(arg(1)){const auto value=integer(arg(1));
            if(type==12 || type==13){for(std::size_t i=0;i+1<value.size();i+=2)text.push_back(static_cast<char16_t>(value[i]|(value[i+1]<<8)));}
            else if(type>=3 && type<=10 && type!=11)for(const auto byte:value)text.push_back(static_cast<char16_t>(byte));}
        const auto end=text.find(u'\0');if(end!=text.npos)text.resize(end);
        std::vector<std::uint8_t> bytes;std::size_t length=0;
        if(wide){bytes=utf16_bytes(text);length=text.size()+1;}else{const auto narrow=utf16_to_utf8(text);bytes.assign(narrow.begin(),narrow.end());bytes.push_back(0);length=bytes.size();}
        if(!arg(2) || !u32(3))result=length;
        else{const std::size_t room=std::min<std::size_t>(u32(3),length),width=wide?2:1;std::vector<std::uint8_t> out(bytes.begin(),bytes.begin()+static_cast<std::ptrdiff_t>(room*width));
            std::fill(out.end()-static_cast<std::ptrdiff_t>(width),out.end(),0);memory.write(GuestAddress{arg(2)},out);result=room;}
    }else if(symbol=="CertIsRDNAttrsInCertificateName"){
        // (dwCertEncodingType, dwFlags, pCertName, pRDN)：CERT_UNICODE_IS_RDN_ATTRS_FLAG（1）時 pRDN 的值為 UTF-16；CERT_CASE_INSENSITIVE_IS_RDN_ATTRS_FLAG（2）不分大小寫。
        if((u32(0)&0xffffU)!=1 || (u32(1)&~3U) || !arg(3))throw CertificateError{0x80070057};const bool unicode=u32(1)&1U,insensitive=u32(1)&2U;
        const auto der=integer(arg(2));const auto* cursor=der.data();std::unique_ptr<X509_NAME,decltype(&X509_NAME_free)> name(d2i_X509_NAME(nullptr,&cursor,static_cast<long>(der.size())),X509_NAME_free);
        if(!name || cursor!=der.data()+der.size())throw CertificateError{0x8009310b};
        struct NameValue {std::string oid;std::uint32_t type;std::vector<std::uint8_t> raw;std::u16string text;};std::vector<NameValue> values;
        for(int i=0;i<X509_NAME_entry_count(name.get());++i){
            auto* entry=X509_NAME_get_entry(name.get(),i);char oid[128]{};OBJ_obj2txt(oid,sizeof oid,X509_NAME_ENTRY_get_object(entry),1);const auto* data=X509_NAME_ENTRY_get_data(entry);
            const auto* bytes=ASN1_STRING_get0_data(data);const auto size=static_cast<std::size_t>(ASN1_STRING_length(data));const int asn=ASN1_STRING_type(data);
            NameValue value{oid,asn==18?3U:asn==19?4U:asn==20?5U:asn==21?6U:asn==22?7U:asn==25?8U:asn==26?9U:asn==27?10U:asn==28?11U:asn==30?12U:asn==12?13U:asn==4?2U:1U,std::vector<std::uint8_t>(bytes,bytes+size),{}};
            if(asn==30)for(std::size_t j=0;j+1<size;j+=2)value.text.push_back(static_cast<char16_t>((bytes[j]<<8)|bytes[j+1]));
            else if(asn==12)value.text=utf8_to_utf16_lossy(std::string(reinterpret_cast<const char*>(bytes),size));
            else for(std::size_t j=0;j<size;++j)value.text.push_back(static_cast<char16_t>(bytes[j]));
            values.push_back(std::move(value));
        }
        const auto fold=[&](std::u16string text){if(insensitive)for(auto& c:text)c=unicode_case_unit(c,false);return text;};
        const auto count=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)},32));const auto list=memory.load(GuestAddress{arg(3)+unit},bits);const std::uint32_t attribute_size=unit==4?16U:32U;
        if(count>65536 || (count && !list))throw CertificateError{87};result=1;
        for(std::uint32_t i=0;i<count && result;++i){
            const auto entry=list+std::uint64_t{i}*attribute_size;const auto pointer=memory.load(GuestAddress{entry},bits);const auto oid=pointer?read_ansi(memory,pointer,1024):std::string{};
            const auto type=static_cast<std::uint32_t>(memory.load(GuestAddress{entry+unit},32));const auto wanted=integer(entry+2*unit);
            std::u16string wanted_text;if(unicode || type==12 || type==13){for(std::size_t j=0;j+1<wanted.size();j+=2)wanted_text.push_back(static_cast<char16_t>(wanted[j]|(wanted[j+1]<<8)));}
            else for(const auto byte:wanted)wanted_text.push_back(static_cast<char16_t>(byte));
            const bool found=std::any_of(values.begin(),values.end(),[&](const NameValue& value){
                if((!oid.empty() && value.oid!=oid) || (type && value.type!=type))return false;
                if(!unicode && !insensitive && type!=12 && type!=13 && value.type!=12 && value.type!=13)return value.raw==wanted;
                return fold(value.text)==fold(wanted_text);});
            if(!found){result=0;last_error_=0x80092009U;}
        }
        if(!result){arg.return_to(state,0);return std::nullopt;}
    }else{
        // (hCertStore, dwCertEncodingType, pCertId)：與 CertFindCertificateInStore(CERT_FIND_SUBJECT_CERT) 相同。
        if(!arg(2))throw CertificateError{87};
        result=nested_service(state,memory,"crypt32.dll","CertFindCertificateInStore",{arg(0),arg(1),0,0x000b0000U,arg(2),0});if(x86_)result&=UINT32_MAX;
        if(!result){arg.return_to(state,0);return std::nullopt;}
    }
    arg.return_to(state,result);return std::nullopt;
}
