#include "x64arm/windows_services.hpp"
#include "x64arm/windows_paths.hpp"
#include "x64arm/unicode.hpp"
#include "setup_driver_store.hpp"
#include "windows_sddl.hpp"
#include "x64arm/security_access.hpp"
#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <memory>
#include <optional>

// SetupAPI 的 INF 解析家族：以宿主原生解析器保存 HINF（可附加多個 INF），INFCONTEXT 為 {Inf, CurrentInf, Section, Line}。
// 欄位讀取時才做 %strkey%（[Strings] 與 [Strings.0009]／[Strings.0409] 覆寫）與 %dirid% 代換；引號內的文字不代換。
namespace x64arm {
struct SetupInfState {
    struct Part {std::u16string text;bool quoted{};};
    using Field=std::vector<Part>;
    struct Line {Field key;bool keyed{};std::vector<Field> fields;};
    struct Section {std::u16string name;std::vector<Line> lines;};
    struct File {std::u16string path;std::uint32_t style{};std::map<std::u16string,Section> sections;std::vector<std::u16string> order;};
    struct Handle {std::vector<File> files;std::map<std::u16string,std::uint32_t> section_ids;std::vector<std::u16string> section_names;};
    std::map<std::uint64_t,Handle> handles;
    // 字串表（windows_setup_string_table.inc）：handle 為程序堆積位址；Entry 保存字串、客體堆積複本與附加資料。
    struct StringTable {struct Entry {std::u16string text;std::uint64_t guest{};std::vector<std::uint8_t> extra;};std::uint32_t max_extra{};std::vector<Entry> entries;};
    std::map<std::uint64_t,StringTable> string_tables;
    std::uint32_t global_flags{};
    // 磁碟空間清單、安裝記錄檔與檔案記錄（windows_setup_disk_log.inc）；handle 為程序堆積位址。
    struct DiskSpace {struct File {std::u16string path;std::int64_t size{};std::uint32_t operation{};};std::uint32_t flags{};std::map<std::u16string,File> files;};
    std::map<std::uint64_t,DiskSpace> disk_spaces;
    struct FileLog {struct Entry {std::u16string target;std::vector<std::u16string> fields;};struct Section {std::u16string name;std::vector<Entry> entries;};
        std::u16string path;std::uint32_t flags{};std::vector<Section> sections;};
    std::map<std::uint64_t,FileLog> file_logs;
    bool log_open{};
};
namespace {
using driver_store::fold;
constexpr std::uint32_t line_not_found=0xe0000102U,section_not_found=0xe0000101U,wrong_style=0xe0000100U,class_mismatch=0xe0000201U,bad_section_line=0xe0000001U,section_too_long=0xe0000002U;
struct Failure {std::uint32_t code;};
using State=SetupInfState;
std::u16string decode(std::span<const std::uint8_t> bytes){
    std::u16string text;
    if(bytes.size()>=2 && bytes[0]==255 && bytes[1]==254){for(std::size_t i=2;i+1<bytes.size();i+=2)text+=static_cast<char16_t>(bytes[i]|(std::uint16_t{bytes[i+1]}<<8));return text;}
    const auto skip=bytes.size()>=3 && bytes[0]==239 && bytes[1]==187 && bytes[2]==191?3U:0U;
    const std::string encoded(reinterpret_cast<const char*>(bytes.data()+skip),bytes.size()-skip);
    try{return utf8_to_utf16(encoded);}catch(const std::invalid_argument&){return cp1252_to_utf16(encoded);}
}
// 解析一行的欄位：逗號分隔、引號內保留逗號與空白（"" 為字面引號），欄位前後空白去除。
std::vector<State::Field> split_fields(std::u16string_view text){
    std::vector<State::Field> fields(1);bool quoted=false;std::u16string pending;
    const auto flush=[&](bool in_quotes){if(!pending.empty())fields.back().push_back({std::move(pending),in_quotes});pending.clear();};
    for(std::size_t i=0;i<text.size();++i){
        const auto c=text[i];
        if(c==u'"'){if(quoted && i+1<text.size() && text[i+1]==u'"'){pending+=c;++i;continue;}flush(quoted);quoted=!quoted;fields.back().push_back({{},true});continue;}
        if(c==u',' && !quoted){flush(false);fields.emplace_back();continue;}
        pending+=c;
    }
    flush(quoted);
    for(auto& field:fields){
        std::erase_if(field,[](const auto& part){return part.text.empty() && !part.quoted;});
        if(!field.empty() && !field.front().quoted){auto& t=field.front().text;t.erase(0,std::min(t.size(),t.find_first_not_of(u" \t")));}
        if(!field.empty() && !field.back().quoted){auto& t=field.back().text;const auto end=t.find_last_not_of(u" \t");t.resize(end==t.npos?0:end+1);}
        std::erase_if(field,[](const auto& part){return part.text.empty() && !part.quoted;});
    }
    return fields;
}
State::File parse_file(std::u16string path,std::span<const std::uint8_t> bytes,std::uint32_t* error_line){
    State::File file;file.path=std::move(path);const auto text=decode(bytes);std::u16string current,pending;unsigned number=0,continued_from=0;
    for(std::size_t at=0;at<=text.size();){
        const auto stop=text.find(u'\n',at),end=stop==text.npos?text.size():stop;auto line=text.substr(at,end-at);at=end+1;++number;
        if(!line.empty() && line.back()==u'\r')line.pop_back();
        bool quote=false;for(std::size_t i=0;i<line.size();++i){if(line[i]==u'"')quote=!quote;else if(line[i]==u';' && !quote){line.resize(i);break;}}
        line=driver_store::trim(std::move(line));
        if(!line.empty() && line.back()==u'\\'){line.pop_back();if(pending.empty())continued_from=number;pending+=line;if(stop!=text.npos)continue;line.clear();}
        if(!pending.empty()){line=pending+line;pending.clear();}else continued_from=number;
        if(line.empty()){if(stop==text.npos)break;continue;}
        if(line.front()==u'['){
            const auto close=line.find(u']');if(close==line.npos){if(error_line)*error_line=continued_from;throw Failure{bad_section_line};}
            auto name=driver_store::trim(line.substr(1,close-1));if(name.size()>255){if(error_line)*error_line=continued_from;throw Failure{section_too_long};}
            current=fold(name);if(!file.sections.contains(current)){file.sections[current].name=name;file.order.push_back(current);}
            if(stop==text.npos)break;continue;
        }
        if(!current.empty()){
            State::Line entry;bool in_quote=false;std::size_t equal=std::u16string::npos;
            for(std::size_t i=0;i<line.size();++i){if(line[i]==u'"')in_quote=!in_quote;else if(line[i]==u'=' && !in_quote){equal=i;break;}}
            if(equal!=std::u16string::npos){auto key=split_fields(std::u16string_view(line).substr(0,equal));entry.key=key.empty()?State::Field{}:key.front();entry.keyed=true;entry.fields=split_fields(std::u16string_view(line).substr(equal+1));}
            else entry.fields=split_fields(line);
            file.sections[current].lines.push_back(std::move(entry));
        }
        if(stop==text.npos)break;
    }
    const auto version=file.sections.find(u"version");std::u16string signature;
    if(version!=file.sections.end())for(const auto& line:version->second.lines)if(line.keyed){std::u16string key;for(const auto& part:line.key)key+=part.text;if(fold(key)==u"signature" && !line.fields.empty()){for(const auto& part:line.fields.front())signature+=part.text;break;}}
    signature=fold(signature);file.style=signature==u"$windows nt$" || signature==u"$chicago$" || signature==u"$windows 95$"?2U:1U;
    return file;
}
}

std::span<const WindowsServiceDescriptor> setup_inf_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"setupapi.dll","SetupOpenInfFileA",4,0,0,4},{"setupapi.dll","SetupOpenInfFileW",4,0,0,4},{"setupapi.dll","SetupOpenAppendInfFileA",3},{"setupapi.dll","SetupOpenAppendInfFileW",3},
        {"setupapi.dll","SetupOpenMasterInf",0},{"setupapi.dll","SetupCloseInfFile",1},{"setupapi.dll","SetupEnumInfSectionsA",5,0,0,0xa},{"setupapi.dll","SetupEnumInfSectionsW",5,0,0,0xa},
        {"setupapi.dll","SetupFindFirstLineA",4},{"setupapi.dll","SetupFindFirstLineW",4},{"setupapi.dll","SetupFindNextLine",2},{"setupapi.dll","SetupFindNextMatchLineA",3},{"setupapi.dll","SetupFindNextMatchLineW",3},
        {"setupapi.dll","SetupGetLineByIndexA",4,0,0,4},{"setupapi.dll","SetupGetLineByIndexW",4,0,0,4},{"setupapi.dll","SetupGetLineCountA",2},{"setupapi.dll","SetupGetLineCountW",2},
        {"setupapi.dll","SetupGetLineTextA",7,0,0,0x20},{"setupapi.dll","SetupGetLineTextW",7,0,0,0x20},{"setupapi.dll","SetupGetFieldCount",1},
        {"setupapi.dll","SetupGetStringFieldA",5,0,0,0xa},{"setupapi.dll","SetupGetStringFieldW",5,0,0,0xa},{"setupapi.dll","SetupGetIntField",3,0,0,2},
        {"setupapi.dll","SetupGetMultiSzFieldA",5,0,0,0xa},{"setupapi.dll","SetupGetMultiSzFieldW",5,0,0,0xa},{"setupapi.dll","SetupGetBinaryField",5,0,0,0xa},
        {"setupapi.dll","SetupGetInfInformationA",5,0,0,0xa},{"setupapi.dll","SetupGetInfInformationW",5,0,0,0xa},
        {"setupapi.dll","SetupQueryInfFileInformationA",5,0,0,0xa},{"setupapi.dll","SetupQueryInfFileInformationW",5,0,0,0xa},
        {"setupapi.dll","SetupQueryInfVersionInformationA",6,0,0,0x12},{"setupapi.dll","SetupQueryInfVersionInformationW",6,0,0,0x12},
        {"setupapi.dll","SetupQueryInfOriginalFileInformationA",4,0,0,2},{"setupapi.dll","SetupQueryInfOriginalFileInformationW",4,0,0,2},
        {"setupapi.dll","SetupGetInfFileListA",5,0,0,0xa},{"setupapi.dll","SetupGetInfFileListW",5,0,0,0xa},
        {"setupapi.dll","SetupGetSourceInfoA",6,0,0,0x16},{"setupapi.dll","SetupGetSourceInfoW",6,0,0,0x16},
        {"setupapi.dll","SetupGetSourceFileLocationA",7,0,0,0x20},{"setupapi.dll","SetupGetSourceFileLocationW",7,0,0,0x20},
        {"setupapi.dll","SetupGetTargetPathA",6,0,0,0x10},{"setupapi.dll","SetupGetTargetPathW",6,0,0,0x10},
        {"setupapi.dll","SetupDiGetINFClassA",5,0,0,8},{"setupapi.dll","SetupDiGetINFClassW",5,0,0,8},
        {"setupapi.dll","SetupDiGetActualSectionToInstallA",6,0,0,8},{"setupapi.dll","SetupDiGetActualSectionToInstallW",6,0,0,8},
        {"setupapi.dll","SetupDiGetActualSectionToInstallExA",8,0,0,0x10},{"setupapi.dll","SetupDiGetActualSectionToInstallExW",8,0,0,0x10},
        // 字串表與公用輔助函式（windows_setup_string_table.inc）
        {"setupapi.dll","StringTableInitialize",0},{"setupapi.dll","StringTableInitializeEx",2,0,0,0x3},{"setupapi.dll","StringTableDestroy",1},
        {"setupapi.dll","StringTableAddString",3,0,0,0x4},{"setupapi.dll","StringTableAddStringEx",5,0,0,0x14},{"setupapi.dll","StringTableLookUpString",3,0,0,0x4},
        {"setupapi.dll","StringTableLookUpStringEx",5,0,0,0x14},{"setupapi.dll","StringTableStringFromId",2,0,0,0x2},{"setupapi.dll","StringTableStringFromIdEx",4,0,0,0x2},
        {"setupapi.dll","StringTableGetExtraData",4,0,0,0xa},{"setupapi.dll","StringTableSetExtraData",4,0,0,0xa},{"setupapi.dll","StringTableDuplicate",1},{"setupapi.dll","StringTableTrim",1},
        {"setupapi.dll","pSetupStringTableInitialize",0},{"setupapi.dll","pSetupStringTableInitializeEx",2,0,0,0x3},{"setupapi.dll","pSetupStringTableDestroy",1},
        {"setupapi.dll","pSetupStringTableAddString",3,0,0,0x4},{"setupapi.dll","pSetupStringTableAddStringEx",5,0,0,0x14},{"setupapi.dll","pSetupStringTableLookUpString",3,0,0,0x4},
        {"setupapi.dll","pSetupStringTableLookUpStringEx",5,0,0,0x14},{"setupapi.dll","pSetupStringTableGetExtraData",4,0,0,0xa},{"setupapi.dll","pSetupStringTableSetExtraData",4,0,0,0xa},
        {"setupapi.dll","pSetupStringTableDuplicate",1},
        {"setupapi.dll","MyMalloc",1,0,0,0x1},{"setupapi.dll","MyFree",1},{"setupapi.dll","MyRealloc",2,0,0,0x2},{"setupapi.dll","pSetupMalloc",1,0,0,0x1},{"setupapi.dll","pSetupFree",1},
        {"setupapi.dll","pSetupRealloc",2,0,0,0x2},{"setupapi.dll","DuplicateString",1},{"setupapi.dll","MultiByteToUnicode",2,0,0,0x2},{"setupapi.dll","UnicodeToMultiByte",2,0,0,0x2},
        {"setupapi.dll","CaptureStringArg",2},{"setupapi.dll","CaptureAndConvertAnsiArg",2},{"setupapi.dll","IsUserAdmin",0},{"setupapi.dll","pSetupIsUserAdmin",0},
        {"setupapi.dll","FileExists",2},{"setupapi.dll","pSetupGetGlobalFlags",0},{"setupapi.dll","pSetupSetGlobalFlags",1,0,0,0x1},
        // 磁碟空間清單、安裝記錄檔、檔案記錄與非互動模式（windows_setup_disk_log.inc）
        {"setupapi.dll","SetupCreateDiskSpaceListA",3,0,0,0x6},{"setupapi.dll","SetupCreateDiskSpaceListW",3,0,0,0x6},{"setupapi.dll","SetupDuplicateDiskSpaceListA",4,0,0,0xc},
        {"setupapi.dll","SetupDuplicateDiskSpaceListW",4,0,0,0xc},{"setupapi.dll","SetupDestroyDiskSpaceList",1},
        {"setupapi.dll","SetupAddToDiskSpaceListA",6,0x4,0,0x28},{"setupapi.dll","SetupAddToDiskSpaceListW",6,0x4,0,0x28},
        {"setupapi.dll","SetupRemoveFromDiskSpaceListA",5,0,0,0x14},{"setupapi.dll","SetupRemoveFromDiskSpaceListW",5,0,0,0x14},
        {"setupapi.dll","SetupQuerySpaceRequiredOnDriveA",5,0,0,0x10},{"setupapi.dll","SetupQuerySpaceRequiredOnDriveW",5,0,0,0x10},
        {"setupapi.dll","SetupQueryDrivesInDiskSpaceListA",4,0,0,0x4},{"setupapi.dll","SetupQueryDrivesInDiskSpaceListW",4,0,0,0x4},
        {"setupapi.dll","SetupOpenLog",1,0,0,0x1},{"setupapi.dll","SetupCloseLog",0},{"setupapi.dll","SetupLogErrorA",2,0,0,0x2},{"setupapi.dll","SetupLogErrorW",2,0,0,0x2},
        {"setupapi.dll","SetupInitializeFileLogA",2,0,0,0x2},{"setupapi.dll","SetupInitializeFileLogW",2,0,0,0x2},{"setupapi.dll","SetupTerminateFileLog",1},
        {"setupapi.dll","SetupLogFileA",9,0,0,0x110},{"setupapi.dll","SetupLogFileW",9,0,0,0x110},{"setupapi.dll","SetupQueryFileLogA",7,0,0,0x28},{"setupapi.dll","SetupQueryFileLogW",7,0,0,0x28},
        {"setupapi.dll","SetupRemoveFileLogEntryA",3},{"setupapi.dll","SetupRemoveFileLogEntryW",3},
        {"setupapi.dll","SetupGetNonInteractiveMode",0},{"setupapi.dll","SetupSetNonInteractiveMode",1,0,0,0x1}};
    return table;
}

