        // Authenticode 結構的 WVTAsn1Spc* 編解碼（SPC_LINK、SPC_PE_IMAGE_DATA、SPC_INDIRECT_DATA_CONTENT、SPC_SP_OPUS_INFO、SPC_FINANCIAL_CRITERIA），
        // 以及 WTHelperGetFileHandle／GetFileName、WintrustGet／SetRegPolicyFlags 與 TrustIsCertificateSelfSigned。DER 由 catalog_codec.hpp 處理。
        // 編碼：(EncodingType, StructType, pInfo, pbEncoded, pcbEncoded)；解碼：(EncodingType, StructType, pbEncoded, cbEncoded, Flags, pvStructInfo, pcbStructInfo)，
        // 結構之後依指標寬度對齊放入字串與資料；輸出緩衝區為 NULL 時只回報大小，不足時 ERROR_MORE_DATA。
        if(symbol.starts_with("WVTAsn1Spc")){
            namespace codec=catalog_codec;
            const bool encode=symbol.ends_with("Encode");
            const auto kind=symbol.substr(10,symbol.size()-10-6); // Link、PeImageData、IndirectDataContent、SpOpusInfo、FinancialCriteriaInfo
            const auto blob_bytes=[&](std::uint64_t base)->std::vector<std::uint8_t>{const auto count=load(base,0);const auto pointer=load(base,unit,bits);require(count<=1048576 && (!count || pointer),87);
                std::vector<std::uint8_t> bytes(count);if(count)memory.read(GuestAddress{pointer},bytes);return bytes;};
            const auto wide_text=[&](std::uint64_t pointer){require(pointer,87);return read_utf16(memory,GuestAddress{pointer},32768);};
            const auto read_link=[&](std::uint64_t at)->codec::Link{
                require(at,87);codec::Link link;link.choice=static_cast<std::uint32_t>(load(at,0));
                if(link.choice==1 || link.choice==3)link.text=wide_text(load(at,unit,bits));
                else if(link.choice==2){memory.read(GuestAddress{at+unit},link.class_id);link.serialized=blob_bytes(at+unit+16);}
                else require(false,0x80092002U);
                return link;};
            const auto deliver=[&](std::uint64_t buffer,std::uint64_t size_at,const std::vector<std::uint8_t>& bytes)->std::uint64_t{
                require(size_at,87);memory.check(GuestAddress{size_at},4,Access::Write);const auto available=load(size_at,0);put(size_at,0,bytes.size());
                if(!buffer){last_error_=0;return 1;}if(available<bytes.size()){last_error_=234;return 0;}memory.write(GuestAddress{buffer},bytes);last_error_=0;return 1;};
            if(encode){
                require(arg(2),87);std::optional<std::vector<std::uint8_t>> bytes;const auto info=arg(2);
                if(kind=="Link")bytes=codec::encode_link(read_link(info));
                else if(kind=="PeImageData"){codec::PeImageData image;image.flags=blob_bytes(info);image.unused_bits=static_cast<std::uint32_t>(load(info,2*unit));require(image.unused_bits<8,87);
                    if(const auto file=load(info,3*unit,bits))image.file=read_link(file);bytes=codec::encode_pe_image(image);}
                else if(kind=="IndirectDataContent"){const auto ansi=[&](std::uint64_t pointer){require(pointer,87);return read_ansi(memory,pointer,256);};
                    bytes=codec::encode_indirect({ansi(load(info,0,bits)),blob_bytes(info+unit),ansi(load(info,3*unit,bits)),blob_bytes(info+4*unit),blob_bytes(info+6*unit)});}
                else if(kind=="SpOpusInfo"){codec::OpusInfo opus;if(const auto program=load(info,0,bits))opus.program=wide_text(program);
                    if(const auto more=load(info,unit,bits))opus.more=read_link(more);if(const auto publisher=load(info,2*unit,bits))opus.publisher=read_link(publisher);bytes=codec::encode_opus(opus);}
                else if(kind=="FinancialCriteriaInfo")bytes=codec::encode_financial(load(info,0)!=0,load(info,4)!=0);
                else throw HostCallFault("未實作 WVTAsn1 結構");
                require(bytes.has_value(),0x80092002U);return deliver(arg(3),arg(4),*bytes);
            }
            require(arg(2) && arg(3)<=1048576,87);std::vector<std::uint8_t> encoded(static_cast<std::size_t>(arg(3)));if(!encoded.empty())memory.read(GuestAddress{arg(2)},encoded);
            // 輸出版面：先放結構，其餘依指標寬度對齊依序附加；pointer 欄位寫入「輸出緩衝區位址＋位移」。
            const auto base=arg(5);std::vector<std::uint8_t> out;
            const auto reserve=[&](std::size_t size){while(out.size()%unit)out.push_back(0);const auto at=out.size();out.resize(at+size);return at;};
            const auto store=[&](std::size_t at,std::uint64_t value,unsigned width){for(unsigned i=0;i<width/8;++i)out[at+i]=static_cast<std::uint8_t>(value>>(8*i));};
            const auto address=[&](std::size_t at){return base+at;};
            const auto append=[&](std::span<const std::uint8_t> bytes){const auto at=reserve(bytes.size());std::copy(bytes.begin(),bytes.end(),out.begin()+static_cast<std::ptrdiff_t>(at));return address(at);};
            const auto append_wide=[&](const std::u16string& text){return append(utf16_bytes(text));};
            const auto append_ansi=[&](const std::string& text){std::vector<std::uint8_t> bytes(text.begin(),text.end());bytes.push_back(0);return append(bytes);};
            const auto put_blob=[&](std::size_t at,std::span<const std::uint8_t> bytes){store(at,bytes.size(),32);store(at+unit,bytes.empty()?0:append(bytes),bits);};
            const unsigned link_size=x86_?28U:40U;
            const auto write_link=[&](std::size_t at,const codec::Link& link){
                store(at,link.choice,32);
                if(link.choice==2){std::copy(link.class_id.begin(),link.class_id.end(),out.begin()+static_cast<std::ptrdiff_t>(at+unit));put_blob(at+unit+16,link.serialized);}
                else store(at+unit,append_wide(link.text),bits);};
            if(kind=="Link"){const auto link=codec::decode_link(encoded);require(link.has_value(),0x8009310bU);const auto at=reserve(link_size);write_link(at,*link);}
            else if(kind=="PeImageData"){
                const auto image=codec::decode_pe_image(encoded);require(image.has_value(),0x8009310bU);const auto at=reserve(x86_?16U:32U);
                put_blob(at,image->flags);store(at+2*unit,image->unused_bits,32);
                if(image->file){const auto link_at=reserve(link_size);store(at+3*unit,address(link_at),bits);write_link(link_at,*image->file);}
            }
            else if(kind=="IndirectDataContent"){
                const auto data=codec::decode_indirect(encoded);require(data.has_value(),0x8009310bU);const auto at=reserve(x86_?32U:64U);
                store(at,append_ansi(data->type),bits);put_blob(at+unit,data->value);store(at+3*unit,append_ansi(data->algorithm),bits);put_blob(at+4*unit,data->parameters);put_blob(at+6*unit,data->digest);
            }
            else if(kind=="SpOpusInfo"){
                const auto opus=codec::decode_opus(encoded);require(opus.has_value(),0x8009310bU);const auto at=reserve(3U*unit);
                if(opus->program)store(at,append_wide(*opus->program),bits);
                if(opus->more){const auto link_at=reserve(link_size);store(at+unit,address(link_at),bits);write_link(link_at,*opus->more);}
                if(opus->publisher){const auto link_at=reserve(link_size);store(at+2*unit,address(link_at),bits);write_link(link_at,*opus->publisher);}
            }
            else if(kind=="FinancialCriteriaInfo"){const auto value=codec::decode_financial(encoded);require(value.has_value(),0x8009310bU);const auto at=reserve(8);store(at,value->first?1:0,32);store(at+4,value->second?1:0,32);}
            else throw HostCallFault("未實作 WVTAsn1 結構");
            return deliver(base,arg(6),out);
        }
        if(symbol=="WTHelperGetFileHandle" || symbol=="WTHelperGetFileName"){
            // (WINTRUST_DATA*)：dwUnionChoice 為 WTD_CHOICE_FILE（1）時回傳 WINTRUST_FILE_INFO 的 hFile／pcwszFilePath；其他回 INVALID_HANDLE_VALUE／NULL。
            const bool handle=symbol=="WTHelperGetFileHandle";const auto none=handle?(x86_?0xffffffffULL:~0ULL):0ULL;
            if(!arg(0) || load(arg(0),x86_?20:32)!=1)return none;const auto file=load(arg(0),x86_?24:40,bits);if(!file)return none;
            return load(file,handle?2*unit:unit,bits);
        }
        if(symbol=="WintrustGetRegPolicyFlags" || symbol=="WintrustSetRegPolicyFlags"){
            // 軟體發行者信任提供者的 State（DWORD）：HKCU\Software\Microsoft\Windows\CurrentVersion\WinTrust\Trust Providers\Software Publishing；沒有值時為 0。
            GuestScratch scratch(*this,memory,state);const auto key_at=scratch.allocate(unit),value_at=scratch.allocate(4),size_at=scratch.allocate(4);
            const auto subkey=scratch.text(u"Software\\Microsoft\\Windows\\CurrentVersion\\WinTrust\\Trust Providers\\Software Publishing"),name=scratch.text(u"State");
            require(key_at && value_at && size_at && subkey && name,8);const std::uint64_t user=x86_?0x80000001ULL:0xffffffff80000001ULL;
            if(symbol=="WintrustGetRegPolicyFlags"){
                require(arg(0),87);memory.check(GuestAddress{arg(0)},4,Access::Write);put(size_at,0,4);put(value_at,0,0);
                if(nested_service(state,memory,"advapi32.dll","RegGetValueW",{user,subkey,name,0x10,0,value_at,size_at}))put(value_at,0,0);
                put(arg(0),0,load(value_at,0));last_error_=0;return 0;
            }
            if(const auto error=static_cast<std::uint32_t>(nested_service(state,memory,"advapi32.dll","RegCreateKeyExW",{user,subkey,0,0,0,0x20006,0,key_at,0})))require(false,error);
            const auto key=load(key_at,0,bits);put(value_at,0,static_cast<std::uint32_t>(arg(0)));
            const auto error=static_cast<std::uint32_t>(nested_service(state,memory,"advapi32.dll","RegSetValueExW",{key,name,0,4,value_at,4}));(void)nested_service(state,memory,"advapi32.dll","RegCloseKey",{key});
            require(!error,error);last_error_=0;return 1;
        }
        if(symbol=="TrustIsCertificateSelfSigned"){
            // (CERT_CONTEXT*, EncodingType, Flags)：主體與簽發者相同且簽章能以本身公開金鑰驗證。
            require(arg(0),87);const auto encoded=load(arg(0),unit,bits);const auto size=load(arg(0),2*unit);require(encoded && size && size<=1048576,87);
            std::vector<std::uint8_t> der(size);memory.read(GuestAddress{encoded},der);last_error_=0;return certificate_self_signed(der)?1:0;
        }
