#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include "x64arm/windows_paths.hpp"
#include "x64arm/registry_store.hpp"
#include "registry_text_view.hpp"
#include <unicode/uchar.h>

namespace x64arm {
std::span<const WindowsServiceDescriptor> path_service_descriptors() {
    static constexpr WindowsServiceDescriptor table[]{
        {"shlwapi.dll","PathCombineA",3},{"shlwapi.dll","PathCombineW",3},
        {"shlwapi.dll","PathAppendA",2},{"shlwapi.dll","PathAppendW",2},
        {"shlwapi.dll","PathRemoveFileSpecA",1},{"shlwapi.dll","PathRemoveFileSpecW",1},
        {"shlwapi.dll","PathStripToRootA",1},{"shlwapi.dll","PathStripToRootW",1},
        {"shlwapi.dll","PathCanonicalizeA",2},{"shlwapi.dll","PathCanonicalizeW",2},{"shlwapi.dll","PathIsDirectoryA",1},{"shlwapi.dll","PathIsDirectoryW",1},{"kernel32.dll","SearchPathA",6},{"kernel32.dll","SearchPathW",6},{"kernel32.dll","SetSearchPathMode",1},
        {"shlwapi.dll","PathMatchSpecA",2},{"shlwapi.dll","PathMatchSpecW",2},
        {"shlwapi.dll","PathMatchSpecExA",3},{"shlwapi.dll","PathMatchSpecExW",3},
        {"shlwapi.dll","PathAddExtensionA",2},{"shlwapi.dll","PathAddExtensionW",2},
        {"shlwapi.dll","PathRemoveExtensionA",1},{"shlwapi.dll","PathRemoveExtensionW",1},
        {"shlwapi.dll","PathRenameExtensionA",2},{"shlwapi.dll","PathRenameExtensionW",2},
        {"shlwapi.dll","AssocQueryStringA",6},{"shlwapi.dll","AssocQueryStringW",6},
        {"pathcch.dll","PathCchSkipRoot",2},{"pathcch.dll","PathCchCombineEx",5},
        {"pathcch.dll","PathCchCombine",4},{"pathcch.dll","PathCchCanonicalizeEx",4},
        {"pathcch.dll","PathCchCanonicalize",3},
        {"pathcch.dll","PathCchRemoveBackslash",2},{"pathcch.dll","PathCchRemoveBackslashEx",4},
        {"pathcch.dll","PathCchAddBackslash",2},{"pathcch.dll","PathCchAddBackslashEx",4},
        {"pathcch.dll","PathCchRemoveFileSpec",2},{"pathcch.dll","PathCchStripToRoot",2},
        {"pathcch.dll","PathCchIsRoot",1},{"pathcch.dll","PathCchFindExtension",3}
    };return table;
}
namespace {
using AssociationRegistry=RegistryTextView;
const auto association_fold=registry_text_fold;
bool path_pattern(std::u16string_view file,std::u16string_view pattern){
    if(pattern==u"*.*")return true;
    std::size_t f=0,p=0,star=pattern.npos,retry=0;
    while(f<file.size()){
        if(p<pattern.size() && pattern[p]==u'*'){star=p++;retry=f;}
        else if(p<pattern.size() && (pattern[p]==u'?' || u_toupper(pattern[p])==u_toupper(file[f]))){++p;++f;}
        else if(star!=pattern.npos){p=star+1;f=++retry;}
        else return false;
    }
    while(p<pattern.size() && pattern[p]==u'*')++p;
    return p==pattern.size();
}
constexpr std::uint32_t invalid=0x80070057U,too_long=0x800700ceU,too_small=0x8007007aU;
bool drive(std::u16string_view s) {return s.size()>=2 && ((s[0]>=u'A' && s[0]<=u'Z') || (s[0]>=u'a' && s[0]<=u'z')) && s[1]==u':';}
bool prefix(std::u16string_view value,std::u16string_view expected) {
    if(value.size()<expected.size())return false;
    for(std::size_t i=0;i<expected.size();++i){auto c=value[i];if(c>=u'A' && c<=u'Z')c+=u'a'-u'A';if(c!=expected[i])return false;}return true;
}
std::optional<std::size_t> root_end(std::u16string_view s) {
    if(s.empty())return {};
    const auto share=[&](std::size_t at){
        for(unsigned part=0;part<2 && at<s.size();++part){const auto end=s.find(u'\\',at);at=end==s.npos?s.size():end+1;}return at;
    };
    if(s.starts_with(u"\\\\?\\")){
        if(prefix(s,u"\\\\?\\unc\\"))return share(8);
        if(drive(s.substr(4)))return 6+(s.size()>6 && s[6]==u'\\'?1U:0U);
        if(prefix(s,u"\\\\?\\volume{") && s.size()>=48 && s[47]==u'}'){
            for(std::size_t i=11;i<47;++i){const auto c=s[i];if(i==19 || i==24 || i==29 || i==34){if(c!=u'-')return {};}else if(!((c>=u'0' && c<=u'9') || (c>=u'a' && c<=u'f') || (c>=u'A' && c<=u'F')))return {};}
            return 48+(s.size()>48 && s[48]==u'\\'?1U:0U);
        }
        return {};
    }
    if(s.starts_with(u"\\\\"))return share(2);
    if(s.front()==u'\\')return 1;
    if(drive(s))return 2+(s.size()>2 && s[2]==u'\\'?1U:0U);
    return {};
}
std::u16string strip_extended(std::u16string s) {
    if(prefix(s,u"\\\\?\\unc\\"))return u"\\\\"+s.substr(8);
    if(s.starts_with(u"\\\\?\\") && drive(std::u16string_view(s).substr(4)))s.erase(0,4);
    return s;
}
std::u16string combine_path(std::u16string input,std::u16string more) {
    if(more.starts_with(u"\\\\") || drive(more) || input.empty())return more;
    if(more.empty())return input;
    if(more.front()==u'\\'){
        const auto root=root_end(input);input=root?input.substr(0,*root):std::u16string{};
        if(!input.empty() && input.back()==u'\\')more.erase(0,1);
    }else if(input.back()!=u'\\')input.push_back(u'\\');
    return input+more;
}
std::uint32_t canonicalize(std::u16string input,std::uint32_t flags,std::size_t capacity,std::u16string& output) {
    if((flags&~0x3fU) || ((flags&6U) && !(flags&1U)) || (flags&6U)==6U || ((flags&16U) && (flags&1U)))return invalid;
    if(input.size()>=32768)return too_long;
    input=strip_extended(std::move(input));
    const auto root=root_end(input).value_or(0);output=input.substr(0,root);
    std::vector<std::u16string> parts;
    for(std::size_t at=root;at<input.size();){
        const auto end=input.find(u'\\',at);auto part=input.substr(at,end==input.npos?input.size()-at:end-at);
        if(part==u".."){if(!parts.empty())parts.pop_back();}
        else if(part!=u".")parts.push_back(std::move(part));
        if(end==input.npos)break;at=end+1;
    }
    for(std::size_t i=0;i<parts.size();++i){
        if(i || (!output.empty() && output.back()!=u'\\' && output.back()!=u':'))output.push_back(u'\\');
        output+=parts[i];
    }
    if(!(flags&24U))while(!output.empty() && output.back()==u'.'){
        if(output.size()>1 && output[output.size()-2]==u'*')break;output.pop_back();
    }
    if(output.empty())output=u"\\";
    if((output.size()==2 && drive(output)) || ((flags&32U) && output.back()!=u'\\'))output.push_back(u'\\');
    const bool long_allowed=(flags&1U) && capacity>260;
    if(output.size()>=260 && !long_allowed && !(flags&16U))return too_long;
    if((flags&16U) || (long_allowed && output.size()>=260)){
        if(output.starts_with(u"\\\\") && !output.starts_with(u"\\\\?\\"))output=u"\\\\?\\UNC\\"+output.substr(2);
        else if(drive(output) && output.size()>2 && output[2]==u'\\')output=u"\\\\?\\"+output;
    }
    if(output.size()>=32768)return too_long;
    return output.size()>=capacity?too_small:0;
}
}
std::uint32_t WindowsServices::query_association(std::uint32_t flags,std::uint32_t kind,std::u16string source,const std::u16string& extra,std::u16string& output) {
    const auto done=[](std::uint32_t code){return code;};
        const auto mode=flags&0x5800U;
        if(source.empty() || !kind || kind>=24 || (flags&~0x1ffffU) || (mode && (mode&(mode-1))) || ((flags&0x2000) && !(flags&0x800)))return done(invalid);
        // 封裝應用程式解析、間接資源與檔案版本驗證尚無共同後端。
        if((flags&0x10000) || kind==19 || kind>=21 || ((flags&0x40) && kind==4))return done(0x80070032U);
        if(flags&0x4000){const auto colon=source.find(u':');if(colon==source.npos || !colon)return done(invalid);source.resize(colon);}
        if(!registry_file_.empty() && !runtime_.network.external_io)throw HostCallFault("比較模式不允許持久登錄檔 I/O");
        RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)return done(0x80070000U|transaction.error);
        initialize_tokens();
        const bool user_settings=!(flags&0x8010U);
        const auto user=registry_user_root(runtime_.registry,token_user_sid_string(effective_token_id()),token_user_sid_string(1));
        AssociationRegistry lookup{runtime_.registry,class_registry_roots(runtime_.registry,token_user_sid_string(effective_token_id()),token_user_sid_string(1),user_settings),registry_view(0,x86_)};
        auto primary=source,progid=source;
        if(flags&2){const auto slash=source.find_last_of(u"\\/");source=source.substr(slash==source.npos?0:slash+1);primary=u"Applications\\"+source;progid=primary;}
        else if(source.front()==u'{'){
            primary=u"CLSID\\"+source;progid=primary;
            if(!(flags&1))if(auto mapped=lookup.value(primary+u"\\ProgID");mapped && !mapped->empty())primary=progid=*mapped;
        }else if(source.front()==u'.' || (flags&0x5000)){
            std::optional<std::u16string> choice;
            if(user_settings && !(flags&0x800))choice=lookup.value_at(user+(source.front()==u'.'?u"\\software\\microsoft\\windows\\currentversion\\explorer\\fileexts\\":u"\\software\\microsoft\\windows\\shell\\associations\\urlassociations\\")+source+u"\\userchoice",u"ProgId");
            if(!choice || choice->empty())choice=source.front()==u'.'?lookup.value(source):std::optional<std::u16string>{source};
            if(choice && !choice->empty())primary=progid=*choice;else progid.clear();
        }
        if((flags&0x400) && association_fold(progid)==u"unknown")return done(0x80070483U);
        std::vector<std::u16string> candidates;
        const auto append=[&](const std::u16string& path){if(!path.empty() && std::none_of(candidates.begin(),candidates.end(),[&](const auto& old){return association_fold(old)==association_fold(path);}))candidates.push_back(path);};
        append(primary);if(source.front()==u'.')append(source);
        if(!(flags&0x200))for(std::size_t i=0;i<candidates.size() && i<16;++i)if(auto base=lookup.value(candidates[i],u"BaseClass"))append(*base);
        if(flags&4)append(u"*");if(flags&8)append(u"Folder");
        const auto expand=[&](const std::u16string& text){std::u16string out;for(std::size_t at=0;at<text.size();){if(text[at]==u'%'){const auto end=text.find(u'%',at+1);if(end!=text.npos){auto name=text.substr(at+1,end-at-1);for(auto& c:name)c=unicode_case_unit(c,true);const auto env=runtime_.environment.find(name);if(env!=runtime_.environment.end()){out+=env->second.second;at=end+1;continue;}}}out+=text[at++];}return out;};
        const auto executable=[&](const std::u16string& command)->std::optional<std::u16string>{
            const auto text=expand(command);const auto words=parse_windows_arguments(text);if(words.empty())return {};
            std::vector<std::u16string> names;
            if((flags&0x80) && words.size()>1){auto name=association_fold(words[0]);name=name.substr(name.find_last_of(u"\\/")+1);if(name==u"rundll32.exe" || name==u"rundll.exe"){name=words[1];name.resize(name.find(u',')==name.npos?name.size():name.find(u','));names.push_back(name);}}
            if(names.empty()){
                if(text.front()==u'"')names.push_back(words[0]);
                else for(std::size_t end=0;end<text.size();){end=text.find_first_of(u" \t",end+1);names.push_back(text.substr(0,end));if(end==text.npos)break;}
            }
            for(auto name:names){if(name.empty())continue;const auto slash=name.find_last_of(u"\\/:");if(name.find(u'.',slash==name.npos?0:slash+1)==name.npos)name+=u".exe";
                std::vector<std::u16string> directories;
                if(name.find_first_of(u"\\/:")!=name.npos)directories.emplace_back();
                else{directories={runtime_.application_directory,runtime_.current_directory,u"C:\\Windows\\System32",u"C:\\Windows\\System",u"C:\\Windows"};if(const auto env=runtime_.environment.find(u"PATH");env!=runtime_.environment.end()){const auto& list=env->second.second;for(std::size_t at=0;at<=list.size();){const auto end=list.find(u';',at);directories.push_back(list.substr(at,end==list.npos?list.size()-at:end-at));if(end==list.npos)break;at=end+1;}}}
                for(auto directory:directories){if(!directory.empty() && directory.back()!=u'\\')directory+=u'\\';try{auto path=normalized_windows_path(directory+name,runtime_.current_directory);const auto info=filesystem_.information(utf16_to_utf8(path));if(!info.error && !(info.metadata.attributes&0x10))return path;}catch(const std::invalid_argument&) {}}
            }return {};
        };
        std::optional<std::u16string> value;
        if(kind==20){if(!progid.empty() && lookup.exists(primary))value=progid;}
        else for(const auto& path:candidates){
            const auto verb=lookup.verb(path,extra),shell=path+u"\\shell\\"+verb;
            switch(kind){
            case 1:case 2:case 4:{auto command=lookup.value(shell+u"\\command");if(!command)break;
                if(kind==1){value=expand(*command);if((flags&0x40) && !executable(*value))value.reset();}
                else if(kind==2)value=executable(*command);
                else {value=lookup.value(path,u"FriendlyAppName");if(!value)if(auto exe=executable(*command)){auto name=exe->substr(exe->find_last_of(u'\\')+1);value=lookup.value(u"Applications\\"+name,u"FriendlyAppName");if(!value){name.resize(windows_extension_offset(name));value=name;}}}
                break;}
            case 3:value=lookup.value(path,u"FriendlyTypeName");if(!value)value=lookup.value(path);break;
            case 5:value=lookup.value(path,u"NoOpen");break;
            case 6:if(extra.empty())return done(invalid);value=lookup.value(path+u"\\ShellNew",extra);break;
            case 7:value=lookup.value(shell+u"\\ddeexec");break;
            case 8:value=lookup.value(shell+u"\\ddeexec\\ifexec");break;
            case 9:value=lookup.value(shell+u"\\ddeexec\\application");break;
            case 10:value=lookup.value(shell+u"\\ddeexec\\topic");break;
            case 11:value=lookup.value(path,u"InfoTip");break;
            case 12:value=lookup.value(path,u"QuickTip");break;
            case 13:value=lookup.value(path,u"TileInfo");break;
            case 14:value=lookup.value(source,u"Content Type");break;
            case 15:value=lookup.value(path+u"\\DefaultIcon");break;
            case 16:if(extra.empty())return done(invalid);value=lookup.value(path+u"\\shellex\\"+extra);break;
            case 17:value=lookup.value(shell+u"\\DropTarget",u"CLSID");break;
            case 18:value=lookup.value(shell+u"\\command",u"DelegateExecute");break;
            default:break;
            }
            if(value)break;
        }
        if(!value)return done(0x80070483U);
        if((kind==3 || kind==4 || kind==11) && value->starts_with(u"@"))return done(0x80070032U);
    output=std::move(*value);return 0;
}
std::optional<std::uint32_t> WindowsServices::invoke_path(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    if(service.symbol.starts_with("PathStripToRoot")){
        const WindowsCall call(state,memory,service);const bool wide=service.symbol.ends_with('W');std::uint64_t result=0;
        if(call(0)){
            // A 版本按原始位元組縮短，保留 ACP／UTF-8 路徑的原有編碼與大小寫。
            const auto path=wide?read_utf16(memory,GuestAddress{call(0)},259):[&]{const auto bytes=read_ansi(memory,call(0),259);return std::u16string(bytes.begin(),bytes.end());}();
            auto end=root_end(path).value_or(0);
            if(path.starts_with(u"\\\\") && end>2 && path[end-1]==u'\\')--end;
            result=end && !(drive(path) && end==2);
            if(end<path.size())memory.store(GuestAddress{call(0)+end*(wide?2U:1U)},wide?16:8,0);
        }
        call.return_to(state,result);return std::nullopt;
    }
    if(service.symbol.starts_with("PathCombine") || service.symbol.starts_with("PathAppend") || service.symbol.starts_with("PathRemoveFileSpec")){
        const WindowsCall call(state,memory,service);const bool wide=service.symbol.ends_with('W');
        const bool remove=service.symbol.starts_with("PathRemove"),append=service.symbol.starts_with("PathAppend");
        std::uint64_t result=0;
        const auto read=[&](std::uint64_t at){if(wide)return read_utf16(memory,GuestAddress{at},259);const auto bytes=read_ansi(memory,at,259);return std::u16string(bytes.begin(),bytes.end());};
        try {
            if(call(0) && (remove || (append?call(1)!=0:(call(1)!=0 || call(2)!=0)))){
                if(remove){
                    const auto path=read(call(0));std::size_t floor=0;
                    if(path.starts_with(u"\\\\"))floor=2;
                    else if(!path.empty() && path.front()==u'\\')floor=1;
                    else if(drive(path))floor=2+(path.size()>2 && path[2]==u'\\'?1U:0U);
                    const auto last=path.find_last_of(u'\\');const auto end=std::max(floor,last==path.npos?std::size_t{0}:last);
                    if(end<path.size()){memory.store(GuestAddress{call(0)+end*(wide?2U:1U)},wide?16:8,0);result=1;}
                }else{
                    auto first=(append || call(1))?read(call(append?0:1)):std::u16string{};
                    auto more=call(append?1:2)?read(call(append?1:2)):std::u16string{};
                    // PathAppend 的前導反斜線表示附加元件；PathCombine 則保留根相對語義。
                    if(append)more.erase(0,more.find_first_not_of(u'\\'));
                    auto combined=combine_path(std::move(first),std::move(more));
                    // 舊 SHLWAPI 對起始 .\ 與 ..\ 移除句點但保留根反斜線。
                    if(combined.starts_with(u".\\"))combined.erase(0,1);
                    else if(combined.starts_with(u"..\\"))combined.erase(0,2);
                    std::u16string output;const auto error=canonicalize(std::move(combined),0,260,output);
                    if(!error){
                        if(wide)memory.write(GuestAddress{call(0)},utf16_bytes(output));
                        else{std::vector<std::uint8_t> bytes(output.begin(),output.end());bytes.push_back(0);memory.write(GuestAddress{call(0)},bytes);}
                        result=append?1:call(0);
                    }else if(!append)memory.store(GuestAddress{call(0)},wide?16:8,0);
                }
            }
        }catch(const std::invalid_argument&){if(!remove && !append && call(0))memory.store(GuestAddress{call(0)},wide?16:8,0);}
        call.return_to(state,result);return {};
    }
    if(service.symbol=="PathCanonicalizeA" || service.symbol=="PathCanonicalizeW"){
        const WindowsCall call(state,memory,service);std::uint64_t result=0;
        if(!call(0) || !call(1))last_error_=87;
        else {const bool wide=service.symbol.ends_with('W');const auto input=wide?read_utf16(memory,GuestAddress{call(1)}):utf8_to_utf16(read_ansi(memory,call(1)));std::u16string out;
            const auto error=canonicalize(input,0,260,out);const auto bytes=wide?std::string{}:utf16_to_utf8(out);
            if(error || (!wide && bytes.size()>=260)){last_error_=206;memory.store(GuestAddress{call(0)},wide?16:8,0);}
            else {if(wide)memory.write(GuestAddress{call(0)},utf16_bytes(out));else {std::vector<std::uint8_t> value(bytes.begin(),bytes.end());value.push_back(0);memory.write(GuestAddress{call(0)},value);}result=1;}}
        call.return_to(state,result);return std::nullopt;
    }
    if(service.symbol=="PathIsDirectoryA" || service.symbol=="PathIsDirectoryW"){
        const WindowsCall arg(state,memory,service);std::uint32_t value=0;const auto previous=last_error_;
        try{if(arg(0)){
            const auto path=service.symbol.ends_with('W')?read_utf16(memory,GuestAddress{arg(0)},32768):utf8_to_utf16(read_ansi(memory,arg(0)));
            const auto info=query_native_file_attributes(state,memory,path,false);if(!info.error)value=info.metadata.attributes&0x10U;
        }}catch(const MemoryFault&){}catch(const std::invalid_argument&){}catch(const std::bad_alloc&){}
        last_error_=previous;
        arg.return_to(state,value);return std::nullopt;
    }
    const WindowsCall arg(state,memory,service);std::uint32_t result=0;
    try {
    if(service.symbol.starts_with("PathAddExtension") || service.symbol.starts_with("PathRemoveExtension") || service.symbol.starts_with("PathRenameExtension")){
        const bool wide=service.symbol.ends_with('W'),remove=service.symbol.starts_with("PathRemove"),add=service.symbol.starts_with("PathAdd");
        const auto read=[&](std::uint64_t at){if(wide)return read_utf16(memory,GuestAddress{at},259);const auto bytes=read_ansi(memory,at,259);return std::u16string(bytes.begin(),bytes.end());};
        if(arg(0) && (remove || add || arg(1))){
            auto path=read(arg(0));const auto offset=windows_extension_offset(path);
            if(remove){memory.store(GuestAddress{arg(0)+offset*(wide?2:1)},wide?16:8,0);}
            else if(!add || offset==path.size()){
                auto extension=arg(1)?read(arg(1)):std::u16string{};if(add && extension.empty())extension=u".exe";
                path.resize(offset);if(path.size()+extension.size()<260){path+=extension;if(wide)memory.write(GuestAddress{arg(0)},utf16_bytes(path));else{std::vector<std::uint8_t> bytes(path.begin(),path.end());bytes.push_back(0);memory.write(GuestAddress{arg(0)},bytes);}result=1;}
            }
        }
    }else if(service.symbol.starts_with("AssocQueryString")){
        const bool wide=service.symbol.ends_with('W');const auto flags=static_cast<std::uint32_t>(arg(0)),kind=static_cast<std::uint32_t>(arg(1));
        const auto done=[&](std::uint32_t value)->std::optional<std::uint32_t>{arg.return_to(state,value);return {};};
        if(!arg(2) || !arg(5))return done(invalid);
        const auto read=[&](unsigned i){return arg(i)?(wide?read_utf16(memory,GuestAddress{arg(i)},32768):utf8_to_utf16(read_ansi(memory,arg(i),32768))):std::u16string{};};
        std::u16string value;const auto status=query_association(flags,kind,read(2),read(3),value);if(status)return done(status);
        std::vector<std::uint8_t> bytes;
        if(wide)bytes=utf16_bytes(value);else{const auto text=utf16_to_utf8(value);bytes.assign(text.begin(),text.end());bytes.push_back(0);}
        const auto width=wide?2U:1U,required=static_cast<std::uint32_t>(bytes.size()/width),capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(5)},32));
        memory.store(GuestAddress{arg(5)},32,required);
        if(!arg(4))return done(1);
        if(capacity<required){if(flags&0x20){if(capacity)memory.store(GuestAddress{arg(4)},wide?16:8,0);return done(0x80004003U);}memory.write(GuestAddress{arg(4)},std::span(bytes).first(capacity*width));return done(too_small);}
        memory.write(GuestAddress{arg(4)},bytes);
    }else if(service.symbol.starts_with("PathMatchSpec")){

        const bool wide=service.symbol.ends_with('W'),extended=service.symbol.starts_with("PathMatchSpecEx");const auto flags=extended?static_cast<std::uint32_t>(arg(2)):1U;
        if(!arg(0) || !arg(1) || (flags&~0x10001U))result=extended?invalid:0;
        else{
            const auto read=[&](unsigned i){return wide?read_utf16(memory,GuestAddress{arg(i)},32768):utf8_to_utf16(read_ansi(memory,arg(i),32768));};
            const auto file=read(0),spec=read(1);bool matched=false;
            for(std::size_t at=0;at<=spec.size();){const auto end=(flags&1)?spec.find(u';',at):spec.npos;auto part=std::u16string_view(spec).substr(at,end==spec.npos?spec.size()-at:end-at);
                if(!(flags&0x10000))while(!part.empty() && part.front()==u' ')part.remove_prefix(1);
                if(path_pattern(file,part)){matched=true;break;}if(end==spec.npos)break;at=end+1;
            }
            result=extended?(matched?0:1):(matched?1:0);
        }
    }else if(service.symbol=="SetSearchPathMode"){
        const auto flags=static_cast<std::uint32_t>(arg(0));
        if(flags!=1 && flags!=0x10000 && flags!=0x8001)last_error_=87;
        else if((runtime_.search_path_mode&0x8000) && flags!=0x8001)last_error_=5;
        else{runtime_.search_path_mode=flags;result=1;}
    }else if(service.symbol.starts_with("SearchPath")){
        const bool wide=service.symbol.ends_with('W');const auto read=[&](unsigned i){return arg(i)?(wide?read_utf16(memory,GuestAddress{arg(i)},32768):utf8_to_utf16(read_ansi(memory,arg(i),32768))):std::u16string{};};
        auto name=read(1);const auto extension=read(2);std::replace(name.begin(),name.end(),u'/',u'\\');
        if(name.empty() || (!extension.empty() && extension.front()!=u'.'))last_error_=87;
        else{
            const auto component=name.find_last_of(u"\\:");if(name.find(u'.',component==name.npos?0:component+1)==name.npos)name+=extension;
            std::vector<std::u16string> paths;
            const auto append=[&](std::u16string_view list){for(std::size_t at=0;at<=list.size();){const auto end=list.find(u';',at);auto entry=list.substr(at,end==list.npos?list.size()-at:end-at);if(entry.size()>=2 && entry.front()==u'"' && entry.back()==u'"')entry=entry.substr(1,entry.size()-2);paths.emplace_back(entry);if(end==list.npos)break;at=end+1;}};
            if(name.find_first_of(u"\\:")!=name.npos)paths.emplace_back();
            else if(arg(0))append(read(0));
            else{
                bool safe=(runtime_.search_path_mode&1)!=0;
                if(!runtime_.search_path_mode){
                    if(!registry_file_.empty() && !runtime_.network.external_io)throw HostCallFault("比較模式不允許持久登錄檔 I/O");
                    RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);
                    if(transaction.error){last_error_=transaction.error;arg.return_to(state,0);return std::nullopt;}
                    const auto key=runtime_.registry.keys.find(std::u16string(1,char16_t{2})+u"\\system\\currentcontrolset\\control\\session manager");
                    if(key!=runtime_.registry.keys.end()){const auto value=key->second.find(u"safeprocesssearchmode");if(value!=key->second.end() && value->second.type==4 && value->second.bytes.size()==4)safe=value->second.bytes[0]==1 && !value->second.bytes[1] && !value->second.bytes[2] && !value->second.bytes[3];}
                }
                auto executable=runtime_.image.path;
                if(executable.empty()){const auto argv=memory.load(GuestAddress{runtime_.wargv_cell},x86_?32:64);executable=read_utf16(memory,GuestAddress{memory.load(GuestAddress{argv},x86_?32:64)});}
                executable=normalized_windows_path(executable);paths.push_back(executable.substr(0,executable.find_last_of(u'\\')+1));
                if(!safe)paths.push_back(runtime_.current_directory);
                paths.insert(paths.end(),{u"C:\\Windows\\System32",u"C:\\Windows\\System",u"C:\\Windows"});
                if(safe)paths.push_back(runtime_.current_directory);
                if(const auto env=runtime_.environment.find(u"PATH");env!=runtime_.environment.end())append(env->second.second);
            }
            std::u16string found;std::uint32_t lookup_error=2;
            for(auto path:paths){
                if(!path.empty() && path.back()!=u'\\')path+=u'\\';path=normalized_windows_path(path+name,runtime_.current_directory);
                const auto info=filesystem_.information(utf16_to_utf8(path));
                if(!info.error && !(info.metadata.attributes&0x10)){found=std::move(path);break;}
                if(info.error && name.find_first_of(u"\\:")!=name.npos){lookup_error=info.error;break;}
                if(info.error && info.error!=2 && info.error!=3 && lookup_error==2)lookup_error=info.error;
            }
            if(found.empty())last_error_=lookup_error;
            else{
                const auto ansi=utf16_to_utf8(found);const auto count=wide?found.size():ansi.size();const auto capacity=static_cast<std::uint32_t>(arg(3));
                if(!arg(4) || capacity<=count)result=static_cast<std::uint32_t>(count+1);
                else{
                    if(arg(5))memory.check(GuestAddress{arg(5)},x86_?4:8,Access::Write);
                    if(wide)memory.write(GuestAddress{arg(4)},utf16_bytes(found));else{std::vector<std::uint8_t> bytes(ansi.begin(),ansi.end());bytes.push_back(0);memory.write(GuestAddress{arg(4)},bytes);}
                    if(arg(5)){const auto slash=found.find_last_of(u'\\');const auto offset=wide?(slash+1)*2:utf16_to_utf8(found.substr(0,slash+1)).size();memory.store(GuestAddress{arg(5)},x86_?32:64,arg(4)+offset);}
                    result=static_cast<std::uint32_t>(count);
                }
            }
        }
    }else if(service.symbol=="PathCchIsRoot"){
        // BOOL：整個字串恰為根（"C:\\"、"\\\\server\\share"、"\\"）。
        if(arg(0)){const auto path=read_utf16(memory,GuestAddress{arg(0)});const auto root=root_end(path);result=root && *root==path.size()?1U:0U;}
    }else if(service.symbol.starts_with("PathCchRemoveBackslash") || service.symbol.starts_with("PathCchAddBackslash") ||
             service.symbol=="PathCchRemoveFileSpec" || service.symbol=="PathCchStripToRoot" || service.symbol=="PathCchFindExtension"){
        // 就地修改：cch 含結尾 NUL，上限 PATHCCH_MAX_CCH；S_OK 表示已變更、S_FALSE 表示不需變更。
        const auto capacity=arg(1);const auto name=service.symbol;
        if(!arg(0) || !capacity || capacity>32768)result=invalid;
        else {
            auto path=read_utf16(memory,GuestAddress{arg(0)});
            if(path.size()>=capacity)result=invalid;
            else {
                const auto root=root_end(path).value_or(0);bool changed=false;
                if(name.starts_with("PathCchRemoveBackslash")){if(!path.empty() && path.back()==u'\\' && path.size()>root){path.pop_back();changed=true;}}
                else if(name.starts_with("PathCchAddBackslash")){
                    if(path.empty() || path.back()!=u'\\'){if(path.size()+2>capacity)result=too_small;else {path.push_back(u'\\');changed=true;}}
                }else if(name=="PathCchRemoveFileSpec"){
                    if(path.size()>root){const auto slash=path.find_last_of(u'\\');path.resize(slash==path.npos || slash<root?root:std::max<std::size_t>(slash,root));changed=true;}
                }else if(name=="PathCchStripToRoot"){
                    if(!root_end(path))result=invalid;else if(path.size()>root){path.resize(root);changed=true;}
                }else {
                    if(!arg(2))result=invalid;
                    else {const auto slash=path.find_last_of(u'\\');const auto dot=path.find_last_of(u'.');
                        const auto at=dot!=path.npos && dot>=root && (slash==path.npos || dot>slash)?dot:path.size();
                        memory.store(GuestAddress{arg(2)},x86_?32:64,arg(0)+at*2);}
                }
                if(!result && name!="PathCchFindExtension"){
                    if(changed)memory.write(GuestAddress{arg(0)},utf16_bytes(path));else result=1;
                    if(name.ends_with("Ex")){
                        if(arg(2))memory.store(GuestAddress{arg(2)},x86_?32:64,arg(0)+path.size()*2);
                        if(arg(3))memory.store(GuestAddress{arg(3)},x86_?32:64,capacity-path.size());
                    }
                }
            }
        }
    }else if(service.symbol=="PathCchSkipRoot"){
        if(!arg(0) || !arg(1))result=invalid;
        else {const auto path=read_utf16(memory,GuestAddress{arg(0)});const auto root=root_end(path);
            if(root)memory.store(GuestAddress{arg(1)},x86_?32:64,arg(0)+*root*2);else result=invalid;}
    }else {
        const auto capacity=arg(1);if(!arg(0) || !capacity || capacity>32768)result=invalid;
        else {
            const bool combine=service.symbol.starts_with("PathCchCombine"),extended=service.symbol.ends_with("Ex");
            const auto flags=extended?static_cast<std::uint32_t>(arg(combine?4:3)):0U;
            auto input=arg(2)?read_utf16(memory,GuestAddress{arg(2)}):std::u16string{};
            if(combine){
                auto more=arg(3)?read_utf16(memory,GuestAddress{arg(3)}):std::u16string{};
                input=combine_path(std::move(input),std::move(more));
            }
            // 先讀完所有來源，允許輸出與任一輸入使用相同緩衝區。
            std::u16string output;result=canonicalize(std::move(input),flags,static_cast<std::size_t>(capacity),output);
            if(result)memory.store(GuestAddress{arg(0)},16,0);
            else memory.write(GuestAddress{arg(0)},utf16_bytes(output));
        }
    }
    } catch(const std::invalid_argument&) {
        if(service.symbol.starts_with("AssocQueryString"))result=invalid;
        else if(service.symbol.starts_with("PathAddExtension") || service.symbol.starts_with("PathRemoveExtension") || service.symbol.starts_with("PathRenameExtension"))result=0;
        else if(service.symbol.starts_with("SearchPath")){last_error_=123;result=0;}
        else if(service.symbol.starts_with("PathMatchSpec"))result=service.symbol.starts_with("PathMatchSpecEx")?invalid:0;
        else{result=too_long;if(service.symbol!="PathCchSkipRoot" && arg(0) && arg(1) && arg(1)<=32768)memory.store(GuestAddress{arg(0)},16,0);}
    }
    arg.return_to(state,result);return {};
}
}
