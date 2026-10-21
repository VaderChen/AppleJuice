}else if(symbol=="CertSaveStore"){
    const auto store=certs.stores.find(arg(0));if(store==certs.stores.end() || !store->second.references)throw CertificateError{6};
    const auto format=u32(2),destination=u32(3);if(u32(5) || format<1 || format>2 || destination<1 || destination>4 || !arg(4))throw CertificateError{87};
    if(format==2 && (u32(1)&0xffff0000U)!=0x10000U)throw CertificateError{0x80070057};
    auto_resync_store(certs,arg(0),filesystem_);const auto bytes=save_certificate_store(certs,arg(0),format==2,output_limit);
    if(destination==2){
        const auto capacity=memory.load(GuestAddress{arg(4)},32),target=memory.load(GuestAddress{arg(4)+unit},bits);
        memory.store(GuestAddress{arg(4)},32,bytes.size());if(target){if(capacity<bytes.size())throw CertificateError{234};memory.write(GuestAddress{target},bytes);}result=1;
    }else {
        const auto write=[&](HostFile& file,std::uint64_t& position){
            std::size_t done=0;while(done<bytes.size()){
                const auto io=file.write(std::span<const std::uint8_t>(bytes).subspan(done),position);position+=io.bytes;done+=io.bytes;
                if(io.error)throw CertificateError{io.error};if(!io.bytes)throw CertificateError{29};
            }
        };
        if(destination==1){
            const auto found=files_.find(arg(4));if(found==files_.end())throw CertificateError{6};auto& file=found->second;
            if(!(file.access&(0x40000000U|0x10000000U|2U|4U)))throw CertificateError{5};
            if(!file.native)throw CertificateError{50};
            auto& cursor=runtime_.win_file_positions.at(file.object);const auto guard=cursor.lock();
            std::uint64_t position=format==1?0:static_cast<std::uint64_t>(cursor);
            if(!(file.access&(0x40000000U|0x10000000U|2U)) && (file.access&4U))position=file.native->information().metadata.size;
            try{write(*file.native,position);}catch(...){cursor=position;throw;}cursor=position;file.metadata=file.native->information().metadata;
        }else {
            const auto path=destination==4?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(4)})):read_ansi(memory,arg(4));
            const auto opened=filesystem_.open_host(path,0x40000000U,2,0,0);if(opened.error || !opened.file)throw CertificateError{opened.error?opened.error:2U};
            std::uint64_t position=0;write(*opened.file,position);if(const auto error=opened.file->flush())throw CertificateError{error};
        }
        result=1;
    }
