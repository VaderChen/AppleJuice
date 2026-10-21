#pragma once
#include "x64arm/windows_graphics.hpp"
#include "dxbc_reflection.hpp"
#include <regex>
#include <algorithm>
#include <bit>

namespace x64arm::stream_output {
inline bool equal(std::string_view a,std::string_view b){
    if(a.size()!=b.size())return false;
    for(std::size_t i=0;i<a.size();++i){const auto upper=[](char c){return c>='a' && c<='z'?c-'a'+'A':int(c);};if(upper(a[i])!=upper(b[i]))return false;}return true;
}
inline void validate(GraphicsShader& shader,const dxbc::Reflection& reflection){
    if(shader.stream_output.size()>64 || (shader.rasterized_stream!=0 && shader.rasterized_stream!=UINT32_MAX))throw std::invalid_argument("SO 宣告超出支援的單一 stream 範圍");
    std::array<unsigned,4> sizes{};
    for(const auto& e:shader.stream_output){
        if(e.slot>=4 || e.stream || !e.count || e.count>4 || e.first>4-e.count)throw std::invalid_argument("SO 宣告範圍不合法");
        if(!e.semantic.empty()){
            const auto found=std::find_if(reflection.signatures[1].begin(),reflection.signatures[1].end(),[&](const auto& s){return equal(s.name,e.semantic) && s.index==e.index && s.stream==e.stream;});
            if(found==reflection.signatures[1].end() || e.first+e.count>static_cast<unsigned>(std::popcount(found->mask)))throw std::invalid_argument("SO semantic 與著色器輸出不符");
        }
        sizes[e.slot]+=e.count*4;
    }
    for(unsigned i=0;i<4;++i){if(!shader.stream_strides[i])shader.stream_strides[i]=sizes[i];if(shader.stream_strides[i]<sizes[i] || shader.stream_strides[i]>2048 || (shader.stream_strides[i]&3))throw std::invalid_argument("SO stride 不合法");}
}
inline void parse(GraphicsShader& shader,const dxbc::Reflection& reflection,std::string_view declaration){
    static const std::regex syntax(R"(^\s*(?:([0-3])\s*:\s*)?([A-Za-z_$][A-Za-z_$]*)([0-9]*)(?:\.([xyzw]+))?\s*$)");
    while(!declaration.empty()){
        const auto end=declaration.find(';');const std::string item(declaration.substr(0,end));
        declaration=end==declaration.npos?std::string_view{}:declaration.substr(end+1);
        if(item.find_first_not_of(" \r\n\t")==item.npos)continue;
        std::smatch match;if(!std::regex_match(item,match,syntax))throw std::invalid_argument("FX SO 宣告語法不合法");
        GraphicsStreamElement e;e.slot=match[1].matched?static_cast<unsigned>(std::stoul(match[1])):0;e.semantic=match[2];e.index=match[3].length()?static_cast<unsigned>(std::stoul(match[3])):0;
        if(e.semantic=="$SKIP"){if(match[3].length())throw std::invalid_argument("SO SKIP 不接受 semantic index");e.semantic.clear();}
        if(match[4].matched){const auto mask=match[4].str();e.first=static_cast<unsigned>(std::string_view("xyzw").find(mask.front()));e.count=static_cast<unsigned>(mask.size());if(std::string_view("xyzw").substr(e.first,e.count)!=mask)throw std::invalid_argument("SO mask 必須為連續分量");}
        else if(e.semantic.empty())e.count=4;
        else {const auto found=std::find_if(reflection.signatures[1].begin(),reflection.signatures[1].end(),[&](const auto& s){return equal(s.name,e.semantic) && s.index==e.index;});if(found==reflection.signatures[1].end())throw std::invalid_argument("SO semantic 不存在");e.first=0;e.count=static_cast<unsigned>(std::popcount(found->mask));}
        shader.stream_output.push_back(std::move(e));
    }
    validate(shader,reflection);
}
}
