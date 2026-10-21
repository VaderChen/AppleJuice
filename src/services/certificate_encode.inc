// 與 Decode 共用型別表、DER 工具及客體配置流程的反向編碼器。
struct Encode {
    GuestMemory& memory;unsigned unit;std::uint32_t flags;
    std::function<std::string(std::uint64_t)> string;
    using Bytes=std::vector<std::uint8_t>;
    std::uint64_t get(std::uint64_t at,unsigned width=4)const{return memory.load(GuestAddress{at},static_cast<std::uint8_t>(width*8));}
    std::uint64_t pointer(std::uint64_t at)const{return get(at,unit);}
    Bytes read(std::uint64_t at,std::uint64_t size)const{if(size>16U*1024U*1024U)throw CertificateError{8};if(size && !at)throw CertificateError{87};Bytes result(static_cast<std::size_t>(size));memory.read(GuestAddress{at},result);return result;}
    Bytes blob(std::uint64_t at)const{return read(pointer(at+unit),get(at));}
    static void append(Bytes& target,const Bytes& source){if(source.size()>16U*1024U*1024U-target.size())throw CertificateError{8};target.insert(target.end(),source.begin(),source.end());}
    static Bytes sequence(const std::vector<Bytes>& fields,int type=V_ASN1_SEQUENCE){Bytes content;for(const auto& field:fields)append(content,field);return der_container(type,content);}
    static Bytes raw(Bytes bytes){if(bytes.empty() || take(bytes).encoded.size()!=bytes.size())throw CertificateError{corrupt};return bytes;}
    Bytes oid(std::uint64_t at)const{if(!at)throw CertificateError{87};const auto name=string(at);std::unique_ptr<ASN1_OBJECT,decltype(&ASN1_OBJECT_free)> value(OBJ_txt2obj(name.c_str(),1),ASN1_OBJECT_free);if(!value)throw CertificateError{corrupt};return encoded(value.get(),i2d_ASN1_OBJECT);}
    static Bytes number(Bytes bytes,bool positive=false,unsigned type=2){
        if(bytes.empty())bytes.push_back(0);std::reverse(bytes.begin(),bytes.end());
        while(bytes.size()>1 && ((bytes[0]==0 && (positive || !(bytes[1]&128))) || (!positive && bytes[0]==255 && (bytes[1]&128))))bytes.erase(bytes.begin());
        if(positive && (bytes[0]&128))bytes.insert(bytes.begin(),0);return der_object(static_cast<int>(type),bytes);
    }
    static Bytes integer32(std::uint32_t value,bool positive=false,unsigned type=2){Bytes bytes;for(unsigned i=0;i<4;++i)bytes.push_back(static_cast<std::uint8_t>(value>>(8*i)));return number(std::move(bytes),positive,type);}
    Bytes algorithm(std::uint64_t at)const{auto fields=std::vector<Bytes>{oid(pointer(at))};auto parameters=blob(at+unit);if(!parameters.empty())fields.push_back(raw(std::move(parameters)));return sequence(fields);}
    Bytes bits(std::uint64_t at,bool reverse=false)const{auto content=blob(at);const auto unused=get(at+2*unit);if(unused>7 || (content.empty() && unused))throw CertificateError{corrupt};if(reverse)std::reverse(content.begin(),content.end());if(unused && !content.empty())content.back()&=static_cast<std::uint8_t>(0xffU<<unused);content.insert(content.begin(),static_cast<std::uint8_t>(unused));return der_object(3,content);}
    Bytes date(std::uint64_t at,bool utc=false)const{
        const auto ticks=get(at,8);const auto seconds=static_cast<std::int64_t>(ticks/10000000)-11644473600LL;
        std::unique_ptr<ASN1_TIME,decltype(&ASN1_TIME_free)> value(ASN1_TIME_set(nullptr,static_cast<std::time_t>(seconds)),ASN1_TIME_free);
        if(!value || (utc && ASN1_STRING_type(value.get())!=V_ASN1_UTCTIME))throw CertificateError{corrupt};return encoded(value.get(),i2d_ASN1_TIME);
    }
    Bytes name_value(std::uint64_t at,bool unicode,std::string_view name="2.5.4.3")const{
        const auto kind=get(at);auto bytes=blob(at+unit);if(kind==1)return raw(std::move(bytes));if(kind==2)return der_object(4,bytes);
        constexpr int tags[]{0,0,4,18,19,20,21,22,25,26,27,28,30,12};if(kind>=std::size(tags))throw CertificateError{87};
        if(unicode || !kind){
            std::u16string text;if(unicode){if(bytes.size()%2)throw CertificateError{corrupt};for(std::size_t i=0;i<bytes.size();i+=2)text+=static_cast<char16_t>(bytes[i]|(unsigned{bytes[i+1]}<<8));}
            else text=utf8_to_utf16_lossy(std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size()));
            if(!kind){
                const auto mapped=((flags&0x80000000U)?0x20000U:0U)|((flags&0x20000000U)?0x40000U:0U)|((flags&0x10000000U)?0x80000U:0U)|((flags&0x20000U)?0x200000U:0U);
                const auto attribute=encode_certificate_name_entry({std::string{name},text,0,false},mapped);const auto pair=children(take(attribute));return Bytes(pair[1].encoded.begin(),pair[1].encoded.end());
            }
            bytes.clear();
            if(kind==13){const auto utf8=utf16_to_utf8(text);bytes.assign(utf8.begin(),utf8.end());}
            else if(kind==12){for(const auto c:text){bytes.push_back(static_cast<std::uint8_t>(c>>8));bytes.push_back(static_cast<std::uint8_t>(c));}}
            else if(kind==11){for(std::size_t i=0;i<text.size();++i){std::uint32_t c=text[i];if(c>=0xd800 && c<=0xdbff){if(i+1==text.size() || text[i+1]<0xdc00 || text[i+1]>0xdfff)throw CertificateError{corrupt};c=0x10000+((c-0xd800)<<10)+(text[++i]-0xdc00);}else if(c>=0xdc00 && c<=0xdfff)throw CertificateError{corrupt};for(int shift=24;shift>=0;shift-=8)bytes.push_back(static_cast<std::uint8_t>(c>>shift));}}
            else for(const auto c:text){
                if(!(flags&0x40000000U)){
                    if(kind==3 && !(c==u' ' || (c>=u'0' && c<=u'9')))throw CertificateError{0x80092020U};
                    if(kind==4 && !((c>=u'A' && c<=u'Z') || (c>=u'a' && c<=u'z') || (c>=u'0' && c<=u'9') || std::u16string_view(u" '()+,-./:=?").find(c)!=std::u16string_view::npos))throw CertificateError{0x80092021U};
                    if(kind==7 && c>127)throw CertificateError{0x80092022U};if(c>255)throw CertificateError{corrupt};
                }bytes.push_back(static_cast<std::uint8_t>(c));
            }
        }else if(kind==11 || kind==12){const unsigned width=kind==12?2:4;if(bytes.size()%width)throw CertificateError{corrupt};for(std::size_t i=0;i<bytes.size();i+=width)std::reverse(bytes.begin()+static_cast<std::ptrdiff_t>(i),bytes.begin()+static_cast<std::ptrdiff_t>(i+width));}
        return der_object(tags[kind],bytes);
    }
    std::pair<std::uint64_t,std::uint64_t> list(std::uint64_t at)const{const auto count=get(at),data=pointer(at+unit);if(count>4096 || (count && !data))throw CertificateError{87};return {count,data};}
    Bytes name(std::uint64_t at,bool unicode)const{
        const auto [count,data]=list(at);std::vector<Bytes> groups;
        for(std::uint64_t i=0;i<count;++i){const auto [n,attrs]=list(data+i*2*unit);if(!n)throw CertificateError{corrupt};std::vector<Bytes> entries;
            for(std::uint64_t j=0;j<n;++j){const auto item=attrs+j*4*unit;entries.push_back(sequence({oid(pointer(item)),name_value(item+unit,unicode,string(pointer(item)))}));}
            std::sort(entries.begin(),entries.end());groups.push_back(sequence(entries,V_ASN1_SET));
        }return sequence(groups);
    }
    Bytes public_key(std::uint64_t at)const{return sequence({algorithm(at),bits(at+3*unit)});}
    Bytes extensions(std::uint64_t at)const{
        const auto [count,data]=list(at);std::vector<Bytes> entries;
        for(std::uint64_t i=0;i<count;++i){const auto item=data+i*4*unit;std::vector<Bytes> fields{oid(pointer(item))};if(get(item+unit))fields.push_back(der_object(1,Bytes{255}));fields.push_back(der_object(4,blob(item+2*unit)));entries.push_back(sequence(fields));}return sequence(entries);
    }
    static std::u16string punycode(std::u16string text,std::size_t start,std::size_t end){
        const auto host=std::u16string_view(text).substr(start,end-start);
        if(std::any_of(host.begin(),host.end(),[](char16_t c){return c>127;})){
            const auto converted=idn_detail::convert(host,true,0,idn_detail::Profile::certificate_uts46);
            if(!converted)throw CertificateError{0x80092022U};text.replace(start,end-start,*converted);
        }return text;
    }
    static std::u16string uri(std::u16string text){
        // 只轉換 authority 內的主機；路徑依 UTF-8 位元組逸出，保留既有百分比編碼。
        const auto scheme=text.find(u"://");
        if(scheme!=text.npos){
            auto start=scheme+3;auto end=text.find_first_of(u"/?#",start);if(end==text.npos)end=text.size();
            const auto user=text.rfind(u'@',end?end-1:0);if(user!=text.npos && user>=start)start=user+1;
            if(start<end && text[start]!=u'['){const auto port=text.find(u':',start);if(port<end)end=port;text=punycode(std::move(text),start,end);}
        }
        const auto utf8=utf16_to_utf8(text);std::u16string escaped;constexpr std::string_view punctuation="-._~:/?#[]@!$&'()*+,;=%";
        for(const char byte:utf8){const auto c=static_cast<unsigned char>(byte);if((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || punctuation.find(static_cast<char>(c))!=punctuation.npos)escaped+=c;
            else {escaped+=u'%';escaped+=u"0123456789ABCDEF"[c>>4];escaped+=u"0123456789ABCDEF"[c&15];}}
        return escaped;
    }
    Bytes alternate_entry(std::uint64_t at)const{
        const auto choice=get(at);if(choice<1 || choice>9)throw CertificateError{87};const auto p=pointer(at+unit);Bytes value;const auto tag=static_cast<int>(choice-1);
        if(choice==2 || choice==3 || choice==7){
            auto text=read_utf16(memory,GuestAddress{p},65536);
            if(flags&0x20000U){if(choice==7)text=uri(std::move(text));else {const auto split=choice==2?text.rfind(u'@'):text.npos;const auto start=split==text.npos?0:split+1;const auto end=text.size();text=punycode(std::move(text),start,end);}}
            for(auto c:text){if(c>127)throw CertificateError{0x80092022U};value.push_back(static_cast<std::uint8_t>(c));}return der_object(tag,value,false,V_ASN1_CONTEXT_SPECIFIC);
        }
        if(choice==8){value=blob(at+unit);if(value.size()!=4 && value.size()!=16)throw CertificateError{87};return der_object(tag,value,false,V_ASN1_CONTEXT_SPECIFIC);}
        if(choice==5)return der_object(tag,raw(blob(at+unit)),true,V_ASN1_CONTEXT_SPECIFIC);
        if(choice==9){value=oid(p);const auto inner=take(value);return der_object(tag,inner.content,false,V_ASN1_CONTEXT_SPECIFIC);}
        if(choice==1){value=oid(pointer(p));append(value,der_object(0,raw(blob(p+unit)),true,V_ASN1_CONTEXT_SPECIFIC));return der_object(0,value,true,V_ASN1_CONTEXT_SPECIFIC);}
        throw CertificateError{2};
    }
    Bytes alternate(std::uint64_t at)const{const auto [count,data]=list(at);std::vector<Bytes> entries;for(std::uint64_t i=0;i<count;++i)entries.push_back(alternate_entry(data+i*3*unit));return sequence(entries);}
    Bytes rsa(std::uint64_t at,unsigned kind)const{
        if(kind==72){if(get(at)!=0x31415352 || get(at+16) || get(at+20))throw CertificateError{87};const auto e=get(at+8),n=get(at+12);auto exponent=read(at+24,e),modulus=read(at+24+e,n);std::reverse(exponent.begin(),exponent.end());std::reverse(modulus.begin(),modulus.end());return sequence({number(modulus,true),number(exponent,true)});}
        const bool priv=kind==43;const auto bits=get(at+12),width=bits/8;if(get(at,1)!=(priv?7U:6U) || get(at+1,1)!=2 || get(at+8)!=(priv?0x32415352U:0x31415352U) || !width || bits%8 || (priv && width%2))throw CertificateError{87};
        std::vector<Bytes> fields;if(priv)fields.push_back(integer32(0));fields.push_back(number(read(at+20,width),true));fields.push_back(integer32(static_cast<std::uint32_t>(get(at+16)),true));
        if(priv){fields.push_back(number(read(at+20+width+5*(width/2),width),true));for(unsigned i=0;i<5;++i)fields.push_back(number(read(at+20+width+i*(width/2),width/2),true));}return sequence(fields);
    }
    Bytes encode(unsigned kind,std::uint64_t at)const{
        if(!at)throw CertificateError{87};
        switch(kind){
        case 1:return sequence({raw(blob(at)),algorithm(at+2*unit),bits(at+5*unit,!(flags&8U))});
        case 2:{std::vector<Bytes> fields;const auto version=get(at);if(version>2)throw CertificateError{87};if(version)fields.push_back(der_object(0,integer32(static_cast<std::uint32_t>(version)),true,V_ASN1_CONTEXT_SPECIFIC));fields.push_back(number(blob(at+unit)));fields.push_back(algorithm(at+3*unit));fields.push_back(raw(blob(at+6*unit)));fields.push_back(sequence({date(at+8*unit),date(at+8*unit+8)}));fields.push_back(raw(blob(at+8*unit+16)));fields.push_back(public_key(at+10*unit+16));
            for(unsigned i=0;i<2;++i){const auto field=at+(unit==4?80U:144U)+i*3*unit;if(get(field)){auto value=bits(field);value[0]=static_cast<std::uint8_t>(129+i);fields.push_back(std::move(value));}}
            const auto ext=at+(unit==4?104U:192U);if(get(ext))fields.push_back(der_object(3,extensions(ext),true,V_ASN1_CONTEXT_SPECIFIC));return sequence(fields);}
        case 5:return extensions(at);case 6:case 24:return name_value(at,kind==24);case 7:case 20:return name(at,kind==20);case 8:return public_key(at);case 12:return alternate(at);
        case 14:case 26:return bits(at);
        case 15:{std::vector<Bytes> fields;if(get(at))fields.push_back(der_object(1,Bytes{255}));if(get(at+4))fields.push_back(integer32(static_cast<std::uint32_t>(get(at+8)),true));return sequence(fields);}
        case 17:case 30:return date(at,kind==17);case 19:case 43:case 72:return rsa(at,kind);case 25:return der_object(4,blob(at));
        case 27:case 29:return integer32(static_cast<std::uint32_t>(get(at)),false,kind==29?10:2);
        case 28:case 38:return number(blob(at),kind==38);
        case 39:case 47:{std::vector<Bytes> fields;for(unsigned i=0;i<(kind==39?3U:2U);++i)fields.push_back(number(blob(at+i*2*unit),true));return sequence(fields);}
        case 40:return sequence({number(read(at,20),true),number(read(at+20,20),true)});
        case 31:{std::vector<Bytes> fields;if(get(at))fields.push_back(der_object(0,blob(at),false,V_ASN1_CONTEXT_SPECIFIC));if(get(at+2*unit)){auto value=alternate(at+2*unit);value[0]=161;fields.push_back(std::move(value));}if(get(at+4*unit)){auto value=number(blob(at+4*unit));value[0]=130;fields.push_back(std::move(value));}return sequence(fields);}
        case 32:{const auto [count,data]=list(at);std::vector<Bytes> fields;for(std::uint64_t i=0;i<count;++i){const auto item=data+i*4*unit;fields.push_back(sequence({oid(pointer(item)),alternate_entry(item+unit)}));}return sequence(fields);}
        case 33:{std::vector<Bytes> fields{oid(pointer(at))};if(get(at+unit))fields.push_back(der_object(0,raw(blob(at+unit)),true,V_ASN1_CONTEXT_SPECIFIC));return sequence(fields);}
        case 34:case 36:{const auto [count,data]=list(at);std::vector<Bytes> fields;for(std::uint64_t i=0;i<count;++i)fields.push_back(kind==34?raw(blob(data+i*2*unit)):oid(pointer(data+i*unit)));return sequence(fields);}
        case 73:return oid(pointer(at));case 74:return algorithm(at);default:throw CertificateError{2};
        }
    }
};
