#pragma once
#include "msvc_rtti_storage.hpp"
#include <string>

namespace x64arm::msvcp_detail {
inline std::string stream_type(std::string_view kind,bool wide){const std::string character=wide?"_W":"D";return ".?AV?$basic_"+std::string(kind)+"@"+character+"U?$char_traits@"+character+"@std@@@std@@";}
inline msvc_rtti::Shape facet_shape(std::string root,unsigned p,std::string middle={}){
    msvc_rtti::Shape result;result.flags=1;
    if(root!=".?AVfacet@locale@std@@")result.bases.push_back({std::move(root),static_cast<std::uint32_t>(middle.empty()?3:4)});
    if(!middle.empty())result.bases.push_back({std::move(middle),3});
    result.bases.push_back({".?AVfacet@locale@std@@",2});result.bases.push_back({".?AV_Facet_base@std@@",0});result.bases.push_back({".?AU_Crt_new_delete@std@@",0,static_cast<std::int32_t>(p)});return result;
}
inline msvc_rtti::Shape stream_shape(std::string_view kind,bool wide,unsigned p){
    msvc_rtti::Shape result;const auto ios=stream_type("ios",wide);
    const auto bases=[&](bool virtual_base){result.bases.push_back({ios,2,0,virtual_base?0:-1,virtual_base?4:0,virtual_base?80U:64U});
        result.bases.push_back({".?AVios_base@std@@",1,0,virtual_base?0:-1,virtual_base?4:0});result.bases.push_back({".?AV?$_Iosb@H@std@@",0,static_cast<std::int32_t>(p),virtual_base?0:-1,virtual_base?4:0});};
    if(kind=="streambuf"){result.bases.push_back({stream_type(kind,wide),0});return result;}
    if(kind=="ios"){bases(false);return result;}
    if(kind=="ios_base"){result.bases={{".?AVios_base@std@@",1},{".?AV?$_Iosb@H@std@@",0,static_cast<std::int32_t>(p)}};return result;}
    result.constructor_displacement=4;
    if(kind=="iostream"){
        result.flags=3;result.offset=p==4?24U:32U;result.bases.push_back({stream_type(kind,wide),8});result.bases.push_back({stream_type("istream",wide),3});bases(true);
        result.bases.push_back({stream_type("ostream",wide),3,16});bases(true);
    }else{result.offset=kind=="ostream"?(p==4?8U:16U):(p==4?20U:24U);result.bases.push_back({stream_type(kind,wide),3});bases(true);}
    return result;
}
}
