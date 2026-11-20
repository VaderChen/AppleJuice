#include "shell_icon_location.h"
#include "shell_unc_path.h"
#include "shell_drive_root.h"
#include "shell_file_name.h"
#include "shell_quote_text.h"
#include "shell_parse_field.h"
#include "shell_ole_string.h"
#include "shell_extension_text.h"
#include "shell_end_slash.h"
#include "shell_remove_file_spec.h"
#include "shell_path_join.h"
#include "x64arm/windows_system_paths.hpp"
#include "x64arm/windows_services.hpp"
#include "service_callback_task.hpp"
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
        {"shlwapi.dll","PathAddBackslashA",1},{"shlwapi.dll","PathAddBackslashW",1},{"shlwapi.dll","PathRemoveBackslashA",1},{"shlwapi.dll","PathRemoveBackslashW",1},
        {"shlwapi.dll","PathIsRootA",1},{"shlwapi.dll","PathIsRootW",1},{"shlwapi.dll","PathSkipRootA",1},{"shlwapi.dll","PathSkipRootW",1},
        {"shlwapi.dll","PathStripToRootA",1},{"shlwapi.dll","PathStripToRootW",1},
        {"shlwapi.dll","PathCanonicalizeA",2},{"shlwapi.dll","PathCanonicalizeW",2},{"shlwapi.dll","PathIsDirectoryA",1},{"shlwapi.dll","PathIsDirectoryW",1},{"kernel32.dll","SearchPathA",6},{"kernel32.dll","SearchPathW",6},{"kernel32.dll","SetSearchPathMode",1},
        {"shlwapi.dll","PathMatchSpecA",2},{"shlwapi.dll","PathMatchSpecW",2},
        {"shlwapi.dll","PathMatchSpecExA",3},{"shlwapi.dll","PathMatchSpecExW",3},
        {"internal-shell-ole-string","OleStrToStrN",4,0,0,0xa},{"internal-shell-ole-string","StrToOleStr",2},
        {"internal-shell-field","ParseFieldW",4},
        {"internal-shell-extension","PathGetExtension",3,0,0,6},
        {"shlwapi.dll","PathFindExtensionA",1},{"shlwapi.dll","PathFindExtensionW",1},
        {"shlwapi.dll","PathAddExtensionA",2},{"shlwapi.dll","PathAddExtensionW",2},
        {"shlwapi.dll","PathRemoveExtensionA",1},{"shlwapi.dll","PathRemoveExtensionW",1},
        {"shlwapi.dll","PathRenameExtensionA",2},{"shlwapi.dll","PathRenameExtensionW",2},
        {"shlwapi.dll","AssocQueryStringA",6,0,0,0x3},{"shlwapi.dll","AssocQueryStringW",6,0,0,0x3},
        {"shlwapi.dll","AssocQueryStringByKeyA",6,0,0,0x3},{"shlwapi.dll","AssocQueryStringByKeyW",6,0,0,0x3},
        {"shlwapi.dll","AssocQueryKeyA",5,0,0,0x3},{"shlwapi.dll","AssocQueryKeyW",5,0,0,0x3},
        {"shlwapi.dll","AssocGetPerceivedType",4},{"shlwapi.dll","AssocIsDangerous",1},{"shlwapi.dll","AssocCreate",3,0,0x1},
        {"internal-query-associations","QueryInterface",3},{"internal-query-associations","AddRef",1},{"internal-query-associations","Release",1},
        {"internal-query-associations","Init",5,0,0,0x2},{"internal-query-associations","GetString",6,0,0,0x6},{"internal-query-associations","GetKey",5,0,0,0x6},
        {"internal-query-associations","GetData",6,0,0,0x6},{"internal-query-associations","GetEnum",6,0,0,0x6},
        {"pathcch.dll","PathCchSkipRoot",2},{"pathcch.dll","PathCchCombineEx",5},
        {"pathcch.dll","PathCchCombine",4},{"pathcch.dll","PathCchCanonicalizeEx",4},
        {"pathcch.dll","PathCchCanonicalize",3},
        {"pathcch.dll","PathCchRemoveBackslash",2},{"pathcch.dll","PathCchRemoveBackslashEx",4},
        {"pathcch.dll","PathCchAddBackslash",2},{"pathcch.dll","PathCchAddBackslashEx",4},
        {"pathcch.dll","PathCchRemoveFileSpec",2},{"pathcch.dll","PathCchStripToRoot",2},
        {"pathcch.dll","PathCchIsRoot",1},{"pathcch.dll","PathCchFindExtension",3},
        // 副檔名增刪改、\\?\ 前綴移除、附加路徑、配置結果的正規化／合併與 UNC 判斷（Windows 8 起的 pathcch 介面）。
        {"pathcch.dll","PathCchAddExtension",3},{"pathcch.dll","PathCchRemoveExtension",2},{"pathcch.dll","PathCchRenameExtension",3},
        {"pathcch.dll","PathCchStripPrefix",2},{"pathcch.dll","PathCchAppend",3},{"pathcch.dll","PathCchAppendEx",4,0,0,0x8},
        {"pathcch.dll","PathAllocCanonicalize",3,0,0,0x2},{"pathcch.dll","PathAllocCombine",4,0,0,0x4}
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
// 最後一段的副檔名起點（'.' 的位置）；沒有副檔名時為字串長度。根之前的 '.' 不算。
std::size_t extension_at(std::u16string_view path) {
    const auto root=root_end(path).value_or(0);const auto slash=path.find_last_of(u'\\');const auto dot=path.find_last_of(u'.');
    return dot!=path.npos && dot>=root && (slash==path.npos || dot>slash)?dot:path.size();
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
std::uint32_t WindowsServices::query_association(std::uint32_t flags,std::uint32_t kind,std::u16string source,const std::u16string& extra,std::u16string& output,std::u16string* location) {
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
        if(kind==20){if(!progid.empty() && lookup.exists(primary)){value=progid;if(location)*location=primary;}}
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
            if(value){if(location)*location=path;break;}
        }
        if(!value)return done(0x80070483U);
        if((kind==3 || kind==4 || kind==11) && value->starts_with(u"@"))return done(0x80070032U);
    output=std::move(*value);return 0;
}
// 檔案關聯家族：AssocQueryString(ByKey)、AssocQueryKey、AssocGetPerceivedType、AssocIsDangerous、AssocCreate 與 IQueryAssociations。
// 查詢一律交給 query_association；ByKey 與 IQueryAssociations::Init(hkProgid) 以 NtQueryKey 取得機碼名稱，轉成相對 HKEY_CLASSES_ROOT 的類別路徑。
std::optional<std::uint32_t> WindowsServices::invoke_association(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const bool object=service.family=="internal-query-associations";
    const bool wide=object || name.ends_with('W') || name=="AssocGetPerceivedType" || name=="AssocIsDangerous";
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const std::uint64_t classes_root=x86_?0x80000000ULL:0xffffffff80000000ULL;
    const auto done=[&](std::uint64_t value)->std::optional<std::uint32_t>{arg.return_to(state,value);return {};};
    const auto read=[&](std::uint64_t at)->std::u16string{if(!at)return {};return wide?read_utf16(memory,GuestAddress{at},32768):utf8_to_utf16(read_ansi(memory,at,32768));};
    const auto host=[&](std::string_view dll,std::string_view symbol,std::initializer_list<std::uint64_t> args){return nested_service(state,memory,dll,symbol,args);};
    // 字串輸出（AssocQueryString 慣例）：*pcch 寫回含 NUL 的所需長度；輸出為 NULL 回 S_FALSE；空間不足時 NOTRUNCATE（0x20）回 E_POINTER，否則截斷並回 ERROR_INSUFFICIENT_BUFFER。
    const auto write_string=[&](bool as_wide,std::uint32_t flags,const std::u16string& value,std::uint64_t out,std::uint64_t count)->std::uint32_t{
        std::vector<std::uint8_t> bytes;if(as_wide)bytes=utf16_bytes(value);else {const auto text=utf16_to_utf8(value);bytes.assign(text.begin(),text.end());bytes.push_back(0);}
        const auto width=as_wide?2U:1U,required=static_cast<std::uint32_t>(bytes.size()/width),capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{count},32));
        memory.store(GuestAddress{count},32,required);if(!out)return 1;
        if(capacity<required){if(flags&0x20){if(capacity)memory.store(GuestAddress{out},as_wide?16:8,0);return 0x80004003U;}memory.write(GuestAddress{out},std::span(bytes).first(capacity*width));return too_small;}
        memory.write(GuestAddress{out},bytes);return 0;};
    // HKEY → 相對 HKCR 的類別路徑：查登錄 handle 表的邏輯路徑；HKCR 根（代碼 0）直接取其後，HKLM／使用者的 Software\Classes 與 <SID>_Classes 取 Classes 之後。
    const auto key_source=[&](std::uint64_t key)->std::optional<std::u16string>{
        if(!key)return std::nullopt;if(!registry_file_.empty() && !runtime_.network.external_io)throw HostCallFault("比較模式不允許持久登錄檔 I/O");
        RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)return std::nullopt;
        const auto found=runtime_.registry.handles.find(key);if(found==runtime_.registry.handles.end())return std::nullopt;
        const auto& path=registry_logical_path(found->second);if(path.size()<3 || path[1]!=u'\\')return std::nullopt;
        if(path[0]==0)return path.substr(2);
        auto folded=path;for(auto& c:folded)c=unicode_case_unit(c,false);
        for(const auto marker:{std::u16string_view(u"\\software\\classes\\"),std::u16string_view(u"_classes\\")})
            if(const auto at=folded.find(marker);at!=folded.npos && at+marker.size()<path.size())return path.substr(at+marker.size());
        return std::nullopt;};
    // 開啟 HKCR 下的類別機碼並寫到輸出 HKEY*。
    const auto open_class=[&](const std::u16string& path,std::uint64_t out)->std::uint32_t{
        GuestScratch temporary(*this,memory,state);const auto text=temporary.text(path);if(!text)return 0x8007000eU;
        const auto status=static_cast<std::uint32_t>(host("advapi32.dll","RegOpenKeyExW",{classes_root,text,0,0x20019,out}));return status?0x80070000U|(status&0xffffU):0;};
    // ASSOCKEY：SHELLEXECCLASS 1（有指令的類別）、APP 2（Applications\執行檔）、CLASS 3（ProgID）、BASECLASS 4。
    const auto query_key=[&](std::uint32_t flags,std::uint32_t key,const std::u16string& source,const std::u16string& extra,std::uint64_t out)->std::uint32_t{
        if(!out)return invalid;memory.store(GuestAddress{out},bits,0);if(source.empty() || key<1 || key>4)return invalid;
        std::u16string value,location;
        if(key==1){const auto status=query_association(flags,1,source,extra,value,&location);if(status)return status;return open_class(location,out);}
        if(key==2){const auto status=query_association(flags,2,source,extra,value);if(status)return status;return open_class(u"Applications\\"+value.substr(value.find_last_of(u"\\/")+1),out);}
        auto status=query_association(flags,20,source,extra,value,&location);
        if(status && source.front()==u'.'){location=source;status=0;}if(status)return status;
        if(key==3)return open_class(location,out);
        GuestScratch temporary(*this,memory,state);const auto path=temporary.text(location),field=temporary.text(u"BaseClass"),data=temporary.allocate(520),size=temporary.allocate(4);if(!path || !field || !data || !size)return 0x8007000eU;
        memory.store(GuestAddress{size},32,520);if(host("advapi32.dll","RegGetValueW",{classes_root,path,field,2,0,data,size})&UINT32_MAX)return 0x80070483U; // ERROR_NO_ASSOCIATION
        return open_class(read_utf16(memory,GuestAddress{data},259),out);};
    // ASSOCDATA：MSIDESCRIPTOR 1、NOACTIVATEHANDLER 2、HASPERUSERASSOC 4、EDITFLAGS 5、VALUE 6（pszExtra 為值名稱）。輸出為 NULL 時回 S_FALSE 與所需大小。
    const auto query_data=[&](std::uint32_t flags,std::uint32_t data,const std::u16string& source,const std::u16string& extra,std::uint64_t out,std::uint64_t count)->std::uint32_t{
        if(!count || source.empty())return invalid;std::u16string value,location;
        if(query_association(flags,20,source,extra,value,&location)){if(source.front()!=u'.')return 0x80070483U;location=source;}
        std::u16string key=location,field;
        if(data==5)field=u"EditFlags";else if(data==6){if(extra.empty())return invalid;field=extra;}
        else if(data==1 || data==2){std::u16string command;std::u16string at;if(query_association(flags,1,source,u"",command,&at))return 0x80070483U;
            const auto verb=extra.empty()?std::u16string(u"open"):extra;key=at+u"\\shell\\"+verb+(data==1?u"\\command":u"\\ddeexec");field=data==1?u"command":u"NoActivateHandler";}
        else if(data==4){std::u16string unused;return query_association(flags,20,source,extra,unused)==0 && source.front()==u'.'?0U:1U;}
        else return 0x80070032U; // QUERYCLASSSTORE：沒有類別存放區
        GuestScratch temporary(*this,memory,state);const auto path=temporary.text(key),name_text=temporary.text(field),size=temporary.allocate(4),type=temporary.allocate(4);if(!path || !name_text || !size || !type)return 0x8007000eU;
        memory.store(GuestAddress{size},32,0);auto status=static_cast<std::uint32_t>(host("advapi32.dll","RegGetValueW",{classes_root,path,name_text,0xffff,type,0,size}));
        if(status)return data==2?1U:0x80070000U|(status&0xffffU);if(data==2)return 0;
        const auto required=static_cast<std::uint32_t>(memory.load(GuestAddress{size},32)),capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{count},32));memory.store(GuestAddress{count},32,required);
        if(!out)return 1;if(capacity<required)return 0x80004003U;memory.store(GuestAddress{size},32,capacity);
        status=static_cast<std::uint32_t>(host("advapi32.dll","RegGetValueW",{classes_root,path,name_text,0xffff,type,out,size}));if(status)return 0x80070000U|(status&0xffffU);
        memory.store(GuestAddress{count},32,memory.load(GuestAddress{size},32));return 0;};
    try{
        if(name.starts_with("AssocQueryString")){
            // (flags, ASSOCSTR, pszAssoc 或 hkAssoc, pszExtra, out, pcch)
            const bool by_key=name.starts_with("AssocQueryStringByKey");if(!arg(2) || !arg(5))return done(invalid);
            std::u16string source;if(by_key){const auto found=key_source(arg(2));if(!found)return done(invalid);source=*found;}else source=read(arg(2));
            std::u16string value;const auto status=query_association(static_cast<std::uint32_t>(arg(0)),static_cast<std::uint32_t>(arg(1)),source,read(arg(3)),value);if(status)return done(status);
            return done(write_string(wide,static_cast<std::uint32_t>(arg(0)),value,arg(4),arg(5)));
        }
        if(name.starts_with("AssocQueryKey"))return done(query_key(static_cast<std::uint32_t>(arg(0)),static_cast<std::uint32_t>(arg(1)),read(arg(2)),read(arg(3)),arg(4)));
        if(name=="AssocGetPerceivedType"){
            // (副檔名, PERCEIVED*, PERCEIVEDFLAG*, PWSTR* 可為 NULL)：登錄的 PerceivedType 優先（SOFTCODED 1），其次內建表（HARDCODED 2，另含 NATIVESUPPORT／GDIPLUS／WMSDK／ZIPFOLDER），
            // 再其次 Content Type 的主類型；都沒有時為 UNSPECIFIED（-2）並回 ERROR_FILE_NOT_FOUND。
            static constexpr std::u16string_view names[]{u"text",u"image",u"audio",u"video",u"compressed",u"document",u"system",u"application",u"gamemedia",u"contacts"};
            struct Builtin {std::u16string_view extension;std::int32_t type;std::uint32_t flags;};
            static constexpr Builtin builtin[]{
                {u".txt",1,2},{u".bmp",2,0x16},{u".dib",2,0x16},{u".emf",2,0x16},{u".gif",2,0x16},{u".ico",2,0x16},{u".jfif",2,0x16},{u".jpe",2,0x16},{u".jpeg",2,0x16},{u".jpg",2,0x16},
                {u".png",2,0x16},{u".rle",2,0x16},{u".tif",2,0x16},{u".tiff",2,0x16},{u".wmf",2,0x16},
                {u".aif",3,0x22},{u".aifc",3,0x22},{u".aiff",3,0x22},{u".au",3,0x22},{u".m3u",3,0x22},{u".mid",3,0x22},{u".midi",3,0x22},{u".mp2",3,0x22},{u".mp3",3,0x22},
                {u".rmi",3,0x22},{u".snd",3,0x22},{u".wav",3,0x22},{u".wax",3,0x22},{u".wma",3,0x22},
                {u".asf",4,0x22},{u".asx",4,0x22},{u".avi",4,0x22},{u".ivf",4,0x22},{u".m1v",4,0x22},{u".mpa",4,0x22},{u".mpe",4,0x22},{u".mpeg",4,0x22},{u".mpg",4,0x22},
                {u".mpv2",4,0x22},{u".wm",4,0x22},{u".wmv",4,0x22},{u".wvx",4,0x22},
                {u".zip",5,0x46},{u".cab",5,0x06},{u".exe",8,0x02},{u".com",8,0x02},{u".bat",8,0x02},{u".cmd",8,0x02},{u".pif",8,0x02},{u".scr",8,0x02},{u".dll",7,0x02},{u".sys",7,0x02},
            };
            if(arg(1))memory.store(GuestAddress{arg(1)},32,static_cast<std::uint32_t>(-2));if(arg(2))memory.store(GuestAddress{arg(2)},32,0);if(arg(3))memory.store(GuestAddress{arg(3)},bits,0);
            auto extension=read(arg(0));if(extension.empty() || extension.front()!=u'.')return done(invalid);for(auto& c:extension)c=unicode_case_unit(c,false);
            std::int32_t type=-2;std::uint32_t flags=0;std::u16string type_name;
            GuestScratch temporary(*this,memory,state);const auto key=temporary.text(extension),data=temporary.allocate(520),size=temporary.allocate(4);if(!key || !data || !size)return done(0x8007000eU);
            const auto registry_text=[&](std::u16string_view field)->std::optional<std::u16string>{const auto name_text=temporary.text(field);if(!name_text)return std::nullopt;memory.store(GuestAddress{size},32,520);
                if(host("advapi32.dll","RegGetValueW",{classes_root,key,name_text,2,0,data,size})&UINT32_MAX)return std::nullopt;return read_utf16(memory,GuestAddress{data},259);};
            if(auto perceived=registry_text(u"PerceivedType")){auto folded=*perceived;for(auto& c:folded)c=unicode_case_unit(c,false);
                for(std::size_t i=0;i<std::size(names);++i)if(folded==names[i])type=static_cast<std::int32_t>(i+1);if(type==-2)type=-3;flags=1;type_name=*perceived;}
            if(type==-2)for(const auto& item:builtin)if(item.extension==extension){type=item.type;flags=item.flags;type_name=std::u16string(names[item.type-1]);break;}
            if(type==-2)if(auto content=registry_text(u"Content Type")){auto folded=*content;for(auto& c:folded)c=unicode_case_unit(c,false);
                for(const auto& [prefix,kind]:std::initializer_list<std::pair<std::u16string_view,std::int32_t>>{{u"text/",1},{u"image/",2},{u"audio/",3},{u"video/",4}})
                    if(folded.starts_with(prefix)){type=kind;flags=1;type_name=std::u16string(names[kind-1]);break;}}
            if(arg(1))memory.store(GuestAddress{arg(1)},32,static_cast<std::uint32_t>(type));if(arg(2))memory.store(GuestAddress{arg(2)},32,flags);
            if(type==-2)return done(0x80070002U);
            if(arg(3) && !type_name.empty()){auto bytes=utf16_bytes(type_name);auto block=host("ole32.dll","CoTaskMemAlloc",{bytes.size()});if(x86_)block&=UINT32_MAX;if(!block)return done(0x8007000eU);memory.write(GuestAddress{block},bytes);memory.store(GuestAddress{arg(3)},bits,block);}
            return done(0);
        }
        if(name=="AssocIsDangerous"){
            // 內建的危險副檔名，或 ProgID 的 EditFlags 含 FTA_AlwaysUnsafe（0x20000）。
            static constexpr std::u16string_view dangerous[]{u".ade",u".adp",u".app",u".asp",u".bas",u".bat",u".cer",u".chm",u".cmd",u".cnt",u".com",u".cpl",u".crt",u".csh",u".der",u".exe",u".fxp",
                u".gadget",u".hlp",u".hpj",u".hta",u".inf",u".ins",u".isp",u".its",u".js",u".jse",u".ksh",u".lnk",u".mad",u".maf",u".mag",u".mam",u".maq",u".mar",u".mas",u".mat",u".mau",u".mav",u".maw",
                u".mda",u".mdb",u".mde",u".mdt",u".mdw",u".mdz",u".msc",u".msh",u".msh1",u".msh2",u".mshxml",u".msh1xml",u".msh2xml",u".msi",u".msp",u".mst",u".ops",u".pcd",u".pif",u".plg",u".prf",u".prg",
                u".ps1",u".ps1xml",u".ps2",u".ps2xml",u".psc1",u".psc2",u".pst",u".reg",u".scf",u".scr",u".sct",u".shb",u".shs",u".tmp",u".url",u".vb",u".vbe",u".vbs",u".vsmacros",u".vsw",u".ws",u".wsc",u".wsf",u".wsh",u".xnk"};
            auto source=read(arg(0));if(source.empty())return done(0);auto folded=source;for(auto& c:folded)c=unicode_case_unit(c,false);
            if(std::find(std::begin(dangerous),std::end(dangerous),folded)!=std::end(dangerous))return done(1);
            GuestScratch temporary(*this,memory,state);const auto out=temporary.allocate(4),count=temporary.allocate(4);if(!out || !count)return done(0);memory.store(GuestAddress{count},32,4);
            if(query_data(0,5,source,u"",out,count))return done(0);return done((memory.load(GuestAddress{out},32)&0x20000U)?1U:0U);
        }
        if(name=="AssocCreate"){
            // (CLSID 傳值, riid, ppv)：只支援 CLSID_QueryAssociations，介面為 IQueryAssociations 或 IUnknown。
            constexpr std::array<std::uint8_t,16> clsid{0xfd,0x34,0x70,0xa0,0xaa,0x6c,0x54,0x49,0xac,0x3f,0x97,0xa2,0x72,0x16,0xf9,0x8a};
            constexpr std::array<std::uint8_t,16> iid{0x90,0xa5,0x6c,0xc4,0x3f,0x3c,0xd2,0x11,0xbe,0xe6,0x00,0x00,0xf8,0x05,0xca,0x57},unknown{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
            if(!arg(2))return done(invalid);memory.store(GuestAddress{arg(2)},bits,0);if(!arg(1))return done(invalid);
            std::array<std::uint8_t,16> requested{},interface_id{};memory.read(GuestAddress{arg(0)},requested);memory.read(GuestAddress{arg(1)},interface_id);
            if(requested!=clsid)return done(0x80040111U);if(interface_id!=iid && interface_id!=unknown)return done(0x80004002U);
            if(!query_associations_vtable_){std::vector<std::string_view> methods;for(const auto& entry:path_service_descriptors())if(entry.family=="internal-query-associations")methods.push_back(entry.symbol);
                const auto table=runtime_allocate(memory,{},methods.size()*unit);if(!table)return done(0x8007000eU);
                for(std::size_t i=0;i<methods.size();++i)memory.store(GuestAddress{table+i*unit},bits,resolve("internal-query-associations",methods[i]).value);query_associations_vtable_=table;}
            const auto instance=allocate_heap(memory,unit);if(!instance)return done(0x8007000eU);memory.store(GuestAddress{instance},bits,query_associations_vtable_);
            query_associations_.emplace(instance,QueryAssociationsObject{});memory.store(GuestAddress{arg(2)},bits,instance);return done(0);
        }
        if(object){
            const auto found=query_associations_.find(arg(0));if(found==query_associations_.end())return done(0x80010108U);auto& instance=found->second;
            if(name=="AddRef")return done(++instance.references);
            if(name=="Release"){if(--instance.references)return done(instance.references);query_associations_.erase(found);(void)free_heap(memory,arg(0),state);return done(0);}
            if(name=="QueryInterface"){
                constexpr std::array<std::uint8_t,16> iid{0x90,0xa5,0x6c,0xc4,0x3f,0x3c,0xd2,0x11,0xbe,0xe6,0x00,0x00,0xf8,0x05,0xca,0x57},unknown{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
                if(!arg(2))return done(0x80004003U);memory.store(GuestAddress{arg(2)},bits,0);if(!arg(1))return done(0x80004003U);
                std::array<std::uint8_t,16> requested{};memory.read(GuestAddress{arg(1)},requested);if(requested!=iid && requested!=unknown)return done(0x80004002U);
                ++instance.references;memory.store(GuestAddress{arg(2)},bits,arg(0));return done(0);
            }
            if(name=="Init"){
                // (ASSOCF 初始化旗標, pszAssoc, hkProgid, hwnd)：字串優先，否則以機碼路徑作為來源。
                std::u16string source=read(arg(2));if(source.empty() && arg(3)){if(const auto path=key_source(arg(3)))source=*path;}
                if(source.empty())return done(invalid);instance.source=std::move(source);instance.flags=static_cast<std::uint32_t>(arg(1));return done(0);
            }
            if(instance.source.empty())return done(0x8000ffffU); // E_UNEXPECTED：尚未 Init
            const auto flags=instance.flags|static_cast<std::uint32_t>(arg(1));
            if(name=="GetString"){if(!arg(5))return done(invalid);std::u16string value;const auto status=query_association(flags,static_cast<std::uint32_t>(arg(2)),instance.source,read(arg(3)),value);
                if(status)return done(status);return done(write_string(true,flags,value,arg(4),arg(5)));}
            if(name=="GetKey")return done(query_key(flags,static_cast<std::uint32_t>(arg(2)),instance.source,read(arg(3)),arg(4)));
            if(name=="GetData")return done(query_data(flags,static_cast<std::uint32_t>(arg(2)),instance.source,read(arg(3)),arg(4),arg(5)));
            if(name=="GetEnum"){if(arg(5))memory.store(GuestAddress{arg(5)},bits,0);return done(0x80004001U);} // Windows 亦未實作
        }
    }catch(const std::invalid_argument&){return done(invalid);}
    throw HostCallFault("未知的檔案關聯服務");
}
// 未指定搜尋路徑時的預設順序（SearchPath 與 RtlGetExePath 共用）：應用程式目錄、目前目錄（非安全模式時在前）、System32、System、
// Windows、目前目錄（安全模式時在後），共五項；有 PATH 環境變數時第六項是尚未拆開的 PATH。安全模式依 SetSearchPathMode，未設定時讀登錄 SafeProcessSearchMode。
std::uint32_t WindowsServices::default_search_paths(GuestMemory& memory,std::vector<std::u16string>& paths,std::size_t* current_index) {
    bool safe=(runtime_.search_path_mode&1)!=0;
    if(!runtime_.search_path_mode){
        if(!registry_file_.empty() && !runtime_.network.external_io)throw HostCallFault("比較模式不允許持久登錄檔 I/O");
        RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);
        if(transaction.error)return transaction.error;
        const auto key=runtime_.registry.keys.find(std::u16string(1,char16_t{2})+u"\\system\\currentcontrolset\\control\\session manager");
        if(key!=runtime_.registry.keys.end()){const auto value=key->second.find(u"safeprocesssearchmode");if(value!=key->second.end() && value->second.type==4 && value->second.bytes.size()==4)safe=value->second.bytes[0]==1 && !value->second.bytes[1] && !value->second.bytes[2] && !value->second.bytes[3];}
    }
    auto executable=runtime_.image.path;
    if(executable.empty()){const auto argv=memory.load(GuestAddress{runtime_.wargv_cell},x86_?32:64);executable=read_utf16(memory,GuestAddress{memory.load(GuestAddress{argv},x86_?32:64)});}
    executable=normalized_windows_path(executable);paths.push_back(executable.substr(0,executable.find_last_of(u'\\')+1));
    if(!safe){if(current_index)*current_index=paths.size();paths.push_back(runtime_.current_directory);}
    paths.insert(paths.end(),{u"C:\\Windows\\System32",u"C:\\Windows\\System",u"C:\\Windows"});
    if(safe){if(current_index)*current_index=paths.size();paths.push_back(runtime_.current_directory);}
    if(const auto env=runtime_.environment.find(u"PATH");env!=runtime_.environment.end())paths.push_back(env->second.second);
    return 0;
}

