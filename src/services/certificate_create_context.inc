if(symbol=="CertCreateContext"){
    // (dwContextType, dwEncodingType, pbEncoded, cbEncoded, dwFlags, pCreatePara)：憑證（1）與 CRL（2）同 CertCreateCertificateContext／CertCreateCRLContext，
    // context 一律保存自己的編碼複本。CERT_CREATE_CONTEXT_NOCOPY_FLAG 且 CERT_CREATE_CONTEXT_PARA 有 pfnFree 時，pbEncoded 的擁有權已交給 context，
    // 建立後（失敗時亦同）立即以 pfnFree(pvFree，沒有時為 pbEncoded) 釋放，之後再設定錯誤碼。SORTED／NO_HCRYPTMSG／NO_ENTRY 只影響 CTL；CTL（3）尚未提供 CTL context。
    return nested_continuation(state,memory,"CertCreateContext",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall call(s,m,service);const unsigned size=x86_?4U:8U;const auto width=static_cast<std::uint8_t>(size*8);
        std::uint64_t release_function=0,release_argument=0,created=0;std::uint32_t error=0;
        try{
            const auto type=static_cast<std::uint32_t>(call(0)),flags=static_cast<std::uint32_t>(call(4)),encoding=static_cast<std::uint32_t>(call(1)),length=static_cast<std::uint32_t>(call(3));
            if(call(5)){
                // CERT_CREATE_CONTEXT_PARA：cbSize、pfnFree、pvFree、pfnSort、pvSort。
                if(m.load(GuestAddress{call(5)},32)<3U*size)throw CertificateError{0x80070057U};
                if(flags&1U){release_function=m.load(GuestAddress{call(5)+size},width);release_argument=m.load(GuestAddress{call(5)+2*size},width);if(!release_argument)release_argument=call(2);}
            }
            if((flags&~0xfU) || (type!=1 && type!=2))throw CertificateError{0x80070057U};
            if((encoding&0xffffU)!=1)throw CertificateError{0x80070057U};if(!call(2) || !length)throw CertificateError{0x8009310bU};if(length>output_limit)throw CertificateError{8};
            std::vector<std::uint8_t> der(length);m.read(GuestAddress{call(2)},der);
            created=create_certificate(m,der,0,0,0,0,type==2);
        }catch(const CertificateError& failure){error=failure.code;}catch(const MemoryFault&){error=87;}catch(const std::bad_alloc&){error=8;}
        if(release_function)(void)co_await service_callback::call(release_function,{release_argument});
        if(error)last_error_=error;call.return_to(s,error?0:created);co_return std::optional<std::uint32_t>{};
    });
}
