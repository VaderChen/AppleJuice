// X.500 名稱的共同標籤表與反向編碼；沿用憑證後台的 DER、Unicode、IDN。
constexpr std::pair<std::string_view,std::string_view> certificate_name_labels[]{
    {"2.5.4.3","CN"},{"2.5.4.6","C"},{"2.5.4.7","L"},{"2.5.4.8","S"},{"2.5.4.10","O"},{"2.5.4.11","OU"},
    {"2.5.4.9","STREET"},{"2.5.4.4","SN"},{"2.5.4.12","T"},{"2.5.4.42","G"},{"2.5.4.43","I"},
    {"1.2.840.113549.1.9.1","E"},{"0.9.2342.19200300.100.1.25","DC"}
};
struct CertificateNameError {std::uint32_t code;std::size_t offset;};
struct CertificateNameEntry {std::string oid;std::u16string value;std::size_t offset;bool octets;};
inline std::vector<std::uint8_t> encode_certificate_name_entry(const CertificateNameEntry& entry,std::uint32_t flags){
    const auto fail=[](std::size_t offset,std::uint32_t code=0x80092023U){throw CertificateNameError{code,offset};};
    const auto space=[](char16_t c){return c==u' ' || (c>=u'\t' && c<=u'\r');};
        int type=V_ASN1_PRINTABLESTRING;std::vector<std::uint8_t> bytes;auto value=entry.value;
        if(entry.octets){
            type=V_ASN1_OCTET_STRING;int high=-1;
            for(std::size_t i=1;i<value.size();++i){const auto c=value[i];if(space(c))continue;const auto digit=c>=u'0' && c<=u'9'?c-u'0':c>=u'a' && c<=u'f'?c-u'a'+10:c>=u'A' && c<=u'F'?c-u'A'+10:-1;if(digit<0)fail(entry.offset+i);if(high<0)high=digit;else {bytes.push_back(static_cast<std::uint8_t>(16*high+digit));high=-1;}}
            if(high>=0)fail(entry.offset);
        }else {
            const bool email=entry.oid=="1.2.840.113549.1.9.1",domain=entry.oid=="0.9.2342.19200300.100.1.25";
            if(email && (flags&0x00200000U)){const auto split=value.rfind(u'@');if(split!=value.npos){auto ascii=idn_detail::convert(std::u16string_view(value).substr(split+1),true,0,idn_detail::Profile::certificate_uts46);if(!ascii)fail(entry.offset+split+1,0x80092022U);value.resize(split+1);value+=*ascii;}}
            bool printable=true,ascii=true,latin=true,supplementary=false;
            for(std::size_t i=0;i<value.size();++i){const auto c=value[i];ascii&=c<128;latin&=c<=255;printable&=(c>=u'A' && c<=u'Z') || (c>=u'a' && c<=u'z') || (c>=u'0' && c<=u'9') || std::u16string_view(u" '()+,-./:=?").find(c)!=std::u16string_view::npos;
                if(c>=0xd800 && c<=0xdbff){if(i+1==value.size() || value[i+1]<0xdc00 || value[i+1]>0xdfff)fail(entry.offset+i);supplementary=true;++i;}else if(c>=0xdc00 && c<=0xdfff)fail(entry.offset+i);
            }
            const bool force_utf8=(flags&0x00080000U) && !(flags&0x00100000U);
            if(email || (domain && ascii)){if(!ascii)fail(entry.offset,0x80092022U);type=V_ASN1_IA5STRING;}
            else if(entry.oid=="2.5.4.6"){if(!printable)fail(entry.offset,0x80092021U);}
            else if(force_utf8 || (!printable && ((flags&0x00040000U) || supplementary || domain)))type=V_ASN1_UTF8STRING;
            else if(!printable)type=(flags&0x00020000U) && latin?V_ASN1_T61STRING:V_ASN1_BMPSTRING;
            if(type==V_ASN1_UTF8STRING){const auto utf8=utf16_to_utf8(value);bytes.assign(utf8.begin(),utf8.end());}
            else if(type==V_ASN1_BMPSTRING){for(const auto c:value){bytes.push_back(static_cast<std::uint8_t>(c>>8));bytes.push_back(static_cast<std::uint8_t>(c));}}
            else for(const auto c:value)bytes.push_back(static_cast<std::uint8_t>(c));
        }
        std::unique_ptr<ASN1_OBJECT,decltype(&ASN1_OBJECT_free)> object(OBJ_txt2obj(entry.oid.c_str(),1),ASN1_OBJECT_free);
        std::unique_ptr<ASN1_STRING,decltype(&ASN1_STRING_free)> string(ASN1_STRING_type_new(type),ASN1_STRING_free);
        std::unique_ptr<ASN1_TYPE,decltype(&ASN1_TYPE_free)> typed_value(ASN1_TYPE_new(),ASN1_TYPE_free);
        if(!object || !string || !typed_value || !ASN1_STRING_set(string.get(),bytes.data(),static_cast<int>(bytes.size())))throw CertificateError{8};
        ASN1_TYPE_set(typed_value.get(),type,string.release());auto content=encoded(object.get(),i2d_ASN1_OBJECT);const auto encoded_value=encoded(typed_value.get(),i2d_ASN1_TYPE);content.insert(content.end(),encoded_value.begin(),encoded_value.end());return der_container(V_ASN1_SEQUENCE,content);
}
inline std::vector<std::uint8_t> encode_certificate_name(std::u16string_view text,std::uint32_t flags){
    if((flags&65535)!=2 && (flags&65535)!=3)throw CertificateError{87};
    if(flags&~0x7e3e0003U)throw CertificateError{87};
    const auto fail=[](std::size_t offset,std::uint32_t code=0x80092023U){throw CertificateNameError{code,offset};};
    const auto space=[](char16_t c){return c==u' ' || (c>=u'\t' && c<=u'\r');};
    const auto separator=[&](char16_t c){if(flags&0x08000000U)return c==u'\r' || c==u'\n';if(flags&0x40000000U)return c==u';';if(flags&0x04000000U)return c==u',';return c==u',' || c==u';' || c==u'\r' || c==u'\n';};
    const auto plus=[&](char16_t c){return c==u'+' && !(flags&0x20000000U);};
    std::vector<std::vector<CertificateNameEntry>> groups(1);std::size_t at=0;
    while(at<text.size()){
        while(at<text.size() && space(text[at]))++at;if(at==text.size())break;
        const auto key_start=at;while(at<text.size() && text[at]!=u'=' && !separator(text[at]) && !plus(text[at]))++at;
        if(at==text.size() || text[at]!=u'=')fail(key_start);
        auto key_end=at;while(key_end>key_start && space(text[key_end-1]))--key_end;
        std::string key;for(auto c:text.substr(key_start,key_end-key_start)){if(c>127)fail(key_start);key+=c>=u'a' && c<=u'z'?static_cast<char>(c-32):static_cast<char>(c);}
        if(key=="EMAIL")key="E";else if(key=="ST")key="S";else if(key=="TITLE")key="T";else if(key=="GIVENNAME")key="G";else if(key=="INITIALS")key="I";
        std::string oid;for(const auto& label:certificate_name_labels)if(key==label.second){oid=label.first;break;}
        if(oid.empty()){if(key.starts_with("OID."))key.erase(0,4);if(key.empty() || key.find_first_not_of("0123456789.")!=key.npos)fail(key_start);oid=key;}
        std::unique_ptr<ASN1_OBJECT,decltype(&ASN1_OBJECT_free)> object(OBJ_txt2obj(oid.c_str(),1),ASN1_OBJECT_free);if(!object){ERR_clear_error();fail(key_start);}
        ++at;while(at<text.size() && space(text[at]) && !separator(text[at]))++at;const auto value_start=at;
        std::u16string value;const bool quoted=at<text.size() && text[at]==u'"' && !(flags&0x10000000U);
        if(quoted){
            ++at;bool closed=false;while(at<text.size()){auto c=text[at++];if(c==u'"'){if(at<text.size() && text[at]==u'"')++at;else {closed=true;break;}}value+=c;}
            if(!closed)fail(value_start);while(at<text.size() && space(text[at]) && !separator(text[at]))++at;
            if(at<text.size() && !separator(text[at]) && !plus(text[at]))fail(at);
        }else {const auto start=at;while(at<text.size() && !separator(text[at]) && !plus(text[at]))++at;auto end=at;while(end>start && space(text[end-1]))--end;value=text.substr(start,end-start);}
        groups.back().push_back({std::move(oid),std::move(value),value_start,!quoted && value_start<text.size() && text[value_start]==u'#'});
        if(groups.size()>4096 || groups.back().size()>4096)throw CertificateError{8};
        if(at==text.size())break;const bool same=plus(text[at]);++at;if(!same)groups.emplace_back();
        if(at==text.size())fail(at-1);
        if(!same)while(at<text.size() && (text[at]==u'\r' || text[at]==u'\n'))++at;
    }
    if(flags&0x02000000U)std::reverse(groups.begin(),groups.end());
    std::vector<std::uint8_t> name;
    for(auto& group:groups){std::vector<std::vector<std::uint8_t>> attributes;for(auto& entry:group){
        attributes.push_back(encode_certificate_name_entry(entry,flags));
    }
        // DER 的 SET OF 必須依完整編碼排序；只反轉 RDN 群組，不反轉同群組屬性。
        if(attributes.empty())continue;std::sort(attributes.begin(),attributes.end());std::vector<std::uint8_t> content;
        for(const auto& attribute:attributes)content.insert(content.end(),attribute.begin(),attribute.end());const auto set=der_container(V_ASN1_SET,content);name.insert(name.end(),set.begin(),set.end());
    }
    return der_container(V_ASN1_SEQUENCE,name);
}
