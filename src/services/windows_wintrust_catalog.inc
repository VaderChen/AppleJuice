        // 簽署目錄的讀寫（CryptCAT*）與 CAT_NAMEVALUE／CAT_MEMBERINFO 的 WVTAsn1 編解碼；DER 由 catalog_codec.hpp 處理。
        // 回傳的 CRYPTCATMEMBER／CRYPTCATATTRIBUTE（含字串、SIP_INDIRECT_DATA 與編碼後資料）配置在程序堆積，保留到 CryptCATClose。
        if(symbol.starts_with("CryptCAT") || symbol.starts_with("WVTAsn1Cat")){
            namespace codec=catalog_codec;
            const auto invalid=x86_?0xffffffffULL:~0ULL;
            const auto document_of=[&](std::uint64_t handle)->CatalogDocument&{const auto found=runtime_.catalog_documents.find(handle);require(found!=runtime_.catalog_documents.end(),87);return found->second;};
            const auto keep=[&](CatalogDocument& document,std::span<const std::uint8_t> bytes)->std::uint64_t{
                const auto at=allocate_heap(memory,std::max<std::size_t>(bytes.size(),1));require(at!=0,8);document.allocations.push_back(at);if(!bytes.empty())memory.write(GuestAddress{at},bytes);return at;};
            const auto keep_text=[&](CatalogDocument& document,const std::u16string& text){return keep(document,utf16_bytes(text));};
            const auto keep_ansi=[&](CatalogDocument& document,const std::string& text){std::vector<std::uint8_t> bytes(text.begin(),text.end());bytes.push_back(0);return keep(document,bytes);};
            const auto tag_of=[](std::span<const std::uint8_t> identifier){std::u16string tag;for(std::size_t i=0;i+1<identifier.size();i+=2)tag.push_back(static_cast<char16_t>(identifier[i]|(identifier[i+1]<<8)));while(!tag.empty() && !tag.back())tag.pop_back();return tag;};
            const auto folded=[](std::u16string text){for(auto& c:text)if(c>=u'a' && c<=u'z')c=static_cast<char16_t>(c-32);return text;};
            const auto blob=[&](std::uint64_t at,std::uint64_t count,std::uint64_t pointer){put(at,0,count);put(at,unit,pointer,bits);};
            // 寫出 CRYPTCATATTRIBUTE（cbStruct、標記、旗標、資料）。
            const auto materialize_attribute=[&](CatalogDocument& document,CatalogAttributeEntry& entry)->std::uint64_t{
                const unsigned size=x86_?24U:40U;if(!entry.guest){entry.guest=keep(document,std::vector<std::uint8_t>(size));}
                memory.fill(GuestAddress{entry.guest},0,size);put(entry.guest,0,size);put(entry.guest,unit,keep_text(document,entry.tag),bits);
                put(entry.guest,2*unit,entry.flags);put(entry.guest,2*unit+4,entry.value.size());put(entry.guest,2*unit+8,entry.value.empty()?0:keep(document,entry.value),bits);
                return entry.guest;};
            // SpcIndirectDataContent 解成 SIP_INDIRECT_DATA（Data、DigestAlgorithm、Digest）。
            const auto indirect_struct=[&](CatalogDocument& document,const std::vector<std::uint8_t>& encoded)->std::uint64_t{
                const auto decoded=codec::decode_indirect(encoded);if(!decoded)return 0;
                const unsigned size=x86_?32U:64U;const auto at=keep(document,std::vector<std::uint8_t>(size));
                put(at,0,keep_ansi(document,decoded->type),bits);if(!decoded->value.empty())blob(at+unit,decoded->value.size(),keep(document,decoded->value));
                put(at,3*unit,keep_ansi(document,decoded->algorithm),bits);if(!decoded->parameters.empty())blob(at+4*unit,decoded->parameters.size(),keep(document,decoded->parameters));
                blob(at+6*unit,decoded->digest.size(),keep(document,decoded->digest));return at;};
            // 寫出 CRYPTCATMEMBER（x64 104 位元組、x86 64 位元組）。
            const auto materialize_member=[&](CatalogDocument& document,CatalogMemberEntry& entry)->std::uint64_t{
                const unsigned size=x86_?64U:104U;if(!entry.guest)entry.guest=keep(document,std::vector<std::uint8_t>(size));const auto at=entry.guest;
                memory.fill(GuestAddress{at},0,size);put(at,0,size);put(at,unit,keep_text(document,entry.tag),bits);put(at,2*unit,entry.file.empty()?0:keep_text(document,entry.file),bits);
                const unsigned guid_at=x86_?12U:24U;if(const auto guid=parse_windows_guid(entry.subject,true))memory.write(GuestAddress{at+guid_at},*guid);
                const unsigned indirect_at=x86_?32U:48U,version_at=x86_?36U:56U,encoded_at=x86_?48U:72U,info_at=x86_?56U:88U;
                if(!entry.indirect.empty()){put(at,indirect_at,indirect_struct(document,entry.indirect),bits);blob(at+encoded_at,entry.indirect.size(),keep(document,entry.indirect));}
                put(at,version_at,entry.version);
                if(!entry.subject.empty()){const auto info=codec::encode_member_info(entry.subject,entry.version);blob(at+info_at,info.size(),keep(document,info));}
                return at;};
            const auto find_member=[&](CatalogDocument& document,std::uint64_t guest)->CatalogMemberEntry*{for(auto& entry:document.members)if(entry.guest && entry.guest==guest)return &entry;return nullptr;};
            // 屬性資料：DATABASE64 由 Base64 文字（UTF-16）解碼，其他照原樣保存；DATAREPLACE 只是動作，不保存。
            const auto attribute_value=[&](std::uint32_t flags,std::uint64_t count,std::uint64_t pointer)->std::vector<std::uint8_t>{
                require(count<=16U*1024U*1024U && (!count || pointer),87);std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count));if(count)memory.read(GuestAddress{pointer},bytes);
                if(!(flags&0x20000U))return bytes;
                std::string text;for(std::size_t i=0;i+1<bytes.size();i+=2){const auto c=static_cast<char16_t>(bytes[i]|(bytes[i+1]<<8));if(!c)break;if(c<128 && c>' ')text.push_back(static_cast<char>(c));}
                std::vector<std::uint8_t> decoded;unsigned buffer=0;int bits_held=0;
                for(const char c:text){int v=c>='A' && c<='Z'?c-'A':c>='a' && c<='z'?c-'a'+26:c>='0' && c<='9'?c-'0'+52:c=='+'?62:c=='/'?63:c=='='?-2:-1;require(v!=-1,13);if(v==-2)break;
                    buffer=(buffer<<6)|static_cast<unsigned>(v);bits_held+=6;if(bits_held>=8){bits_held-=8;decoded.push_back(static_cast<std::uint8_t>(buffer>>bits_held));}}
                return decoded;};
            const auto put_attribute=[&](CatalogDocument& document,std::vector<CatalogAttributeEntry>& list,std::uint64_t tag_at,std::uint32_t flags,std::uint64_t count,std::uint64_t pointer)->std::uint64_t{
                require(tag_at,87);require((flags&0x30000000U) && (flags&3U),87);const auto tag=read_utf16(memory,GuestAddress{tag_at},32768);require(!tag.empty(),87);
                auto value=attribute_value(flags,count,pointer);
                const auto found=std::find_if(list.begin(),list.end(),[&](const auto& entry){return folded(entry.tag)==folded(tag);});
                if(found!=list.end()){require(flags&0x40000U,0x80092005U);found->value=std::move(value);found->flags=flags&~0x40000U;return materialize_attribute(document,*found);}
                list.push_back({tag,flags&~0x40000U,std::move(value),0});return materialize_attribute(document,list.back());};
            if(symbol=="CryptCATOpen"){
                // (FileName, OpenFlags, hProv, PublicVersion, EncodingType)：CREATENEW 1、ALWAYS 2、EXISTING 4（未指定時視為 EXISTING）；版本 0x100／0x200。失敗回 INVALID_HANDLE_VALUE。
                const auto flags=static_cast<std::uint32_t>(arg(1));const auto version=static_cast<std::uint32_t>(arg(3));
                try{
                    require(arg(0) && !(flags&~0xf0000037U) && (version==0 || version==0x100 || version==0x200),87);
                    const auto path=utf16_to_utf8(full_windows_environment_path(read_utf16(memory,GuestAddress{arg(0)},32768),runtime_.current_directory,runtime_.environment));
                    CatalogDocument document;document.path=path;document.version=version==0x200?2:1;
                    const auto mode=flags&7U;const bool create=mode&1U,always=mode&2U;
                    const auto info=filesystem_.information(path);const bool present=!info.error && !(info.metadata.attributes&0x10U);
                    if(create || (always && !present)){
                        // 新目錄的 ListIdentifier：16 個位元組，以時間與控制代碼序號混合產生。
                        document.identifier.resize(16);auto seed=static_cast<std::uint64_t>(std::time(nullptr))^(runtime_.next_crypto_handle<<17);for(auto& byte:document.identifier){seed=seed*6364136223846793005ULL+1442695040888963407ULL;byte=static_cast<std::uint8_t>(seed>>56);}
                    }else{
                        require(present,info.error?info.error:2);
                        const auto opened=filesystem_.open_reader(path);require(!opened.error,opened.error);require(opened.metadata.size<=64ULL*1024*1024,8);
                        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(opened.metadata.size));const auto io=opened.file->read(bytes,0);require(!io.error && io.bytes==bytes.size(),io.error?io.error:38);
                        const auto parsed=codec::decode(bytes);require(parsed.has_value(),0x8009310bU);
                        document.version=parsed->version;document.identifier=parsed->list_identifier;
                        for(const auto& member:parsed->members){
                            CatalogMemberEntry entry;entry.tag=tag_of(member.identifier);entry.subject=member.subject.value_or(u"");entry.version=member.version;if(member.indirect)entry.indirect=*member.indirect;
                            for(const auto& value:member.attributes)entry.attributes.push_back({value.tag,value.flags,value.value,0});document.members.push_back(std::move(entry));
                        }
                        for(const auto& value:parsed->attributes)document.attributes.push_back({value.tag,value.flags,value.value,0});
                    }
                    require(runtime_.catalog_documents.size()<1024,8);const auto handle=runtime_.next_crypto_handle++;runtime_.catalog_documents.emplace(handle,std::move(document));last_error_=0;return handle;
                }catch(const Error& error){last_error_=error.code;return invalid;}
            }
            if(symbol=="CryptCATClose"){
                const auto found=runtime_.catalog_documents.find(arg(0));require(found!=runtime_.catalog_documents.end(),87);
                for(const auto block:found->second.allocations)(void)free_heap(memory,block,state);runtime_.catalog_documents.erase(found);last_error_=0;return 1;
            }
            if(symbol=="CryptCATEnumerateMember" || symbol=="CryptCATGetMemberInfo"){
                // Enumerate(hCatalog, PrevMember)：NULL 取第一個；GetMemberInfo(hCatalog, ReferenceTag)：參考標記不分大小寫。
                auto& document=document_of(arg(0));
                if(symbol=="CryptCATGetMemberInfo"){
                    require(arg(1),87);const auto tag=folded(read_utf16(memory,GuestAddress{arg(1)},32768));
                    for(auto& entry:document.members)if(folded(entry.tag)==tag){last_error_=0;return materialize_member(document,entry);}
                    last_error_=0x80092004U;return 0;
                }
                std::size_t index=0;if(arg(1)){const auto found=std::find_if(document.members.begin(),document.members.end(),[&](const auto& entry){return entry.guest==arg(1);});require(found!=document.members.end(),87);index=static_cast<std::size_t>(found-document.members.begin())+1;}
                if(index>=document.members.size()){last_error_=0x80092004U;return 0;}last_error_=0;return materialize_member(document,document.members[index]);
            }
            if(symbol=="CryptCATEnumerateAttr" || symbol=="CryptCATGetAttrInfo" || symbol=="CryptCATEnumerateCatAttr" || symbol=="CryptCATGetCatAttrInfo"){
                // 成員屬性：(hCatalog, Member, Prev／Tag)；目錄屬性：(hCatalog, Prev／Tag)。
                auto& document=document_of(arg(0));const bool member_level=symbol=="CryptCATEnumerateAttr" || symbol=="CryptCATGetAttrInfo";
                std::vector<CatalogAttributeEntry>* list=&document.attributes;
                if(member_level){auto* member=find_member(document,arg(1));require(member!=nullptr,87);list=&member->attributes;}
                const auto key=arg(member_level?2:1);
                if(symbol.ends_with("Info")){
                    require(key,87);const auto tag=folded(read_utf16(memory,GuestAddress{key},32768));
                    for(auto& entry:*list)if(folded(entry.tag)==tag){last_error_=0;return materialize_attribute(document,entry);}
                    last_error_=0x80092004U;return 0;
                }
                std::size_t index=0;if(key){const auto found=std::find_if(list->begin(),list->end(),[&](const auto& entry){return entry.guest==key;});require(found!=list->end(),87);index=static_cast<std::size_t>(found-list->begin())+1;}
                if(index>=list->size()){last_error_=0x80092004U;return 0;}last_error_=0;return materialize_attribute(document,(*list)[index]);
            }
            if(symbol=="CryptCATPutMemberInfo"){
                // (hCatalog, FileName, ReferenceTag, SubjectType, CertVersion, cbSIPIndirectData, SIP_INDIRECT_DATA*)：已存在的標記回傳既有成員。
                auto& document=document_of(arg(0));require(arg(2) && arg(3),87);const auto tag=read_utf16(memory,GuestAddress{arg(2)},32768);require(!tag.empty(),87);
                for(auto& entry:document.members)if(folded(entry.tag)==folded(tag)){last_error_=0;return materialize_member(document,entry);}
                CatalogMemberEntry entry;entry.tag=tag;if(arg(1))entry.file=read_utf16(memory,GuestAddress{arg(1)},32768);
                WindowsGuid subject{};memory.read(GuestAddress{arg(3)},subject);entry.subject=windows_guid_text(subject);entry.version=static_cast<std::uint32_t>(arg(4));
                if(arg(6)){
                    const unsigned size=x86_?32U:64U;require(arg(5)>=size,87);const auto at=arg(6);
                    const auto string_at=[&](std::uint64_t pointer){require(pointer,87);return read_ansi(memory,pointer,256);};
                    const auto blob_bytes=[&](std::uint64_t base)->std::vector<std::uint8_t>{const auto count=load(base,0);const auto pointer=load(base,unit,bits);require(count<=1048576 && (!count || pointer),87);
                        std::vector<std::uint8_t> bytes(count);if(count)memory.read(GuestAddress{pointer},bytes);return bytes;};
                    entry.indirect=codec::encode_indirect({string_at(load(at,0,bits)),blob_bytes(at+unit),string_at(load(at,3*unit,bits)),blob_bytes(at+4*unit),blob_bytes(at+6*unit)});
                }
                document.members.push_back(std::move(entry));last_error_=0;return materialize_member(document,document.members.back());
            }
            if(symbol=="CryptCATPutAttrInfo"){
                // (hCatalog, Member, ReferenceTag, AttrTypeAndAction, cbData, pbData)
                auto& document=document_of(arg(0));auto* member=find_member(document,arg(1));require(member!=nullptr,87);
                const auto result=put_attribute(document,member->attributes,arg(2),static_cast<std::uint32_t>(arg(3)),static_cast<std::uint32_t>(arg(4)),arg(5));last_error_=0;return result;
            }
            if(symbol=="CryptCATPutCatAttrInfo"){
                // (hCatalog, ReferenceTag, AttrTypeAndAction, cbData, pbData)
                auto& document=document_of(arg(0));const auto result=put_attribute(document,document.attributes,arg(1),static_cast<std::uint32_t>(arg(2)),static_cast<std::uint32_t>(arg(3)),arg(4));last_error_=0;return result;
            }
            if(symbol=="CryptCATPersistStore"){
                // 寫出未簽署目錄（成員標記以 UTF-16LE 存成 SubjectIdentifier）。
                auto& document=document_of(arg(0));codec::Catalog catalog;catalog.version=document.version;catalog.list_identifier=document.identifier;
                for(const auto& entry:document.members){
                    codec::Member member;const auto identifier=utf16_bytes(entry.tag);member.identifier.assign(identifier.begin(),identifier.end()-2);
                    if(!entry.indirect.empty())member.indirect=entry.indirect;if(!entry.subject.empty())member.subject=entry.subject;member.version=entry.version;
                    for(const auto& value:entry.attributes)member.attributes.push_back({value.tag,value.flags,value.value});catalog.members.push_back(std::move(member));
                }
                for(const auto& value:document.attributes)catalog.attributes.push_back({value.tag,value.flags,value.value});
                const auto bytes=codec::encode(catalog,std::time(nullptr));
                auto opened=filesystem_.open_host(document.path,0x40000000U,2,0x80,3);require(!opened.error,opened.error);
                const auto written=opened.file->write(bytes,0);require(!written.error,written.error);if(const auto error=opened.file->truncate(bytes.size()))require(false,error);
                last_error_=0;return 1;
            }
            if(symbol.starts_with("WVTAsn1Cat")){
                // Encode(EncodingType, StructType, pInfo, pbEncoded, pcbEncoded)；Decode(EncodingType, StructType, pbEncoded, cbEncoded, Flags, pvStructInfo, pcbStructInfo)。
                // 輸出緩衝區為 NULL 時只回報大小；不足時回 ERROR_MORE_DATA。CAT_NAMEVALUE {Tag, Flags, Value}、CAT_MEMBERINFO {SubjGuid, CertVersion}。
                const bool name_value=symbol.starts_with("WVTAsn1CatNameValue"),encode=symbol.ends_with("Encode");
                const auto deliver=[&](std::uint64_t buffer,std::uint64_t size_at,std::size_t needed,const auto& write)->std::uint64_t{
                    require(size_at,87);memory.check(GuestAddress{size_at},4,Access::Write);const auto available=load(size_at,0);put(size_at,0,needed);
                    if(!buffer){last_error_=0;return 1;}if(available<needed){last_error_=234;return 0;}write(buffer);last_error_=0;return 1;};
                if(encode){
                    require(arg(2),87);std::vector<std::uint8_t> bytes;
                    if(name_value){const auto tag=load(arg(2),0,bits);require(tag,87);const auto count=load(arg(2),2*unit),pointer=load(arg(2),2*unit+unit,bits);require(count<=1048576 && (!count || pointer),87);
                        std::vector<std::uint8_t> value(count);if(count)memory.read(GuestAddress{pointer},value);bytes=codec::encode_name_value({read_utf16(memory,GuestAddress{tag},32768),static_cast<std::uint32_t>(load(arg(2),unit)),value});}
                    else{const auto subject=load(arg(2),0,bits);require(subject,87);bytes=codec::encode_member_info(read_utf16(memory,GuestAddress{subject},256),static_cast<std::uint32_t>(load(arg(2),unit)));}
                    return deliver(arg(3),arg(4),bytes.size(),[&](std::uint64_t at){memory.write(GuestAddress{at},bytes);});
                }
                require(arg(2) && arg(3)<=1048576,87);std::vector<std::uint8_t> encoded(static_cast<std::size_t>(arg(3)));if(!encoded.empty())memory.read(GuestAddress{arg(2)},encoded);
                if(name_value){
                    const auto decoded=codec::decode_name_value(encoded);require(decoded.has_value(),0x8009310bU);const unsigned head=x86_?16U:32U;const auto tag=utf16_bytes(decoded->tag);
                    const auto needed=head+tag.size()+decoded->value.size();
                    return deliver(arg(5),arg(6),needed,[&](std::uint64_t at){memory.fill(GuestAddress{at},0,head);put(at,0,at+head,bits);put(at,unit,decoded->flags);put(at,2*unit,decoded->value.size());
                        put(at,3*unit,decoded->value.empty()?0:at+head+tag.size(),bits);memory.write(GuestAddress{at+head},tag);if(!decoded->value.empty())memory.write(GuestAddress{at+head+tag.size()},decoded->value);});
                }
                const auto decoded=codec::decode_member_info(encoded);require(decoded.has_value(),0x8009310bU);const unsigned head=x86_?8U:16U;const auto subject=utf16_bytes(decoded->first);
                return deliver(arg(5),arg(6),head+subject.size(),[&](std::uint64_t at){put(at,0,at+head,bits);put(at,unit,decoded->second);memory.write(GuestAddress{at+head},subject);});
            }
        }
