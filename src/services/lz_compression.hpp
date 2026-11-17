#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// LZ（SZDD／COMPRESS.EXE 格式）解壓：kernel32 的 LZ 家族與 SetupAPI 的解壓／壓縮資訊共用。
// 檔頭 14 位元組：8 位元組簽章、壓縮模式 'A'、檔名最後一個字元、展開後長度（32 位元）；之後為壓縮資料。
namespace x64arm {
inline constexpr std::array<std::uint8_t,8> szdd_magic{'S','Z','D','D',0x88,0xf0,0x27,0x33};
// SZDD：4096 位元組視窗以空白初始化、起點 4080；旗標位元由低到高，1 為字面值，0 為 12 位元位移＋4 位元長度（加 3）。
inline std::optional<std::vector<std::uint8_t>> lz_expand(std::span<const std::uint8_t> input,std::uint32_t size){
    std::vector<std::uint8_t> output;output.reserve(size);std::array<std::uint8_t,4096> window;window.fill(' ');unsigned at=4096-16;std::size_t in=0;
    while(output.size()<size && in<input.size()){
        const auto flags=input[in++];
        for(unsigned bit=0;bit<8 && output.size()<size && in<input.size();++bit){
            if(flags&(1U<<bit)){const auto value=input[in++];output.push_back(value);window[at]=value;at=(at+1)&4095U;continue;}
            if(in+1>=input.size())return {};
            const unsigned low=input[in],high=input[in+1];in+=2;
            unsigned offset=low|((high&0xf0U)<<4);const unsigned length=(high&0x0fU)+3;
            for(unsigned k=0;k<length && output.size()<size;++k){const auto value=window[offset];offset=(offset+1)&4095U;output.push_back(value);window[at]=value;at=(at+1)&4095U;}
        }
    }
    if(output.size()!=size)return {};
    return output;
}
// 壓縮檔名的最後一個字元由檔頭補回：底線換成檔頭字元，大小寫跟隨原本的檔名。
inline std::string lz_expanded_name(std::string name,char last){
    if(!last || name.empty() || name.back()!='_')return name;
    bool upper=false;for(auto it=name.rbegin()+1;it!=name.rend() && *it!='.' && *it!='\\' && *it!='/';++it)if(*it>='A' && *it<='Z'){upper=true;break;}else if(*it>='a' && *it<='z')break;
    name.back()=upper?static_cast<char>(last>='a' && last<='z'?last-32:last):static_cast<char>(last>='A' && last<='Z'?last+32:last);return name;
}
// 完整 SZDD 檔案（含檔頭）解壓；不是 SZDD 或資料不完整回傳空值。
inline std::optional<std::vector<std::uint8_t>> lz_expand_file(std::span<const std::uint8_t> file,std::uint32_t limit=512U*1024U*1024U){
    if(file.size()<14 || !std::equal(szdd_magic.begin(),szdd_magic.end(),file.begin()) || file[8]!='A')return std::nullopt;
    const std::uint32_t size=file[10]|(std::uint32_t{file[11]}<<8)|(std::uint32_t{file[12]}<<16)|(std::uint32_t{file[13]}<<24);
    if(size>limit)return std::nullopt;return lz_expand(file.subspan(14),size);
}
}
