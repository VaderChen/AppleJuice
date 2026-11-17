#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <ctime>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// 簽署目錄（.cat）的 DER 編解碼：外層 PKCS#7 SignedData（內容類型 szOID_CTL），CTL 的 TrustedSubjects 為成員（SubjectIdentifier 為參考標記的 UTF-16LE），
// 成員屬性 SPC_INDIRECT_DATA（1.3.6.1.4.1.311.2.1.4）、CAT_MEMBERINFO（.12.2.2）與 CAT_NAMEVALUE（.12.2.1）；目錄屬性放在 ctlExtensions 的 CAT_NAMEVALUE 擴充。
// 寫出的目錄沒有簽署者（與 CryptCATPersistStore 相同，之後另行簽署）。CryptCAT* 與 WVTAsn1Cat* 共用。
namespace x64arm::catalog_codec {
using Bytes=std::vector<std::uint8_t>;
struct NameValue {std::u16string tag;std::uint32_t flags{};Bytes value;};
struct Member {Bytes identifier;std::optional<Bytes> indirect;std::optional<std::u16string> subject;std::uint32_t version{};std::vector<NameValue> attributes;};
struct Catalog {std::uint32_t version{1};Bytes list_identifier;std::vector<Member> members;std::vector<NameValue> attributes;};

struct Tlv {std::uint8_t tag{};std::span<const std::uint8_t> value;std::span<const std::uint8_t> whole;};
// 讀出一個 TLV（長度最多 4 個位元組）；資料不足時回傳空值。
inline std::optional<Tlv> read(std::span<const std::uint8_t>& data){
    if(data.size()<2)return std::nullopt;const auto tag=data[0];std::size_t length=data[1],header=2;
    if(length&0x80U){const auto count=length&0x7fU;if(!count || count>4 || data.size()<2+count)return std::nullopt;length=0;for(std::size_t i=0;i<count;++i)length=(length<<8)|data[2+i];header=2+count;}
    if(data.size()-header<length)return std::nullopt;
    Tlv result{tag,data.subspan(header,length),data.subspan(0,header+length)};data=data.subspan(header+length);return result;
}
inline Bytes tlv(std::uint8_t tag,std::span<const std::uint8_t> body){
    Bytes out{tag};const auto n=body.size();
    if(n<0x80)out.push_back(static_cast<std::uint8_t>(n));
    else{unsigned count=0;for(auto v=n;v;v>>=8)++count;out.push_back(static_cast<std::uint8_t>(0x80U|count));for(unsigned i=count;i-- >0;)out.push_back(static_cast<std::uint8_t>(n>>(8*i)));}
    out.insert(out.end(),body.begin(),body.end());return out;
}
inline Bytes concat(std::initializer_list<Bytes> parts){Bytes out;for(const auto& part:parts)out.insert(out.end(),part.begin(),part.end());return out;}
inline Bytes sequence(std::initializer_list<Bytes> parts){return tlv(0x30,concat(parts));}
inline Bytes oid(std::string_view text){
    std::vector<std::uint64_t> parts;std::uint64_t value=0;bool any=false;
    for(const char c:text){if(c=='.'){parts.push_back(value);value=0;any=false;}else{value=value*10+static_cast<std::uint64_t>(c-'0');any=true;}}if(any)parts.push_back(value);
    Bytes body;if(parts.size()<2)return tlv(6,body);body.push_back(static_cast<std::uint8_t>(parts[0]*40+parts[1]));
    for(std::size_t i=2;i<parts.size();++i){Bytes encoded;auto v=parts[i];encoded.push_back(static_cast<std::uint8_t>(v&0x7fU));v>>=7;while(v){encoded.push_back(static_cast<std::uint8_t>(0x80U|(v&0x7fU)));v>>=7;}
        body.insert(body.end(),encoded.rbegin(),encoded.rend());}
    return tlv(6,body);
}
// 物件識別碼的點分文字（CRYPTCATATTRIBUTE 的 NAMEOBJID 與 SIP_INDIRECT_DATA 使用）。
inline std::string oid_text(std::span<const std::uint8_t> body){
    std::string text;if(body.empty())return text;text=std::to_string(body[0]/40)+"."+std::to_string(body[0]%40);std::uint64_t value=0;
    for(std::size_t i=1;i<body.size();++i){value=(value<<7)|(body[i]&0x7fU);if(!(body[i]&0x80U)){text+="."+std::to_string(value);value=0;}}
    return text;
}
inline Bytes integer(std::uint64_t value){Bytes body;do{body.insert(body.begin(),static_cast<std::uint8_t>(value&0xffU));value>>=8;}while(value);if(body.front()&0x80U)body.insert(body.begin(),0);return tlv(2,body);}
inline std::optional<std::uint32_t> integer_value(const Tlv& item){
    if(item.tag!=2 || item.value.empty() || item.value.size()>5 || (item.value.size()==5 && item.value[0]))return std::nullopt;
    std::uint64_t value=0;for(const auto byte:item.value)value=(value<<8)|byte;return static_cast<std::uint32_t>(value);
}
// BMPString 為 UTF-16 大端序。
inline Bytes bmp(std::u16string_view text){Bytes body;for(const auto c:text){body.push_back(static_cast<std::uint8_t>(c>>8));body.push_back(static_cast<std::uint8_t>(c));}return tlv(0x1e,body);}
inline std::u16string bmp_text(std::span<const std::uint8_t> body){std::u16string text;for(std::size_t i=0;i+1<body.size();i+=2)text.push_back(static_cast<char16_t>((body[i]<<8)|body[i+1]));while(!text.empty() && !text.back())text.pop_back();return text;}
inline bool same(std::span<const std::uint8_t> a,const Bytes& b){return std::equal(a.begin(),a.end(),b.begin(),b.end());}
inline Bytes body_of(const Bytes& encoded){std::span<const std::uint8_t> data(encoded);const auto item=read(data);return item?Bytes(item->value.begin(),item->value.end()):Bytes{};}
inline const Bytes& oid_name_value(){static const Bytes value=body_of(oid("1.3.6.1.4.1.311.12.2.1"));return value;}
inline const Bytes& oid_member_info(){static const Bytes value=body_of(oid("1.3.6.1.4.1.311.12.2.2"));return value;}
inline const Bytes& oid_indirect(){static const Bytes value=body_of(oid("1.3.6.1.4.1.311.2.1.4"));return value;}
inline const Bytes& oid_ctl(){static const Bytes value=body_of(oid("1.3.6.1.4.1.311.10.1"));return value;}
inline const Bytes& oid_signed(){static const Bytes value=body_of(oid("1.2.840.113549.1.7.2"));return value;}
inline const Bytes& oid_member_v2(){static const Bytes value=body_of(oid("1.3.6.1.4.1.311.12.1.3"));return value;}

// CAT_NAMEVALUE ::= SEQUENCE { tag BMPString, flags INTEGER, value OCTET STRING }
inline Bytes encode_name_value(const NameValue& item){return sequence({bmp(item.tag),integer(item.flags),tlv(4,item.value)});}
inline std::optional<NameValue> decode_name_value(std::span<const std::uint8_t> data){
    const auto outer=read(data);if(!outer || outer->tag!=0x30 || !data.empty())return std::nullopt;auto fields=outer->value;
    const auto tag=read(fields),flags=tag?read(fields):std::nullopt,value=flags?read(fields):std::nullopt;
    if(!value || tag->tag!=0x1e || value->tag!=4)return std::nullopt;const auto number=integer_value(*flags);if(!number)return std::nullopt;
    return NameValue{bmp_text(tag->value),*number,Bytes(value->value.begin(),value->value.end())};
}
// CAT_MEMBERINFO ::= SEQUENCE { subjectGuid BMPString, certVersion INTEGER }
inline Bytes encode_member_info(std::u16string_view subject,std::uint32_t version){return sequence({bmp(subject),integer(version)});}
inline std::optional<std::pair<std::u16string,std::uint32_t>> decode_member_info(std::span<const std::uint8_t> data){
    const auto outer=read(data);if(!outer || outer->tag!=0x30 || !data.empty())return std::nullopt;auto fields=outer->value;
    const auto subject=read(fields),version=subject?read(fields):std::nullopt;if(!version || subject->tag!=0x1e)return std::nullopt;
    const auto number=integer_value(*version);if(!number)return std::nullopt;return std::pair{bmp_text(subject->value),*number};
}

// 解出目錄；不是簽署目錄或 CTL 結構不合法時回傳空值。
inline std::optional<Catalog> decode(std::span<const std::uint8_t> file){
    auto data=file;const auto info=read(data);if(!info || info->tag!=0x30)return std::nullopt;auto fields=info->value;
    const auto type=read(fields);const auto explicit_content=type?read(fields):std::nullopt;if(!explicit_content || type->tag!=6 || !same(type->value,oid_signed()) || explicit_content->tag!=0xa0)return std::nullopt;
    auto signed_body=explicit_content->value;const auto signed_data=read(signed_body);if(!signed_data || signed_data->tag!=0x30)return std::nullopt;auto parts=signed_data->value;
    const auto version=read(parts);const auto algorithms=version?read(parts):std::nullopt;const auto content=algorithms?read(parts):std::nullopt;if(!content || content->tag!=0x30)return std::nullopt;
    auto inner=content->value;const auto inner_type=read(inner);const auto wrapped=inner_type?read(inner):std::nullopt;
    if(!wrapped || inner_type->tag!=6 || !same(inner_type->value,oid_ctl()) || wrapped->tag!=0xa0)return std::nullopt;
    auto ctl_data=wrapped->value;const auto ctl=read(ctl_data);if(!ctl || ctl->tag!=0x30)return std::nullopt;auto items=ctl->value;
    Catalog catalog;auto next=read(items);
    if(next && next->tag==2)next=read(items);                         // version
    if(!next || next->tag!=0x30)return std::nullopt;next=read(items);   // subjectUsage
    if(next && next->tag==4){catalog.list_identifier.assign(next->value.begin(),next->value.end());next=read(items);}
    if(next && next->tag==2)next=read(items);                         // sequenceNumber
    if(!next || (next->tag!=0x17 && next->tag!=0x18))return std::nullopt;next=read(items);
    if(next && (next->tag==0x17 || next->tag==0x18))next=read(items);
    if(!next || next->tag!=0x30)return std::nullopt;                   // subjectAlgorithm
    {auto algorithm=next->value;const auto identifier=read(algorithm);if(identifier && identifier->tag==6 && same(identifier->value,oid_member_v2()))catalog.version=2;}
    next=read(items);
    if(next && next->tag==0x30){
        auto subjects=next->value;
        while(!subjects.empty()){
            const auto subject=read(subjects);if(!subject || subject->tag!=0x30)return std::nullopt;auto subject_fields=subject->value;
            const auto identifier=read(subject_fields);if(!identifier || identifier->tag!=4)return std::nullopt;
            Member member;member.identifier.assign(identifier->value.begin(),identifier->value.end());
            if(const auto attributes=read(subject_fields);attributes && attributes->tag==0x31){
                auto list=attributes->value;
                while(!list.empty()){
                    const auto attribute=read(list);if(!attribute || attribute->tag!=0x30)return std::nullopt;auto pair=attribute->value;
                    const auto kind=read(pair);const auto values=kind?read(pair):std::nullopt;if(!values || kind->tag!=6 || values->tag!=0x31)return std::nullopt;
                    auto value_list=values->value;const auto first=read(value_list);if(!first)continue;
                    if(same(kind->value,oid_indirect()))member.indirect=Bytes(first->whole.begin(),first->whole.end());
                    else if(same(kind->value,oid_member_info())){if(const auto decoded=decode_member_info(first->whole)){member.subject=decoded->first;member.version=decoded->second;}}
                    else if(same(kind->value,oid_name_value())){if(const auto decoded=decode_name_value(first->whole))member.attributes.push_back(*decoded);}
                }
            }
            catalog.members.push_back(std::move(member));
        }
        next=read(items);
    }
    if(next && next->tag==0xa0){
        auto wrapper=next->value;const auto extensions=read(wrapper);if(!extensions || extensions->tag!=0x30)return std::nullopt;auto list=extensions->value;
        while(!list.empty()){
            const auto extension=read(list);if(!extension || extension->tag!=0x30)return std::nullopt;auto pair=extension->value;
            const auto kind=read(pair);auto value=kind?read(pair):std::nullopt;if(value && value->tag==1)value=read(pair);
            if(!value || kind->tag!=6 || value->tag!=4)return std::nullopt;
            if(same(kind->value,oid_name_value()))if(const auto decoded=decode_name_value(value->value))catalog.attributes.push_back(*decoded);
        }
    }
    return catalog;
}

// 寫出未簽署目錄；this_update 為 UTC 秒數。DER 的 SET OF 依編碼排序。
inline Bytes encode(const Catalog& catalog,std::time_t this_update){
    char stamp[16]{};std::tm parts{};gmtime_r(&this_update,&parts);std::strftime(stamp,sizeof(stamp),"%y%m%d%H%M%SZ",&parts);
    const std::string_view stamp_text(stamp);const Bytes time=tlv(0x17,std::span(reinterpret_cast<const std::uint8_t*>(stamp_text.data()),stamp_text.size()));
    const auto set=[](std::vector<Bytes> items){std::sort(items.begin(),items.end());Bytes body;for(const auto& item:items)body.insert(body.end(),item.begin(),item.end());return tlv(0x31,body);};
    Bytes subjects;
    for(const auto& member:catalog.members){
        std::vector<Bytes> attributes;
        for(const auto& value:member.attributes)attributes.push_back(sequence({oid("1.3.6.1.4.1.311.12.2.1"),set({encode_name_value(value)})}));
        if(member.subject)attributes.push_back(sequence({oid("1.3.6.1.4.1.311.12.2.2"),set({encode_member_info(*member.subject,member.version)})}));
        if(member.indirect)attributes.push_back(sequence({oid("1.3.6.1.4.1.311.2.1.4"),set({*member.indirect})}));
        const auto subject=attributes.empty()?sequence({tlv(4,member.identifier)}):sequence({tlv(4,member.identifier),set(std::move(attributes))});
        subjects.insert(subjects.end(),subject.begin(),subject.end());
    }
    Bytes extensions;for(const auto& value:catalog.attributes){const auto extension=sequence({oid("1.3.6.1.4.1.311.12.2.1"),tlv(4,encode_name_value(value))});extensions.insert(extensions.end(),extension.begin(),extension.end());}
    Bytes ctl_body=concat({sequence({oid("1.3.6.1.4.1.311.12.1.1")}),tlv(4,catalog.list_identifier),time,
        sequence({oid(catalog.version==2?"1.3.6.1.4.1.311.12.1.3":"1.3.6.1.4.1.311.12.1.2"),Bytes{5,0}}),tlv(0x30,subjects)});
    if(!catalog.attributes.empty()){const auto wrapped=tlv(0xa0,tlv(0x30,extensions));ctl_body.insert(ctl_body.end(),wrapped.begin(),wrapped.end());}
    const auto ctl=tlv(0x30,ctl_body);
    const auto signed_data=sequence({integer(1),tlv(0x31,sequence({oid("1.3.14.3.2.26"),Bytes{5,0}})),sequence({oid("1.3.6.1.4.1.311.10.1"),tlv(0xa0,ctl)}),tlv(0x31,Bytes{})});
    return sequence({oid("1.2.840.113549.1.7.2"),tlv(0xa0,signed_data)});
}

// Authenticode 結構（WVTAsn1Spc* 與目錄成員的 SPC_INDIRECT_DATA 共用）。
// SpcString ::= CHOICE { unicode [0] IMPLICIT BMPString, ascii [1] IMPLICIT IA5String }
inline Bytes encode_spc_string(std::u16string_view text){Bytes body;for(const auto c:text){body.push_back(static_cast<std::uint8_t>(c>>8));body.push_back(static_cast<std::uint8_t>(c));}return tlv(0x80,body);}
inline std::optional<std::u16string> decode_spc_string(const Tlv& item){
    if(item.tag==0x80 || item.tag==0x1e){std::u16string text;for(std::size_t i=0;i+1<item.value.size();i+=2)text.push_back(static_cast<char16_t>((item.value[i]<<8)|item.value[i+1]));return text;}
    if(item.tag==0x81 || item.tag==0x16)return std::u16string(item.value.begin(),item.value.end());return std::nullopt;
}
// SpcLink ::= CHOICE { url [0] IMPLICIT IA5String, moniker [1] IMPLICIT SpcSerializedObject, file [2] EXPLICIT SpcString }；choice 1 URL、2 moniker、3 file。
struct Link {std::uint32_t choice{};std::u16string text;std::array<std::uint8_t,16> class_id{};Bytes serialized;};
inline std::optional<Bytes> encode_link(const Link& link){
    if(link.choice==1){Bytes body;for(const auto c:link.text){if(c>0x7f)return std::nullopt;body.push_back(static_cast<std::uint8_t>(c));}return tlv(0x80,body);}
    if(link.choice==2)return tlv(0xa1,concat({tlv(4,link.class_id),tlv(4,link.serialized)}));
    if(link.choice==3)return tlv(0xa2,encode_spc_string(link.text));
    return std::nullopt;
}
inline std::optional<Link> decode_link_item(const Tlv& item){
    Link link;
    if(item.tag==0x80){link.choice=1;link.text.assign(item.value.begin(),item.value.end());return link;}
    if(item.tag==0xa1){auto fields=item.value;const auto id=read(fields),data=id?read(fields):std::nullopt;if(!data || id->tag!=4 || id->value.size()!=16 || data->tag!=4)return std::nullopt;
        link.choice=2;std::copy(id->value.begin(),id->value.end(),link.class_id.begin());link.serialized.assign(data->value.begin(),data->value.end());return link;}
    if(item.tag==0xa2){auto inner=item.value;const auto text=read(inner);if(!text)return std::nullopt;const auto value=decode_spc_string(*text);if(!value)return std::nullopt;link.choice=3;link.text=*value;return link;}
    return std::nullopt;
}
inline std::optional<Link> decode_link(std::span<const std::uint8_t> data){const auto item=read(data);if(!item || !data.empty())return std::nullopt;return decode_link_item(*item);}
// SpcPeImageData ::= SEQUENCE { flags BIT STRING, file [0] EXPLICIT SpcLink OPTIONAL }
struct PeImageData {Bytes flags;std::uint32_t unused_bits{};std::optional<Link> file;};
inline std::optional<Bytes> encode_pe_image(const PeImageData& image){
    Bytes bits{static_cast<std::uint8_t>(image.unused_bits)};bits.insert(bits.end(),image.flags.begin(),image.flags.end());Bytes body=tlv(3,bits);
    if(image.file){const auto link=encode_link(*image.file);if(!link)return std::nullopt;const auto wrapped=tlv(0xa0,*link);body.insert(body.end(),wrapped.begin(),wrapped.end());}
    return tlv(0x30,body);
}
inline std::optional<PeImageData> decode_pe_image(std::span<const std::uint8_t> data){
    const auto outer=read(data);if(!outer || outer->tag!=0x30 || !data.empty())return std::nullopt;auto fields=outer->value;PeImageData image;
    auto next=read(fields);
    if(next && next->tag==3){if(next->value.empty() || next->value[0]>7)return std::nullopt;image.unused_bits=next->value[0];image.flags.assign(next->value.begin()+1,next->value.end());next=read(fields);}
    if(next && next->tag==0xa0){auto inner=next->value;const auto item=read(inner);if(!item)return std::nullopt;image.file=decode_link_item(*item);if(!image.file)return std::nullopt;next=read(fields);}
    if(next)return std::nullopt;return image;
}
// SpcIndirectDataContent ::= SEQUENCE { data SEQUENCE { type OID, value ANY OPTIONAL }, messageDigest SEQUENCE { SEQUENCE { algorithm OID, parameters ANY OPTIONAL }, digest OCTET STRING } }
struct IndirectData {std::string type;Bytes value;std::string algorithm;Bytes parameters;Bytes digest;};
inline Bytes encode_indirect(const IndirectData& item){
    const auto data=item.value.empty()?sequence({oid(item.type)}):sequence({oid(item.type),item.value});
    const auto algorithm=sequence({oid(item.algorithm),item.parameters.empty()?Bytes{5,0}:item.parameters});
    return sequence({data,sequence({algorithm,tlv(4,item.digest)})});
}
inline std::optional<IndirectData> decode_indirect(std::span<const std::uint8_t> data){
    const auto outer=read(data);if(!outer || outer->tag!=0x30 || !data.empty())return std::nullopt;auto fields=outer->value;
    const auto attribute=read(fields),digest=attribute?read(fields):std::nullopt;if(!digest || attribute->tag!=0x30 || digest->tag!=0x30)return std::nullopt;
    auto attribute_fields=attribute->value;const auto type=read(attribute_fields);if(!type || type->tag!=6)return std::nullopt;
    auto digest_fields=digest->value;const auto algorithm=read(digest_fields),hash=algorithm?read(digest_fields):std::nullopt;if(!hash || algorithm->tag!=0x30 || hash->tag!=4)return std::nullopt;
    auto algorithm_fields=algorithm->value;const auto algorithm_id=read(algorithm_fields);if(!algorithm_id || algorithm_id->tag!=6)return std::nullopt;
    return IndirectData{oid_text(type->value),Bytes(attribute_fields.begin(),attribute_fields.end()),oid_text(algorithm_id->value),Bytes(algorithm_fields.begin(),algorithm_fields.end()),Bytes(hash->value.begin(),hash->value.end())};
}
// SpcSpOpusInfo ::= SEQUENCE { programName [0] EXPLICIT SpcString OPTIONAL, moreInfo [1] EXPLICIT SpcLink OPTIONAL, publisherInfo [2] EXPLICIT SpcLink OPTIONAL }
struct OpusInfo {std::optional<std::u16string> program;std::optional<Link> more,publisher;};
inline std::optional<Bytes> encode_opus(const OpusInfo& info){
    Bytes body;
    if(info.program){const auto wrapped=tlv(0xa0,encode_spc_string(*info.program));body.insert(body.end(),wrapped.begin(),wrapped.end());}
    for(const auto& [tag,link]:{std::pair<std::uint8_t,const std::optional<Link>*>{0xa1,&info.more},{0xa2,&info.publisher}})
        if(*link){const auto encoded=encode_link(**link);if(!encoded)return std::nullopt;const auto wrapped=tlv(tag,*encoded);body.insert(body.end(),wrapped.begin(),wrapped.end());}
    return tlv(0x30,body);
}
inline std::optional<OpusInfo> decode_opus(std::span<const std::uint8_t> data){
    const auto outer=read(data);if(!outer || outer->tag!=0x30 || !data.empty())return std::nullopt;auto fields=outer->value;OpusInfo info;
    while(!fields.empty()){
        const auto item=read(fields);if(!item)return std::nullopt;auto inner=item->value;const auto value=read(inner);if(!value)return std::nullopt;
        if(item->tag==0xa0){info.program=decode_spc_string(*value);if(!info.program)return std::nullopt;}
        else if(item->tag==0xa1){info.more=decode_link_item(*value);if(!info.more)return std::nullopt;}
        else if(item->tag==0xa2){info.publisher=decode_link_item(*value);if(!info.publisher)return std::nullopt;}
        else return std::nullopt;
    }
    return info;
}
// SpcFinancialCriteria ::= SEQUENCE { financialInfoAvailable BOOLEAN, meetsCriteria BOOLEAN }
inline Bytes encode_financial(bool available,bool meets){return sequence({tlv(1,Bytes{static_cast<std::uint8_t>(available?0xff:0)}),tlv(1,Bytes{static_cast<std::uint8_t>(meets?0xff:0)})});}
inline std::optional<std::pair<bool,bool>> decode_financial(std::span<const std::uint8_t> data){
    const auto outer=read(data);if(!outer || outer->tag!=0x30 || !data.empty())return std::nullopt;auto fields=outer->value;
    const auto first=read(fields),second=first?read(fields):std::nullopt;if(!second || first->tag!=1 || second->tag!=1 || first->value.size()!=1 || second->value.size()!=1 || !fields.empty())return std::nullopt;
    return std::pair{first->value[0]!=0,second->value[0]!=0};
}
}
