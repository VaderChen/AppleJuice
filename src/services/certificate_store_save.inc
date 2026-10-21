// Windows serialized store 使用固定 32 位元相對位移；不保存宿主指標或私鑰。
std::vector<std::uint8_t> serialized_key_info(const std::vector<std::uint8_t>& value){
    const auto info=key_info(value);std::vector<std::uint8_t> out(28+info.parameters.size()*16);
    const auto put=[&](std::size_t at,std::uint32_t n){for(unsigned i=0;i<4;++i)out[at+i]=static_cast<std::uint8_t>(n>>(8*i));};
    const auto text=[&](std::size_t at,const std::string& s){if(s.empty())return;const auto bytes=utf16_bytes(utf8_to_utf16_lossy(s));put(at,static_cast<std::uint32_t>(out.size()));out.insert(out.end(),bytes.begin(),bytes.end());};
    text(0,info.container);text(4,info.name);put(8,info.type);put(12,info.flags);put(16,static_cast<std::uint32_t>(info.parameters.size()));put(20,info.parameters.empty()?0:28);put(24,info.spec);
    for(std::size_t i=0;i<info.parameters.size();++i){const auto& p=info.parameters[i];const auto at=28+i*16;put(at,p.id);put(at+4,p.bytes.empty()?0:static_cast<std::uint32_t>(out.size()));put(at+8,static_cast<std::uint32_t>(p.bytes.size()));put(at+12,p.flags);out.insert(out.end(),p.bytes.begin(),p.bytes.end());}
    return out;
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