std::optional<std::uint32_t> WindowsServices::invoke_setup_inf(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const bool wide=name.ends_with('W');const unsigned unit=x86_?4U:8U;const auto width=static_cast<std::uint8_t>(unit*8);
    if(!setup_inf_)setup_inf_=std::make_shared<SetupInfState>();auto& handles=setup_inf_->handles;
    const auto finish=[&](std::uint64_t value)->std::optional<std::uint32_t>{arg.return_to(state,value);return std::nullopt;};
    const auto fail=[&](std::uint32_t error,std::uint64_t value=0){last_error_=error;return finish(value);};
    const auto succeed=[&](std::uint64_t value=1){last_error_=0;return finish(value);};
    const auto invalid_handle=x86_?std::uint64_t{UINT32_MAX}:UINT64_MAX;
    const auto read_text=[&](std::uint64_t pointer)->std::u16string{if(!pointer)return {};return wide?read_utf16(memory,GuestAddress{pointer},32768):utf8_to_utf16(read_ansi(memory,pointer,32768));};
    const auto environment_value=[&](std::u16string_view key){for(const auto& [entry_key,entry]:runtime_.environment)if(fold(entry_key)==fold(std::u16string(key)))return entry.second;return std::u16string{};};
    const auto full_path=[&](const std::u16string& path){return full_windows_environment_path(path,runtime_.current_directory,runtime_.environment);};
    const auto exists=[&](const std::u16string& path){const auto info=filesystem_.information(utf16_to_utf8(path));return !info.error && !(info.metadata.attributes&0x10U);};
    // 不含路徑分隔字元的名稱依序找 %windir%\inf 與 %windir%\system32；其餘依目前目錄解析。
    const auto locate=[&](const std::u16string& file)->std::u16string{
        if(file.find_first_of(u"\\/:")==std::u16string::npos){for(const auto* directory:{u"C:\\Windows\\INF\\",u"C:\\Windows\\System32\\"}){const auto candidate=std::u16string(directory)+file;if(exists(candidate))return candidate;}throw Failure{2};}
        const auto path=full_path(file);if(!exists(path))throw Failure{2};return path;
    };
    const auto load=[&](const std::u16string& path,std::uint32_t* error_line){
        try{const auto bytes=driver_store::read_file(filesystem_,path);return parse_file(path,*bytes,error_line);}catch(const driver_store::Error& error){throw Failure{error.code};}
    };
    const auto find_handle=[&](std::uint64_t value)->State::Handle&{const auto found=handles.find(value);if(found==handles.end())throw Failure{6};return found->second;};
    const auto section_id=[&](State::Handle& handle,const std::u16string& section){const auto key=fold(section);if(const auto found=handle.section_ids.find(key);found!=handle.section_ids.end())return found->second;const auto id=static_cast<std::uint32_t>(handle.section_names.size());handle.section_ids.emplace(key,id);handle.section_names.push_back(key);return id;};
    const auto lines_of=[&](State::Handle& handle,std::size_t file,std::uint32_t section)->const std::vector<State::Line>*{
        const auto& files=handle.files;if(file>=files.size() || section>=handle.section_names.size())return nullptr;const auto found=files[file].sections.find(handle.section_names[section]);return found==files[file].sections.end()?nullptr:&found->second.lines;
    };
    // [Strings] 與語言覆寫；全部附加的 INF 共用。
    const auto string_value=[&](const State::Handle& handle,const std::u16string& key)->std::optional<std::u16string>{
        std::optional<std::u16string> result;
        for(const auto* section:{u"strings",u"strings.0009",u"strings.0409"})for(const auto& file:handle.files){
            const auto found=file.sections.find(section);if(found==file.sections.end())continue;
            for(const auto& line:found->second.lines){if(!line.keyed)continue;std::u16string name_text;for(const auto& part:line.key)name_text+=part.text;if(fold(name_text)!=key || line.fields.empty())continue;std::u16string value;for(const auto& part:line.fields.front())value+=part.text;result=value;}
        }
        return result;
    };
    // DIRID：1 為 INF 所在目錄，其餘依 Windows 標準目錄；殼層資料夾為 16384+CSIDL。
    const auto dirid=[&](std::int64_t id,const State::File& file)->std::optional<std::u16string>{
        const bool program_x86=x86_;
        switch(id){
        case -1:return std::u16string{};case 1:return driver_store::parent_of(file.path);
        case 10:case 25:return u"C:\\Windows";case 11:return u"C:\\Windows\\System32";case 12:case 52:return u"C:\\Windows\\System32\\drivers";
        case 17:return u"C:\\Windows\\INF";case 18:return u"C:\\Windows\\Help";case 20:return u"C:\\Windows\\Fonts";case 21:return u"C:\\Windows\\System32\\viewers";
        case 23:return u"C:\\Windows\\System32\\spool\\drivers\\color";case 24:case 30:case 54:return u"C:\\";case 50:return u"C:\\Windows\\System";
        case 51:return u"C:\\Windows\\System32\\spool";case 53:return environment_value(u"USERPROFILE");case 55:return u"C:\\Windows\\System32\\spool\\prtprocs";
        case 16419:return u"C:\\ProgramData";case 16422:return program_x86?u"C:\\Program Files (x86)":u"C:\\Program Files";case 16426:return u"C:\\Program Files (x86)";
        case 16427:return program_x86?u"C:\\Program Files (x86)\\Common Files":u"C:\\Program Files\\Common Files";case 16428:return u"C:\\Program Files (x86)\\Common Files";
        default:return std::nullopt;
        }
    };
    const auto substitute=[&](const State::Handle& handle,const State::File& file,const std::u16string& text){
        std::u16string result;
        for(std::size_t i=0;i<text.size();){
            if(text[i]!=u'%'){result+=text[i++];continue;}
            const auto close=text.find(u'%',i+1);if(close==text.npos){result+=text.substr(i);break;}
            if(close==i+1){result+=u'%';i=close+1;continue;}
            const auto token=text.substr(i+1,close-i-1);
            if(const auto value=string_value(handle,fold(token)))result+=*value;
            else if(const auto number=driver_store::number(token); number && !token.empty()){if(const auto path=dirid(static_cast<std::int64_t>(*number),file))result+=*path;else result+=text.substr(i,close-i+1);}
            else if(token==u"-1")result+=u"";
            else result+=text.substr(i,close-i+1);
            i=close+1;
        }
        return result;
    };
    const auto field_text=[&](const State::Handle& handle,const State::File& file,const State::Field& field){std::u16string text;for(const auto& part:field)text+=part.quoted?part.text:substitute(handle,file,part.text);return text;};
    // INFCONTEXT 版面：Inf、CurrentInf（附加檔案序號＋1）、Section、Line。
    struct Context {std::uint64_t inf{};std::size_t file{};std::uint32_t section{},line{};};
    const auto read_context=[&](std::uint64_t pointer){Context context;context.inf=memory.load(GuestAddress{pointer},width);context.file=static_cast<std::size_t>(memory.load(GuestAddress{pointer+unit},width));context.section=static_cast<std::uint32_t>(memory.load(GuestAddress{pointer+2*unit},32));context.line=static_cast<std::uint32_t>(memory.load(GuestAddress{pointer+2*unit+4},32));
        if(!context.file)throw Failure{87};--context.file;return context;};
    const auto write_context=[&](std::uint64_t pointer,const Context& context){memory.check(GuestAddress{pointer},2*unit+8,Access::Write);memory.store(GuestAddress{pointer},width,context.inf);memory.store(GuestAddress{pointer+unit},width,context.file+1);memory.store(GuestAddress{pointer+2*unit},32,context.section);memory.store(GuestAddress{pointer+2*unit+4},32,context.line);};
    const auto context_line=[&](const Context& context)->std::tuple<State::Handle&,const State::File&,const State::Line&>{
        auto& handle=find_handle(context.inf);const auto* lines=lines_of(handle,context.file,context.section);if(!lines || context.line>=lines->size())throw Failure{87};
        return {handle,handle.files[context.file],(*lines)[context.line]};
    };
    const auto key_text=[&](const State::Line& line){std::u16string text;for(const auto& part:line.key)text+=part.text;return text;};
    // 自 (file, line) 起在同名區段跨附加檔案往後找；key 為空時取第一行。
    const auto search=[&](State::Handle& handle,std::uint64_t inf,std::uint32_t section,std::size_t file,std::size_t line,const std::optional<std::u16string>& key)->std::optional<Context>{
        for(;file<handle.files.size();++file,line=0){
            const auto* lines=lines_of(handle,file,section);if(!lines)continue;
            for(;line<lines->size();++line)if(!key || ((*lines)[line].keyed && fold(key_text((*lines)[line]))==*key))return Context{inf,file,section,static_cast<std::uint32_t>(line)};
        }
        return std::nullopt;
    };
    const auto section_exists=[&](State::Handle& handle,std::uint32_t section){for(std::size_t file=0;file<handle.files.size();++file)if(lines_of(handle,file,section))return true;return false;};
    const auto encoded=[&](std::u16string_view text){std::vector<std::uint8_t> bytes;if(wide)bytes=utf16_bytes(text);else {const auto ansi=utf16_to_utf8(std::u16string(text));bytes.assign(ansi.begin(),ansi.end());bytes.push_back(0);}return bytes;};
    // 文字輸出：所需長度（含 NUL，A 為位元組）寫入 RequiredSize；緩衝區為 NULL 且大小為 0 時只回報長度。
    const auto output=[&](std::uint64_t buffer,std::uint64_t size,std::uint64_t required_at,std::vector<std::uint8_t> bytes)->std::optional<std::uint32_t>{
        const auto required=wide?bytes.size()/2:bytes.size();if(required_at){memory.check(GuestAddress{required_at},4,Access::Write);memory.store(GuestAddress{required_at},32,required);}
        if(!buffer){if(size)return fail(87);return succeed();}
        if(static_cast<std::uint32_t>(size)<required)return fail(122);memory.write(GuestAddress{buffer},bytes);return succeed();
    };
    const auto multi=[&](const std::vector<std::u16string>& items){std::u16string text;for(const auto& item:items){text+=item;text.push_back(0);}if(items.empty())text.push_back(0);std::vector<std::uint8_t> bytes;if(wide){bytes=utf16_bytes(text);}else {const auto ansi=utf16_to_utf8(text);bytes.assign(ansi.begin(),ansi.end());bytes.push_back(0);}return bytes;};
    const auto open_handle=[&](State::File file){std::uint64_t value=0;while(!value || handles.contains(value)){value=allocate_heap(memory,16);if(!value)throw Failure{8};}State::Handle handle;handle.files.push_back(std::move(file));handles.emplace(value,std::move(handle));return value;};
    const auto version_value=[&](const State::Handle& handle,const State::File& file,std::u16string_view key)->std::optional<std::u16string>{
        const auto found=file.sections.find(u"version");if(found==file.sections.end())return std::nullopt;
        for(const auto& line:found->second.lines)if(line.keyed && fold(key_text(line))==key){std::u16string text;for(std::size_t i=0;i<line.fields.size();++i){if(i)text+=u',';text+=field_text(handle,file,line.fields[i]);}return text;}
        return std::nullopt;
    };
    // 以 x64 為原生平台選擇裝飾區段（WOW64 程序同樣安裝原生驅動）。
    const auto decorated=[&](State::Handle& handle,const std::u16string& base,unsigned architecture)->std::u16string{
        const auto suffix=architecture==0?u".ntx86":architecture==12?u".ntarm64":architecture==5?u".ntarm":architecture==6?u".ntia64":u".ntamd64";
        for(const auto& candidate:{base+suffix,base+u".nt"}){if(section_exists(handle,section_id(handle,candidate)))return candidate;}
        return base;
    };
    try{
        if(name=="SetupOpenInfFileA" || name=="SetupOpenInfFileW" || name=="SetupOpenMasterInf"){
            // (FileName, InfClass, InfStyle, ErrorLine)：INF_STYLE_OLDNT＝1、INF_STYLE_WIN4＝2，快取旗標不影響結果。
            const bool master=name=="SetupOpenMasterInf";const auto style=master?2U:static_cast<std::uint32_t>(arg(2));
            std::uint32_t error_line=0;if(!master && arg(3))memory.check(GuestAddress{arg(3)},4,Access::Write);
            if(!master && (!arg(0) || !(style&3U)))return fail(87,invalid_handle);
            const auto path=master?locate(u"layout.inf"):locate(read_text(arg(0)));State::File file;
            try{file=load(path,&error_line);}catch(const Failure&){if(!master && arg(3))memory.store(GuestAddress{arg(3)},32,error_line);throw;}
            if(!(style&file.style))return fail(wrong_style,invalid_handle);
            if(!master && arg(1)){
                State::Handle probe;probe.files.push_back(file);const auto klass=version_value(probe,probe.files.front(),u"class");
                if(!klass || fold(*klass)!=fold(read_text(arg(1))))return fail(class_mismatch,invalid_handle);
            }
            return succeed(open_handle(std::move(file)));
        }
        if(name=="SetupOpenAppendInfFileA" || name=="SetupOpenAppendInfFileW"){
            // (FileName, InfHandle, ErrorLine)：FileName 為 NULL 時附加 [Version] LayoutFile 列出的 INF。
            auto& handle=find_handle(arg(1));std::vector<std::u16string> names;
            if(arg(0))names.push_back(read_text(arg(0)));
            else {
                const auto& first=handle.files.front();const auto found=first.sections.find(u"version");if(found==first.sections.end())return fail(line_not_found);
                for(const auto& line:found->second.lines)if(line.keyed && fold(key_text(line))==u"layoutfile")for(const auto& field:line.fields)names.push_back(field_text(handle,first,field));
                if(names.empty())return fail(line_not_found);
            }
            std::vector<State::File> files;std::uint32_t error_line=0;
            for(const auto& file:names){
                std::u16string path=file.find_first_of(u"\\/:")==std::u16string::npos && exists(driver_store::parent_of(handle.files.front().path)+u"\\"+file)?driver_store::parent_of(handle.files.front().path)+u"\\"+file:locate(file);
                try{files.push_back(load(path,&error_line));}catch(const Failure&){if(arg(2)){memory.check(GuestAddress{arg(2)},4,Access::Write);memory.store(GuestAddress{arg(2)},32,error_line);}throw;}
            }
            for(auto& file:files)handle.files.push_back(std::move(file));
            return succeed();
        }
        if(name=="SetupCloseInfFile"){if(const auto found=handles.find(arg(0));found!=handles.end()){handles.erase(found);(void)free_heap(memory,arg(0),state);}return finish(0);}
        if(name.starts_with("SetupEnumInfSections")){
            // (InfHandle, Index, Buffer, Size, SizeNeeded)：依首次出現的順序列出全部附加 INF 的區段名稱。
            auto& handle=find_handle(arg(0));std::vector<std::u16string> seen,names;
            for(const auto& file:handle.files)for(const auto& key:file.order)if(std::find(seen.begin(),seen.end(),key)==seen.end()){seen.push_back(key);names.push_back(file.sections.at(key).name);}
            const auto index=static_cast<std::uint32_t>(arg(1));if(index>=names.size())return fail(259);
            return output(arg(2),arg(3),arg(4),encoded(names[index]));
        }
        if(name.starts_with("SetupFindFirstLine")){
            // (InfHandle, Section, Key, Context)：區段不存在回 ERROR_SECTION_NOT_FOUND，找不到 key 回 ERROR_LINE_NOT_FOUND。
            if(!arg(1) || !arg(3))return fail(87);auto& handle=find_handle(arg(0));const auto section=section_id(handle,read_text(arg(1)));
            if(!section_exists(handle,section))return fail(section_not_found);
            const auto key=arg(2)?std::optional<std::u16string>{fold(read_text(arg(2)))}:std::nullopt;const auto found=search(handle,arg(0),section,0,0,key);
            if(!found)return fail(line_not_found);write_context(arg(3),*found);return succeed();
        }
        if(name=="SetupFindNextLine" || name.starts_with("SetupFindNextMatchLine")){
            const bool match=name!="SetupFindNextLine";const auto out_at=arg(match?2:1);if(!arg(0) || !out_at)return fail(87);
            const auto context=read_context(arg(0));auto& handle=find_handle(context.inf);
            const auto key=match && arg(1)?std::optional<std::u16string>{fold(read_text(arg(1)))}:std::nullopt;
            const auto found=search(handle,context.inf,context.section,context.file,std::size_t{context.line}+1,key);
            if(!found)return fail(line_not_found);write_context(out_at,*found);return succeed();
        }
        if(name.starts_with("SetupGetLineByIndex") || name.starts_with("SetupGetLineCount")){
            const bool count=name.starts_with("SetupGetLineCount");if(!arg(1))return count?fail(87,UINT32_MAX):fail(87);
            auto& handle=find_handle(arg(0));const auto section=section_id(handle,read_text(arg(1)));
            if(!section_exists(handle,section))return count?fail(section_not_found,UINT32_MAX):fail(section_not_found);
            std::size_t total=0;for(std::size_t file=0;file<handle.files.size();++file)if(const auto* lines=lines_of(handle,file,section))total+=lines->size();
            if(count){last_error_=0;return finish(static_cast<std::uint32_t>(total));}
            if(!arg(3))return fail(87);auto index=static_cast<std::size_t>(static_cast<std::uint32_t>(arg(2)));if(index>=total)return fail(line_not_found);
            for(std::size_t file=0;file<handle.files.size();++file)if(const auto* lines=lines_of(handle,file,section)){if(index<lines->size()){write_context(arg(3),{arg(0),file,section,static_cast<std::uint32_t>(index)});return succeed();}index-=lines->size();}
            return fail(line_not_found);
        }
        if(name.starts_with("SetupGetLineText")){
            // (Context, InfHandle, Section, Key, ReturnBuffer, ReturnBufferSize, RequiredSize)：欄位代換後以逗號串接。
            Context context;
            if(arg(0))context=read_context(arg(0));
            else {
                if(!arg(2) || !arg(3))return fail(87);auto& handle=find_handle(arg(1));const auto section=section_id(handle,read_text(arg(2)));
                if(!section_exists(handle,section))return fail(section_not_found);const auto found=search(handle,arg(1),section,0,0,fold(read_text(arg(3))));if(!found)return fail(line_not_found);context=*found;
            }
            auto [handle,file,line]=context_line(context);std::u16string text;
            for(std::size_t i=0;i<line.fields.size();++i){if(i)text+=u',';text+=field_text(handle,file,line.fields[i]);}
            return output(arg(4),arg(5),arg(6),encoded(text));
        }
        if(name=="SetupGetFieldCount"){
            if(!arg(0))return fail(87);auto [handle,file,line]=context_line(read_context(arg(0)));(void)handle;(void)file;
            const auto count=line.fields.size()==1 && line.fields.front().empty()?0U:static_cast<std::uint32_t>(line.fields.size());last_error_=0;return finish(count);
        }
        if(name.starts_with("SetupGetStringField") || name=="SetupGetIntField" || name.starts_with("SetupGetMultiSzField") || name=="SetupGetBinaryField"){
            // 欄位 0 為 key；沒有 key 的行以第一個欄位為 key。
            if(!arg(0))return fail(87);const auto bound=context_line(read_context(arg(0)));auto& handle=std::get<0>(bound);const auto& file=std::get<1>(bound);const auto& line=std::get<2>(bound);const auto index=static_cast<std::uint32_t>(arg(1));
            const auto field=[&](std::uint32_t at)->std::optional<std::u16string>{
                if(at==0)return line.keyed?field_text(handle,file,line.key):line.fields.empty()?std::u16string{}:field_text(handle,file,line.fields.front());
                if(at>line.fields.size())return std::nullopt;return field_text(handle,file,line.fields[at-1]);
            };
            if(name.starts_with("SetupGetStringField")){const auto text=field(index);if(!text)return fail(87);return output(arg(2),arg(3),arg(4),encoded(*text));}
            if(name=="SetupGetIntField"){
                if(!arg(2))return fail(87);const auto text=field(index);if(!text)return fail(87);auto digits=*text;bool negative=false;
                if(!digits.empty() && (digits.front()==u'-' || digits.front()==u'+')){negative=digits.front()==u'-';digits.erase(0,1);}
                const auto value=digits.empty()?std::nullopt:driver_store::number(digits);if(!value)return fail(13);
                memory.check(GuestAddress{arg(2)},4,Access::Write);memory.store(GuestAddress{arg(2)},32,negative?static_cast<std::uint32_t>(0U-*value):*value);return succeed();
            }
            if(index==0)return fail(87);std::vector<std::u16string> items;
            for(auto at=index;at<=line.fields.size();++at)items.push_back(*field(at));
            if(name.starts_with("SetupGetMultiSzField")){std::erase_if(items,[](const auto& item){return item.empty();});return output(arg(2),arg(3),arg(4),multi(items));}
            std::vector<std::uint8_t> bytes;
            for(const auto& item:items){const auto value=item.empty()?std::nullopt:driver_store::number(u"0x"+item);if(!value || *value>255 || item.size()>2)return fail(13);bytes.push_back(static_cast<std::uint8_t>(*value));}
            if(arg(4)){memory.check(GuestAddress{arg(4)},4,Access::Write);memory.store(GuestAddress{arg(4)},32,bytes.size());}
            if(!arg(2)){if(arg(3))return fail(87);return succeed();}if(static_cast<std::uint32_t>(arg(3))<bytes.size())return fail(122);memory.write(GuestAddress{arg(2)},bytes);return succeed();
        }
        if(name.starts_with("SetupGetInfInformation")){
            // (InfSpec, SearchControl, ReturnBuffer, ReturnBufferSize, RequiredSize)：SP_INF_INFORMATION {InfStyle, InfCount, VersionData}，
            // VersionData 為本實作的自足格式：每個 INF 一筆 {DWORD 位元組數, UTF-16 路徑, key\0value\0…\0}，供 SetupQueryInf*Information 讀取。
            const auto control=static_cast<std::uint32_t>(arg(1));if(!arg(0) || control<1 || control>5)return fail(87);
            std::vector<State::File> files;State::Handle* source=nullptr;State::Handle temporary;
            if(control==1){source=&find_handle(arg(0));}
            else {const auto spec=read_text(arg(0));temporary.files.push_back(load(control==2?full_path(spec):locate(spec),nullptr));source=&temporary;}
            std::vector<std::uint8_t> data(8);const auto put32=[&](std::size_t at,std::uint32_t value){for(unsigned i=0;i<4;++i)data[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
            put32(0,source->files.front().style);put32(4,static_cast<std::uint32_t>(source->files.size()));
            for(const auto& file:source->files){
                std::u16string record=file.path;record.push_back(0);
                if(const auto found=file.sections.find(u"version");found!=file.sections.end())for(const auto& line:found->second.lines)if(line.keyed){
                    record+=key_text(line);record.push_back(0);for(std::size_t i=0;i<line.fields.size();++i){if(i)record+=u',';record+=field_text(*source,file,line.fields[i]);}record.push_back(0);
                }
                record.push_back(0);const auto bytes=utf16_bytes(record);const auto at=data.size();data.resize(at+4);put32(at,static_cast<std::uint32_t>(bytes.size()-2));data.insert(data.end(),bytes.begin(),bytes.end()-2);
            }
            if(arg(4)){memory.check(GuestAddress{arg(4)},4,Access::Write);memory.store(GuestAddress{arg(4)},32,data.size());}
            if(!arg(2)){if(arg(3))return fail(87);return succeed();}if(static_cast<std::uint32_t>(arg(3))<data.size())return fail(122);memory.write(GuestAddress{arg(2)},data);return succeed();
        }
        if(name.starts_with("SetupQueryInfFileInformation") || name.starts_with("SetupQueryInfVersionInformation") || name.starts_with("SetupQueryInfOriginalFileInformation")){
            if(!arg(0))return fail(87);const auto count=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)+4},32));const auto index=static_cast<std::uint32_t>(arg(1));if(index>=count)return fail(87);
            std::uint64_t at=arg(0)+8;for(std::uint32_t i=0;i<index;++i)at+=4+memory.load(GuestAddress{at},32);
            const auto size=static_cast<std::size_t>(memory.load(GuestAddress{at},32));if(size>1024*1024 || size%2)return fail(13);
            std::vector<std::uint8_t> raw(size);memory.read(GuestAddress{at+4},raw);std::u16string record;for(std::size_t i=0;i+1<raw.size();i+=2)record.push_back(static_cast<char16_t>(raw[i]|(raw[i+1]<<8)));
            std::vector<std::u16string> parts;for(std::size_t begin=0;begin<record.size();){const auto end=record.find(u'\0',begin);if(end==record.npos)break;parts.push_back(record.substr(begin,end-begin));begin=end+1;}
            if(parts.empty())return fail(13);const auto& path=parts.front();
            std::vector<std::pair<std::u16string,std::u16string>> pairs;for(std::size_t i=1;i+1<parts.size();i+=2)if(!parts[i].empty())pairs.emplace_back(parts[i],parts[i+1]);
            if(name.starts_with("SetupQueryInfFileInformation"))return output(arg(2),arg(3),arg(4),encoded(path));
            if(name.starts_with("SetupQueryInfVersionInformation")){
                if(arg(2)){const auto key=fold(read_text(arg(2)));for(const auto& [entry_key,value]:pairs)if(fold(entry_key)==key)return output(arg(3),arg(4),arg(5),encoded(value));return fail(line_not_found);}
                std::vector<std::u16string> items;for(const auto& [entry_key,value]:pairs){items.push_back(entry_key);items.push_back(value);}return output(arg(3),arg(4),arg(5),multi(items));
            }
            // SP_ORIGINAL_FILE_INFO：cbSize、OriginalInfName[MAX_PATH]、OriginalCatalogName[MAX_PATH]；目錄檔取 CatalogFile 的平台裝飾版本。
            if(!arg(3))return fail(87);const unsigned chars=wide?2U:1U;const auto expected=4+520*chars;
            if(memory.load(GuestAddress{arg(3)},32)!=expected)return fail(1784);
            std::u16string catalog;for(const auto* key:{u"catalogfile.ntamd64",u"catalogfile.nt",u"catalogfile"}){for(const auto& [entry_key,value]:pairs)if(fold(entry_key)==key){catalog=value;break;}if(!catalog.empty())break;}
            const auto store=[&](std::uint64_t target,std::u16string text){if(text.size()>259)text.resize(259);auto bytes=encoded(text);bytes.resize(260*chars);memory.write(GuestAddress{target},bytes);};
            store(arg(3)+4,driver_store::leaf_of(path));store(arg(3)+4+260*chars,catalog);return succeed();
        }
        if(name.starts_with("SetupGetInfFileList")){
            // (DirectoryPath, InfStyle, ReturnBuffer, ReturnBufferSize, RequiredSize)：預設為 %windir%\inf；只列出符合樣式的 INF 檔名。
            const auto style=static_cast<std::uint32_t>(arg(1));if(!(style&3U))return fail(87);
            const auto directory=arg(0)?full_path(read_text(arg(0))):std::u16string(driver_store::inf_directory);std::vector<std::u16string> names;
            std::vector<std::u16string> paths;try{paths=driver_store::inf_files(filesystem_,directory);}catch(const driver_store::Error& error){return fail(error.code);}
            for(const auto& path:paths){try{if(load(path,nullptr).style&style)names.push_back(driver_store::leaf_of(path));}catch(const Failure&){}}
            return output(arg(2),arg(3),arg(4),multi(names));
        }
        if(name.starts_with("SetupGetSourceInfo") || name.starts_with("SetupGetSourceFileLocation") || name.starts_with("SetupGetTargetPath")){
            auto& handle=find_handle(arg(0));
            // [SourceDisksNames]／[SourceDisksFiles] 以 .amd64 等平台字尾裝飾；[DestinationDirs] 不裝飾。
            const auto first_line=[&](const std::u16string& section,const std::u16string& key,bool platform)->std::optional<Context>{
                auto chosen=section;if(platform && section_exists(handle,section_id(handle,section+u".amd64")))chosen=section+u".amd64";
                return search(handle,arg(0),section_id(handle,chosen),0,0,fold(key));
            };
            const auto line_field=[&](const Context& context,std::size_t at)->std::u16string{auto [h,file,line]=context_line(context);if(at>=line.fields.size())return {};return field_text(h,file,line.fields[at]);};
            if(name.starts_with("SetupGetSourceInfo")){
                // (InfHandle, SourceId, InfoDesired, ReturnBuffer, ReturnBufferSize, RequiredSize)：[SourceDisksNames] 的描述、標記檔、路徑與旗標。
                const auto info=static_cast<std::uint32_t>(arg(2));if(info<1 || info>5)return fail(87);
                const auto found=first_line(u"SourceDisksNames",utf8_to_utf16(std::to_string(static_cast<std::uint32_t>(arg(1)))),true);if(!found)return fail(line_not_found);
                constexpr std::array<std::size_t,5> columns{3,1,0,4,5};
                return output(arg(3),arg(4),arg(5),encoded(line_field(*found,columns[info-1])));
            }
            if(name.starts_with("SetupGetSourceFileLocation")){
                // (InfHandle, InfContext, FileName, SourceId, ReturnBuffer, ReturnBufferSize, RequiredSize)：[SourceDisksFiles] 的磁碟編號與子目錄。
                std::u16string file;
                if(arg(1)){auto [h,owner,line]=context_line(read_context(arg(1)));file=line.fields.size()>1 && !line.fields[1].empty()?field_text(h,owner,line.fields[1]):line.fields.empty()?std::u16string{}:field_text(h,owner,line.fields.front());}
                else if(arg(2))file=read_text(arg(2));else return fail(87);
                if(!arg(3))return fail(87);const auto found=first_line(u"SourceDisksFiles",file,true);if(!found)return fail(line_not_found);
                const auto id=driver_store::number(line_field(*found,0));if(!id)return fail(13);memory.check(GuestAddress{arg(3)},4,Access::Write);memory.store(GuestAddress{arg(3)},32,*id);
                return output(arg(4),arg(5),arg(6),encoded(line_field(*found,1)));
            }
            // SetupGetTargetPath (InfHandle, InfContext, Section, ReturnBuffer, ReturnBufferSize, RequiredSize)：[DestinationDirs] 的 dirid[,子目錄]，沒有項目時用 DefaultDestDir，再退回 DIRID 11。
            std::u16string section;
            if(arg(1)){const auto context=read_context(arg(1));auto& owner=find_handle(context.inf);if(context.section>=owner.section_names.size())return fail(87);section=owner.section_names[context.section];}
            else if(arg(2))section=read_text(arg(2));
            std::optional<Context> found;if(!section.empty())found=first_line(u"DestinationDirs",section,false);if(!found)found=first_line(u"DestinationDirs",u"DefaultDestDir",false);
            std::u16string path=u"C:\\Windows\\System32";
            if(found){
                const auto number=driver_store::number(line_field(*found,0));auto [h,owner,line]=context_line(*found);(void)h;(void)line;
                if(number){const auto base=dirid(static_cast<std::int64_t>(static_cast<std::int32_t>(*number)),owner);if(!base)return fail(13);path=*base;}else return fail(13);
                if(const auto sub=line_field(*found,1);!sub.empty())path=path.empty()?sub:path+u"\\"+sub;
            }
            return output(arg(3),arg(4),arg(5),encoded(path));
        }
        if(name.starts_with("SetupDiGetINFClass")){
            // (InfName, ClassGuid, ClassName, ClassNameSize, RequiredSize)：讀 [Version] 的 Class 與 ClassGUID（沒有 GUID 時為 GUID_NULL）。
            if(!arg(0) || !arg(1))return fail(87);const auto file=load(locate(read_text(arg(0))),nullptr);State::Handle probe;probe.files.push_back(file);
            const auto klass=version_value(probe,probe.files.front(),u"class").value_or(u"");const auto guid_text=version_value(probe,probe.files.front(),u"classguid").value_or(u"");
            WindowsGuid guid{};if(!guid_text.empty())if(const auto parsed=parse_windows_guid(guid_text,true))guid=*parsed;
            if(klass.empty() && guid_text.empty())return fail(0xe0000200U); // ERROR_NO_ASSOCIATED_CLASS
            memory.check(GuestAddress{arg(1)},16,Access::Write);memory.write(GuestAddress{arg(1)},guid);
            const auto bytes=encoded(klass);const auto required=wide?bytes.size()/2:bytes.size();if(arg(4)){memory.check(GuestAddress{arg(4)},4,Access::Write);memory.store(GuestAddress{arg(4)},32,required);}
            if(!arg(2) || static_cast<std::uint32_t>(arg(3))<required)return fail(122);memory.write(GuestAddress{arg(2)},bytes);return succeed();
        }
        if(name.starts_with("SetupDiGetActualSectionToInstall")){
            // A／W：(InfHandle, InfSectionName, InfSectionWithExt, Size, RequiredSize, Extension)；
            // Ex：(InfHandle, InfSectionName, AlternatePlatformInfo, InfSectionWithExt, Size, RequiredSize, Extension, Reserved)。
            const bool extended=name.find("Ex")!=name.npos;const unsigned shift=extended?1U:0U;
            if(!arg(1) || (extended && arg(7)))return fail(87);auto& handle=find_handle(arg(0));
            unsigned architecture=9;if(extended && arg(2)){const auto size=memory.load(GuestAddress{arg(2)},32);if(size!=20 && size!=28)return fail(1784);const auto platform=memory.load(GuestAddress{arg(2)+4},32);if(platform!=2)return fail(87);architecture=static_cast<unsigned>(memory.load(GuestAddress{arg(2)+16},16));}
            const auto base=read_text(arg(1));auto chosen=decorated(handle,base,architecture);
            if(chosen!=base){
                // 輸出保留呼叫端的大小寫，裝飾字尾採 INF 慣用寫法。
                const auto suffix=fold(chosen.substr(base.size()));chosen=base+(suffix==u".nt"?u".NT":suffix==u".ntx86"?u".NTx86":suffix==u".ntarm64"?u".NTarm64":suffix==u".ntarm"?u".NTarm":suffix==u".ntia64"?u".NTia64":u".NTamd64");}
            const auto bytes=encoded(chosen);const auto required=wide?bytes.size()/2:bytes.size();
            if(arg(4+shift)){memory.check(GuestAddress{arg(4+shift)},4,Access::Write);memory.store(GuestAddress{arg(4+shift)},32,required);}
            const auto buffer=arg(2+shift),extension_at=arg(5+shift);const auto size=static_cast<std::uint32_t>(arg(3+shift));
            if(!buffer){if(size)return fail(87);if(extension_at){memory.check(GuestAddress{extension_at},unit,Access::Write);memory.store(GuestAddress{extension_at},width,0);}return succeed();}
            if(size<required)return fail(122);memory.write(GuestAddress{buffer},bytes);
            if(extension_at){memory.check(GuestAddress{extension_at},unit,Access::Write);const auto offset=wide?base.size()*2:utf16_to_utf8(base).size();memory.store(GuestAddress{extension_at},width,chosen.size()==base.size()?0:buffer+offset);}
            return succeed();
        }
#include "windows_setup_string_table.inc"
#include "windows_setup_disk_log.inc"
        throw HostCallFault("尚未實作 SetupAPI INF 服務："+std::string(name));
    }catch(const Failure& error){
        const bool handle_result=name.starts_with("SetupOpenInfFile") || name=="SetupOpenMasterInf";
        const bool count_result=name.starts_with("SetupGetLineCount");
        return fail(error.code,handle_result?invalid_handle:count_result?std::uint64_t{UINT32_MAX}:0);
    }catch(const MemoryFault&){
        const bool handle_result=name.starts_with("SetupOpenInfFile") || name=="SetupOpenMasterInf";return fail(998,handle_result?invalid_handle:0);
    }catch(const std::bad_alloc&){return fail(8,name.starts_with("SetupOpenInfFile") || name=="SetupOpenMasterInf"?invalid_handle:0);}
}
}