std::optional<std::uint32_t> WindowsServices::invoke_path(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    if(service.symbol.starts_with("Assoc") || service.family=="internal-query-associations")return invoke_association(state,memory,service);
    const auto symbol=service.symbol;
    if(service.family=="internal-shell-ole-string" || symbol=="ParseFieldW" || symbol.starts_with("PathCombine") || symbol.starts_with("PathAppend") || symbol=="PathCchRemoveFileSpec" || symbol.starts_with("PathRemoveFileSpec") ||
       symbol.starts_with("PathCchAddBackslash") || symbol.starts_with("PathCchRemoveBackslash") ||
       symbol.starts_with("PathAddBackslash") || symbol.starts_with("PathRemoveBackslash")){
        const WindowsCall call(state,memory,service);
        const ShellRootInput input{&memory,call(0),symbol.ends_with('A')?1U:2U,
            [](void* ctx,ShellRootAddress at,unsigned unit){
                return static_cast<unsigned>(static_cast<GuestMemory*>(ctx)->load(GuestAddress{at},static_cast<std::uint8_t>(unit*8)));
            }};
        const auto write=[](void* ctx,ShellRootAddress at,unsigned unit,unsigned ch){
            static_cast<GuestMemory*>(ctx)->store(GuestAddress{at},static_cast<std::uint8_t>(unit*8),ch);
        };
        if(service.family=="internal-shell-ole-string"){
            const bool bounded=symbol=="OleStrToStrN";
            try{
                call.return_to(state,shell_ole_string(&memory,call(0),bounded?static_cast<unsigned>(call(1)):UINT32_MAX,
                    call(bounded?2:1),input.read,write));
            }catch(const MemoryFault&){last_error_=87;call.return_to(state,0);}
            return std::nullopt;
        }
        if(symbol=="ParseFieldW"){
            call.return_to(state,shell_parse_field(&memory,call(0),static_cast<unsigned>(call(1)),
                call(2),static_cast<unsigned>(call(3)),input.read,write));
            return std::nullopt;
        }
        if(symbol.starts_with("PathCombine") || symbol.starts_with("PathAppend")){
            const bool append=symbol.starts_with("PathAppend");
            call.return_to(state,shell_path_join(input,call(append?0:1),call(append?1:2),append,write));
            return std::nullopt;
        }
        if(symbol=="PathCchRemoveFileSpec" || symbol.starts_with("PathRemoveFileSpec")){
            const bool cch=symbol=="PathCchRemoveFileSpec";
            call.return_to(state,shell_remove_file_spec(input,cch,cch?call(1):0,write));
            return std::nullopt;
        }
        const bool add=symbol.find("Add")!=symbol.npos;
        if(!symbol.starts_with("PathCch"))call.return_to(state,shell_legacy_end_slash(input,add,write));
        else{
            const bool ex=symbol.ends_with("Ex");
            const auto end_slot=ex?call(2):0,remaining_slot=ex?call(3):0;
            ShellRootAddress end=0,remaining=0;
            const auto status=shell_cch_end_slash(input,call(1),add,!ex || end_slot?&end:nullptr,!ex || remaining_slot?&remaining:nullptr,write);
            if(ex && (add || status!=SHELL_ROOT_INVALID || !call(1) || !end_slot || !remaining_slot)){
                if(end_slot)memory.store(GuestAddress{end_slot},x86_?32:64,end);
                if(remaining_slot)memory.store(GuestAddress{remaining_slot},x86_?32:64,remaining);
            }
            call.return_to(state,status);
        }
        return std::nullopt;
    }
    if(symbol.starts_with("PathStripToRoot") || symbol.starts_with("PathIsRoot") || symbol.starts_with("PathSkipRoot") ||
       symbol=="PathCchIsRoot" || symbol=="PathCchStripToRoot" || symbol=="PathCchSkipRoot"){
        const WindowsCall call(state,memory,service);
        const bool cch=symbol.starts_with("PathCch");
        const auto action=symbol.starts_with("PathStrip") || symbol=="PathCchStripToRoot"?ShellStripRoot:
            symbol=="PathCchSkipRoot"?ShellSkipCchRoot:symbol.starts_with("PathSkip")?ShellSkipLegacyRoot:ShellIsRoot;
        if(action==ShellSkipCchRoot && !call(1)){call.return_to(state,SHELL_ROOT_INVALID);return std::nullopt;}
        const ShellRootInput input{&memory,call(0),symbol.ends_with('A')?1U:2U,
            [](void* ctx,ShellRootAddress at,unsigned unit){
                return static_cast<unsigned>(static_cast<GuestMemory*>(ctx)->load(GuestAddress{at},static_cast<std::uint8_t>(unit*8)));
            }};
        auto value=shell_root_query(input,action,cch && action==ShellStripRoot?call(1):32768,
            [](void* ctx,ShellRootAddress at,unsigned unit,unsigned ch){
                static_cast<GuestMemory*>(ctx)->store(GuestAddress{at},static_cast<std::uint8_t>(unit*8),ch);
            });
        if(action==ShellSkipCchRoot){
            if(value!=SHELL_ROOT_KEEP_OUTPUT)memory.store(GuestAddress{call(1)},x86_?32:64,value);
            value=value && value!=SHELL_ROOT_KEEP_OUTPUT?0:SHELL_ROOT_INVALID;
        }else if(action==ShellStripRoot && !cch)value=value<=1;
        call.return_to(state,value);return std::nullopt;
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
            if(!path.empty()){const auto info=query_native_file_attributes(state,memory,path,false);if(!info.error)value=info.metadata.attributes&0x10U;}
        }}catch(const MemoryFault&){}catch(const std::invalid_argument&){}catch(const std::bad_alloc&){}
        last_error_=previous;
        arg.return_to(state,value);return std::nullopt;
    }
    const WindowsCall arg(state,memory,service);std::uint32_t result=0;
    try {
    if(service.symbol.ends_with("ExtensionA") || service.symbol.ends_with("ExtensionW") || service.family=="internal-shell-extension"){
        const auto name=service.symbol;
        const auto action=name.starts_with("PathFind")?ShellFindExtension:name.starts_with("PathGet")?ShellGetExtension:
            name.starts_with("PathRemove")?ShellRemoveExtension:name.starts_with("PathAdd")?ShellAddExtension:ShellRenameExtension;
        const auto ext=action==ShellAddExtension || action==ShellRenameExtension?arg(1):0;
        const auto value=shell_extension_text(&memory,arg(0),ext,name.ends_with('A')?1U:2U,action,
            [](void* ctx,unsigned long long at,unsigned unit)->unsigned{
                return static_cast<unsigned>(static_cast<GuestMemory*>(ctx)->load(GuestAddress{at},static_cast<std::uint8_t>(unit*8)));
            },
            [](void* ctx,unsigned long long at,unsigned unit,unsigned c){
                static_cast<GuestMemory*>(ctx)->store(GuestAddress{at},static_cast<std::uint8_t>(unit*8),c);
            });
        arg.return_to(state,value);return std::nullopt;
    }else if(service.symbol.starts_with("PathMatchSpec")){

        const bool wide=service.symbol.ends_with('W'),extended=service.symbol.starts_with("PathMatchSpecEx");const auto flags=extended?static_cast<std::uint32_t>(arg(2)):1U;
        if(!arg(0) || !arg(1) || (flags&~0x10001U))result=extended?invalid:0;
        else{
            const auto read=[&](unsigned i){return wide?read_utf16(memory,GuestAddress{arg(i)},32768):utf8_to_utf16(read_ansi(memory,arg(i),32768));};
            const auto file=read(0),spec=read(1);bool matched=false;
            for(std::size_t at=0;at<spec.size();){const auto end=(flags&1)?spec.find(u';',at):spec.npos;auto part=std::u16string_view(spec).substr(at,end==spec.npos?spec.size()-at:end-at);
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
                std::vector<std::u16string> defaults;
                if(const auto error=default_search_paths(memory,defaults)){last_error_=error;arg.return_to(state,0);return std::nullopt;}
                paths.insert(paths.end(),defaults.begin(),defaults.begin()+5);if(defaults.size()>5)append(defaults[5]);
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
    }else if(service.symbol=="PathCchFindExtension"){
        const auto capacity=arg(1);
        if(!arg(0) || !capacity || capacity>32768 || !arg(2))result=invalid;
        else{
            const auto path=read_utf16(memory,GuestAddress{arg(0)});
            if(path.size()>=capacity)result=invalid;
            else memory.store(GuestAddress{arg(2)},x86_?32:64,arg(0)+extension_at(path)*2);
        }
    }else if(service.symbol=="PathCchAddExtension" || service.symbol=="PathCchRemoveExtension" || service.symbol=="PathCchRenameExtension" || service.symbol=="PathCchStripPrefix"){
        // 就地修改，cch 規則同上。Add：已有副檔名 S_FALSE、空副檔名 S_OK 不變；Remove／StripPrefix：沒有可移除者 S_FALSE；
        // Rename：移除原副檔名後加上新的（"" 只移除）。副檔名可帶或不帶開頭的 '.'，含檔名非法字元或其他 '.' 為 E_INVALIDARG。
        const auto capacity=arg(1);const auto name=service.symbol;const bool takes_extension=name=="PathCchAddExtension" || name=="PathCchRenameExtension";
        if(!arg(0) || !capacity || capacity>32768 || (takes_extension && !arg(2)))result=invalid;
        else {
            auto path=read_utf16(memory,GuestAddress{arg(0)});std::u16string extension;
            if(path.size()>=capacity)result=invalid;
            if(!result && takes_extension){
                extension=read_utf16(memory,GuestAddress{arg(2)},32768);if(!extension.empty() && extension.front()==u'.')extension.erase(0,1);
                if(extension.find_first_of(u"\\/:*?\"<>|.")!=extension.npos)result=invalid;
            }
            if(!result){
                const auto at=extension_at(path);bool changed=false;
                if(name=="PathCchStripPrefix"){auto stripped=strip_extended(path);if(stripped!=path){path=std::move(stripped);changed=true;}}
                else if(name=="PathCchRemoveExtension"){if(at<path.size()){path.resize(at);changed=true;}}
                else if(name=="PathCchAddExtension"){if(at<path.size())result=1;else if(!extension.empty()){path+=u"."+extension;changed=true;}}
                else {if(at<path.size()){path.resize(at);changed=true;}if(!extension.empty()){path+=u"."+extension;changed=true;}}
                if(!result){
                    if(path.size()>=capacity)result=too_long;
                    else if(changed)memory.write(GuestAddress{arg(0)},utf16_bytes(path));
                    else if(name=="PathCchRemoveExtension" || name=="PathCchStripPrefix")result=1;
                }
            }
        }
    }else if(service.symbol=="PathCchAppend" || service.symbol=="PathCchAppendEx"){
        // UNC 與 \\?\ 開頭的 pszMore 視為完整路徑並取代原路徑；其餘去掉開頭的反斜線後接在後面（中間補一個反斜線），再與 PathCchCombineEx 同樣正規化。
        const auto capacity=arg(1);const auto flags=service.symbol.ends_with("Ex")?static_cast<std::uint32_t>(arg(3)):0U;
        if(!arg(0) || !capacity || capacity>32768)result=invalid;
        else {
            auto input=read_utf16(memory,GuestAddress{arg(0)});
            auto more=arg(2)?read_utf16(memory,GuestAddress{arg(2)}):std::u16string{};
            if(more.starts_with(u"\\") && !more.starts_with(u"\\\\"))more.erase(0,1);
            std::u16string output;
            result=canonicalize(combine_path(std::move(input),std::move(more)),flags,32768,output);
            if(!result && output.size()>=capacity)result=too_small;
            if(!result)memory.write(GuestAddress{arg(0)},utf16_bytes(output));
        }
    }else if(service.symbol=="PathAllocCanonicalize" || service.symbol=="PathAllocCombine"){
        // 結果以 LocalAlloc 配置（呼叫端 LocalFree）；未指定 PATHCCH_ALLOW_LONG_PATHS 時以 MAX_PATH 為上限。失敗時輸出為 NULL。
        const bool combine=service.symbol=="PathAllocCombine";const auto flags=static_cast<std::uint32_t>(arg(combine?2:1));const auto out=arg(combine?3:2);
        if(out && combine && !arg(0) && !arg(1))memory.store(GuestAddress{out},x86_?32:64,0);
        if(!out || (!combine && !arg(0)) || (combine && !arg(0) && !arg(1)))result=invalid;
        else {
            memory.store(GuestAddress{out},x86_?32:64,0);
            auto input=arg(0)?read_utf16(memory,GuestAddress{arg(0)}):std::u16string{};
            if(combine)input=combine_path(std::move(input),arg(1)?read_utf16(memory,GuestAddress{arg(1)}):std::u16string{});
            std::u16string output;result=canonicalize(std::move(input),flags,32768,output);
            if(!result){
                const auto bytes=utf16_bytes(output);const auto block=nested_service(state,memory,"kernel32.dll","LocalAlloc",{0,bytes.size()});
                if(!block)result=0x8007000eU;else {memory.write(GuestAddress{block},bytes);memory.store(GuestAddress{out},x86_?32:64,block);}
            }
        }
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
            std::u16string output;result=combine && !arg(2) && !arg(3)?invalid:canonicalize(std::move(input),flags,combine?32768:static_cast<std::size_t>(capacity),output);
            if(!result && output.size()>=capacity)result=too_small;
            if(result)memory.store(GuestAddress{arg(0)},16,0);
            else memory.write(GuestAddress{arg(0)},utf16_bytes(output));
        }
    }
    } catch(const std::invalid_argument&) {
        if(service.symbol.starts_with("PathAddExtension") || service.symbol.starts_with("PathRemoveExtension") || service.symbol.starts_with("PathRenameExtension"))result=0;
        else if(service.symbol.starts_with("SearchPath")){last_error_=123;result=0;}
        else if(service.symbol.starts_with("PathMatchSpec"))result=service.symbol.starts_with("PathMatchSpecEx")?invalid:0;
        else{result=too_long;if(service.symbol!="PathCchSkipRoot" && arg(0) && arg(1) && arg(1)<=32768)memory.store(GuestAddress{arg(0)},16,0);}
    }
    arg.return_to(state,result);return {};
}
// shlwapi 路徑補充家族：與上方路徑服務共用 root_end／combine_path／canonicalize。A 版沿用本檔慣例以位元組為碼元
// （ACP＝UTF-8，路徑分隔字元皆為 ASCII，不會落在多位元組字元中段）；W 版以 UTF-16 碼元處理。
std::span<const WindowsServiceDescriptor> path_extra_service_descriptors() {
    static constexpr WindowsServiceDescriptor table[]{
#define X64ARM_PATH_PAIR(name,count) {"shlwapi.dll",name "A",count},{"shlwapi.dll",name "W",count}
        X64ARM_PATH_PAIR("PathGetDriveNumber",1),
        {"pathcch.dll","PathIsUNCEx",2},X64ARM_PATH_PAIR("PathIsRelative",1),X64ARM_PATH_PAIR("PathIsUNC",1),
        X64ARM_PATH_PAIR("PathIsUNCServer",1),X64ARM_PATH_PAIR("PathIsUNCServerShare",1),
        X64ARM_PATH_PAIR("PathIsPrefix",2),X64ARM_PATH_PAIR("PathCommonPrefix",3),X64ARM_PATH_PAIR("PathIsSameRoot",2),X64ARM_PATH_PAIR("PathIsFileSpec",1),
        {"shell32.dll","SheRemoveQuotesA",1},{"shell32.dll","SheRemoveQuotesW",1},
        X64ARM_PATH_PAIR("PathFindFileName",1),
        X64ARM_PATH_PAIR("PathStripPath",1),X64ARM_PATH_PAIR("PathRemoveBlanks",1),X64ARM_PATH_PAIR("PathQuoteSpaces",1),X64ARM_PATH_PAIR("PathGetArgs",1),
        X64ARM_PATH_PAIR("PathRemoveArgs",1),X64ARM_PATH_PAIR("PathIsURL",1),X64ARM_PATH_PAIR("PathFindSuffixArray",3),X64ARM_PATH_PAIR("PathRelativePathTo",5),
        X64ARM_PATH_PAIR("PathParseIconLocation",1),X64ARM_PATH_PAIR("PathIsNetworkPath",1),X64ARM_PATH_PAIR("PathIsLFNFileSpec",1),X64ARM_PATH_PAIR("PathMakePretty",1),
        {"shlwapi.dll","#440",3,0,0,4},{"shlwapi.dll","#441",3,0,0,4},
        {"internal-shell-path-search","PathFileExistsDefExtA",2},
        {"internal-shell-path-search","PathFileExistsDefExtW",2},
        {"internal-shell-path-search","PathFindOnPathExA",3},
        {"internal-shell-path-search","PathFindOnPathExW",3},
        {"internal-shell-path-search","PathFileExistsAndAttributesA",2},
        {"internal-shell-path-search","PathFileExistsAndAttributesW",2},
        {"internal-shell-path-search","PathFileExistsDefExtAndAttributesW",3},
        X64ARM_PATH_PAIR("PathIsDirectoryEmpty",1),X64ARM_PATH_PAIR("PathFindOnPath",2),
        {"shlwapi.dll","PathBuildRootA",2,0,0,0x2},{"shlwapi.dll","PathBuildRootW",2,0,0,0x2},
        X64ARM_PATH_PAIR("PathUnquoteSpaces",1),X64ARM_PATH_PAIR("PathUndecorate",1),X64ARM_PATH_PAIR("PathIsContentType",2),
        X64ARM_PATH_PAIR("PathMakeSystemFolder",1),X64ARM_PATH_PAIR("PathUnmakeSystemFolder",1),
        {"shlwapi.dll","PathIsSystemFolderA",2,0,0,0x2},{"shlwapi.dll","PathIsSystemFolderW",2,0,0,0x2},
        {"shlwapi.dll","PathCompactPathExA",4,0,0,0xc},{"shlwapi.dll","PathCompactPathExW",4,0,0,0xc},{"shlwapi.dll","PathCompactPathA",3,0,0,0x4},{"shlwapi.dll","PathCompactPathW",3,0,0,0x4},
        {"shlwapi.dll","PathSearchAndQualifyA",3,0,0,0x4},{"shlwapi.dll","PathSearchAndQualifyW",3,0,0,0x4},
        {"shlwapi.dll","PathSetDlgItemPathA",3,0,0,0x2},{"shlwapi.dll","PathSetDlgItemPathW",3,0,0,0x2},
#undef X64ARM_PATH_PAIR
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_path_extra(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const bool wide=name.ends_with('W') || name=="#441";const unsigned unit=wide?2U:1U;
    const unsigned pointer_size=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(pointer_size*8);
    const auto done=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    // 讀到 NUL 為止（最多 MAX_PATH 以外的長字串亦可），A 版每個位元組一個碼元。
    const auto read=[&](std::uint64_t at)->std::u16string{
        std::u16string text;if(!at)return text;
        for(std::size_t i=0;i<32768;++i){const auto c=static_cast<char16_t>(memory.load(GuestAddress{at+i*unit},static_cast<std::uint8_t>(unit*8)));if(!c)break;text.push_back(c);}
        return text;};
    const auto write=[&](std::uint64_t at,std::u16string_view text){
        for(std::size_t i=0;i<text.size();++i)memory.store(GuestAddress{at+i*unit},static_cast<std::uint8_t>(unit*8),text[i]);
        memory.store(GuestAddress{at+text.size()*unit},static_cast<std::uint8_t>(unit*8),0);};
    const auto fold=[](char16_t c){return c>=u'a' && c<=u'z'?static_cast<char16_t>(c-32):c;};
    // A 版碼元（位元組）與 UTF-16 互轉；需要交給宿主 W 服務或量測字元時使用，避免切開 UTF-8 字元。
    const auto to_wide=[&](std::u16string text)->std::u16string{if(wide)return text;std::string bytes(text.begin(),text.end());return utf8_to_utf16_lossy(bytes);};
    const auto from_wide=[&](const std::u16string& text)->std::u16string{if(wide)return text;const auto bytes=utf16_to_utf8_lossy(text);std::u16string units;for(const auto c:bytes)units.push_back(static_cast<unsigned char>(c));return units;};
    const auto same_text=[&](std::u16string_view a,std::u16string_view b){
        if(a.size()!=b.size())return false;for(std::size_t i=0;i<a.size();++i)if(fold(a[i])!=fold(b[i]))return false;return true;};
    const auto host=[&](std::string_view symbol,std::initializer_list<std::uint64_t> args){return nested_service(state,memory,"kernel32.dll",symbol,args);};
    // 兩條路徑以元件為單位的共同前綴長度（不分大小寫）；根（"C:\"、"\\server\share"）整體視為一個元件。
    const auto common_prefix=[&](std::u16string_view a,std::u16string_view b)->std::size_t{
        const auto root_a=root_end(a),root_b=root_end(b);
        if(root_a.value_or(0)!=root_b.value_or(0) || !same_text(a.substr(0,root_a.value_or(0)),b.substr(0,root_b.value_or(0))))return 0;
        std::size_t common=root_a.value_or(0),at=common;
        while(at<a.size() && at<b.size() && fold(a[at])==fold(b[at])){
            ++at;
            const bool end_a=at==a.size() || a[at]==u'\\',end_b=at==b.size() || b[at]==u'\\';
            if(end_a && end_b)common=at;
        }
        return common;
    };
    try{
        if(name=="#440" || name=="#441"){
            constexpr auto failure=0x80004005U;
            const auto capacity=static_cast<std::uint32_t>(arg(2));
            if(!arg(0) || !arg(1) || !capacity)return done(failure);
            const auto file=to_wide(read(arg(0)));
            const std::u16string base=std::u16string(windows_directory)+u"\\Web\\";
            const auto fallback=from_wide(base+file);
            if(fallback.size()>=capacity){
                const auto directory=from_wide(windows_directory);
                if(directory.size()<capacity)write(arg(1),directory);
                return done(failure);
            }
            GuestScratch scratch(*this,memory,state);
            const auto exists=[&](const std::u16string& candidate){
                const auto path=scratch.text(candidate);
                return path && nested_service(state,memory,"shlwapi.dll","PathFileExistsW",{path});
            };
            const auto user=host("GetUserDefaultUILanguage",{});
            if(user!=host("GetSystemDefaultUILanguage",{})){
                std::u16string local=base+u"mui\\";
                for(int shift=12;shift>=0;shift-=4)local+=u"0123456789abcdef"[(user>>shift)&15U];
                local+=u'\\';
                local+=file;
                const auto encoded=from_wide(local);
                if(encoded.size()<capacity && exists(local)){
                    write(arg(1),encoded);
                    return done(0);
                }
            }
            write(arg(1),fallback);
            return done(exists(base+file)?0U:failure);
        }
        if(name.starts_with("PathBuildRoot")){
            return done(shell_build_drive_root(&memory,arg(0),unit,static_cast<std::uint32_t>(arg(1)),
                [](void* ctx,unsigned long long at,unsigned width,unsigned value){
                    static_cast<GuestMemory*>(ctx)->store(GuestAddress{at},static_cast<std::uint8_t>(width*8),value);
                }));
        }
        if(name.starts_with("PathGetDriveNumber")){
            const int number=shell_drive_number(&memory,arg(0),unit,
                [](void* ctx,unsigned long long at,unsigned width)->unsigned{
                    return static_cast<unsigned>(static_cast<GuestMemory*>(ctx)->load(GuestAddress{at},static_cast<std::uint8_t>(width*8)));
                });
            return done(static_cast<std::uint32_t>(number));
        }
        if(name.starts_with("PathIsUNC") || name.starts_with("PathIsRelative")){
            const auto query=name=="PathIsUNCEx"?ShellUNCServerAddress:name.starts_with("PathIsRelative")?ShellRelative:
                name.starts_with("PathIsUNCServerShare")?ShellShare:name.starts_with("PathIsUNCServer")?ShellServer:ShellUNC;
            const auto value=shell_unc_query(&memory,arg(0),name.ends_with('A')?1U:2U,query,
                [](void* ctx,unsigned long long at,unsigned width)->unsigned{
                    return static_cast<unsigned>(static_cast<GuestMemory*>(ctx)->load(GuestAddress{at},static_cast<std::uint8_t>(width*8)));
                });
            if(query==ShellUNCServerAddress && arg(1))memory.store(GuestAddress{arg(1)},bits,value);
            return done(value!=0);
        }
        if(name.starts_with("PathCommonPrefix") || name.starts_with("PathIsPrefix")){
            const bool is_prefix=name.starts_with("PathIsPrefix");const auto a=read(arg(0)),b=read(arg(1));
            if(!arg(0) || !arg(1))return done(0);
            const auto common=common_prefix(a,b);
            if(is_prefix)return done(common && (common==a.size() || (common+1==a.size() && a.back()==u'\\'))?1U:0U);
            if(arg(2))write(arg(2),a.substr(0,common));return done(common);
        }
        if(name.starts_with("PathIsSameRoot")){
            const auto a=read(arg(0)),b=read(arg(1));const auto ra=root_end(a),rb=root_end(b);
            return done(ra && rb && same_text(std::u16string_view(a).substr(0,*ra),std::u16string_view(b).substr(0,*rb))?1U:0U);
        }
        if(name.starts_with("PathIsFileSpec")){const auto path=read(arg(0));return done(path.find_first_of(u"\\:")==path.npos?1U:0U);}
        if(name.starts_with("PathFindFileName") || name.starts_with("PathStripPath")){
            return done(shell_file_name(&memory,arg(0),unit,name.starts_with("PathStripPath"),
                [](void* ctx,unsigned long long at,unsigned width)->unsigned{
                    return static_cast<unsigned>(static_cast<GuestMemory*>(ctx)->load(GuestAddress{at},static_cast<std::uint8_t>(width*8)));
                },
                [](void* ctx,unsigned long long at,unsigned width,unsigned c){
                    static_cast<GuestMemory*>(ctx)->store(GuestAddress{at},static_cast<std::uint8_t>(width*8),c);
                }));
        }
        if(name.starts_with("PathRemoveBlanks") || name.starts_with("PathParseIconLocation")){
            const auto load=[](void* ctx,ShellQuoteAddress at,unsigned width)->unsigned{
                return static_cast<unsigned>(static_cast<GuestMemory*>(ctx)->load(GuestAddress{at},static_cast<std::uint8_t>(width*8)));
            };
            const auto store=[](void* ctx,ShellQuoteAddress at,unsigned width,unsigned value){
                static_cast<GuestMemory*>(ctx)->store(GuestAddress{at},static_cast<std::uint8_t>(width*8),value);
            };
            if(name.starts_with("PathParseIconLocation"))return done(shell_parse_icon_location(&memory,arg(0),unit,load,store));
            shell_remove_blanks(&memory,arg(0),unit,load,store);
            return done(0);
        }
        if(name.starts_with("SheRemoveQuotes") || name.starts_with("PathQuoteSpaces") || name.starts_with("PathUnquoteSpaces") ||
           name.starts_with("PathGetArgs") || name.starts_with("PathRemoveArgs")){
            const auto action=name.starts_with("PathGetArgs")?ShellFindArguments:name.starts_with("PathRemoveArgs")?ShellRemoveArguments:
                              name.starts_with("SheRemoveQuotes")?ShellExtractQuoted:
                              name.starts_with("PathUnquoteSpaces")?ShellUnquoteSpaces:ShellQuoteSpaces;
            return done(shell_quote_text(&memory,arg(0),unit,action,
                [](void* ctx,ShellQuoteAddress at,unsigned width)->unsigned{
                    return static_cast<unsigned>(static_cast<GuestMemory*>(ctx)->load(GuestAddress{at},static_cast<std::uint8_t>(width*8)));
                },
                [](void* ctx,ShellQuoteAddress at,unsigned width,unsigned value){
                    static_cast<GuestMemory*>(ctx)->store(GuestAddress{at},static_cast<std::uint8_t>(width*8),value);
                }));
        }
        if(name.starts_with("PathIsURL")){
            const auto path=read(arg(0));const auto colon=path.find(u':');if(colon==path.npos || colon<2)return done(0);
            for(std::size_t i=0;i<colon;++i){const auto c=path[i];
                if(!((c>=u'a' && c<=u'z') || (c>=u'A' && c<=u'Z') || (i && ((c>=u'0' && c<=u'9') || c==u'+' || c==u'-' || c==u'.'))))return done(0);}
            return done(1);
        }
        if(name.starts_with("PathFindSuffixArray")){
            // (Path, Suffixes[], Count)：不分大小寫比對結尾，回傳相符的字尾字串指標。
            const auto path=read(arg(0));const auto count=static_cast<std::int32_t>(static_cast<std::uint32_t>(arg(2)));
            for(std::int32_t i=0;i<count;++i){const auto item=memory.load(GuestAddress{arg(1)+static_cast<std::uint64_t>(i)*pointer_size},bits);const auto suffix=read(item);
                if(!suffix.empty() && suffix.size()<=path.size() && same_text(std::u16string_view(path).substr(path.size()-suffix.size()),suffix))return done(item);}
            return done(0);
        }
        if(name.starts_with("PathRelativePathTo")){
            // (Out, From, FromAttributes, To, ToAttributes)：From 不是目錄時去掉檔名；沒有共同根時回 FALSE。
            if(!arg(0) || !arg(1) || !arg(3))return done(0);
            auto from=read(arg(1));const auto to=read(arg(3));
            if(!(static_cast<std::uint32_t>(arg(2))&0x10U)){const auto slash=from.find_last_of(u'\\');const auto root=root_end(from).value_or(0);from.resize(std::max(root,slash==from.npos?std::size_t{0}:slash));}
            while(from.size()>root_end(from).value_or(0) && from.back()==u'\\')from.pop_back();
            const auto common=common_prefix(from,to);if(!common)return done(0);
            std::u16string result;std::size_t ups=0;
            // 共同前綴之後 From 剩下的非空元件數，即需要的 ".." 數。
            for(std::size_t at=common;at<from.size();){
                while(at<from.size() && from[at]==u'\\')++at;if(at>=from.size())break;
                ++ups;while(at<from.size() && from[at]!=u'\\')++at;
            }
            for(std::size_t i=0;i<ups;++i)result+=i?u"\\..":u"..";
            auto rest=to.substr(common);if(!rest.empty() && rest.front()!=u'\\')rest.insert(rest.begin(),u'\\');
            result=ups?result+rest:u"."+rest;if(result.size()>=260)return done(0);
            write(arg(0),result);return done(1);
        }
        if(name.starts_with("PathIsNetworkPath")){
            const auto path=read(arg(0));if(path.starts_with(u"\\\\"))return done(1);if(!drive(path))return done(0);
            GuestScratch temporary(*this,memory,state);const auto text=temporary.text(std::u16string{path[0],u':',u'\\'});if(!text)return done(0);
            return done(host("GetDriveTypeW",{text})==4?1U:0U); // DRIVE_REMOTE
        }
        if(name.starts_with("PathIsLFNFileSpec")){
            // 不符合 8.3（主檔名 1～8、副檔名 0～3、無空白、至多一個點）即為長檔名。
            const auto path=read(arg(0));const auto dot=path.find(u'.');const auto base=path.substr(0,dot);
            const auto extension=dot==path.npos?std::u16string{}:path.substr(dot+1);
            return done(base.empty() || base.size()>8 || extension.size()>3 || path.find(u' ')!=path.npos || extension.find(u'.')!=extension.npos?1U:0U);
        }
        if(name.starts_with("PathMakePretty")){
            // 全部大寫時，第一個字元以外改成小寫；回傳是否變更。
            auto path=read(arg(0));if(path.empty() || std::any_of(path.begin(),path.end(),[](char16_t c){return c>=u'a' && c<=u'z';}))return done(0);
            for(std::size_t i=1;i<path.size();++i)if(path[i]>=u'A' && path[i]<=u'Z')path[i]=static_cast<char16_t>(path[i]+32);
            write(arg(0),path);return done(1);
        }
#include "windows_shell_path_search.inc"
        if(name.starts_with("PathIsDirectoryEmpty")){
            if(!arg(0))return done(0);
            const auto path=to_wide(read(arg(0)));
            // 與 Shell 的 MAX_PATH 列舉契約一致，預留反斜線、*.* 及 NUL。
            if(path.empty() || path.size()+(path.back()==u'\\'?0U:1U)>256)return done(0);
            const auto info=query_native_file_attributes(state,memory,path,false);
            if(info.error || !(info.metadata.attributes&0x10U))return done(0);
            const auto listing=filesystem_.list(utf16_to_utf8(path));
            if(listing.error){last_error_=listing.error;return done(0);}
            return done(listing.entries.empty());
        }
        if(name.starts_with("PathUndecorate")){
            // 移除檔名中副檔名前的 "[數字]" 副本編號，例如 "a[2].txt" → "a.txt"；"[" 前必須還有主檔名。
            if(!arg(0))return done(0);auto path=read(arg(0));const auto start=path.find_last_of(u"\\/:");const auto base=start==path.npos?0:start+1;
            auto dot=path.rfind(u'.');if(dot==path.npos || dot<base)dot=path.size();
            if(dot>base && path[dot-1]==u']'){auto open=dot-1;while(open>base && path[open-1]>=u'0' && path[open-1]<=u'9')--open;
                if(open>base+1 && open<dot-1 && path[open-1]==u'['){path.erase(open-1,dot-open+1);write(arg(0),path);}}
            return done(0);
        }
        if(name.starts_with("PathCompactPathEx")){
            // (out, path, cchMax, flags)：寬度為字元數（含 NUL；A 為 UTF-8 位元組），flags 必須為 0。
            if(!arg(0) || !arg(1) || static_cast<std::uint32_t>(arg(3)))return done(0);const auto cch=static_cast<std::uint32_t>(arg(2));if(!cch || cch>32768)return done(0);
            const auto units=[&](std::u16string_view text){return static_cast<std::int32_t>(wide?text.size():utf16_to_utf8_lossy(std::u16string(text)).size());};
            const auto limit=static_cast<std::int32_t>(cch-1);auto result=compact_path_text(to_wide(read(arg(1))),limit,units);
            while(!result.empty() && units(result)>limit){result.pop_back();if(!result.empty() && result.back()>=0xd800 && result.back()<=0xdbff)result.pop_back();}
            write(arg(0),from_wide(result));return done(1);
        }
        if(name.starts_with("PathCompactPath")){
            // (HDC, path 輸入輸出, 像素寬度)：以 DC 目前字型量測；HDC 為 NULL 時使用螢幕 DC。結果仍放不下時回 FALSE。
            if(!arg(1))return done(0);auto dc=arg(0);const bool screen=!dc;
            if(screen){dc=nested_service(state,memory,"user32.dll","GetDC",{0});if(x86_)dc&=UINT32_MAX;}
            const auto path=to_wide(read(arg(1)));const auto result=compact_path_for_dc(dc,path,static_cast<std::int32_t>(static_cast<std::uint32_t>(arg(2))));
            if(screen && dc)(void)nested_service(state,memory,"user32.dll","ReleaseDC",{0,dc});
            if(result.first!=path)write(arg(1),from_wide(result.first));return done(result.second?1U:0U);
        }
        if(name.starts_with("PathSearchAndQualify")){
            // 不含路徑分隔字元的名稱先以 SearchPath 尋找，找不到或含路徑時以 GetFullPathName 取完整路徑。
            if(!arg(0) || !arg(1))return done(0);const auto cch=static_cast<std::uint32_t>(arg(2));if(!cch)return done(0);
            GuestScratch temporary(*this,memory,state);const auto path=to_wide(read(arg(0)));const auto source=temporary.text(path),output=temporary.allocate(2*32768);if(!source || !output)return done(0);
            std::uint64_t length=0;if(path.find_first_of(u"\\/:")==path.npos)length=host("SearchPathW",{0,source,0,32767,output,0})&UINT32_MAX;
            if(!length || length>=32767)length=host("GetFullPathNameW",{source,32767,output,0})&UINT32_MAX;
            if(!length || length>=32767)return done(0);const auto full=from_wide(read_utf16(memory,GuestAddress{output},32767));
            if(full.size()>=cch)return done(0);write(arg(1),full);return done(1);
        }
        if(name.starts_with("PathIsContentType")){
            // 副檔名在 HKEY_CLASSES_ROOT 的 "Content Type" 值與指定類型相同（不分大小寫）。
            if(!arg(0) || !arg(1))return done(0);const auto path=to_wide(read(arg(0))),wanted=to_wide(read(arg(1)));
            const auto dot=path.rfind(u'.'),slash=path.find_last_of(u"\\/");if(dot==path.npos || (slash!=path.npos && dot<slash))return done(0);
            GuestScratch temporary(*this,memory,state);const auto key=temporary.text(path.substr(dot)),value=temporary.text(u"Content Type"),data=temporary.allocate(1024),size=temporary.allocate(4);
            if(!key || !value || !data || !size)return done(0);memory.store(GuestAddress{size},32,1024);
            const std::uint64_t classes_root=x86_?0x80000000ULL:0xffffffff80000000ULL;
            if(nested_service(state,memory,"advapi32.dll","RegGetValueW",{classes_root,key,value,2,0,data,size})&UINT32_MAX)return done(0); // RRF_RT_REG_SZ
            return done(same_text(read_utf16(memory,GuestAddress{data},511),wanted)?1U:0U);
        }
        if(name.starts_with("PathIsSystemFolder") || name.starts_with("PathMakeSystemFolder") || name.starts_with("PathUnmakeSystemFolder")){
            // 系統資料夾＝目錄且具 SYSTEM 或 READONLY 屬性。IsSystemFolder 的路徑為空時改用傳入的屬性；Make 加上 SYSTEM，Unmake 移除 SYSTEM 與 READONLY。
            const bool query=name.starts_with("PathIsSystemFolder");auto attributes=query?static_cast<std::uint32_t>(arg(1)):0U;
            GuestScratch temporary(*this,memory,state);const auto path=arg(0)?to_wide(read(arg(0))):std::u16string{};std::uint64_t text=0;
            if(!path.empty()){text=temporary.text(path);if(!text)return done(0);attributes=static_cast<std::uint32_t>(host("GetFileAttributesW",{text}));if(attributes==UINT32_MAX)return done(0);}
            else if(!query)return done(0);
            if(query)return done((attributes&0x10U) && (attributes&0x5U)?1U:0U);if(!(attributes&0x10U))return done(0);
            auto updated=(name.starts_with("PathMake")?attributes|0x4U:attributes&~0x5U)&~0x10U;if(!updated)updated=0x80U;
            return done(host("SetFileAttributesW",{text,updated})&UINT32_MAX?1U:0U);
        }
        if(name.starts_with("PathSetDlgItemPath")){
            // 依控制項用戶區寬度與字型省略路徑後設定文字（NULL 路徑設為空字串）；WM_GETFONT 與 SetWindowText 可能回呼客體視窗程序，經 nested_continuation 續行。
            return nested_continuation(state,memory,"PathSetDlgItemPath",[this,service,wide](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall call(s,m,service);
                const auto dialog=call(0),id=call(1),source=call(2);
                const auto entry=[&](std::string_view dll,std::string_view target){const auto stub=host_service_stub(dll,target);return stub.value?stub.value:resolve(dll,target).value;};
                const auto finish=[&]{call.return_to(s,std::uint64_t{0});return std::optional<std::uint32_t>{};};
                const auto handle=[&](std::uint64_t value){return x86_?value&UINT32_MAX:value;};
                const auto control=handle(co_await service_callback::call_value(entry("user32.dll","GetDlgItem"),{dialog,id}));if(!control)co_return finish();
                std::u16string text;
                if(source && wide){
                    for(unsigned i=0;i<260;++i){
                        const auto c=static_cast<char16_t>(m.load(GuestAddress{source+i*2U},16));
                        if(!c)break;
                        text.push_back(c);
                    }
                }else if(source){
                    text=utf8_to_utf16_lossy(read_ansi(m,source));
                    if(text.size()>=260)text.resize(259);
                }
                GuestScratch temporary(*this,m,s);const auto rect=temporary.allocate(16);if(!rect)co_return finish();
                (void)co_await service_callback::call(entry("user32.dll","GetClientRect"),{control,rect});
                const auto width=static_cast<std::int32_t>(static_cast<std::uint32_t>(m.load(GuestAddress{rect+8},32)))-static_cast<std::int32_t>(static_cast<std::uint32_t>(m.load(GuestAddress{rect},32)));
                const auto dc=handle(co_await service_callback::call_value(entry("user32.dll","GetDC"),{dialog}));
                if(dc){
                    const auto font=handle(co_await service_callback::call_value(entry("user32.dll","SendMessageW"),{control,0x31,0,0}));
                    const auto previous=handle(co_await service_callback::call_value(entry("gdi32.dll","SelectObject"),{dc,font}));
                    if(previous){
                        text=compact_path_for_dc(dc,text,width).first;
                        (void)co_await service_callback::call_value(entry("gdi32.dll","SelectObject"),{dc,previous});
                    }
                    (void)co_await service_callback::call(entry("user32.dll","ReleaseDC"),{dialog,dc});
                }
                const auto value=temporary.text(text);if(value)(void)co_await service_callback::call(entry("user32.dll","SetWindowTextW"),{control,value});co_return finish();
            });
        }
    }catch(const std::invalid_argument&){return done(0);}
    throw HostCallFault("未知的 shlwapi 路徑補充服務");
}
}
