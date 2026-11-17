// 由 windows_url_security.cpp 的 invoke_url_security 引入：urlmon 的 URL 字串工具、MIME 偵測與工作階段選項。
// URL 的正規化、合併、比較、跳脫與 file:// 轉換沿用 shlwapi 的 Url*／Path*Url 服務；scheme 與主機沿用安全區域判定的 parse()。
// FindMimeFromData 依官方「MIME Type Detection in Internet Explorer」的六個步驟；副檔名對應讀 HKCR\.ext 的 Content Type。
if(name=="CoInternetParseUrl" || name=="CoInternetCombineUrl" || name=="CoInternetCompareUrl" || name=="IsValidURL" || name=="FindMimeFromData" ||
   name=="ObtainUserAgentString" || name=="UrlMkGetSessionOption" || name=="UrlMkSetSessionOption"){
    constexpr std::uint32_t s_false=1,e_fail=0x80004005U,e_pointer=0x80004003U,e_outofmemory=0x8007000eU;
    const auto host=[&](std::string_view family,std::string_view symbol,std::initializer_list<std::uint64_t> args){return static_cast<std::uint32_t>(nested_service(state,memory,family,symbol,args));};
    const auto wide=[&](unsigned index){return read_utf16(memory,GuestAddress{arg(index)},32768);};
    GuestScratch scratch(*this,memory,state);
    // 主機端算出的結果：寫入呼叫端緩衝；太小時 *pcch 為含 NUL 的所需長度並回 S_FALSE，成功時為不含 NUL 的長度。
    const auto store_text=[&](const std::u16string& text,std::uint64_t out,std::uint32_t capacity,std::uint64_t count,std::uint32_t too_small)->std::uint32_t{
        const auto length=static_cast<std::uint32_t>(text.size());
        if(!out || capacity<=length){if(count)memory.store(GuestAddress{count},32,length+1);return out?too_small:e_pointer;}
        memory.write(GuestAddress{out},utf16_bytes(text));if(count)memory.store(GuestAddress{count},32,length);return 0;
    };
    // 交給 shlwapi（UrlXxxW(input, output, &cch, flags) 形式）：E_POINTER 依各動作改回報值，cch 回填到 *pcch。
    const auto shell_url=[&](std::string_view symbol,std::uint64_t input,std::uint64_t out,std::uint32_t capacity,std::uint64_t count,std::uint32_t flags,std::uint32_t too_small)->std::uint32_t{
        if(!out)return e_pointer;const auto cch=scratch.allocate(4);if(!cch)return e_outofmemory;memory.store(GuestAddress{cch},32,capacity);
        const auto status=host("shlwapi.dll",symbol,{input,out,cch,flags});
        if(count)memory.store(GuestAddress{count},32,memory.load(GuestAddress{cch},32));
        return status==e_pointer?too_small:status;
    };
    if(name=="CoInternetParseUrl"){
        // (Url, ParseAction, Flags, Result, cchResult, pcchResult, Reserved)
        if(!arg(0))return finish(invalid);
        const auto action=static_cast<std::uint32_t>(arg(1)),flags=static_cast<std::uint32_t>(arg(2)),capacity=static_cast<std::uint32_t>(arg(4));
        const auto url=wide(0);const auto parsed=parse(url);
        switch(action){
        case 1:return finish(shell_url("UrlCanonicalizeW",arg(0),arg(3),capacity,arg(5),flags,s_false));                  // PARSE_CANONICALIZE
        case 7:case 19:return finish(shell_url("UrlUnescapeW",arg(0),arg(3),capacity,arg(5),flags,e_pointer));            // PARSE_ENCODE／PARSE_UNESCAPE
        case 8:case 18:return finish(shell_url("UrlEscapeW",arg(0),arg(3),capacity,arg(5),flags,e_pointer));              // PARSE_DECODE／PARSE_ESCAPE
        case 9:return finish(shell_url("PathCreateFromUrlW",arg(0),arg(3),capacity,arg(5),0,s_false));                  // PARSE_PATH_FROM_URL
        case 10:return finish(shell_url("UrlCreateFromPathW",arg(0),arg(3),capacity,arg(5),0,s_false));                 // PARSE_URL_FROM_PATH（非 DOS 路徑回 S_FALSE）
        case 5:return finish(store_text(url.substr(0,url.find(u'#')),arg(3),capacity,arg(5),s_false));                  // PARSE_DOCUMENT
        case 6:case 16:{const auto hash=url.find(u'#');return finish(store_text(hash==url.npos?std::u16string{}:url.substr(hash),arg(3),capacity,arg(5),s_false));} // PARSE_ANCHOR／LOCATION
        case 13:if(parsed.scheme.empty())return finish(e_fail);return finish(store_text(parsed.scheme,arg(3),capacity,arg(5),s_false)); // PARSE_SCHEMA
        case 14:case 15:if(parsed.host.empty())return finish(e_fail);return finish(store_text(parsed.host,arg(3),capacity,arg(5),s_false)); // PARSE_SITE／DOMAIN
        case 4:if(parsed.host.empty() || parsed.local || parsed.unc)return finish(default_action);                      // PARSE_ROOTDOCUMENT：scheme://host
            return finish(store_text(parsed.scheme+u"://"+parsed.host,arg(3),capacity,arg(5),s_false));
        case 17:if(parsed.scheme.empty())return finish(e_fail);return finish(store_text(parsed.scheme+u":"+parsed.host,arg(3),capacity,arg(5),s_false)); // PARSE_SECURITY_DOMAIN
        case 3:if(parsed.local && parsed.scheme==u"file" && !url.starts_with(u"file:"))                                // PARSE_SECURITY_URL：DOS 路徑換成 file:// URL
                   return finish(shell_url("UrlCreateFromPathW",arg(0),arg(3),capacity,arg(5),0,s_false));
            return finish(store_text(url,arg(3),capacity,arg(5),s_false));
        default:return finish(default_action); // PARSE_FRIENDLY、PARSE_MIME、PARSE_SERVER 與未知動作：使用預設動作。
        }
    }
    if(name=="CoInternetCombineUrl"){
        // (BaseUrl, RelativeUrl, CombineFlags, Result, cchResult, pcchResult, Reserved)：UrlCombineW；緩衝太小回 S_FALSE 與所需長度。
        if(!arg(0) || !arg(1))return finish(invalid);
        if(!arg(3))return finish(e_pointer);const auto cch=scratch.allocate(4);if(!cch)return finish(e_outofmemory);memory.store(GuestAddress{cch},32,static_cast<std::uint32_t>(arg(4)));
        const auto status=host("shlwapi.dll","UrlCombineW",{arg(0),arg(1),arg(3),cch,static_cast<std::uint32_t>(arg(2))});
        if(arg(5))memory.store(GuestAddress{arg(5)},32,memory.load(GuestAddress{cch},32));
        return finish(status==e_pointer?s_false:status);
    }
    if(name=="CoInternetCompareUrl"){
        // (Url1, Url2, CompareFlags)：相同回 S_OK，不同回 S_FALSE（比較規則同 UrlCompareW，保留結尾斜線差異）。
        if(!arg(0) || !arg(1))return finish(invalid);
        return finish(host("shlwapi.dll","UrlCompareW",{arg(0),arg(1),0})==0?0U:s_false);
    }
    if(name=="IsValidURL"){
        // (pBC, szURL, Reserved)：只檢查語法；scheme 至少兩個合法字元並以冒號結尾即為有效 URL。
        if(!arg(1) || arg(0) || static_cast<std::uint32_t>(arg(2)))return finish(invalid);
        const unsigned size=x86_?24U:40U;const auto parsed=scratch.allocate(size);if(!parsed)return finish(e_outofmemory);
        memory.write(GuestAddress{parsed},std::vector<std::uint8_t>(size));memory.store(GuestAddress{parsed},32,size);
        return finish(host("shlwapi.dll","ParseURLW",{arg(1),parsed})==0?0U:s_false);
    }
    if(name=="FindMimeFromData"){
        // (pBC, Url, Buffer, cbSize, MimeProposed, MimeFlags, ppwzMimeOut, Reserved)：結果以 CoTaskMemAlloc 配置。
        if(!arg(6) || (!arg(1) && !arg(2)) || static_cast<std::uint32_t>(arg(7)))return finish(invalid); // DWORD 參數只看低 32 位元。
        memory.store(GuestAddress{arg(6)},bits,0);const auto flags=static_cast<std::uint32_t>(arg(5));
        const auto fold=[](std::u16string text){for(auto& c:text)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);return text;};
        auto proposed=arg(4)?wide(4):std::u16string{};
        if(const auto semicolon=proposed.find(u';');semicolon!=proposed.npos)proposed.resize(semicolon);
        while(!proposed.empty() && (proposed.back()==u' ' || proposed.back()==u'\t'))proposed.pop_back();
        while(!proposed.empty() && (proposed.front()==u' ' || proposed.front()==u'\t'))proposed.erase(proposed.begin());
        const auto proposed_key=fold(proposed);
        static constexpr std::array<std::u16string_view,35> known_types{u"text/plain",u"text/html",u"text/xml",u"text/richtext",u"text/scriptlet",u"audio/x-aiff",u"audio/basic",
            u"audio/mid",u"audio/wav",u"image/gif",u"image/jpeg",u"image/pjpeg",u"image/png",u"image/x-png",u"image/tiff",u"image/bmp",u"image/x-xbitmap",u"image/x-jg",u"image/x-emf",
            u"image/x-wmf",u"video/avi",u"video/mpeg",u"application/octet-stream",u"application/postscript",u"application/base64",u"application/macbinhex40",u"application/pdf",
            u"application/xml",u"application/atom+xml",u"application/rss+xml",u"application/x-compressed",u"application/x-zip-compressed",u"application/x-gzip-compressed",
            u"application/java",u"application/x-msdownload"};
        const auto known=[&](const std::u16string& type){return std::find(known_types.begin(),known_types.end(),type)!=known_types.end();};
        const auto ambiguous=[&](const std::u16string& type){return type.empty() || type==u"text/plain" || type==u"application/octet-stream";};
        const auto text_format=[&](const std::u16string& type){return type.starts_with(u"text/") || type==u"application/postscript" || type==u"application/base64" ||
            type==u"application/macbinhex40" || type==u"application/xml" || type==u"application/atom+xml" || type==u"application/rss+xml";};
        // 副檔名：URL 去掉查詢與錨點（FMFD_URLASFILENAME 時視為檔名），取最後一段的副檔名，讀 HKCR\.ext 的 Content Type 與預設 ProgID。
        std::u16string extension;
        if(arg(1)){
            auto file=wide(1);if(!(flags&1U)){const auto end=file.find_first_of(u"?#");if(end!=file.npos)file.resize(end);}
            const auto slash=file.find_last_of(u"/\\");const auto leaf=slash==file.npos?file:file.substr(slash+1);
            if(const auto dot=leaf.rfind(u'.');dot!=leaf.npos && dot+1<leaf.size())extension=fold(leaf.substr(dot));
        }
        const std::uint64_t classes_root=x86_?0x80000000ULL:0xffffffff80000000ULL;
        const auto class_value=[&](const std::u16string& key,std::u16string_view value)->std::optional<std::u16string>{
            const auto path=scratch.text(key),field=value.empty()?0:scratch.text(std::u16string(value)),data=scratch.allocate(1024),size=scratch.allocate(4);
            if(!path || (!value.empty() && !field) || !data || !size)return std::nullopt;memory.store(GuestAddress{size},32,1024);
            if(host("advapi32.dll","RegGetValueW",{classes_root,path,field,2,0,data,size}))return std::nullopt;return read_utf16(memory,GuestAddress{data},511);};
        const auto extension_type=extension.empty()?std::u16string{}:fold(class_value(extension,u"Content Type").value_or(u""));
        const auto respond=[&](std::u16string type)->std::optional<std::uint32_t>{
            if(flags&0x20U){if(type==u"image/pjpeg")type=u"image/jpeg";else if(type==u"image/x-png")type=u"image/png";} // FMFD_RETURNUPDATEDIMGMIMES
            const auto bytes=utf16_bytes(type);const auto out=static_cast<std::uint64_t>(nested_service(state,memory,"ole32.dll","CoTaskMemAlloc",{bytes.size()}));
            if(!out)return finish(e_outofmemory);memory.write(GuestAddress{out},bytes);memory.store(GuestAddress{arg(6)},bits,x86_?out&UINT32_MAX:out);return finish(0);};
        std::vector<std::uint8_t> data;
        if(arg(2) && static_cast<std::uint32_t>(arg(3))){data.resize(std::min<std::size_t>(static_cast<std::uint32_t>(arg(3)),256));memory.read(GuestAddress{arg(2)},data);}
        // 沒有資料：只看副檔名，其次採用建議型別，都沒有則 E_FAIL。
        if(data.empty()){if(!extension_type.empty())return respond(extension_type);if(!proposed.empty())return respond(proposed);return finish(e_fail);}
        // FMFD_SERVERMIME：建議型別具權威（text/plain 且 FMFD_IGNOREMIMETEXTPLAIN 時仍偵測）；FMFD_RESPECTTEXTPLAIN：建議 text/plain 時不偵測。
        if((flags&8U) && !proposed.empty() && !((flags&4U) && proposed_key==u"text/plain"))return respond(proposed);
        if((flags&0x10U) && proposed_key==u"text/plain")return respond(proposed);
        // 1. 建議型別既非已知也非模糊：直接採用。
        if(!known(proposed_key) && !ambiguous(proposed_key))return respond(proposed);
        // 2. 偵測資料；同時判斷文字或二進位（控制字元，除 TAB、LF、FF、CR、ESC 外；有 BOM 的 Unicode 文字視為文字）。
        const auto starts=[&](std::string_view signature,std::size_t at=0){
            if(data.size()<at+signature.size())return false;for(std::size_t i=0;i<signature.size();++i)if(data[at+i]!=static_cast<std::uint8_t>(signature[i]))return false;return true;};
        bool text=starts("\xff\xfe") || starts("\xfe\xff") || starts("\xef\xbb\xbf");
        if(!text){text=true;for(const auto byte:data)if(byte<0x20 && byte!=9 && byte!=10 && byte!=12 && byte!=13 && byte!=27){text=false;break;}}
        std::string lower(data.begin(),data.end());for(auto& c:lower)if(c>='A' && c<='Z')c=static_cast<char>(c+32);
        std::size_t skip=0;while(skip<lower.size() && (lower[skip]==' ' || lower[skip]=='\t' || lower[skip]=='\r' || lower[skip]=='\n'))++skip;
        const auto leading=[&](std::string_view prefix){return lower.compare(skip,prefix.size(),prefix)==0;};
        std::u16string match;
        if(starts("GIF87a") || starts("GIF89a"))match=u"image/gif";
        else if(starts("\xff\xd8\xff"))match=u"image/pjpeg";
        else if(starts("\x89PNG\r\n\x1a\n"))match=u"image/x-png";
        else if(starts("II*",0) && data.size()>3 && data[3]==0)match=u"image/tiff";
        else if(starts("MM") && data.size()>3 && data[2]==0 && data[3]==42)match=u"image/tiff";
        else if(starts("BM") && data.size()>=14 && !data[6] && !data[7] && !data[8] && !data[9])match=u"image/bmp";
        else if(starts("JG\x04\x0e"))match=u"image/x-jg";
        else if(starts("\x01") && data.size()>=44 && !data[1] && !data[2] && !data[3] && starts(" EMF",40))match=u"image/x-emf";
        else if(starts("\xd7\xcd\xc6\x9a") || ((starts("\x01") || starts("\x02")) && data.size()>=6 && !data[1] && data[2]==9 && !data[3] && !data[4] && data[5]==3))match=u"image/x-wmf";
        else if(starts("RIFF") && starts("WAVE",8))match=u"audio/wav";
        else if(starts("RIFF") && starts("AVI ",8))match=u"video/avi";
        else if(starts("FORM") && (starts("AIFF",8) || starts("AIFC",8)))match=u"audio/x-aiff";
        else if(starts(".snd"))match=u"audio/basic";
        else if(starts("MThd"))match=u"audio/mid";
        else if(data.size()>=4 && !data[0] && !data[1] && data[2]==1 && (data[3]==0xba || data[3]==0xb3))match=u"video/mpeg";
        else if(starts("%PDF"))match=u"application/pdf";
        else if(starts("PK\x03\x04"))match=u"application/x-zip-compressed";
        else if(starts("\x1f\x8b"))match=u"application/x-gzip-compressed";
        else if(starts("\x1f\x9d"))match=u"application/x-compressed";
        else if(starts("\xca\xfe\xba\xbe"))match=u"application/java";
        else if(starts("MZ"))match=u"application/x-msdownload";
        else if(text){
            if(leading("{\\rtf"))match=u"text/richtext";
            else if(leading("%!"))match=u"application/postscript";
            else if(lower.find("(this file must be converted with binhex")!=lower.npos)match=u"application/macbinhex40";
            else if(leading("<?xml")){match=lower.find("<rss")!=lower.npos?u"application/rss+xml":lower.find("<feed")!=lower.npos?u"application/atom+xml":u"text/xml";}
            else if(lower.find("<scriptlet")!=lower.npos)match=u"text/scriptlet";
            else for(const std::string_view tag:{"<!doctype html","<html","<head","<title","<body","<script","<iframe","<table","<a href","<pre","<font","<img","<div","<style","<p>","<br","<h1"})
                if(lower.find(tag)!=lower.npos){match=u"text/html";break;}
        }
        // IE8 起不把建議的影像型別升級為 text/html。
        if(!match.empty() && !(match==u"text/html" && proposed_key.starts_with(u"image/")))return respond(match);
        // 3. 建議型別已知且與資料的文字／二進位格式一致：採用建議型別。
        if(known(proposed_key) && !ambiguous(proposed_key) && text_format(proposed_key)==text)return respond(proposed);
        // 4. 副檔名對應的型別不是已知也不是模糊型別：採用。
        if(!extension_type.empty() && !known(extension_type) && !ambiguous(extension_type))return respond(extension_type);
        // 5. 副檔名有關聯的應用程式（ProgID\shell\open\command）：application/octet-stream。
        if(!extension.empty())if(const auto progid=class_value(extension,u"");progid && !progid->empty() && class_value(*progid+u"\\shell\\open\\command",u""))return respond(u"application/octet-stream");
        // 6. 依資料格式。
        return respond(text?u"text/plain":u"application/octet-stream");
    }
    // 目前 User-Agent：UrlMkSetSessionOption 設定的字串，否則依 IE 相容模式與登錄 Post Platform 權杖組成。
    const auto platform=std::string(x86_?"Windows NT 10.0; WOW64":"Windows NT 10.0; Win64; x64");
    const auto registry_tokens=[&]{
        std::string tokens;std::vector<std::u16string> seen;
        for(const std::uint64_t hive:{x86_?0x80000002ULL:0xffffffff80000002ULL,x86_?0x80000001ULL:0xffffffff80000001ULL}){
            const auto path=scratch.text(u"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings\\5.0\\User Agent\\Post Platform");
            const auto key=scratch.allocate(8),value=scratch.allocate(512),length=scratch.allocate(4);if(!path || !key || !value || !length)break;
            if(host("advapi32.dll","RegOpenKeyExW",{hive,path,0,0x20019,key}))continue;
            const auto handle=memory.load(GuestAddress{key},x86_?32:64);
            for(std::uint32_t index=0;index<256;++index){
                memory.store(GuestAddress{length},32,256);
                if(host("advapi32.dll","RegEnumValueW",{handle,index,value,length,0,0,0,0}))break;
                const auto token=read_utf16(memory,GuestAddress{value},256);
                if(std::find(seen.begin(),seen.end(),token)==seen.end()){seen.push_back(token);tokens+="; "+utf16_to_utf8(token);}
            }
            (void)host("advapi32.dll","RegCloseKey",{handle});
        }
        return tokens;
    };
    const auto user_agent=[&](std::uint32_t option)->std::optional<std::string>{
        if(!option && urlmon_user_agent_)return *urlmon_user_agent_;
        const auto tokens=registry_tokens();
        switch(option){
        case 0:case 7:return "Mozilla/4.0 (compatible; MSIE 7.0; "+platform+"; Trident/7.0"+tokens+")";
        case 7|0x1000:return "Mozilla/4.0 (compatible; MSIE 7.0; "+platform+tokens+")";
        case 8:return "Mozilla/4.0 (compatible; MSIE 8.0; "+platform+"; Trident/7.0"+tokens+")";
        case 9:case 10:return "Mozilla/5.0 (compatible; MSIE "+std::to_string(option)+".0; "+platform+"; Trident/7.0"+tokens+")";
        case 11:return "Mozilla/5.0 ("+platform+"; Trident/7.0"+tokens+"; rv:11.0) like Gecko";
        default:return std::nullopt;
        }
    };
    if(name=="ObtainUserAgentString"){
        // (Option, Output, cbSize*)：*cbSize 輸入緩衝大小、輸出含 NUL 的長度；太小回 E_OUTOFMEMORY 並回填所需大小。
        if(!arg(1) || !arg(2))return finish(invalid);
        const auto agent=user_agent(static_cast<std::uint32_t>(arg(0)));if(!agent)return finish(invalid);
        const auto required=static_cast<std::uint32_t>(agent->size()+1),capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)},32));
        memory.store(GuestAddress{arg(2)},32,required);if(capacity<required)return finish(e_outofmemory);
        std::vector<std::uint8_t> bytes(agent->begin(),agent->end());bytes.push_back(0);memory.write(GuestAddress{arg(1)},bytes);return finish(0);
    }
    if(name=="UrlMkGetSessionOption"){
        // (Option, Buffer, BufferLength, pdwBufferLengthOut, Reserved)
        const auto option=static_cast<std::uint32_t>(arg(0)),capacity=static_cast<std::uint32_t>(arg(2));
        if(!arg(3))return finish(invalid);
        if(option==0x10000001U){ // URLMON_OPTION_USERAGENT：ANSI 字串，長度含 NUL。
            const auto agent=*user_agent(0);const auto required=static_cast<std::uint32_t>(agent.size()+1);memory.store(GuestAddress{arg(3)},32,required);
            if(!arg(1) || capacity<required)return finish(e_outofmemory);
            std::vector<std::uint8_t> bytes(agent.begin(),agent.end());bytes.push_back(0);memory.write(GuestAddress{arg(1)},bytes);return finish(0);
        }
        std::uint32_t value=0;
        if(option==0x10000004U){
            // URLMON_OPTION_URL_ENCODING：預設以 UTF-8 送出 URL（URL_ENCODING_ENABLE_UTF8）；Internet Settings 的 UrlEncoding 為 1 時停用。
            value=urlmon_url_encoding_.value_or(0x10000000U);
            if(!urlmon_url_encoding_){
                const auto path=scratch.text(u"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings"),field=scratch.text(u"UrlEncoding");
                const auto data=scratch.allocate(4),size=scratch.allocate(4);
                if(path && field && data && size){memory.store(GuestAddress{size},32,4);
                    if(!host("advapi32.dll","RegGetValueW",{x86_?0x80000001ULL:0xffffffff80000001ULL,path,field,0x10,0,data,size}) && memory.load(GuestAddress{data},32)==1)value=0x20000000U;}
            }
        }else if(option==0x10000008U || option==0x10000010U)value=1; // USE_BINDSTRINGCREDS／USE_BROWSERAPPSDOCUMENTS 固定為 1。
        else return finish(invalid);
        memory.store(GuestAddress{arg(3)},32,4);if(!arg(1) || capacity<4)return finish(invalid);
        memory.store(GuestAddress{arg(1)},32,value);return finish(0);
    }
    // UrlMkSetSessionOption(Option, Buffer, BufferLength, Reserved)：User-Agent 為程序範圍；代理伺服器選項交給 InternetSetOptionA 的全域設定。
    const auto option=static_cast<std::uint32_t>(arg(0)),length=static_cast<std::uint32_t>(arg(2));
    if(option==0x10000001U){
        if(!arg(1) || !length)return finish(invalid);
        std::string agent(length,'\0');memory.read(GuestAddress{arg(1)},std::span(reinterpret_cast<std::uint8_t*>(agent.data()),agent.size()));
        if(const auto end=agent.find('\0');end!=agent.npos)agent.resize(end);
        urlmon_user_agent_=std::move(agent);return finish(0);
    }
    if(option==0x10000002U){urlmon_user_agent_.reset();return finish(0);} // URLMON_OPTION_USERAGENT_REFRESH：改回依登錄組成的字串。
    if(option==0x10000004U){
        if(!arg(1) || length<4)return finish(invalid);const auto value=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)},32));
        if(value!=0x10000000U && value!=0x20000000U)return finish(invalid);urlmon_url_encoding_=value;return finish(0);
    }
    if(option==37 || option==38 || option==39 || option==95) // INTERNET_OPTION_REFRESH／PROXY／SETTINGS_CHANGED／PROXY_SETTINGS_CHANGED
        return finish(host("wininet.dll","InternetSetOptionA",{0,option,arg(1),length})?0U:invalid);
    return finish(invalid);
}
