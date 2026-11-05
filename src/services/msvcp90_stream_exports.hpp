#pragma once
#include "x64arm/windows_abi.hpp"
#include <string>
#include <vector>

namespace x64arm::msvcp90_stream {
// VC9 的 streamsize 是指標寬度；x86 建構子另含隱藏 virtual-base 初始化參數。
// 名稱與 ABI 描述共用一份表，不把 VC9 的物件直接別名到 VC10。
struct Binding {std::string family,name;unsigned arguments{};bool x86{};};
inline const std::vector<Binding>& bindings(){
    static const auto entries=[] {
        std::vector<Binding> v;
        for(const bool x86:{true,false}){
            const std::string m=x86?"QAE":"QEAA",k=x86?"QBE":"QEBA",virt=x86?"UAE":"UEAA";
            const std::string ptr=x86?"PA":"PEA",cptr=x86?"PB":"PEB",ref=x86?"AAV12@":"AEAV12@",size=x86?"H":"_J",off=x86?"J":"_J";
            const auto type=[](std::string_view kind){return "?$basic_"+std::string(kind)+"@DU?$char_traits@D@std@@@std@@";};
            const auto add=[&](std::string name,unsigned args){v.push_back({"msvcp90.dll",std::move(name),args+(!x86?1U:0U),x86});};
            const auto method=[&](std::string_view kind,std::string_view name,std::string suffix,unsigned args){add("?"+std::string(name)+"@"+type(kind)+suffix,args);};
            const auto stream=type("ifstream"),buffer=type("filebuf"),input=type("istream"),ios=type("ios");
            add("??0"+stream+m+"@XZ",1);
            for(const std::string path:{"D","_W","G"}){
                add("??0"+stream+m+"@"+cptr+path+"HH@Z",4);
                method("ifstream","open",m+"X"+cptr+path+"HH@Z",3);
                method("filebuf","open",m+ptr+"V12@"+cptr+path+"HH@Z",3);
            }
            add("??0"+stream+m+"@"+ptr+"U_iobuf@@@Z",2);
            add("??1"+stream+virt+"@XZ",0);add("??_D"+stream+m+"XXZ",0);
            method("ifstream","close",m+"XXZ",0);method("ifstream","is_open",k+"_NXZ",0);
            method("ifstream","rdbuf",k+ptr+"V?$basic_filebuf@DU?$char_traits@D@std@@@2@XZ",0);
            add("??0"+buffer+m+"@"+ptr+"U_iobuf@@@Z",1);add("??_F"+buffer+m+"XXZ",0);
            add("??1"+buffer+virt+"@XZ",0);
            method("filebuf","is_open",k+"_NXZ",0);method("filebuf","close",m+ptr+"V12@XZ",0);
            method("istream","read",m+ref+ptr+"D"+size+"@Z",2);
            method("istream","seekg",m+ref+off+"H@Z",2);
            method("istream","seekg",m+ref+"V?$fpos@H@2@@Z",x86?6:1);
            method("istream","tellg",m+"?AV?$fpos@H@2@XZ",1);
            method("istream","gcount",k+size+"XZ",0);
            method("istream","peek",m+"HXZ",0);method("istream","get",m+"HXZ",0);
            method("istream","get",m+ref+(x86?"AAD":"AEAD")+"@Z",1);
            method("istream","putback",m+ref+"D@Z",1);
            method("istream","_Ipfx",m+"_N_N@Z",1);
            for(const auto name:{"clear","setstate"})method("ios",name,m+"XH_N@Z",2);
            method("ios","rdbuf",k+ptr+"V?$basic_streambuf@DU?$char_traits@D@std@@@2@XZ",0);
            add("??1"+ios+virt+"@XZ",0);
            for(const auto name:{"good","eof","fail","bad"})add("?"+std::string(name)+"@ios_base@std@@"+k+"_NXZ",0);
            add("?rdstate@ios_base@std@@"+k+"HXZ",0);
            add("?exceptions@ios_base@std@@"+k+"HXZ",0);add("?exceptions@ios_base@std@@"+m+"XH@Z",1);
            for(const auto name:{"sgetc","sbumpc","snextc"})method("streambuf",name,m+"HXZ",0);
            method("streambuf","sgetn",m+size+ptr+"D"+size+"@Z",2);
            method("streambuf","sputn",m+size+cptr+"D"+size+"@Z",2);
            method("streambuf","sputc",m+"HD@Z",1);
            method("streambuf","in_avail",m+size+"XZ",0);
            // 虛擬表同一套語義供具體 filebuf 與外部覆寫使用。
            for(const auto& e:std::vector<std::pair<std::string,unsigned>>{
                {"FileDelete",1},{"FileOverflow",1},{"FilePbackfail",1},{"FileUnderflow",0},{"FileUflow",0},
                {"StreamShowmany",0},{"FileXsgetn",2},{"FileXsgetnSafe",3},{"FileXsputn",2},{"FileSeekoff",4},
                {"FileSeekpos",x86?8U:3U},{"FileSetbuf",2},{"FileSync",0},{"FileImbue",1},{"FileIosDelete",1}})
                v.push_back({"internal-msvcp90",e.first+(x86?"32":"64"),e.second+(!x86?1U:0U),x86});
        }
        return v;
    }();return entries;
}
inline std::vector<WindowsServiceDescriptor> descriptors(){
    std::vector<WindowsServiceDescriptor> result;
    for(const auto& b:bindings())result.push_back({b.family,b.name,b.arguments});return result;
}
inline bool family(std::string_view name){return name.find("basic_ifstream@")!=name.npos || name.find("basic_filebuf@")!=name.npos ||
    name.find("basic_istream@")!=name.npos || name.find("basic_streambuf@")!=name.npos || name.find("basic_ios@")!=name.npos || name.find("ios_base@")!=name.npos;}
inline std::string_view native_name(std::string_view name){
    if(name.starts_with("?gcount@"))return "guest_stream_count";
    if(name.starts_with("?is_open@"))return name.find("basic_ifstream@")!=name.npos?"guest_ifstream_open":"guest_filebuf_open";
    if(name.starts_with("?rdbuf@"))return name.find("basic_ifstream@")!=name.npos?"guest_ifstream_buffer":"guest_stream_buffer";
    if(name.starts_with("?good@"))return "guest_stream_good";if(name.starts_with("?eof@"))return "guest_stream_eof";
    if(name.starts_with("?fail@"))return "guest_stream_fail";if(name.starts_with("?bad@"))return "guest_stream_bad";
    if(name.starts_with("?rdstate@"))return "guest_stream_state";
    if(name.starts_with("?exceptions@") && (name.find("@QBE")!=name.npos || name.find("@QEBA")!=name.npos))return "guest_stream_exceptions";
    return {};
}
}
