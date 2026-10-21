#pragma once
#include <stdexcept>
#include <string>
#include <string_view>

namespace x64arm {
inline constexpr std::string_view default_certificate_roots_path="ProgramData/MacWin/Certificates/ROOT.pem";
// 儲存的設定只接受 C 槽內相對路徑，與宿主掛載點及客體目前目錄無關。
inline std::string certificate_roots_relative_path(std::string_view input) {
    if(input.empty())return {};
    std::string path(input);
    for(auto& c:path)if(c=='\\')c='/';
    if(path.front()=='/')throw std::invalid_argument("根憑證檔案必須使用相對於 Sandbox C 槽的路徑");
    std::size_t start=0;
    while(start<path.size()){
        const auto end=path.find('/',start);const auto part=std::string_view(path).substr(start,end==std::string::npos?path.size()-start:end-start);
        if(part.empty() || part=="." || part==".." || part.back()=='.' || part.back()==' ')
            throw std::invalid_argument("根憑證相對路徑包含無效目錄");
        for(const auto c:part)if(static_cast<unsigned char>(c)<32 || std::string_view(":*?\"<>|").find(c)!=std::string_view::npos)
            throw std::invalid_argument("根憑證相對路徑包含無效字元");
        if(end==std::string::npos)break;
        start=end+1;
        if(start==path.size())throw std::invalid_argument("根憑證路徑必須指定檔案");
    }
    return path;
}
}
