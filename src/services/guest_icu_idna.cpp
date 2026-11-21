#include "icu_idna_layout.hpp"
#include <unicode/uidna.h>

namespace {
using namespace x64arm::icu_idna;

bool overlaps(const void* first,size_t first_size,const void* second,size_t second_size){
    const auto a=reinterpret_cast<uintptr_t>(first);
    const auto b=reinterpret_cast<uintptr_t>(second);
    return a<=b?b-a<first_size:a-b<second_size;
}

template<class Char>
int32_t simple_ascii_length(const Char* input,int32_t length,bool label){
    constexpr int32_t max_label=63,max_domain=254;
    if(!input || length< -1 || length>max_domain || (label && length>max_label))return -1;
    const auto limit=length<0?(label?max_label:max_domain):length;
    int32_t size=0,part=0;
    uint32_t previous=0;
    for(;size<limit;++size){
        const auto c=static_cast<uint32_t>(input[size]);
        if(length<0 && !c)break;
        if(c=='.' && !label){
            if(!part || previous=='-')return -1;
            part=0;
        }else{
            if(!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-'))return -1;
            if(++part>max_label || (c=='-' && (part==1 || (part==4 && previous=='-'))))return -1;
        }
        previous=c;
    }
    if(!size || previous=='-' || (size==max_domain && previous!='.') ||
       (length<0 && size==limit && input[size]))return -1;
    return size;
}

template<Conversion operation,bool label,class Char>
int32_t convert(const UIDNA* idna,const Char* input,int32_t length,Char* output,int32_t capacity,
                UIDNAInfo* info,UErrorCode* status){
    if(!status || U_FAILURE(*status))return 0;
    const auto* view=reinterpret_cast<const View*>(idna);
    if(!view || view->magic!=View::signature){*status=U_ILLEGAL_ARGUMENT_ERROR;return 0;}
    // UTS #46 的 ASCII 快速路徑限已符合 DNS 長度及 LDH 規則的標籤。
    // 容量預查、ACE、Unicode 及較大 UIDNAInfo 全部使用相同物件的宿主入口。
    if(*status==U_ZERO_ERROR && output && capacity>0 && info && info->size==sizeof(UIDNAInfo)){
        const auto size=simple_ascii_length(input,length,label);
        if(size>=0 && capacity>size){
            const auto read_bytes=sizeof(Char)*(static_cast<size_t>(size)+(length<0?1U:0U));
            const auto write_bytes=sizeof(Char)*(static_cast<size_t>(size)+1U);
            if(!overlaps(input,read_bytes,output,write_bytes) &&
               !overlaps(input,read_bytes,info,sizeof(*info)) && !overlaps(output,write_bytes,info,sizeof(*info)) &&
               !overlaps(output,write_bytes,status,sizeof(*status)) && !overlaps(info,sizeof(*info),status,sizeof(*status))){
                for(int32_t n=0;n<size;++n){
                    const auto c=input[n];
                    output[n]=static_cast<Char>(c>='A' && c<='Z'?c+('a'-'A'):c);
                }
                output[size]=0;
                info->isTransitionalDifferent=false;
                info->reservedB3=false;
                info->errors=0;
                info->reservedI2=0;
                info->reservedI3=0;
                return size;
            }
        }
    }
    using Fallback=int32_t (*)(const UIDNA*,const Char*,int32_t,Char*,int32_t,UIDNAInfo*,UErrorCode*);
    const auto fallback=reinterpret_cast<Fallback>(static_cast<uintptr_t>(view->fallback[static_cast<size_t>(operation)]));
    return fallback(idna,input,length,output,capacity,info,status);
}
}

#define ICU_IDNA_API(...)
#define ICU_IDNA_QUERY(name,count,type,label) \
    extern "C" int32_t name(const UIDNA* idna,const type* input,int32_t length,type* output,int32_t capacity, \
                           UIDNAInfo* info,UErrorCode* status){ \
        return convert<Conversion::name,label>(idna,input,length,output,capacity,info,status); \
    }
#include "icu_idna_services.inc"
#undef ICU_IDNA_QUERY
#undef ICU_IDNA_API
