// PKCS#7 訊息的編碼、控制與簡化 API（由 windows_certificates.cpp 的 invoke_certificate 引入，沿用 crypt_message_sign.inc 的核心與既有解碼）：
// - CryptMsgOpenToEncode：CMSG_DATA、CMSG_SIGNED（CMSG_DETACHED_FLAG 為分離簽章）、CMSG_HASHED；CryptMsgUpdate 累積內容，最後一次時產生編碼；
//   CryptMsgGetParam 的 CMSG_CONTENT_PARAM 為整個 ContentInfo、CMSG_BARE_CONTENT_PARAM 為內層內容，其他參數沿用解碼路徑。不支援串流（pStreamInfo）。
// - CryptMsgControl：VERIFY_SIGNATURE（CERT_INFO）、VERIFY_SIGNATURE_EX（公鑰／憑證／鏈）、ADD／DEL_CERT、ADD／DEL_CRL；其他控制為 CRYPT_E_CONTROL_TYPE。
// - CryptSignMessage／CryptVerifyMessageSignature／CryptVerifyDetachedMessageSignature（可用 pfnGetSignerCertificate 回呼）、CryptGetMessageSignerCount、
//   CryptGetMessageCertificates、CryptMsgGetAndVerifySigner、CryptHashMessage／CryptVerifyMessageHash／CryptVerifyDetachedMessageHash。
{
const auto encoder_message=[&]()->CryptMessage*{if(symbol!="CryptMsgUpdate" && symbol!="CryptMsgGetParam")return nullptr;const auto found=certs.messages.find(arg(0));return found!=certs.messages.end() && found->second.encoder?&found->second:nullptr;};
if(symbol=="CryptMsgOpenToEncode" || symbol=="CryptMsgControl" || encoder_message() || symbol=="CryptSignMessage" || symbol=="CryptVerifyMessageSignature" ||
   symbol=="CryptVerifyDetachedMessageSignature" || symbol=="CryptGetMessageSignerCount" || symbol=="CryptGetMessageCertificates" || symbol=="CryptMsgGetAndVerifySigner" ||
   symbol=="CryptHashMessage" || symbol=="CryptVerifyMessageHash" || symbol=="CryptVerifyDetachedMessageHash"){
    const auto load=[&](std::uint64_t at){return memory.load(GuestAddress{at},bits);};
    const auto load32=[&](std::uint64_t at)->std::uint64_t{return static_cast<std::uint32_t>(memory.load(GuestAddress{at},32));};
    const auto bytes_at=[&](std::uint64_t pointer,std::uint64_t size){if(size>output_limit)throw CertificateError{8};if(size && !pointer)throw CertificateError{87};std::vector<std::uint8_t> out(static_cast<std::size_t>(size));if(size)memory.read(GuestAddress{pointer},out);return out;};
    const auto blob_at=[&](std::uint64_t address){return bytes_at(load(address+unit),load32(address));};
    const auto digest_of=[&](std::uint64_t algorithm){const auto pointer=load(algorithm);if(!pointer)throw CertificateError{87};const auto oid=read_ansi(memory,pointer,128);
        std::unique_ptr<ASN1_OBJECT,decltype(&ASN1_OBJECT_free)> object(OBJ_txt2obj(oid.c_str(),1),ASN1_OBJECT_free);const auto* md=object?EVP_get_digestbyobj(object.get()):nullptr;if(!md)throw CertificateError{0x80091002};return std::pair{oid,md};};
    // 呼叫端的內容片段（rgpb／rgcb）串接。
    const auto pieces=[&](std::uint64_t count,std::uint64_t pointers,std::uint64_t sizes){std::vector<std::uint8_t> out;if(count>65536 || (count && (!pointers || !sizes)))throw CertificateError{87};
        for(std::uint32_t i=0;i<count;++i){const auto part=bytes_at(load(pointers+std::uint64_t{i}*unit),load32(sizes+std::uint64_t{i}*4));if(out.size()+part.size()>output_limit)throw CertificateError{8};out.insert(out.end(),part.begin(),part.end());}return out;};
    // 兩段式輸出：緩衝區為 NULL 時只回報大小，不足時 ERROR_MORE_DATA。
    const auto output=[&](std::span<const std::uint8_t> data,std::uint64_t pointer,std::uint64_t size_pointer,std::size_t reported){
        if(!size_pointer){if(pointer)throw CertificateError{87};return;}const auto capacity=load32(size_pointer);memory.check(GuestAddress{size_pointer},4,Access::Write);
        if(!pointer){memory.store(GuestAddress{size_pointer},32,reported);return;}memory.store(GuestAddress{size_pointer},32,data.size());if(capacity<data.size())throw CertificateError{234};memory.write(GuestAddress{pointer},data);};
    const auto attributes_at=[&](std::uint64_t count,std::uint64_t table){std::vector<std::pair<std::string,std::vector<std::vector<std::uint8_t>>>> out;if(count>4096 || (count && !table))throw CertificateError{87};
        for(std::uint32_t i=0;i<count;++i){const auto entry=table+std::uint64_t{i}*3*unit;const auto oid=load(entry);if(!oid)throw CertificateError{87};std::pair<std::string,std::vector<std::vector<std::uint8_t>>> item{read_ansi(memory,oid,128),{}};
            const auto values=load32(entry+unit),list=load(entry+2*unit);if(values>4096 || (values && !list))throw CertificateError{87};for(std::uint32_t j=0;j<values;++j)item.second.push_back(blob_at(list+std::uint64_t{j}*2*unit));out.push_back(std::move(item));}return out;};
    // 金鑰 handle（CSP 依 key spec，NCrypt 為 CERT_NCRYPT_KEY_SPEC）的 DER。
    const auto key_der=[&](std::uint64_t handle,std::uint64_t spec)->std::pair<std::vector<std::uint8_t>,bool>{
        if(spec==UINT32_MAX){const auto found=runtime_.storage_crypto_keys.find(handle);if(found==runtime_.storage_crypto_keys.end())throw CertificateError{6};return {found->second.der,found->second.private_key};}
        const auto found=runtime_.legacy_crypto_user_keys.find({handle,static_cast<std::uint32_t>(spec)});if(found==runtime_.legacy_crypto_user_keys.end())throw CertificateError{0x8009000dU};return {found->second.der,found->second.private_key};};
    const auto make_signer=[&](const std::vector<std::uint8_t>& der,bool private_key,std::vector<std::uint8_t> issuer,std::vector<std::uint8_t> serial,const EVP_MD* md,const auto& authenticated,const auto& unauthenticated){
        MessageSignerInput signer;try{signer.key=cng::decode(der,private_key);}catch(const cng::Error& error){throw CertificateError{error.code};}if(!private_key)throw CertificateError{0x8009000dU};
        signer.issuer=std::move(issuer);signer.serial=std::move(serial);signer.md=md;for(const auto& [oid,values]:authenticated)signer.authenticated.push_back({oid,values});for(const auto& [oid,values]:unauthenticated)signer.unauthenticated.push_back({oid,values});return signer;};
    const auto context_der=[&](std::uint64_t context,bool crl)->const std::vector<std::uint8_t>&{const auto& table=crl?certs.crl_contexts:certs.contexts;const auto found=table.find(context);if(found==table.end())throw CertificateError{0x80070057};return found->second.der;};
    if(symbol=="CryptMsgOpenToEncode"){
        // (dwMsgEncodingType, dwFlags, dwMsgType, pvMsgEncodeInfo, pszInnerContentObjID, pStreamInfo)
        if((u32(0)&0xffff0000U)!=0x10000U || (u32(1)&~4U) || arg(5))throw CertificateError{50};
        if(arg(4) && read_ansi(memory,arg(4),128)!="1.2.840.113549.1.7.1")throw CertificateError{50};
        if(certs.messages.size()>=128)throw CertificateError{8};CryptMessage message;message.encoder=true;message.detached=(u32(1)&4U)!=0;message.type=u32(2);
        if(message.type==2){
            const auto info=arg(3);if(!info)throw CertificateError{87};if(load32(info)<8+5*unit)throw CertificateError{0x80070057};
            const auto signers=load32(info+4),table=load(info+8);if(signers>16 || (signers && !table))throw CertificateError{87};
            const auto stride=signers?load32(table):0U;if(signers && stride<12*unit)throw CertificateError{0x80070057};
            for(std::uint32_t i=0;i<signers;++i){
                const auto entry=table+std::uint64_t{i}*stride;const auto cert_info=load(entry+unit);if(!cert_info)throw CertificateError{87};
                const auto spec=load32(entry+3*unit);const auto [der,private_key]=key_der(load(entry+2*unit),spec);const auto [oid,md]=digest_of(entry+4*unit);(void)md;
                CryptMessageSigner signer;signer.key=der;signer.private_key=private_key;signer.issuer=blob_at(cert_info+6*unit);signer.serial=blob_at(cert_info+unit);signer.hash_oid=oid;
                signer.authenticated=attributes_at(load32(entry+8*unit),load(entry+9*unit));signer.unauthenticated=attributes_at(load32(entry+10*unit),load(entry+11*unit));
                if(!private_key)throw CertificateError{0x8009000dU};message.signers.push_back(std::move(signer));
            }
            const auto certificates=load32(info+8+unit),certificate_table=load(info+8+2*unit),crls=load32(info+8+3*unit),crl_table=load(info+8+4*unit);
            if(certificates>4096 || crls>4096 || (certificates && !certificate_table) || (crls && !crl_table))throw CertificateError{87};
            for(std::uint32_t i=0;i<certificates;++i){auto der=blob_at(certificate_table+std::uint64_t{i}*2*unit);(void)decode_certificate(der);message.certificates.push_back(std::move(der));}
            for(std::uint32_t i=0;i<crls;++i){auto der=blob_at(crl_table+std::uint64_t{i}*2*unit);(void)decode_crl(der);message.crls.push_back(std::move(der));}
        }else if(message.type==5){
            const auto info=arg(3);if(!info)throw CertificateError{87};if(load32(info)<5*unit)throw CertificateError{0x80070057};message.hash_oid=digest_of(info+2*unit).first;
        }else if(message.type!=1)throw CertificateError{50};
        result=runtime_.allocate_wait_handle();certs.messages.emplace(result,std::move(message));
    }else if(auto* encoding_message=encoder_message()){
        if(symbol=="CryptMsgUpdate"){
            // (hCryptMsg, pbData, cbData, fFinal)
            if(encoding_message->finalized)throw CertificateError{0x80091001};const auto data=bytes_at(arg(1),u32(2));if(encoding_message->content.size()+data.size()>output_limit)throw CertificateError{8};
            encoding_message->content.insert(encoding_message->content.end(),data.begin(),data.end());
            if(u32(3)){
                if(encoding_message->type==1){SignedMessage data_message(PKCS7_new(),PKCS7_free);
                    if(!data_message || !PKCS7_set_type(data_message.get(),NID_pkcs7_data) || !ASN1_OCTET_STRING_set(data_message->d.data,encoding_message->content.data(),static_cast<int>(encoding_message->content.size())))throw CertificateError{8};
                    encoding_message->encoded=encoded(data_message.get(),i2d_PKCS7);}
                else if(encoding_message->type==5){std::unique_ptr<ASN1_OBJECT,decltype(&ASN1_OBJECT_free)> object(OBJ_txt2obj(encoding_message->hash_oid.c_str(),1),ASN1_OBJECT_free);encoding_message->encoded=hash_pkcs7(encoding_message->content,encoding_message->detached,EVP_get_digestbyobj(object.get()));}
                else{std::vector<MessageSignerInput> inputs;for(const auto& signer:encoding_message->signers){std::unique_ptr<ASN1_OBJECT,decltype(&ASN1_OBJECT_free)> object(OBJ_txt2obj(signer.hash_oid.c_str(),1),ASN1_OBJECT_free);
                        inputs.push_back(make_signer(signer.key,signer.private_key,signer.issuer,signer.serial,EVP_get_digestbyobj(object.get()),signer.authenticated,signer.unauthenticated));}
                    encoding_message->encoded=sign_pkcs7(encoding_message->content,encoding_message->detached,inputs,encoding_message->certificates,encoding_message->crls);}
                encoding_message->finalized=true;
            }
            result=1;
        }else{
            // (hCryptMsg, dwParamType, dwIndex, pvData, pcbData)
            if(!arg(4))throw CertificateError{87};const auto kind=u32(1);
            if(kind==1){std::vector<std::uint8_t> value(4);for(unsigned i=0;i<4;++i)value[i]=static_cast<std::uint8_t>(encoding_message->type>>(8*i));output(value,arg(3),arg(4),4);}
            else{
                if(!encoding_message->finalized)throw CertificateError{0x80091001};
                if(kind==2)output(encoding_message->encoded,arg(3),arg(4),encoding_message->encoded.size());
                else if(kind==3){const auto* cursor=encoding_message->encoded.data();SignedMessage parsed(d2i_PKCS7(nullptr,&cursor,static_cast<long>(encoding_message->encoded.size())),PKCS7_free);if(!parsed)throw CertificateError{0x8009310b};
                    std::vector<std::uint8_t> bare;
                    if(PKCS7_type_is_signed(parsed.get()))bare=encoded(parsed->d.sign,i2d_PKCS7_SIGNED);else if(PKCS7_type_is_digest(parsed.get()))bare=encoded(parsed->d.digest,i2d_PKCS7_DIGEST);
                    else bare=encoded(parsed->d.data,i2d_ASN1_OCTET_STRING);output(bare,arg(3),arg(4),bare.size());}
                else{if(encoding_message->type==5)throw CertificateError{0x80091004};auto parsed=decode_message(encoding_message->encoded);auto packet=message_parameter(x86_,parsed.get(),kind,u32(2));
                    const auto capacity=load32(arg(4));memory.store(GuestAddress{arg(4)},32,packet.bytes.size());if(arg(3)){if(capacity<packet.bytes.size())throw CertificateError{234};packet.relocate(arg(3));memory.write(GuestAddress{arg(3)},packet.bytes);}}
            }
            result=1;
        }
    }else if(symbol=="CryptMsgControl"){
        // (hCryptMsg, dwFlags, dwCtrlType, pvCtrlPara)
        const auto found=certs.messages.find(arg(0));if(found==certs.messages.end())throw CertificateError{6};auto& message=found->second;if(!message.finalized)throw CertificateError{0x80091001};
        auto parsed=decode_message(message.encoded);if(!PKCS7_type_is_signed(parsed.get()))throw CertificateError{0x80091004};const auto control=u32(2);
        if(control==1 || control==19){
            if(!arg(3))throw CertificateError{87};int index=-1;cng::Key key(nullptr,EVP_PKEY_free);
            if(control==1){const auto [issuer,serial]=std::pair{blob_at(arg(3)+6*unit),blob_at(arg(3)+unit)};auto* signers=PKCS7_get_signer_info(parsed.get());
                for(int i=0;signers && i<sk_PKCS7_SIGNER_INFO_num(signers) && index<0;++i){const auto id=pkcs7_signer_id(parsed.get(),i);if(id.first==issuer && id.second==serial)index=i;}
                if(index<0)throw CertificateError{0x8009100eU};key=public_key(arg(3)+10*unit+16);}
            else{index=static_cast<int>(load32(arg(3)+2*unit));const auto type=load32(arg(3)+2*unit+4),signer=load(arg(3)+2*unit+8);if(!signer)throw CertificateError{87};
                std::uint64_t context=0;if(type==1)key=public_key(signer);else if(type==2)context=signer;
                else if(type==3){const auto chain=load(load(signer+16));const auto element=load(load(chain+16));context=load(element+unit);}else throw CertificateError{0x80070057};
                if(context){auto cert=decode_certificate(context_der(context,false));key=cng::Key(X509_get_pubkey(cert.get()),EVP_PKEY_free);if(!key)throw CertificateError{0x8009310b};}}
            if(const auto error=verify_pkcs7_signer(parsed.get(),index,key.get(),message_content(parsed.get())))throw CertificateError{error};result=1;
        }else if(control>=10 && control<=13){
            if(!arg(3))throw CertificateError{87};
            if(control==10){auto cert=decode_certificate(blob_at(arg(3)));if(!PKCS7_add_certificate(parsed.get(),cert.get()))throw CertificateError{8};}
            else if(control==12){auto crl=decode_crl(blob_at(arg(3)));if(!PKCS7_add_crl(parsed.get(),crl.get()))throw CertificateError{8};}
            else{const auto index=static_cast<int>(load32(arg(3)));
                if(control==11){auto* list=parsed->d.sign->cert;if(!list || index<0 || index>=sk_X509_num(list))throw CertificateError{0x80091008};X509_free(sk_X509_delete(list,index));}
                else{auto* list=parsed->d.sign->crl;if(!list || index<0 || index>=sk_X509_CRL_num(list))throw CertificateError{0x80091008};X509_CRL_free(sk_X509_CRL_delete(list,index));}}
            message.encoded=encoded(parsed.get(),i2d_PKCS7);result=1;
        }else throw CertificateError{0x8009100cU};
    }else if(symbol=="CryptGetMessageSignerCount"){
        // (dwMsgEncodingType, pbSignedBlob, cbSignedBlob)：錯誤時回傳 -1。
        try{auto parsed=decode_message(bytes_at(arg(1),u32(2)));if(!PKCS7_type_is_signed(parsed.get()))throw CertificateError{0x80091004};
            auto* signers=PKCS7_get_signer_info(parsed.get());result=static_cast<std::uint32_t>(signers?sk_PKCS7_SIGNER_INFO_num(signers):0);}
        catch(const CertificateError& error){last_error_=error.code;result=UINT32_MAX;}
    }else if(symbol=="CryptGetMessageCertificates"){
        // (dwMsgAndCertEncodingType, hCryptProv, dwFlags, pbSignedBlob, cbSignedBlob)：與 CertOpenStore(CERT_STORE_PROV_PKCS7) 相同。
        GuestScratch scratch(*this,memory,state);const auto store_blob=scratch.allocate(2*unit);if(!store_blob)throw CertificateError{8};memory.store(GuestAddress{store_blob},32,u32(4));memory.store(GuestAddress{store_blob+unit},bits,arg(3));
        result=nested_service(state,memory,"crypt32.dll","CertOpenStore",{5,arg(0),arg(1),arg(2),store_blob});if(x86_)result&=UINT32_MAX;if(!result)throw CertificateError{last_error_};
    }else if(symbol=="CryptSignMessage"){
        // (pSignPara, fDetachedSignature, cToBeSigned, rgpbToBeSigned, rgcbToBeSigned, pbSignedBlob, pcbSignedBlob)
        const auto para=arg(0);if(!para || !arg(6))throw CertificateError{87};if(load32(para)<8+13*unit+8)throw CertificateError{0x80070057};
        const bool detached=u32(1)!=0;if(!detached && u32(2)!=1)throw CertificateError{0x80070057};const auto content=pieces(u32(2),arg(3),arg(4));
        const auto cert_context=load(para+8);if(!cert_context)throw CertificateError{87};auto cert=decode_certificate(context_der(cert_context,false));
        auto serial=asn1_content(encoded(X509_get0_serialNumber(cert.get()),i2d_ASN1_INTEGER));std::reverse(serial.begin(),serial.end());const auto issuer=encoded(X509_get_issuer_name(cert.get()),i2d_X509_NAME);
        const auto [oid,md]=digest_of(para+8+unit);(void)oid;
        std::vector<std::vector<std::uint8_t>> certificates,crls;
        const auto certificate_count=load32(para+8+5*unit),certificate_table=load(para+8+6*unit),crl_count=load32(para+8+7*unit),crl_table=load(para+8+8*unit);
        if(certificate_count>4096 || crl_count>4096 || (certificate_count && !certificate_table) || (crl_count && !crl_table))throw CertificateError{87};
        for(std::uint32_t i=0;i<certificate_count;++i)certificates.push_back(context_der(load(certificate_table+std::uint64_t{i}*unit),false));
        for(std::uint32_t i=0;i<crl_count;++i)crls.push_back(context_der(load(crl_table+std::uint64_t{i}*unit),true));
        const auto authenticated=attributes_at(load32(para+8+9*unit),load(para+8+10*unit)),unauthenticated=attributes_at(load32(para+8+11*unit),load(para+8+12*unit));
        // 私鑰：CryptAcquireCertificatePrivateKey（允許 NCrypt、靜默）。
        GuestScratch scratch(*this,memory,state);const auto slots=scratch.allocate(3*8);if(!slots)throw CertificateError{8};memory.fill(GuestAddress{slots},0,24);
        if(!(nested_service(state,memory,"crypt32.dll","CryptAcquireCertificatePrivateKey",{cert_context,0x10040U,0,slots,slots+8,slots+16})&UINT32_MAX))throw CertificateError{last_error_};
        const auto handle=load(slots);const auto spec=load32(slots+8);const bool owned=load32(slots+16)!=0;
        std::vector<std::uint8_t> signed_message;std::uint32_t failure=0;
        try{const auto [der,private_key]=key_der(handle,spec);std::vector<MessageSignerInput> inputs;inputs.push_back(make_signer(der,private_key,issuer,serial,md,authenticated,unauthenticated));
            const bool ec=EVP_PKEY_is_a(inputs.front().key.get(),"EC");signed_message=sign_pkcs7(content,detached,inputs,certificates,crls);
            if(owned)(void)nested_service(state,memory,spec==UINT32_MAX?"ncrypt.dll":"advapi32.dll",spec==UINT32_MAX?"NCryptFreeObject":"CryptReleaseContext",spec==UINT32_MAX?std::initializer_list<std::uint64_t>{handle}:std::initializer_list<std::uint64_t>{handle,0});
            // ECDSA 簽章長度每次不同：只查大小時多預留，實際寫入時回報真實長度。
            output(signed_message,arg(5),arg(6),signed_message.size()+(ec?16:0));result=1;
        }catch(const CertificateError& error){failure=error.code;}
        if(failure){if(owned && !signed_message.size())(void)nested_service(state,memory,spec==UINT32_MAX?"ncrypt.dll":"advapi32.dll",spec==UINT32_MAX?"NCryptFreeObject":"CryptReleaseContext",spec==UINT32_MAX?std::initializer_list<std::uint64_t>{handle}:std::initializer_list<std::uint64_t>{handle,0});throw CertificateError{failure};}
    }else if(symbol=="CryptVerifyMessageSignature" || symbol=="CryptVerifyDetachedMessageSignature"){
        // Verify(pVerifyPara, dwSignerIndex, pbSignedBlob, cbSignedBlob, pbDecoded, pcbDecoded, ppSignerCert)
        // Detached(pVerifyPara, dwSignerIndex, pbDetachedSignBlob, cbDetachedSignBlob, cToBeSigned, rgpbToBeSigned, rgcbToBeSigned, ppSignerCert)
        // 回呼路徑在續行完成後會重新進入本服務取回結果：重播時不可重做輸出清除、開啟 store 與配置。
        if(service_callback::completed(this,threads_.current,service.symbol,state))
            return nested_continuation(state,memory,service.symbol,[](GuestState&,GuestMemory&)->service_callback::Task<std::optional<std::uint32_t>>{throw HostCallFault("服務續行重播不應重新驗章");});
        const bool detached=symbol=="CryptVerifyDetachedMessageSignature";const auto para=arg(0),signer_out=arg(detached?7:6);
        if(!para)throw CertificateError{87};if(load32(para)<8+3*unit)throw CertificateError{0x80070057};if(signer_out)memory.store(GuestAddress{signer_out},bits,0);
        const auto signed_blob=bytes_at(arg(2),u32(3));auto parsed=decode_message(signed_blob);if(!PKCS7_type_is_signed(parsed.get()))throw CertificateError{0x80091004};
        const auto index=static_cast<int>(u32(1));(void)pkcs7_signer_id(parsed.get(),index);
        const auto content=detached?pieces(u32(4),arg(5),arg(6)):message_content(parsed.get());const auto callback=load(para+8+unit),callback_argument=load(para+8+2*unit),encoding=load32(para+4)&UINT32_MAX;
        const auto finish_outputs=[&](GuestMemory& m,std::uint64_t context){
            if(!detached){if(arg(5)){const auto capacity=static_cast<std::uint32_t>(m.load(GuestAddress{arg(5)},32));m.store(GuestAddress{arg(5)},32,content.size());
                if(arg(4)){if(capacity<content.size())throw CertificateError{234};if(!content.empty())m.write(GuestAddress{arg(4)},content);}}}
            if(signer_out)m.store(GuestAddress{signer_out},bits,context);};
        if(!callback){
            const auto der=pkcs7_signer_certificate(parsed.get(),index);if(!der)throw CertificateError{0x80092004U};
            auto cert=decode_certificate(*der);cng::Key key(X509_get_pubkey(cert.get()),EVP_PKEY_free);if(!key)throw CertificateError{0x8009310b};
            if(const auto error=verify_pkcs7_signer(parsed.get(),index,key.get(),content))throw CertificateError{error};
            std::uint64_t context=0;if(signer_out)context=create_certificate(memory,*der);
            try{finish_outputs(memory,context);}catch(...){if(context)(void)free_certificate(state,memory,context);throw;}result=1;
        }else{
            // 回呼（pvGetArg, dwCertEncodingType, pSignerId, hMsgCertStore）回傳簽署者憑證 context。
            const auto [issuer,serial]=pkcs7_signer_id(parsed.get(),index);const auto info_size=static_cast<std::size_t>(x86_?112U:208U);
            const auto block=allocate_heap(memory,info_size+issuer.size()+serial.size()+signed_blob.size()+2*unit);if(!block)throw CertificateError{8};memory.fill(GuestAddress{block},0,info_size);
            const auto serial_at=block+info_size,issuer_at=serial_at+serial.size(),blob_data=issuer_at+issuer.size(),blob_header=blob_data+signed_blob.size();
            memory.write(GuestAddress{serial_at},serial);memory.write(GuestAddress{issuer_at},issuer);memory.write(GuestAddress{blob_data},signed_blob);
            memory.store(GuestAddress{block+unit},32,serial.size());memory.store(GuestAddress{block+2*unit},bits,serial_at);memory.store(GuestAddress{block+6*unit},32,issuer.size());memory.store(GuestAddress{block+7*unit},bits,issuer_at);
            memory.store(GuestAddress{blob_header},32,signed_blob.size());memory.store(GuestAddress{blob_header+unit},bits,blob_data);
            auto store=nested_service(state,memory,"crypt32.dll","CertOpenStore",{5,encoding?encoding:0x10001U,0,0,blob_header});if(x86_)store&=UINT32_MAX;
            if(!store){(void)free_heap(memory,block,state);throw CertificateError{last_error_};}
            // 續行在本函式返回後才執行：只擷取值，不擷取參照。
            const auto message_bytes=signed_blob;const auto decoded_out=detached?0:arg(4),decoded_size=detached?0:arg(5);const auto width=bits;
            return nested_continuation(state,memory,service.symbol,[this,service,callback,callback_argument,encoding,block,store,message_bytes,content,index,signer_out,decoded_out,decoded_size,width](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall call(s,m,service);std::uint64_t value=0;std::uint32_t error=0;
                auto context=co_await service_callback::call_value(callback,{callback_argument,encoding?encoding:0x10001U,block,store});if(x86_)context&=UINT32_MAX;
                try{
                    if(!context)throw CertificateError{last_error_?last_error_:0x80092004U};
                    const auto found=runtime_.certificates.contexts.find(context);if(found==runtime_.certificates.contexts.end())throw CertificateError{0x80070057};
                    auto cert=decode_certificate(found->second.der);cng::Key key(X509_get_pubkey(cert.get()),EVP_PKEY_free);auto parsed=decode_message(message_bytes);
                    if(const auto failure=verify_pkcs7_signer(parsed.get(),index,key.get(),content))throw CertificateError{failure};
                    if(decoded_size){const auto capacity=static_cast<std::uint32_t>(m.load(GuestAddress{decoded_size},32));m.store(GuestAddress{decoded_size},32,content.size());
                        if(decoded_out){if(capacity<content.size())throw CertificateError{234};if(!content.empty())m.write(GuestAddress{decoded_out},content);}}
                    if(signer_out)m.store(GuestAddress{signer_out},width,context);value=1;
                }catch(const CertificateError& failure){error=failure.code;}catch(const MemoryFault&){error=87;}
                if(context && (!value || !signer_out))(void)free_certificate(s,m,context);
                (void)nested_service(s,m,"crypt32.dll","CertCloseStore",{store,0});(void)free_heap(m,block,s);
                if(error)last_error_=error;call.return_to(s,value);co_return std::nullopt;
            });
        }
        (void)detached;
    }else if(symbol=="CryptMsgGetAndVerifySigner"){
        // (hCryptMsg, cSignerStore, rghSignerStore, dwFlags, ppSigner, pdwSignerIndex)：CMSG_TRUSTED_SIGNER_FLAG 只用指定的 store，CMSG_SIGNER_ONLY_FLAG 不驗章，CMSG_USE_SIGNER_INDEX_FLAG 只看指定索引。
        const auto found=certs.messages.find(arg(0));if(found==certs.messages.end())throw CertificateError{6};if(!found->second.finalized)throw CertificateError{0x80091001};
        auto parsed=decode_message(found->second.encoded);if(!PKCS7_type_is_signed(parsed.get()))throw CertificateError{0x80091004};const auto flags=u32(3);if(flags&~7U)throw CertificateError{0x80070057};
        if(arg(4))memory.store(GuestAddress{arg(4)},bits,0);std::vector<std::vector<std::uint8_t>> extra;
        if(u32(1)>128 || (u32(1) && !arg(2)))throw CertificateError{87};
        for(std::uint32_t i=0;i<u32(1);++i){const auto handle=load(arg(2)+std::uint64_t{i}*unit);if(!certs.stores.contains(handle))throw CertificateError{6};for(const auto& entry:store_entries(certs,handle))extra.push_back(*entry.der);}
        auto* signers=PKCS7_get_signer_info(parsed.get());const int count=signers?sk_PKCS7_SIGNER_INFO_num(signers):0;std::uint32_t last=0x8009200bU;
        int first=0,end=count;if(flags&4U){if(!arg(5))throw CertificateError{87};first=static_cast<int>(load32(arg(5)));end=first+1;if(first>=count)throw CertificateError{0x8009100eU};}
        for(int index=first;index<end && !result;++index){
            std::optional<std::vector<std::uint8_t>> der;
            if(flags&1U){SignedMessage only(PKCS7_dup(parsed.get()),PKCS7_free);if(only && only->d.sign->cert){sk_X509_pop_free(only->d.sign->cert,X509_free);only->d.sign->cert=nullptr;}der=pkcs7_signer_certificate(only.get(),index,extra);}
            else der=pkcs7_signer_certificate(parsed.get(),index,extra);
            if(!der)continue;
            if(!(flags&2U)){auto cert=decode_certificate(*der);cng::Key key(X509_get_pubkey(cert.get()),EVP_PKEY_free);if(const auto error=verify_pkcs7_signer(parsed.get(),index,key.get(),message_content(parsed.get()))){last=error;continue;}}
            if(arg(4))memory.store(GuestAddress{arg(4)},bits,create_certificate(memory,*der));if(arg(5))memory.store(GuestAddress{arg(5)},32,static_cast<std::uint32_t>(index));result=1;
        }
        if(!result)throw CertificateError{last};
    }else{
        // CryptHashMessage(pHashPara, fDetachedHash, cToBeHashed, rgpbToBeHashed, rgcbToBeHashed, pbHashedBlob, pcbHashedBlob, pbComputedHash, pcbComputedHash)
        // CryptVerifyMessageHash(pHashPara, pbHashedBlob, cbHashedBlob, pbToBeHashed, pcbToBeHashed, pbComputedHash, pcbComputedHash)
        // CryptVerifyDetachedMessageHash(pHashPara, pbDetachedHashBlob, cbDetachedHashBlob, cToBeHashed, rgpbToBeHashed, rgcbToBeHashed, pbComputedHash, pcbComputedHash)
        const auto para=arg(0);if(!para)throw CertificateError{87};if(load32(para)<8+4*unit)throw CertificateError{0x80070057};
        if(symbol=="CryptHashMessage"){
            const bool detached=u32(1)!=0;if(!detached && u32(2)!=1)throw CertificateError{0x80070057};const auto content=pieces(u32(2),arg(3),arg(4));const auto [oid,md]=digest_of(para+8+unit);(void)oid;
            const auto hashed=hash_pkcs7(content,detached,md);output(hashed,arg(5),arg(6),hashed.size());const auto digest=message_digest(md,content);output(digest,arg(7),arg(8),digest.size());result=1;
        }else{
            const bool detached=symbol=="CryptVerifyDetachedMessageHash";const auto hashed_blob=bytes_at(arg(1),u32(2));const auto* cursor=hashed_blob.data();
            SignedMessage parsed(d2i_PKCS7(nullptr,&cursor,static_cast<long>(hashed_blob.size())),PKCS7_free);if(!parsed || cursor!=hashed_blob.data()+hashed_blob.size()){ERR_clear_error();throw CertificateError{0x8009310b};}
            if(!PKCS7_type_is_digest(parsed.get()))throw CertificateError{0x80091004};const auto* md=EVP_get_digestbyobj(parsed->d.digest->md->algorithm);if(!md)throw CertificateError{0x80091002};
            std::vector<std::uint8_t> content;
            if(detached)content=pieces(u32(3),arg(4),arg(5));
            else{auto* inner=parsed->d.digest->contents;if(!inner || !PKCS7_type_is_data(inner) || !inner->d.data)throw CertificateError{0x80091004};content.assign(ASN1_STRING_get0_data(inner->d.data),ASN1_STRING_get0_data(inner->d.data)+ASN1_STRING_length(inner->d.data));}
            const auto digest=message_digest(md,content);const auto* expected=parsed->d.digest->digest;
            if(!expected || static_cast<std::size_t>(ASN1_STRING_length(expected))!=digest.size() || !std::equal(digest.begin(),digest.end(),ASN1_STRING_get0_data(expected)))throw CertificateError{0x80091007U};
            if(!detached)output(content,arg(3),arg(4),content.size());output(digest,arg(detached?6:5),arg(detached?7:6),digest.size());result=1;
        }
    }
    arg.return_to(state,result);return std::nullopt;
}
}
