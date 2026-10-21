#pragma once
#include "x64arm/unicode.hpp"
#include <openssl/x509.h>
#include <chrono>
#include <algorithm>
#include <memory>
#include <span>
#include <vector>
namespace x64arm::certificate_detail {
struct CertificateError {std::uint32_t code;};
template<class T,class Encoder> std::vector<std::uint8_t> encoded(T* value,Encoder encoder){
    const auto size=encoder(value,nullptr);if(size<=0)throw CertificateError{0x8009310b};std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));auto* out=bytes.data();if(encoder(value,&out)!=size)throw CertificateError{0x8009310b};return bytes;
}
// 共用 DER 容器組合，保留 Windows 允許但 X509_NAME 高階介面拒絕的值型別。
inline std::vector<std::uint8_t> der_object(int tag,std::span<const std::uint8_t> contents,bool constructed=false,int object_class=V_ASN1_UNIVERSAL){
    if(contents.size()>16U*1024U*1024U)throw CertificateError{8};
    const auto length=static_cast<int>(contents.size()),size=ASN1_object_size(constructed,length,tag);
    if(size<=0)throw CertificateError{0x8009310b};std::vector<std::uint8_t> result(static_cast<std::size_t>(size));auto* at=result.data();
    ASN1_put_object(&at,constructed,length,tag,object_class);std::copy(contents.begin(),contents.end(),at);return result;
}
inline std::vector<std::uint8_t> der_container(int tag,std::span<const std::uint8_t> contents){return der_object(tag,contents,true);}
inline std::string oid_name(const ASN1_OBJECT* object){
    const auto count=OBJ_obj2txt(nullptr,0,object,1);if(count<=0 || count>1024)throw CertificateError{0x8009310b};std::string value(static_cast<std::size_t>(count)+1,'\0');OBJ_obj2txt(value.data(),count+1,object,1);value.resize(static_cast<std::size_t>(count));return value;
}
inline std::u16string certificate_string(const ASN1_STRING* value,bool legacy=false){
    if(!value)return {};const auto length=ASN1_STRING_length(value);if(length<0)throw CertificateError{0x8009310b};
    const auto* data=ASN1_STRING_get0_data(value);
    if(ASN1_STRING_type(value)==V_ASN1_T61STRING){
        const std::string_view text(reinterpret_cast<const char*>(data),static_cast<std::size_t>(length));
        if(!legacy){try{return utf8_to_utf16(text);}catch(const std::invalid_argument&) {}}
        std::u16string result;for(char c:text)result.push_back(static_cast<unsigned char>(c));return result;
    }
    unsigned char* converted=nullptr;const auto size=ASN1_STRING_to_UTF8(&converted,value);
    const auto release=[](unsigned char* p){OPENSSL_free(p);};std::unique_ptr<unsigned char,decltype(release)> owned(converted,release);
    if(size<0)throw CertificateError{0x8009310b};return utf8_to_utf16_lossy(std::string_view(reinterpret_cast<const char*>(converted),static_cast<std::size_t>(size)));
}
inline std::uint64_t certificate_time(const ASN1_TIME* value){
    std::tm parts{};if(!ASN1_TIME_to_tm(value,&parts))throw CertificateError{0x8009310b};
    const auto date=std::chrono::year{parts.tm_year+1900}/std::chrono::month{static_cast<unsigned>(parts.tm_mon+1)}/std::chrono::day{static_cast<unsigned>(parts.tm_mday)};
    if(!date.ok())throw CertificateError{0x8009310b};const auto seconds=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::sys_days(date).time_since_epoch()).count()+parts.tm_hour*3600+parts.tm_min*60+parts.tm_sec;
    if(seconds<-11644473600LL)throw CertificateError{0x8009310b};return static_cast<std::uint64_t>(seconds+11644473600LL)*10000000;
}
struct CertificatePacket {
    unsigned unit;std::vector<std::uint8_t> bytes;std::vector<std::pair<std::size_t,std::size_t>> pointers;
    explicit CertificatePacket(bool x86):unit(x86?4:8){}
    std::size_t reserve(std::size_t count,unsigned alignment=1){const auto offset=(bytes.size()+alignment-1)&~static_cast<std::size_t>(alignment-1);if(offset>16*1024*1024 || count>16*1024*1024-offset)throw CertificateError{8};bytes.resize(offset+count);return offset;}
    void put(std::size_t offset,std::uint64_t value,unsigned width=4){for(unsigned i=0;i<width;++i)bytes.at(offset+i)=static_cast<std::uint8_t>(value>>(i*8));}
    std::size_t data(std::span<const std::uint8_t> value){const auto offset=reserve(value.size());std::copy(value.begin(),value.end(),bytes.begin()+static_cast<std::ptrdiff_t>(offset));return offset;}
    void point(std::size_t field,std::size_t target){pointers.emplace_back(field,target);}
    void text(std::size_t field,std::string_view value){const auto offset=reserve(value.size()+1);std::copy(value.begin(),value.end(),bytes.begin()+static_cast<std::ptrdiff_t>(offset));point(field,offset);}
    void blob(std::size_t field,std::span<const std::uint8_t> value){put(field,value.size());if(!value.empty())point(field+unit,data(value));}
    void bits(std::size_t field,const ASN1_BIT_STRING* value){if(!value)return;const auto size=ASN1_STRING_length(value);if(size<0)throw CertificateError{0x8009310b};blob(field,std::span(ASN1_STRING_get0_data(value),static_cast<std::size_t>(size)));put(field+2*unit,(value->flags&ASN1_STRING_FLAG_BITS_LEFT)?static_cast<unsigned>(value->flags)&7U:0U);}
    void algorithm(std::size_t field,const X509_ALGOR* value){
        const ASN1_OBJECT* object{};int type{};const void* parameter{};X509_ALGOR_get0(&object,&type,&parameter,value);text(field,oid_name(object));
        if(type!=V_ASN1_UNDEF){std::unique_ptr<ASN1_TYPE,decltype(&ASN1_TYPE_free)> item(ASN1_TYPE_new(),ASN1_TYPE_free);if(!item || !ASN1_TYPE_set1(item.get(),type,parameter))throw CertificateError{8};blob(field+unit,encoded(item.get(),i2d_ASN1_TYPE));}
    }
    template<class Getter> void extensions(std::size_t count_field,std::size_t pointer_field,int count,Getter get){
        if(count<0 || count>4096)throw CertificateError{0x8009310b};put(count_field,static_cast<unsigned>(count));if(!count)return;
        const auto table=reserve(static_cast<std::size_t>(count)*4*unit,unit);point(pointer_field,table);
        for(int i=0;i<count;++i){auto* extension=get(i);if(!extension)throw CertificateError{0x8009310b};const auto entry=table+static_cast<std::size_t>(i)*4*unit;
            text(entry,oid_name(X509_EXTENSION_get_object(extension)));put(entry+unit,X509_EXTENSION_get_critical(extension)?1U:0U);
            const auto* value=X509_EXTENSION_get_data(extension);const auto size=ASN1_STRING_length(value);if(size<0)throw CertificateError{0x8009310b};
            blob(entry+2*unit,std::span(ASN1_STRING_get0_data(value),static_cast<std::size_t>(size)));}
    }
    void relocate(std::uint64_t base){for(const auto& [field,target]:pointers)put(field,base+target,unit);}
};
}
