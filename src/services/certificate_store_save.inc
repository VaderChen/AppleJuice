// Windows serialized store 使用固定 32 位元相對位移；不保存宿主指標或私鑰。
std::vector<std::uint8_t> serialized_key_info(const std::vector<std::uint8_t>& value){
    const auto info=key_info(value);std::vector<std::uint8_t> out(28+info.parameters.size()*16);
    const auto put=[&](std::size_t at,std::uint32_t n){for(unsigned i=0;i<4;++i)out[at+i]=static_cast<std::uint8_t>(n>>(8*i));};
    const auto text=[&](std::size_t at,const std::string& s){if(s.empty())return;const auto bytes=utf16_bytes(utf8_to_utf16_lossy(s));put(at,static_cast<std::uint32_t>(out.size()));out.insert(out.end(),bytes.begin(),bytes.end());};
    text(0,info.container);text(4,info.name);put(8,info.type);put(12,info.flags);put(16,static_cast<std::uint32_t>(info.parameters.size()));put(20,info.parameters.empty()?0:28);put(24,info.spec);
    for(std::size_t i=0;i<info.parameters.size();++i){const auto& p=info.parameters[i];const auto at=28+i*16;put(at,p.id);put(at+4,p.bytes.empty()?0:static_cast<std::uint32_t>(out.size()));put(at+8,static_cast<std::uint32_t>(p.bytes.size()));put(at+12,p.flags);out.insert(out.end(),p.bytes.begin(),p.bytes.end());}
    return out;
}
// 序列化 key provider info 的反向轉換：相對位移的 CRYPT_KEY_PROV_INFO 轉回內部格式。
std::vector<std::uint8_t> deserialized_key_info(std::span<const std::uint8_t> value){
    const auto get=[&](std::size_t at){if(at>value.size() || value.size()-at<4)throw CertificateError{0x80092004};std::uint32_t n=0;for(unsigned i=0;i<4;++i)n|=std::uint32_t{value[at+i]}<<(8*i);return n;};
    const auto text=[&](std::size_t at){const auto offset=get(at);if(!offset)return std::string{};std::u16string out;for(auto i=static_cast<std::size_t>(offset);i+1<value.size();i+=2){const auto c=static_cast<char16_t>(value[i]|(value[i+1]<<8));if(!c)break;out+=c;}return utf16_to_utf8(out);};
    KeyProviderInfo info;info.container=text(0);info.name=text(4);info.type=get(8);info.flags=get(12);const auto count=get(16);info.spec=get(24);if(count>64)throw CertificateError{0x80092004};
    for(std::uint32_t i=0;i<count;++i){const auto at=std::size_t{get(20)}+i*16;KeyProviderParameter param;param.id=get(at);const auto offset=get(at+4),size=get(at+8);param.flags=get(at+12);
        if(size){if(offset>value.size() || size>value.size()-offset)throw CertificateError{0x80092004};param.bytes.assign(value.begin()+offset,value.begin()+offset+size);}info.parameters.push_back(std::move(param));}
    return key_info(info);
}
// 不寫入序列化元素的屬性：暫存 handle／key context 與可重算的雜湊。
bool persistent_certificate_property(std::uint32_t id){return !(id==1 || id==3 || id==4 || id==5 || id==6 || id==15 || id==107);}
// 單一元素：屬性紀錄後接元素紀錄（32 憑證、33 CRL），每筆為 {id, 1, 長度, 資料}。
std::vector<std::uint8_t> serialize_store_element(const CertificateProperties& properties,const std::vector<std::uint8_t>& der,std::uint32_t type){
    std::vector<std::uint8_t> out;const auto record=[&](std::uint32_t id,const std::vector<std::uint8_t>& bytes){key_word(out,id);key_word(out,1);key_word(out,static_cast<std::uint32_t>(bytes.size()));out.insert(out.end(),bytes.begin(),bytes.end());};
    for(const auto& [id,value]:properties)if(persistent_certificate_property(id))record(id,id==2?serialized_key_info(value):value);
    record(type,der);return out;
}
struct SerializedStoreElement {std::uint32_t type{};std::vector<std::uint8_t> der;CertificateProperties properties;};
SerializedStoreElement parse_store_element(std::span<const std::uint8_t> bytes){
    SerializedStoreElement element;std::size_t at=0;
    while(at<bytes.size()){
        if(bytes.size()-at<12)throw CertificateError{0x80092004};std::size_t cursor=at;const auto id=key_word(bytes,cursor),encoding=key_word(bytes,cursor),size=key_word(bytes,cursor);(void)encoding;
        if(size>bytes.size()-cursor)throw CertificateError{0x80092004};std::vector<std::uint8_t> value(bytes.begin()+static_cast<std::ptrdiff_t>(cursor),bytes.begin()+static_cast<std::ptrdiff_t>(cursor+size));at=cursor+size;
        if(id>=32 && id<=34){element.type=id;element.der=std::move(value);if(at!=bytes.size())throw CertificateError{0x80092004};return element;}
        if(!id)throw CertificateError{0x80092004};if(persistent_certificate_property(id))element.properties[id]=id==2?deserialized_key_info(value):std::move(value);
    }
    throw CertificateError{0x80092004};
}
std::vector<std::uint8_t> save_certificate_store(CertificateState& state,std::uint64_t handle,bool pkcs7,std::size_t output_limit){
    const auto certificates=store_entries(state,handle),crls=store_entries(state,handle,0,true);
    const auto visible=[&](const StoreEntry& entry){return state.stores.at(handle).enum_archived || !state.properties.at(entry.properties).contains(19);};
    if(pkcs7){
        SignedMessage message(PKCS7_new(),PKCS7_free);
        if(!message || !PKCS7_set_type(message.get(),NID_pkcs7_signed) || !PKCS7_content_new(message.get(),NID_pkcs7_data))throw CertificateError{8};
        if(!PKCS7_set_detached(message.get(),1))throw CertificateError{8};
        for(const auto& entry:certificates)if(visible(entry)){const auto cert=decode_certificate(*entry.der);if(!PKCS7_add_certificate(message.get(),cert.get()))throw CertificateError{8};}
        for(const auto& entry:crls)if(visible(entry)){const auto crl=decode_crl(*entry.der);if(!PKCS7_add_crl(message.get(),crl.get()))throw CertificateError{8};}
        auto bytes=encoded(message.get(),i2d_PKCS7);if(bytes.size()>output_limit)throw CertificateError{8};return bytes;
    }
    std::vector<std::uint8_t> out{0,0,0,0,'C','E','R','T'};
    const auto record=[&](std::uint32_t id,const std::vector<std::uint8_t>& bytes){
        if(bytes.size()>output_limit-12 || out.size()>output_limit-12-bytes.size())throw CertificateError{8};
        key_word(out,id);key_word(out,1);key_word(out,static_cast<std::uint32_t>(bytes.size()));out.insert(out.end(),bytes.begin(),bytes.end());
    };
    const auto append=[&](const auto& entries,std::uint32_t type){for(const auto& entry:entries)if(visible(entry)){
        for(const auto& [id,value]:state.properties.at(entry.properties)){
            // 暫存 handle／key context 及可重算雜湊不持久化。
            if(id==1 || id==3 || id==4 || id==5 || id==6 || id==15 || id==107)continue;
            record(id,id==2?serialized_key_info(value):value);
        }
        record(type,*entry.der);
    }};
    append(certificates,32);append(crls,33);if(out.size()>output_limit-12)throw CertificateError{8};out.resize(out.size()+12);return out;
}
