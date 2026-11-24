#include "ansi_codepage.hpp"
#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <array>
#include <unicode/ucnv.h>

namespace x64arm {
std::span<const WindowsServiceDescriptor> profile_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"shlwapi.dll","#294",5,0,0,8},{"shlwapi.dll","#295",4},
        {"shlwapi.dll","#473",5,0,0,8},{"shlwapi.dll","#474",4},
        {"kernel32.dll","GetProfileIntA",3},{"kernel32.dll","GetProfileIntW",3},
        {"kernel32.dll","GetProfileStringA",5},{"kernel32.dll","GetProfileStringW",5},
        {"kernel32.dll","GetProfileSectionA",3},{"kernel32.dll","GetProfileSectionW",3},
        {"kernel32.dll","WriteProfileStringA",3},{"kernel32.dll","WriteProfileStringW",3},
        {"kernel32.dll","WriteProfileSectionA",2},{"kernel32.dll","WriteProfileSectionW",2},
        {"kernel32.dll","GetPrivateProfileIntA",4},{"kernel32.dll","GetPrivateProfileIntW",4},
        {"kernel32.dll","GetPrivateProfileStringA",6},{"kernel32.dll","GetPrivateProfileStringW",6},
        {"kernel32.dll","GetPrivateProfileSectionA",4},{"kernel32.dll","GetPrivateProfileSectionW",4},
        {"kernel32.dll","GetPrivateProfileSectionNamesA",3},{"kernel32.dll","GetPrivateProfileSectionNamesW",3},
        {"kernel32.dll","WritePrivateProfileStringA",4},{"kernel32.dll","WritePrivateProfileStringW",4},
        {"kernel32.dll","WritePrivateProfileSectionA",3},{"kernel32.dll","WritePrivateProfileSectionW",3},
        {"kernel32.dll","GetPrivateProfileStructA",5},{"kernel32.dll","GetPrivateProfileStructW",5},
        {"kernel32.dll","WritePrivateProfileStructA",5},{"kernel32.dll","WritePrivateProfileStructW",5}
    };return table;
}
namespace {
struct ProfileFailure {std::uint32_t error;};
struct ProfileLock {
    HostFile& file;
    ProfileLock(HostFile& target,bool writing):file(target){
        // INI 的讀取／修改／截斷共用同一個 OFD 鎖，涵蓋尚未存在的尾端。
        // macOS 的 flock 會與 F_OFD_GETLK 衝突，連自己的讀寫也回錯誤 33。
        // 外部區段鎖沿用 HostFile 的錯誤契約，不阻塞客體執行緒等待自己解鎖。
        if(const auto error=file.lock_range(0,UINT64_MAX,writing,false))throw ProfileFailure{error};
    }
    ~ProfileLock(){(void)file.lock_range(0,UINT64_MAX,false,true);}
    ProfileLock(const ProfileLock&)=delete;
    ProfileLock& operator=(const ProfileLock&)=delete;
};
bool space(char16_t c){return c==u' ' || c==u'\t' || c==u'\r';}
std::u16string_view trim(std::u16string_view s){while(!s.empty() && space(s.front()))s.remove_prefix(1);while(!s.empty() && space(s.back()))s.remove_suffix(1);return s;}
bool equal(std::u16string_view a,std::u16string_view b){return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](char16_t x,char16_t y){return (x>=u'A' && x<=u'Z'?x+32:x)==(y>=u'A' && y<=u'Z'?y+32:y);});}
std::optional<std::u16string_view> heading(std::u16string_view line){line=trim(line);if(line.empty() || line.front()!=u'[')return {};const auto end=line.find(u']');if(end==line.npos)return {};return trim(line.substr(1,end-1));}
std::optional<std::pair<std::u16string_view,std::u16string_view>> entry(std::u16string_view line){line=trim(line);if(line.empty() || line.front()==u';' || line.front()==u'[')return {};const auto at=line.find(u'=');if(at==line.npos)return {};return std::pair{trim(line.substr(0,at)),trim(line.substr(at+1))};}
std::u16string value_text(std::u16string_view text){if(text.size()>=2 && (text.front()==u'\'' || text.front()==u'"') && text.back()==text.front()){text.remove_prefix(1);text.remove_suffix(1);}return std::u16string(text);}
struct Profile {
    enum class Encoding {Ansi,Utf8Bom,Utf16LE,Utf16BE};Encoding encoding{Encoding::Ansi};
    std::vector<std::u16string> lines;
    explicit Profile(std::span<const std::uint8_t> bytes){
        std::u16string text;
        if(bytes.size()>=2 && ((bytes[0]==0xff && bytes[1]==0xfe) || (bytes[0]==0xfe && bytes[1]==0xff))){
            encoding=bytes[0]==0xff?Encoding::Utf16LE:Encoding::Utf16BE;if(bytes.size()%2)throw ProfileFailure{1113};
            for(std::size_t i=2;i<bytes.size();i+=2)text.push_back(static_cast<char16_t>(encoding==Encoding::Utf16LE?bytes[i]|(bytes[i+1]<<8):(bytes[i]<<8)|bytes[i+1]));
        }else if(!bytes.empty()) {
            std::size_t begin=0;if(bytes.size()>=3 && bytes[0]==0xef && bytes[1]==0xbb && bytes[2]==0xbf){begin=3;encoding=Encoding::Utf8Bom;}
            // 沒有 BOM 的檔案依程序 ACP 解碼（ACP 為 UTF-8 時即 UTF-8）；UTF-8 BOM 一律以 UTF-8 解碼。
            const std::string_view body(reinterpret_cast<const char*>(bytes.data()+begin),bytes.size()-begin);
            text=begin?utf8_to_utf16_lossy(body):ansi_to_utf16_lossy(body);
        }
        const auto nul=text.find(u'\0');if(nul!=text.npos)text.resize(nul);
        for(std::size_t at=0;at<text.size();){auto end=text.find(u'\n',at);if(end==text.npos)end=text.size();auto line=text.substr(at,end-at);if(!line.empty() && line.back()==u'\r')line.pop_back();lines.push_back(std::move(line));at=end+1;}
    }
    std::pair<std::size_t,std::size_t> section(std::u16string_view name)const{
        for(std::size_t i=0;i<lines.size();++i)if(const auto h=heading(lines[i]);h && equal(*h,name)){
            auto end=i+1;while(end<lines.size() && !heading(lines[end]))++end;return {i,end};
        }
        return {lines.size(),lines.size()};
    }
    std::optional<std::u16string> value(std::u16string_view name,std::u16string_view key)const{
        const auto [begin,end]=section(name);for(auto i=begin+1;i<end;++i)if(const auto item=entry(lines[i]);item && equal(item->first,key))return value_text(item->second);return {};
    }
    void set_value(const std::u16string& app,const std::u16string& key,const std::u16string* value){
        auto [begin,end]=section(app);
        for(auto i=begin+1;i<end;++i)if(const auto item=entry(lines[i]);item && equal(item->first,key)){
            if(value)lines[i]=key+u"="+*value;
            else lines.erase(lines.begin()+static_cast<std::ptrdiff_t>(i));
            return;
        }
        if(!value)return;
        if(begin==lines.size()){lines.push_back(u"["+app+u"]");end=lines.size();}
        lines.insert(lines.begin()+static_cast<std::ptrdiff_t>(end),key+u"="+*value);
    }
    std::vector<std::uint8_t> bytes()const{
        std::u16string text;for(const auto& line:lines){text+=line;text+=u"\r\n";}
        if(encoding==Encoding::Utf16LE || encoding==Encoding::Utf16BE){auto output=utf16_bytes(text);output.resize(output.size()-2);output.insert(output.begin(),{0xff,0xfe});if(encoding==Encoding::Utf16BE)for(std::size_t i=0;i<output.size();i+=2)std::swap(output[i],output[i+1]);return output;}
        const auto encoded=encoding==Encoding::Utf8Bom?utf16_to_utf8(text):utf16_to_ansi(text);std::vector<std::uint8_t> output;if(encoding==Encoding::Utf8Bom)output={0xef,0xbb,0xbf};output.insert(output.end(),encoded.begin(),encoded.end());return output;
    }
};
std::vector<std::uint8_t> read_file(const HostFile& file){
    const auto info=file.information();if(info.error)throw ProfileFailure{info.error};if(info.metadata.size>WindowsServices::output_limit)throw ProfileFailure{223};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.metadata.size));std::size_t at=0;
    while(at<bytes.size()){const auto read=file.read(std::span(bytes).subspan(at),at);if(read.error)throw ProfileFailure{read.error};if(!read.bytes)throw ProfileFailure{1117};at+=static_cast<std::size_t>(read.bytes);}return bytes;
}
// .W 節以 UTF-7 表示 Unicode。沿用 ICU 轉碼，INI 本身的 BOM 與編碼由 Profile 保存。
std::u16string shell_ini_utf7(std::u16string_view text,bool encode){
    if(text.empty())return {};
    const char* from=encode?"UTF-8":"UTF-7";
    const char* to=encode?"UTF-7":"UTF-8";
    const auto input=utf16_to_utf8(text);
    UErrorCode status=U_ZERO_ERROR;
    const auto length=ucnv_convert(to,from,nullptr,0,input.data(),static_cast<int32_t>(input.size()),&status);
    if(status!=U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status))throw ProfileFailure{1113};
    std::string output(static_cast<std::size_t>(length),'\0');status=U_ZERO_ERROR;
    ucnv_convert(to,from,output.data(),length,input.data(),static_cast<int32_t>(input.size()),&status);
    if(U_FAILURE(status))throw ProfileFailure{1113};
    return utf8_to_utf16(output);
}
std::uint32_t integer(std::u16string_view value){
    value=trim(value);bool negative=false;if(!value.empty() && (value.front()==u'+' || value.front()==u'-')){negative=value.front()==u'-';value.remove_prefix(1);}unsigned base=10;
    if(value.size()>2 && value[0]==u'0'){const auto prefix=value[1];if(prefix==u'x' || prefix==u'X')base=16;else if(prefix==u'b' || prefix==u'B')base=2;else if(prefix==u'o' || prefix==u'O')base=8;if(base!=10)value.remove_prefix(2);}
    std::uint32_t result=0;for(const auto c:value){const auto digit=c>=u'0' && c<=u'9'?static_cast<unsigned>(c-u'0'):c>=u'a' && c<=u'f'?static_cast<unsigned>(c-u'a'+10):c>=u'A' && c<=u'F'?static_cast<unsigned>(c-u'A'+10):16;if(digit>=base)break;result=result*base+digit;}return negative?0U-result:result;
}
}
std::optional<std::uint32_t> WindowsServices::invoke_profile(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall call(state,memory,service);
    const auto symbol=service.symbol;
    const bool shell=service.family=="shlwapi.dll",select_key=symbol=="#473" || symbol=="#474";
    const bool wide=shell || symbol.ends_with('W'),writing=symbol.starts_with("Write") || symbol=="#295" || symbol=="#474";
    // Shell 讀取介面省略預設字串，將其參數對齊到既有 PrivateProfile 介面。
    const auto arg=[&](unsigned index)->std::uint64_t{
        if(shell && !writing && index>=2)return index==2?0:call(index-1);
        return call(index);
    };
    const bool number=symbol.starts_with("GetPrivateProfileInt") || symbol.starts_with("GetProfileInt"),names=symbol.starts_with("GetPrivateProfileSectionNames");
    const bool system_profile=symbol.starts_with("GetProfile") || symbol.starts_with("WriteProfile");
    const bool section=symbol.find("Section")!=symbol.npos && !names;
    // Get／WritePrivateProfileStruct：值為結構各位元組的十六進位文字，末尾兩位是位元組和的低八位元檢查碼。
    const bool structure=symbol.find("Struct")!=symbol.npos;
    const auto read=[&](std::uint64_t p){return p?(wide?read_utf16(memory,GuestAddress{p}):ansi_to_utf16_lossy(read_ansi(memory,p))):std::u16string{};};
    const auto copy=[&](std::uint64_t pointer,std::uint32_t capacity,std::u16string text,bool multiple)->std::uint64_t{
        if(!pointer || !capacity)return 0;
        auto encoded=wide?utf16_bytes(text):[&]{const auto utf8=utf16_to_ansi(text);std::vector<std::uint8_t> b(utf8.begin(),utf8.end());b.push_back(0);return b;}();
        const auto width=wide?2U:1U;const auto length=encoded.size()/width-1;const auto terminators=multiple && capacity>1?2U:1U;
        std::size_t result=length,units=length+1;
        if(multiple && text.empty()){result=0;units=terminators;encoded.resize(units*width);}
        else if(units>capacity){result=capacity-terminators;units=capacity;encoded.resize(units*width);std::fill(encoded.begin()+static_cast<std::ptrdiff_t>(result*width),encoded.end(),0);}
        memory.write(GuestAddress{pointer},std::span(encoded).first(units*width));return result;
    };
    const auto filename_index=structure?4U:writing?(section?2U:3U):names?2U:number || section?3U:5U;
    const bool multiple=!structure && (names || section || (!writing && !number && (!arg(0) || !arg(1))));
    std::uint64_t result=0;
    const auto failure=[&](std::uint32_t error){
        last_error_=error;result=0;
        if(!writing && !number && !structure)(void)copy(arg(names?0U:section?1U:3U),static_cast<std::uint32_t>(arg(names?1U:section?2U:4U)),u"",multiple);
    };
    try{
        const bool marked=select_key && arg(1) && memory.load(GuestAddress{arg(1)},16)=='@';
        const bool shadow=shell && arg(0) && arg(1) && (!select_key || marked);
        const auto key_address=arg(1)+(marked?2U:0U);
        auto path=system_profile || !arg(filename_index)?std::u16string(u"win.ini"):read(arg(filename_index));
        if(path.find(u':')==path.npos && !path.starts_with(u'\\') && !path.starts_with(u'/'))path=u"C:\\Windows\\"+path;
        const auto filename=utf16_to_utf8(path);
        if(writing){
            if(!runtime_.network.external_io)throw HostCallFault("比較模式不允許 INI 宿主寫入");
            if(!arg(0) && !arg(1) && (section || !arg(2))){last_error_=0;call.return_to(state,0);return std::nullopt;}
            if(!arg(0))throw ProfileFailure{87};const auto app=read(arg(0));if(app.find_first_of(u"\r\n[]")!=app.npos)throw ProfileFailure{87};
            // 先驗證 guest 輸入，避免無效參數產生空檔案或部分修改。
            std::vector<std::u16string> replacement;std::u16string key,value;
            if(section && arg(1)){
                replacement.push_back(u"["+app+u"]");std::uint64_t at=arg(1);std::size_t total=0;
                for(;;){const auto ansi=wide?std::string{}:read_ansi(memory,at);auto line=wide?read_utf16(memory,GuestAddress{at}):utf8_to_utf16_lossy(ansi);if(line.empty())break;const auto length=(wide?line.size()*2:ansi.size())+(wide?2U:1U);if(length>output_limit-total)throw ProfileFailure{223};total+=length;at+=length;if(line.find_first_of(u"\r\n")!=line.npos)throw ProfileFailure{87};replacement.push_back(std::move(line));}
            }else if(!section && arg(1)){
                key=read(key_address);if(key.empty() || key.find_first_of(u"\r\n=")!=key.npos)throw ProfileFailure{87};
                if(structure && arg(2)){
                    const auto size=static_cast<std::uint32_t>(arg(3));if(size>output_limit/2)throw ProfileFailure{87};std::vector<std::uint8_t> bytes(size);memory.read(GuestAddress{arg(2)},bytes);
                    unsigned sum=0;constexpr char16_t digits[]=u"0123456789ABCDEF";for(const auto b:bytes){value+=digits[b>>4];value+=digits[b&15];sum+=b;}value+=digits[(sum>>4)&15];value+=digits[sum&15];
                }else value=read(arg(2));
                if(value.find_first_of(u"\r\n")!=value.npos)throw ProfileFailure{87};
            }
            // 新檔案採用 UTF-8 ACP；不合法 UTF-16 必須在建立或修改檔案前失敗。
            if(wide){(void)utf16_to_utf8(app);(void)utf16_to_utf8(key);(void)utf16_to_utf8(value);for(const auto& line:replacement)(void)utf16_to_utf8(line);}
            const auto opened=filesystem_.open_host(filename,0xc0000000U,4,0);if(opened.error)throw ProfileFailure{opened.error};
            ProfileLock lock(*opened.file,true);Profile profile(read_file(*opened.file));auto [begin,end]=profile.section(app);
            if(section || !arg(1)){
                profile.lines.erase(profile.lines.begin()+static_cast<std::ptrdiff_t>(begin),profile.lines.begin()+static_cast<std::ptrdiff_t>(end));
                profile.lines.insert(profile.lines.begin()+static_cast<std::ptrdiff_t>(begin),replacement.begin(),replacement.end());
            }else {
                profile.set_value(app,key,arg(2)?&value:nullptr);
                if(shadow){
                    const bool non_ascii=std::any_of(value.begin(),value.end(),[](char16_t c){return c>0x7f;});
                    const auto encoded=non_ascii?shell_ini_utf7(value,true):std::u16string{};
                    profile.set_value(app+u".A",key,non_ascii?&value:nullptr);
                    profile.set_value(app+u".W",key,non_ascii?&encoded:nullptr);
                }
            }
            const auto bytes=profile.bytes();if(bytes.size()>output_limit)throw ProfileFailure{223};std::size_t at=0;
            while(at<bytes.size()){const auto written=opened.file->write(std::span(bytes).subspan(at),at);if(written.error)throw ProfileFailure{written.error};if(!written.bytes)throw ProfileFailure{1117};at+=static_cast<std::size_t>(written.bytes);}
            if(const auto error=opened.file->truncate(bytes.size()))throw ProfileFailure{error};if(const auto error=opened.file->flush())throw ProfileFailure{error};last_error_=0;result=1;
        }else {
            std::vector<std::uint8_t> bytes;std::uint32_t error=0;
            if(filesystem_.writable()){
                const auto opened=filesystem_.open_host(filename,0x80000000U,3,0);error=opened.error;if(!error){ProfileLock lock(*opened.file,false);bytes=read_file(*opened.file);}
            }else {const auto file=filesystem_.open(filename);error=file.error;if(!error){if(file.bytes->size()>output_limit)throw ProfileFailure{223};bytes=*file.bytes;}}
            if(error && error!=2 && error!=3)throw ProfileFailure{error};last_error_=error;
            const Profile profile(bytes);const auto app=names?std::u16string{}:read(arg(0));
            if(structure){
                if(!arg(0) || !arg(1))throw ProfileFailure{87};const auto size=static_cast<std::uint32_t>(arg(3));
                const auto value=profile.value(app,read(arg(1)));if(!value)throw ProfileFailure{2};
                const auto text=trim(*value);if(text.size()!=static_cast<std::size_t>(size)*2+2)throw ProfileFailure{24};
                const auto nibble=[](char16_t c)->int{return c>=u'0'&&c<=u'9'?c-u'0':c>=u'a'&&c<=u'f'?c-u'a'+10:c>=u'A'&&c<=u'F'?c-u'A'+10:-1;};
                std::vector<std::uint8_t> decoded;unsigned sum=0;
                for(std::size_t i=0;i<text.size();i+=2){const auto high=nibble(text[i]),low=nibble(text[i+1]);if(high<0 || low<0)throw ProfileFailure{13};decoded.push_back(static_cast<std::uint8_t>(high*16+low));}
                const auto check=decoded.back();decoded.pop_back();for(const auto b:decoded)sum+=b;if((sum&255U)!=check)throw ProfileFailure{13};
                if(size){if(!arg(2))throw ProfileFailure{87};memory.write(GuestAddress{arg(2)},decoded);}
                last_error_=0;result=1;
            }else if(number){if(!arg(0) || !arg(1))throw ProfileFailure{87};const auto value=profile.value(app,read(arg(1)));result=!value || value->empty()?static_cast<std::uint32_t>(arg(2)):
                system_profile && trim(*value).starts_with(u'-')?0:integer(*value);}
            else {
                const auto output=arg(names?0U:section?1U:3U);const auto capacity=static_cast<std::uint32_t>(arg(names?1U:section?2U:4U));
                if(section && !arg(0))throw ProfileFailure{87};std::u16string text;
                if(names || (!section && !arg(0))){for(const auto& line:profile.lines)if(const auto h=heading(line)){text+=*h;text+=u'\0';}}
                else if(section || !arg(1)){const auto [begin,end]=profile.section(app);for(auto i=begin+1;i<end;++i)if(const auto item=entry(profile.lines[i])){text+=item->first;if(section){text+=u'=';text+=item->second;}text+=u'\0';}}
                else if(shadow){
                    const auto key=read(key_address),encoded=profile.value(app+u".W",key).value_or(u"");
                    text=encoded.empty()?profile.value(app,key).value_or(u""):shell_ini_utf7(encoded,false);
                }
                else if(const auto value=profile.value(app,read(key_address)))text=*value;
                else {text=read(arg(2));while(!text.empty() && text.back()==u' ')text.pop_back();}
                if(symbol=="#294" && multiple){const auto end=text.find(u'\0');if(end!=text.npos)text.resize(end);}
                result=copy(output,capacity,std::move(text),multiple && symbol!="#294");
            }
        }
    }catch(const ProfileFailure& error){failure(error.error);}
     catch(const std::invalid_argument&){failure(1113);}
    call.return_to(state,result);return std::nullopt;
}
}
