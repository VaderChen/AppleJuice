#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace x64arm::detail {
// 以 IEEE 位元分類，不經宿主 FP 運算安靜化 signaling NaN。
// 舊 CRT 將特殊值的識別字串視為十進位數字列，包含精度截短時的字元進位。
inline std::optional<std::string> crt_nonfinite_format(std::uint64_t bits,char conversion,
        std::string_view flags,unsigned width,int precision,bool legacy,bool three_digits) {
    if((bits&UINT64_C(0x7ff0000000000000))!=UINT64_C(0x7ff0000000000000))return std::nullopt;
    const auto fraction=bits&UINT64_C(0xfffffffffffff);
    const bool negative=(bits>>63)!=0,indefinite=bits==UINT64_C(0xfff8000000000000);
    const bool signaling=fraction && !(fraction&UINT64_C(0x8000000000000));
    const bool upper=conversion>='A' && conversion<='Z';
    const char lower=upper?static_cast<char>(conversion+'a'-'A'):conversion;
    std::string prefix=negative?"-":flags.find('+')!=flags.npos?"+":flags.find(' ')!=flags.npos?" ":"";
    std::string number;
    if(!legacy){
        number=!fraction?"inf":indefinite?"nan(ind)":signaling?"nan(snan)":"nan";
        if(upper)for(auto& c:number)if(c>='a' && c<='z')c=static_cast<char>(c-'a'+'A');
    }else{
        const std::string_view token=!fraction?"1#INF":indefinite?"1#IND":signaling?"1#SNAN":"1#QNAN";
        const bool general=lower=='g',hex=lower=='a';
        const unsigned requested=precision<0?(hex?13U:6U):static_cast<unsigned>(precision);
        const unsigned digits=general?std::max(1U,requested):requested+1;
        number=std::string(token.substr(0,digits));number.resize(digits,'0');
        if(digits<token.size() && token[digits]>='5'){
            auto at=number.size();while(at && number[at-1]=='9'){number[--at]='0';}
            if(at)++number[at-1];else number.insert(number.begin(),'1');
        }
        if(general && flags.find('#')==flags.npos)while(number.size()>1 && number.back()=='0')number.pop_back();
        if(number.size()>1 || flags.find('#')!=flags.npos)number.insert(1,1,'.');
        if(hex){prefix+=upper?"0X":"0x";number+=upper?"P+0":"p+0";}
        else if(lower=='e'){number+=upper?"E+":"e+";number.append(three_digits?3:2,'0');}
    }
    const auto total=prefix.size()+number.size();
    if(width>total){
        const auto padding=width-total;
        if(flags.find('-')!=flags.npos)number.append(padding,' ');
        else if(legacy && flags.find('0')!=flags.npos)number.insert(0,padding,'0');
        else prefix.insert(0,padding,' ');
    }
    return prefix+number;
}
}
