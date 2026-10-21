#include "x64arm/windows_services.hpp"
#include "certificate_packet.hpp"
#include "crypto_digest.hpp"
#include "idn_conversion.hpp"
#include <algorithm>
#include <openssl/err.h>
#include <functional>

namespace x64arm {
namespace {
using namespace certificate_detail;
#include "certificate_names.inc"
constexpr std::uint32_t eof=0x80093102,corrupt=0x80093103,large=0x80093104,badtag=0x8009310b;
struct Der {
    std::uint8_t tag;std::span<const std::uint8_t> encoded,content;std::size_t offset,header;
};
Der take(std::span<const std::uint8_t> bytes,std::size_t offset=0){
    if(bytes.size()<2)throw CertificateError{eof};
    if((bytes[0]&31)==31)throw CertificateError{badtag};
    std::size_t header=2,length=bytes[1];
    if(length&128){
        const auto count=length&127;if(!count)throw CertificateError{corrupt};
        if(count>4)throw CertificateError{large};if(count>bytes.size()-2)throw CertificateError{eof};
        if(!bytes[2])throw CertificateError{corrupt};length=0;
        for(std::size_t i=0;i<count;++i)length=(length<<8)|bytes[2+i];
        if(length<128)throw CertificateError{corrupt};header+=count;
    }
    if(length>bytes.size()-header)throw CertificateError{eof};
    return {bytes[0],bytes.first(header+length),bytes.subspan(header,length),offset,header};
}
void tag(const Der& value,unsigned expected){if(value.tag!=expected)throw CertificateError{badtag};}
std::vector<Der> children(const Der& value,unsigned expected=48){
    tag(value,expected);std::vector<Der> items;std::size_t at=0;
    while(at<value.content.size()){
        if(items.size()==4096)throw CertificateError{large};
        auto item=take(value.content.subspan(at),value.offset+value.header+at);at+=item.encoded.size();items.push_back(item);
    }
    return items;
}
void count(const std::vector<Der>& items,std::size_t minimum,std::size_t maximum){if(items.size()<minimum || items.size()>maximum)throw CertificateError{corrupt};}
std::int64_t integer(const Der& value,unsigned expected=2){
    tag(value,expected);const auto bytes=value.content;if(bytes.empty())throw CertificateError{corrupt};
    if(bytes.size()>1 && ((bytes[0]==0 && !(bytes[1]&128)) || (bytes[0]==255 && (bytes[1]&128))))throw CertificateError{corrupt};
    if(bytes.size()>4)throw CertificateError{large};std::uint64_t result=(bytes[0]&128)?UINT64_MAX:0;
    for(auto byte:bytes)result=(result<<8)|byte;return static_cast<std::int64_t>(result);
}
std::vector<std::uint8_t> reversed_integer(const Der& value,bool positive){
    tag(value,2);auto bytes=value.content;if(bytes.empty())throw CertificateError{corrupt};
    if(bytes.size()>1 && ((bytes[0]==0 && !(bytes[1]&128)) || (bytes[0]==255 && (bytes[1]&128))))throw CertificateError{corrupt};
    if(positive){if(bytes[0]&128)throw CertificateError{corrupt};if(bytes.size()>1 && !bytes[0])bytes=bytes.subspan(1);}
    return {bytes.rbegin(),bytes.rend()};
}
std::string object_id(const Der& value){
    tag(value,6);const auto* cursor=value.encoded.data();
    std::unique_ptr<ASN1_OBJECT,decltype(&ASN1_OBJECT_free)> oid(d2i_ASN1_OBJECT(nullptr,&cursor,static_cast<long>(value.encoded.size())),ASN1_OBJECT_free);
    if(!oid || cursor!=value.encoded.data()+value.encoded.size())throw CertificateError{corrupt};return oid_name(oid.get());
}
std::uint64_t time(const Der& value){
    if(value.tag!=23 && value.tag!=24)throw CertificateError{badtag};const auto* cursor=value.encoded.data();
    std::unique_ptr<ASN1_TIME,decltype(&ASN1_TIME_free)> parsed(d2i_ASN1_TIME(nullptr,&cursor,static_cast<long>(value.encoded.size())),ASN1_TIME_free);
    if(!parsed || cursor!=value.encoded.data()+value.encoded.size())throw CertificateError{corrupt};return certificate_time(parsed.get());
}
unsigned string_type(unsigned t){
    switch(t){case 4:return 2;case 18:return 3;case 19:return 4;case 20:return 5;case 21:return 6;case 22:return 7;case 25:return 8;case 26:return 9;case 27:return 10;case 28:return 11;case 30:return 12;case 12:return 13;default:return 1;}
}
std::u16string unicode_string(const Der& value,bool legacy){
    if(string_type(value.tag)==1)throw CertificateError{badtag};
    const auto* cursor=value.encoded.data();std::unique_ptr<ASN1_TYPE,decltype(&ASN1_TYPE_free)> parsed(d2i_ASN1_TYPE(nullptr,&cursor,static_cast<long>(value.encoded.size())),ASN1_TYPE_free);
    if(!parsed || cursor!=value.encoded.data()+value.encoded.size())throw CertificateError{corrupt};
    if(value.tag==12){try{return utf8_to_utf16(std::string_view(reinterpret_cast<const char*>(value.content.data()),value.content.size()));}catch(const std::invalid_argument&){throw CertificateError{corrupt};}}
    if(value.tag==4){std::u16string text;for(auto byte:value.content)text.push_back(byte);return text;}
    return certificate_string(parsed->value.asn1_string,legacy);
}
struct Decode {
    CertificatePacket packet;std::uint64_t base;std::uint32_t flags;
    std::function<std::uint64_t(const std::string&)> share;
    Decode(bool small,std::uint64_t address,std::uint32_t f):packet(small),base(address),flags(f){}
    void raw(std::size_t field,const Der& value,bool whole=false){
        const auto bytes=whole?value.encoded:value.content;packet.put(field,bytes.size());
        if(!bytes.empty()){
            if(flags&1)packet.put(field+packet.unit,base+value.offset+(whole?0:value.header),packet.unit);
            else packet.point(field+packet.unit,packet.data(bytes));
        }
    }
    void oid(std::size_t field,const Der& value){const auto name=object_id(value);if(flags&4)packet.put(field,share(name),packet.unit);else packet.text(field,name);}
    void algorithm(std::size_t field,const Der& value){const auto items=children(value);count(items,1,2);oid(field,items[0]);if(items.size()==2)raw(field+packet.unit,items[1],true);}
    void bits(std::size_t field,const Der& value,unsigned expected=3){
        tag(value,expected);if(value.content.empty())throw CertificateError{corrupt};const auto unused=value.content[0];
        if(unused>7 || (value.content.size()==1 && unused) || (unused && (value.content.back()&((1U<<unused)-1U))))throw CertificateError{corrupt};
        auto body=value;body.content=body.content.subspan(1);++body.header;raw(field,body);packet.put(field+2*packet.unit,unused);
    }
    void number_blob(std::size_t field,const Der& value,bool positive){packet.blob(field,reversed_integer(value,positive));}
    void name_value(std::size_t field,const Der& value,bool unicode){
        const auto type=string_type(value.tag);packet.put(field,type);
        if(unicode && type!=1 && type!=2){const auto bytes=utf16_bytes(unicode_string(value,(flags&0x20000000U)!=0));packet.put(field+packet.unit,bytes.size()-2);packet.point(field+2*packet.unit,packet.data(bytes));}
        else if(type==12 || type==11 || type==13){
            auto bytes=std::vector<std::uint8_t>(value.content.begin(),value.content.end());
            if(type==12 || type==11){const unsigned width=type==12?2:4;if(bytes.size()%width)throw CertificateError{corrupt};for(std::size_t i=0;i<bytes.size();i+=width)std::reverse(bytes.begin()+static_cast<std::ptrdiff_t>(i),bytes.begin()+static_cast<std::ptrdiff_t>(i+width));}
            packet.blob(field+packet.unit,bytes);
        }else raw(field+packet.unit,value,type==1);
    }
    void name(const Der& value,bool unicode){
        const auto groups=children(value);const auto unit=packet.unit;packet.reserve(2*unit);packet.put(0,groups.size());if(groups.empty())return;
        const auto list=packet.reserve(groups.size()*2*unit,unit);packet.point(unit,list);
        for(std::size_t i=0;i<groups.size();++i){const auto attrs=children(groups[i],49);if(attrs.empty())throw CertificateError{corrupt};
            const auto at=list+i*2*unit,array=packet.reserve(attrs.size()*4*unit,unit);packet.put(at,attrs.size());packet.point(at+unit,array);
            for(std::size_t j=0;j<attrs.size();++j){const auto pair=children(attrs[j]);count(pair,2,2);const auto field=array+j*4*unit;oid(field,pair[0]);name_value(field+unit,pair[1],unicode);}
        }
    }
    void public_key(std::size_t field,const Der& value){const auto items=children(value);count(items,2,2);algorithm(field,items[0]);bits(field+3*packet.unit,items[1]);}
    void extensions(std::size_t field,const Der& value){
        const auto items=children(value);const auto unit=packet.unit;packet.put(field,items.size());if(items.empty())return;
        const auto list=packet.reserve(items.size()*4*unit,unit);packet.point(field+unit,list);
        for(std::size_t i=0;i<items.size();++i){const auto fields=children(items[i]);count(fields,2,3);const auto at=list+i*4*unit;oid(at,fields[0]);
            if(fields.size()==3){tag(fields[1],1);if(fields[1].content.size()!=1 || (fields[1].content[0]!=0 && fields[1].content[0]!=255))throw CertificateError{corrupt};packet.put(at+unit,fields[1].content[0]?1:0);}
            tag(fields.back(),4);raw(at+2*unit,fields.back());
        }
    }
    void signed_content(const Der& value){
        const auto items=children(value);count(items,3,3);tag(items[0],48);packet.reserve(8*packet.unit);raw(0,items[0],true);algorithm(2*packet.unit,items[1]);
        if(flags&8)bits(5*packet.unit,items[2]);else {tag(items[2],3);bits(5*packet.unit,items[2]);const auto body=items[2].content.subspan(1);packet.blob(5*packet.unit,std::vector<std::uint8_t>(body.rbegin(),body.rend()));}
    }
    Der to_be_signed(const Der& value){if(flags&2)return value;const auto items=children(value);count(items,3,3);tag(items[1],48);tag(items[2],3);return items[0];}
    void certificate_info(const Der& value){
        const auto fields=children(to_be_signed(value));count(fields,6,10);const auto unit=packet.unit;packet.reserve(unit==4?112:208);std::size_t i=0;
        if(fields[0].tag==160){const auto version=children(fields[i++],160);count(version,1,1);const auto v=integer(version[0]);if(v<0 || v>2)throw CertificateError{corrupt};packet.put(0,static_cast<std::uint64_t>(v));}
        if(fields.size()-i<6)throw CertificateError{corrupt};number_blob(unit,fields[i++],false);algorithm(3*unit,fields[i++]);tag(fields[i],48);raw(6*unit,fields[i++],true);
        const auto validity=children(fields[i++]);count(validity,2,2);packet.put(8*unit,time(validity[0]),8);packet.put(8*unit+8,time(validity[1]),8);
        tag(fields[i],48);raw(8*unit+16,fields[i++],true);public_key(10*unit+16,fields[i++]);
        unsigned previous=0;while(i<fields.size()){
            const auto& item=fields[i++];const unsigned n=item.tag&31;if(n<=previous || n>3 || !(item.tag&128))throw CertificateError{badtag};previous=n;
            if(n<3)bits((unit==4?80U:144U)+(n-1)*(unit==4?12U:24U),item,128+n);
            else {const auto ext=children(item,163);count(ext,1,1);extensions(unit==4?104:192,ext[0]);}
        }
    }
    void alternate(std::size_t field,const Der& value,unsigned expected=48){
        const auto items=children(value,expected);const auto unit=packet.unit;packet.put(field,items.size());if(items.empty())return;
        const auto list=packet.reserve(items.size()*3*unit,unit);packet.point(field+unit,list);
        for(std::size_t i=0;i<items.size();++i)alternate_entry(list+i*3*unit,items[i]);
    }
    void alternate_entry(std::size_t at,const Der& item){
        const auto unit=packet.unit;const unsigned n=item.tag&31;if(!(item.tag&128))throw CertificateError{badtag};packet.put(at,n+1);
            if(n==1 || n==2 || n==6){if(item.tag!=(128+n))throw CertificateError{badtag};std::u16string text;for(auto b:item.content){if(b>127)throw CertificateError{corrupt};text.push_back(b);}packet.point(at+unit,packet.data(utf16_bytes(text)));}
            else if(n==7){tag(item,135);if(item.content.size()!=4 && item.content.size()!=16)throw CertificateError{corrupt};raw(at+unit,item);}
            else if(n==4){const auto inner=children(item,164);count(inner,1,1);tag(inner[0],48);raw(at+unit,inner[0],true);}
            else if(n==8){tag(item,136);auto bytes=std::vector<std::uint8_t>(item.encoded.begin(),item.encoded.end());bytes[0]=6;oid(at+unit,take(bytes));}
            else if(n==0){const auto inner=children(item,160);count(inner,2,2);const auto other=packet.reserve(3*unit,unit);packet.point(at+unit,other);oid(other,inner[0]);const auto payload=children(inner[1],160);count(payload,1,1);raw(other+unit,payload[0],true);}
            else throw CertificateError{50};
    }
    void rsa(const Der& root,unsigned kind){
        const auto values=children(root);const bool priv=kind==43;count(values,priv?9U:2U,priv?9U:2U);
        const std::size_t first=priv?1:0;if(priv && integer(values[0])!=0)throw CertificateError{50};
        const auto modulus=reversed_integer(values[first],true),exponent=reversed_integer(values[first+1],true);
        if(modulus.empty() || !modulus.back() || exponent.empty() || exponent.size()>4)throw CertificateError{large};
        std::uint32_t e=0;for(std::size_t i=0;i<exponent.size();++i)e|=std::uint32_t{exponent[i]}<<(8*i);if(!e)throw CertificateError{corrupt};
        if(kind==72){packet.reserve(24);packet.put(0,0x31415352);packet.put(4,modulus.size()*8);packet.put(8,exponent.size());packet.put(12,modulus.size());
            packet.data(std::vector<std::uint8_t>(exponent.rbegin(),exponent.rend()));packet.data(std::vector<std::uint8_t>(modulus.rbegin(),modulus.rend()));return;}
        const auto width=modulus.size();if(priv && (width<2 || width%2))throw CertificateError{corrupt};
        packet.reserve(20+width+(priv?width+5*(width/2):0));packet.put(0,priv?7:6,1);packet.put(1,2,1);packet.put(4,0xa400);packet.put(8,priv?0x32415352:0x31415352);packet.put(12,width*8);packet.put(16,e);
        std::copy(modulus.begin(),modulus.end(),packet.bytes.begin()+20);
        if(priv){std::size_t at=20+width;for(unsigned i:{4U,5U,6U,7U,8U,3U}){const auto bytes=reversed_integer(values[i],true);const auto n=i==3?width:width/2;
            if(bytes.size()>n)throw CertificateError{corrupt};std::copy(bytes.begin(),bytes.end(),packet.bytes.begin()+static_cast<std::ptrdiff_t>(at));at+=n;}}
    }
    void decode(unsigned kind,const Der& root){
        const auto unit=packet.unit;
        switch(kind){
        case 1:signed_content(root);break;
        case 2:certificate_info(root);break;
        case 5:packet.reserve(2*unit);extensions(0,root);break;
        case 6:case 24:if(kind==24 && string_type(root.tag)<=2)throw CertificateError{0x80092024};packet.reserve(3*unit);name_value(0,root,kind==24);break;
        case 7:case 20:name(root,kind==20);break;
        case 8:packet.reserve(6*unit);public_key(0,root);break;
        case 12:packet.reserve(2*unit);alternate(0,root);break;
        case 14:case 26:packet.reserve(3*unit);bits(0,root);break;
        case 15:{packet.reserve(12);const auto values=children(root);count(values,0,2);std::size_t i=0;
            if(!values.empty() && values[0].tag==1){if(values[0].content.size()!=1 || (values[0].content[0]!=0 && values[0].content[0]!=255))throw CertificateError{corrupt};packet.put(0,values[i++].content[0]?1:0);}
            if(i<values.size()){const auto length=integer(values[i++]);if(length<0)throw CertificateError{corrupt};packet.put(4,1);packet.put(8,static_cast<std::uint64_t>(length));}if(i!=values.size())throw CertificateError{corrupt};break;}
        case 17:case 30:if(kind==17)tag(root,23);packet.reserve(8);packet.put(0,time(root),8);break;
        case 19:case 43:case 72:rsa(root,kind);break;
        case 25:tag(root,4);packet.reserve(2*unit);raw(0,root);break;
        case 27:case 29:packet.reserve(4);packet.put(0,static_cast<std::uint64_t>(integer(root,kind==27?2:10)));break;
        case 40:{const auto fields=children(root);count(fields,2,2);packet.reserve(40);for(unsigned i=0;i<2;++i){const auto bytes=reversed_integer(fields[i],true);if(bytes.size()>20)throw CertificateError{large};std::copy(bytes.begin(),bytes.end(),packet.bytes.begin()+20*i);}break;}
        case 28:case 38:case 39:case 47:{packet.reserve(kind==39?6*unit:kind==47?4*unit:2*unit);
            if(kind==28 || kind==38)number_blob(0,root,kind==38);else {const auto values=children(root);count(values,kind==39?3U:2U,kind==39?3U:2U);for(std::size_t i=0;i<values.size();++i)number_blob(i*2*unit,values[i],true);}break;}
        case 31:{packet.reserve(6*unit);const auto values=children(root);unsigned previous=0;
            for(const auto& value:values){const auto n=value.tag&31U;if(n>2 || !(value.tag&128) || n+1<=previous)throw CertificateError{badtag};previous=n+1;
                if(n==0){tag(value,128);raw(0,value);}else if(n==1)alternate(2*unit,value,161);else {tag(value,130);auto bytes=std::vector<std::uint8_t>(value.encoded.begin(),value.encoded.end());bytes[0]=2;number_blob(4*unit,take(bytes),false);}}break;}
        case 32:{packet.reserve(2*unit);const auto values=children(root);packet.put(0,values.size());if(values.empty())break;const auto list=packet.reserve(values.size()*4*unit,unit);packet.point(unit,list);
            for(std::size_t i=0;i<values.size();++i){const auto pair=children(values[i]);count(pair,2,2);oid(list+i*4*unit,pair[0]);alternate_entry(list+i*4*unit+unit,pair[1]);}break;}
        case 33:{const auto values=children(root);count(values,1,2);packet.reserve(3*unit);oid(0,values[0]);if(values.size()==2){const auto content=children(values[1],160);count(content,1,1);raw(unit,content[0],true);}break;}
        case 34:{const auto values=children(root);packet.reserve(2*unit);packet.put(0,values.size());if(!values.empty()){const auto list=packet.reserve(values.size()*2*unit,unit);packet.point(unit,list);for(std::size_t i=0;i<values.size();++i)raw(list+i*2*unit,values[i],true);}break;}
        case 36:{const auto values=children(root);packet.reserve(2*unit);packet.put(0,values.size());if(!values.empty()){const auto list=packet.reserve(values.size()*unit,unit);packet.point(unit,list);for(std::size_t i=0;i<values.size();++i)oid(list+i*unit,values[i]);}break;}
        case 73:packet.reserve(unit);oid(0,root);break;
        case 74:packet.reserve(3*unit);algorithm(0,root);break;
        default:throw CertificateError{2};
        }
    }
};
unsigned oid_kind(std::string_view oid){
    constexpr std::pair<std::string_view,unsigned> table[]{{"2.5.29.15",14},{"2.5.29.19",15},{"2.5.29.17",12},{"2.5.29.18",12},{"2.5.29.7",12},{"2.5.29.8",12},{"2.5.29.14",25},{"2.5.29.35",31},{"2.5.29.37",36},{"1.3.6.1.5.5.7.1.1",32},{"1.3.6.1.5.5.7.1.11",32},{"1.3.6.1.4.1.311.2.1.14",5},{"1.2.840.10045.2.1",73},{"1.2.840.10045.4.3",74}};
    for(auto [name,kind]:table)if(name==oid)return kind;return 0;
}
#include "certificate_encode.inc"
}
std::span<const WindowsServiceDescriptor> certificate_decode_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{{"crypt32.dll","CryptHashPublicKeyInfo",7},{"crypt32.dll","CryptDecodeObject",7},{"crypt32.dll","CryptDecodeObjectEx",8},{"crypt32.dll","CryptEncodeObject",5},{"crypt32.dll","CryptEncodeObjectEx",7}};return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_certificate_decode(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    using namespace certificate_detail;const WindowsCall call(state,memory,service);const bool ex=service.symbol.ends_with("Ex"),encode=service.symbol.starts_with("CryptEncode");const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto flags=static_cast<std::uint32_t>(encode?(ex?call(3):0):call(4)),encoding=static_cast<std::uint32_t>(call(0));
    const auto out=call(encode?(ex?5:3):(ex?6:5)),length=call(encode?(ex?6:4):(ex?7:6)),parameters=ex?call(encode?4:5):0;
    const auto finish=[&](std::uint64_t result){call.return_to(state,result);return std::optional<std::uint32_t>{};};
    try{
        if(service.symbol=="CryptHashPublicKeyInfo"){
            if((static_cast<std::uint32_t>(call(3))&65535U)!=1)throw CertificateError{2};
            if(!call(4) || !call(6))throw CertificateError{87};
            if(call(2))throw CertificateError{0x80090009U};
            const auto* info=crypto_detail::hash_info(call(1)?static_cast<std::uint32_t>(call(1)):0x8003U);
            if(!info)throw CertificateError{0x80090008U};
            memory.check(GuestAddress{call(6)},4,Access::Write);const auto capacity=memory.load(GuestAddress{call(6)},32);
            Encode encoder{memory,unit,0,[&](std::uint64_t at){return read_ansi(memory,at,65536);}};
            const auto digest=crypto_detail::digest(info->name,encoder.public_key(call(4)));
            memory.store(GuestAddress{call(6)},32,digest.size());
            if(call(5)){if(capacity<digest.size())throw CertificateError{234};memory.write(GuestAddress{call(5)},digest);}
            return finish(1);
        }
        if(!call(1) || !(encoding&0x10001U) || (encoding&~0x10001U))throw CertificateError{87};
        if(flags&~(encode?0xf0028008U:ex?0x2000800fU:0x2000000fU))throw CertificateError{50};
        if(!length && !(flags&0x8000))throw CertificateError{87};if((flags&0x8000) && !out)throw CertificateError{87};
        if(length)memory.check(GuestAddress{length},4,Access::Write);
        if(flags&0x8000){memory.check(GuestAddress{out},unit,Access::Write);memory.store(GuestAddress{out},bits,0);}
        const auto name=call(1)<=65535?"#"+std::to_string(call(1)):"s:"+read_ansi(memory,call(1),1024);
        const auto setname=encode?(ex?"cryptdllencodeobjectex":"cryptdllencodeobject"):(ex?"cryptdlldecodeobjectex":"cryptdlldecodeobject");
        for(const auto& [handle,set]:runtime_.certificates.oid_sets){(void)handle;if(set.name!=setname)continue;
            for(const auto& function:set.functions)if(function.oid==name && function.encoding==(encoding&65535U)){
                std::vector<std::uint64_t> args;for(unsigned i=0;i<(encode?(ex?7U:5U):(ex?8U:7U));++i)args.push_back(call(i));
                start_callbacks(state,memory,{{function.address,std::move(args)}},Continuation::ReturnCallback);return std::nullopt;
            }
        }
        if(encode && ex)for(const auto& [handle,set]:runtime_.certificates.oid_sets){
            (void)handle;if(set.name!="cryptdllencodeobject")continue;
            for(const auto& function:set.functions)if(function.oid==name && function.encoding==(encoding&65535U)){
                if(!(flags&0x8000)){start_callbacks(state,memory,{{function.address,{encoding,call(1),call(2),out,length}}},Continuation::ReturnCallback);return std::nullopt;}
                std::uint64_t allocator=0,release=0;
                if(parameters){if(memory.load(GuestAddress{parameters},32)!=3*unit)throw CertificateError{87};allocator=memory.load(GuestAddress{parameters+unit},bits);release=memory.load(GuestAddress{parameters+2*unit},bits);}
                if(allocator && !handles(GuestAddress{allocator}))memory.check(GuestAddress{allocator},1,Access::Execute);
                if(release && !handles(GuestAddress{release}))memory.check(GuestAddress{release},1,Access::Execute);
                const auto scratch=allocate_heap(memory,112);if(!scratch)throw CertificateError{8};
                try{memory.write(GuestAddress{scratch},std::vector<std::uint8_t>(112));
                    for(auto [offset,value]:std::initializer_list<std::pair<unsigned,std::uint64_t>>{{0,out},{8,length},{24,allocator},{32,release},{48,function.address},{56,1},{64,encoding},{72,call(1)},{80,call(2)}})memory.store(GuestAddress{scratch+offset},64,value);
                    start_callbacks(state,memory,{{function.address,{encoding,call(1),call(2),0,scratch+16}}},Continuation::CertificateDecodeAllocation,scratch);
                }catch(...){(void)free_heap(memory,scratch,state);throw;}return std::nullopt;
            }
        }
        const auto kind=call(1)<=65535?static_cast<unsigned>(call(1)):oid_kind(name.substr(2));if(!kind)throw CertificateError{2};
        CertificatePacket packet(x86_);
        if(encode){Encode encoder{memory,unit,flags,[&](std::uint64_t at){return read_ansi(memory,at,65536);}};packet.bytes=encoder.encode(kind,call(2));}
        else {
            const auto input_size=static_cast<std::uint32_t>(call(3));if(!call(2) || !input_size)throw CertificateError{eof};if(input_size>output_limit)throw CertificateError{8};
            std::vector<std::uint8_t> input(input_size);memory.read(GuestAddress{call(2)},input);const auto root=take(input);if(root.encoded.size()!=input.size())throw CertificateError{corrupt};
            Decode decode(x86_,call(2),flags);
            decode.share=[&](const std::string& oid){auto& cache=runtime_.certificates.decoded_oids;if(auto it=cache.find(oid);it!=cache.end())return it->second;
                if(cache.size()>=4096)throw CertificateError{8};std::vector<std::uint8_t> bytes(oid.begin(),oid.end());bytes.push_back(0);const auto pointer=runtime_allocate(memory,bytes);cache.emplace(oid,pointer);return pointer;};
            decode.decode(kind,root);packet=std::move(decode.packet);
        }
        if(packet.bytes.size()>output_limit)throw CertificateError{8};
        std::uint64_t allocator=0,release=0;
        if(parameters){
            if(memory.load(GuestAddress{parameters},32)!=3*unit)throw CertificateError{87};
            allocator=memory.load(GuestAddress{parameters+unit},bits);release=memory.load(GuestAddress{parameters+2*unit},bits);
        }
        const auto capacity=length && !(flags&0x8000)?memory.load(GuestAddress{length},32):0;if(length)memory.store(GuestAddress{length},32,packet.bytes.size());
        if(!(flags&0x8000)){if(out){if(capacity<packet.bytes.size())throw CertificateError{234};memory.check(GuestAddress{out},packet.bytes.size(),Access::Write);packet.relocate(out);memory.write(GuestAddress{out},packet.bytes);}return finish(1);}
        if(allocator){
            if(!handles(GuestAddress{allocator}))memory.check(GuestAddress{allocator},1,Access::Execute);if(release && !handles(GuestAddress{release}))memory.check(GuestAddress{release},1,Access::Execute);
            const auto tail=(64+packet.bytes.size()+7)&~std::size_t{7},size=tail+16*packet.pointers.size();if(size>output_limit)throw CertificateError{8};const auto scratch=allocate_heap(memory,size);if(!scratch)throw CertificateError{8};
            try{
                for(auto [offset,value]:std::initializer_list<std::pair<unsigned,std::uint64_t>>{{0,out},{8,length},{16,packet.bytes.size()},{24,packet.pointers.size()},{32,release},{40,0},{48,tail},{56,0}})memory.store(GuestAddress{scratch+offset},64,value);
                memory.write(GuestAddress{scratch+64},packet.bytes);for(std::size_t i=0;i<packet.pointers.size();++i){memory.store(GuestAddress{scratch+tail+i*16},64,packet.pointers[i].first);memory.store(GuestAddress{scratch+tail+i*16+8},64,packet.pointers[i].second);}
                start_callbacks(state,memory,{{allocator,{packet.bytes.size()}}},Continuation::CertificateDecodeAllocation,scratch);
            }catch(...){(void)free_heap(memory,scratch,state);throw;}return std::nullopt;
        }
        const auto allocation=allocate_heap(memory,packet.bytes.size());if(!allocation)throw CertificateError{8};
        try{packet.relocate(allocation);memory.write(GuestAddress{allocation},packet.bytes);runtime_.local_allocations.insert(allocation);memory.store(GuestAddress{out},bits,allocation);}
        catch(...){runtime_.local_allocations.erase(allocation);(void)free_heap(memory,allocation,state);throw;}return finish(1);
    }catch(const CertificateError& error){ERR_clear_error();last_error_=error.code;return finish(0);}
    catch(const CertificateNameError& error){ERR_clear_error();last_error_=error.code;return finish(0);}
    catch(const MemoryFault&){last_error_=87;return finish(0);}
    catch(const std::invalid_argument&){last_error_=corrupt;return finish(0);}
    catch(const std::bad_alloc&){last_error_=8;return finish(0);}
    catch(const std::runtime_error&){last_error_=0x80090020U;return finish(0);}
}
std::optional<std::uint32_t> WindowsServices::resume_certificate_decode(GuestState& state,GuestMemory& memory,std::uint64_t scratch,std::uint64_t value){
    const auto read=[&](unsigned offset){return memory.load(GuestAddress{scratch+offset},64);};
    const auto finish=[&](std::uint64_t result){(void)free_heap(memory,scratch,state);complete_call(state,memory,result);return std::optional<std::uint32_t>{};};
    if(read(56)==1){
        // 傳統編碼回呼的兩階段配置橋接，沿用同一客體回呼續接器。
        const auto stage=read(40);const auto pointer_bits=static_cast<std::uint8_t>(x86_?32:64);
        if(stage==3){last_error_=static_cast<std::uint32_t>(read(96));return finish(0);}
        if(stage==2){
            const auto size=read(16),allocation=read(88);auto error=last_error_;
            try{if(value && size<=read(104)){memory.store(GuestAddress{read(0)},pointer_bits,allocation);if(read(8))memory.store(GuestAddress{read(8)},32,size);return finish(1);}if(value)error=234;}
            catch(const MemoryFault&){error=87;}
            if(!read(24)){runtime_.local_allocations.erase(allocation);(void)free_heap(memory,allocation,state);last_error_=error;return finish(0);}
            if(read(32)){memory.store(GuestAddress{scratch+40},64,3);memory.store(GuestAddress{scratch+96},64,error);start_callbacks(state,memory,{{read(32),{allocation}}},Continuation::CertificateDecodeAllocation,scratch);return std::nullopt;}
            last_error_=error;return finish(0);
        }
        if(!value){if(stage==1)last_error_=8;return finish(0);}
        const auto size=read(16);if(!size || size>output_limit){last_error_=size?8U:87U;return finish(0);}
        if(!stage){
            if(read(8))memory.store(GuestAddress{read(8)},32,size);memory.store(GuestAddress{scratch+104},64,size);
            if(read(24)){memory.store(GuestAddress{scratch+40},64,1);start_callbacks(state,memory,{{read(24),{size}}},Continuation::CertificateDecodeAllocation,scratch);return std::nullopt;}
        }
        const auto allocation=stage==1?(x86_?value&UINT32_MAX:value):allocate_heap(memory,size);
        if(!allocation){last_error_=8;return finish(0);}if(!read(24))runtime_.local_allocations.insert(allocation);
        memory.store(GuestAddress{scratch+88},64,allocation);memory.store(GuestAddress{scratch+40},64,2);
        start_callbacks(state,memory,{{read(48),{read(64),read(72),read(80),allocation,scratch+16}}},Continuation::CertificateDecodeAllocation,scratch);return std::nullopt;
    }
    if(read(40)){last_error_=87;return finish(0);}
    const auto allocation=x86_?value&UINT32_MAX:value;if(!allocation){last_error_=8;return finish(0);}
    try{
        const auto size=read(16),count=read(24),tail=read(48);
        const auto owner=allocations_.find(scratch);if(owner==allocations_.end() || size>output_limit || tail!=((64+size+7)&~UINT64_C(7)) || tail>owner->second || count>(owner->second-tail)/16)throw CertificateError{87};
        memory.check(GuestAddress{allocation},static_cast<std::size_t>(size),Access::Write);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));memory.read(GuestAddress{scratch+64},bytes);
        for(std::uint64_t i=0;i<count;++i){const auto field=memory.load(GuestAddress{scratch+tail+i*16},64),target=memory.load(GuestAddress{scratch+tail+i*16+8},64);
            if(field>size || (x86_?4U:8U)>size-field || target>=size || allocation>UINT64_MAX-target)throw CertificateError{87};
            for(unsigned n=0;n<(x86_?4U:8U);++n)bytes.at(static_cast<std::size_t>(field+n))=static_cast<std::uint8_t>((allocation+target)>>(n*8));
        }
        memory.check(GuestAddress{read(0)},x86_?4:8,Access::Write);if(read(8))memory.check(GuestAddress{read(8)},4,Access::Write);
        memory.write(GuestAddress{allocation},bytes);memory.store(GuestAddress{read(0)},x86_?32:64,allocation);if(read(8))memory.store(GuestAddress{read(8)},32,size);return finish(1);
    }catch(const MemoryFault&){last_error_=87;}catch(const CertificateError&){last_error_=87;}
    if(read(32)){memory.store(GuestAddress{scratch+40},64,1);try{start_callbacks(state,memory,{{read(32),{allocation}}},Continuation::CertificateDecodeAllocation,scratch);}catch(...){(void)free_heap(memory,scratch,state);throw;}return std::nullopt;}
    return finish(0);
}
}
