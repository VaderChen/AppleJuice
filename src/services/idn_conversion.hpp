#pragma once
#include <unicode/uidna.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace x64arm::idn_detail {
enum class Profile { windows_idna2003, certificate_uts46 };
// 集中 ICU 轉換、容量查詢與錯誤檢查；呼叫端只處理自己的 ABI 與錯誤碼。
inline std::optional<std::u16string> convert(std::u16string_view source,bool ascii,
                                            std::uint32_t flags,Profile profile){
    if(source.empty() || source.size()>INT32_MAX || source.find(char16_t{})!=source.npos)return {};
    UErrorCode error=U_ZERO_ERROR;
    std::unique_ptr<UIDNA,decltype(&uidna_close)> idna(nullptr,uidna_close);
    if(profile==Profile::certificate_uts46){
        idna.reset(uidna_openUTS46(UIDNA_USE_STD3_RULES|UIDNA_CHECK_BIDI|UIDNA_CHECK_CONTEXTJ|UIDNA_NONTRANSITIONAL_TO_ASCII,&error));
        if(U_FAILURE(error) || !idna)return {};
    }
    const auto run=[&](char16_t* output,int32_t capacity){
        error=U_ZERO_ERROR;
        if(idna){
            UIDNAInfo info=UIDNA_INFO_INITIALIZER;
            const auto length=(ascii?uidna_nameToASCII:uidna_nameToUnicode)(idna.get(),source.data(),static_cast<int32_t>(source.size()),output,capacity,&info,&error);
            if(info.errors)error=U_INVALID_CHAR_FOUND;
            return length;
        }
        // IDNA2003 支援 ALLOW_UNASSIGNED；不能以忽略旗標的 UTS #46 取代。
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        return (ascii?uidna_IDNToASCII:uidna_IDNToUnicode)(source.data(),static_cast<int32_t>(source.size()),output,capacity,static_cast<int32_t>(flags),nullptr,&error);
#pragma clang diagnostic pop
    };
    const auto size=run(nullptr,0);
    if(size<=0 || (U_FAILURE(error) && error!=U_BUFFER_OVERFLOW_ERROR))return {};
    std::u16string result(static_cast<std::size_t>(size),0);
    if(run(result.data(),size)!=size || U_FAILURE(error))return {};
    if(!ascii && profile==Profile::windows_idna2003){
        const auto roundtrip=convert(result,true,flags,profile);if(!roundtrip || roundtrip->size()!=source.size())return {};
        const auto lower=[](char16_t c){return c>=u'A' && c<=u'Z'?static_cast<char16_t>(c+32):c;};
        for(std::size_t i=0;i<source.size();++i)if(lower((*roundtrip)[i])!=lower(source[i]))return {};
        // ICU 的 IDNA2003 ToUnicode 會把無效 ACE label 原樣退回；
        // ToASCII 對純 ASCII 不再解碼，因此還須拒絕這種未解碼的 label。
        for(std::size_t at=0;at<result.size();){
            const auto end=result.find(u'.',at),label_size=(end==result.npos?result.size():end)-at;
            if(label_size>=4 && lower(result[at])==u'x' && lower(result[at+1])==u'n' && result[at+2]==u'-' && result[at+3]==u'-')return {};
            if(end==result.npos)break;at=end+1;
        }
    }
    return result;
}
}
