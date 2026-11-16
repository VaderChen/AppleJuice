#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "service_callback_task.hpp"
#include "rtl_compression.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace x64arm {
namespace {
constexpr std::uint32_t status_invalid_parameter=0xc000000dU,status_not_found=0xc0000034U,status_buffer_too_small=0xc0000023U;
// IPv4 文字：嚴格模式只接受四段十進位；寬鬆模式另接受 0x／0 前綴的十六／八進位與 1～3 段（inet_addr 形式）。
bool ipv4_component(std::u16string_view text,std::size_t& at,bool strict,std::uint32_t& value){
    if(at<text.size() && text[at]==u'.')return false;
    unsigned base=10;
    if(!strict && at<text.size() && text[at]==u'0' && at+1<text.size()){
        if(text[at+1]==u'x' || text[at+1]==u'X'){at+=2;base=16;}
        else if(text[at+1]>=u'0' && text[at+1]<=u'9'){at+=1;base=8;}
    }
    bool any=false;std::uint64_t current=0;
    for(;at<text.size();++at){
        const auto c=text[at];unsigned digit=16;
        if(c>=u'0' && c<=u'9')digit=c-u'0';else if(c>=u'a' && c<=u'f')digit=c-u'a'+10;else if(c>=u'A' && c<=u'F')digit=c-u'A'+10;
        if(digit>=base)break;
        current=current*base+digit;any=true;if(current>UINT32_MAX)return false;
    }
    if(any)value=static_cast<std::uint32_t>(current);return any;
}
// 回傳是否成功；terminator 為停止位置（失敗時同樣更新）。位址為網路位元組順序的四個位元組。
bool ipv4_parse(std::u16string_view text,bool strict,std::size_t& terminator,std::array<std::uint8_t,4>& address){
    std::array<std::uint32_t,4> fields{};unsigned count=0;std::size_t at=0;
    const auto fail=[&]{terminator=at;return false;};
    for(;;){
        if(!ipv4_component(text,at,strict,fields[count]))return fail();
        ++count;if(at>=text.size() || text[at]!=u'.')break;
        if(count==4)return fail();
        ++at;
    }
    if(strict && count<4)return fail();
    std::uint32_t value=0;
    switch(count){
    case 4:if(fields[0]>255 || fields[1]>255 || fields[2]>255 || fields[3]>255)return fail();value=(fields[0]<<24)|(fields[1]<<16)|(fields[2]<<8)|fields[3];break;
    case 3:if(fields[0]>255 || fields[1]>255 || fields[2]>0xffffU)return fail();value=(fields[0]<<24)|(fields[1]<<16)|fields[2];break;
    case 2:if(fields[0]>255 || fields[1]>0xffffffU)return fail();value=(fields[0]<<24)|fields[1];break;
    default:value=fields[0];break;
    }
    for(unsigned i=0;i<4;++i)address[i]=static_cast<std::uint8_t>(value>>(24-8*i));
    terminator=at;return true;
}
// IPv6 文字：小寫十六進位、去掉前導零；最長且至少兩組的連續零以「::」取代（同長取第一個）。
// IPv4 對應（::ffff:a.b.c.d）、相容（::a.b.c.d，第 7 組非零）、轉譯（::ffff:0:a.b.c.d）與 ISATAP（…:5efe:a.b.c.d）以點分十進位結尾。
std::u16string ipv6_format(const std::array<std::uint8_t,16>& bytes){
    std::array<std::uint16_t,8> words{};for(unsigned i=0;i<8;++i)words[i]=static_cast<std::uint16_t>((bytes[2*i]<<8)|bytes[2*i+1]);
    const bool zero_prefix=!words[0] && !words[1] && !words[2] && !words[3];
    unsigned groups=8;
    if((zero_prefix && !words[4] && (words[5]==0xffff || (!words[5] && words[6]))) || (zero_prefix && words[4]==0xffff && !words[5]) || ((!words[4] || words[4]==0x200) && words[5]==0x5efe))groups=6;
    std::size_t best=groups,best_length=0;
    for(std::size_t i=0;i<groups;){if(words[i]){++i;continue;}auto j=i;while(j<groups && !words[j])++j;if(j-i>best_length){best=i;best_length=j-i;}i=j;}
    if(best_length<2)best=groups;
    std::u16string text;
    for(std::size_t i=0;i<groups;){
        if(i==best){text+=u"::";i+=best_length;continue;}
        if(!text.empty() && text.back()!=u':')text+=u':';
        char hex[8];std::snprintf(hex,sizeof hex,"%x",words[i]);for(const char* c=hex;*c;++c)text+=static_cast<char16_t>(*c);++i;
    }
    if(groups==6){if(!text.empty() && text.back()!=u':')text+=u':';for(unsigned i=12;i<16;++i){if(i>12)text+=u'.';for(const auto c:std::to_string(bytes[i]))text+=static_cast<char16_t>(c);}}
    return text;
}
bool hex_digit(char16_t c){return (c>=u'0' && c<=u'9') || (c>=u'a' && c<=u'f') || (c>=u'A' && c<=u'F');}
// 解析到第一個不屬於位址的字元：每組最多四位十六進位，最多一個「::」，結尾可為點分十進位 IPv4。
bool ipv6_parse(std::u16string_view text,std::size_t& end,std::array<std::uint8_t,16>& address){
    std::array<std::uint16_t,8> words{};std::size_t count=0,at=0,gap=8;bool tail=false;
    const auto fail=[&]{end=at;return false;};
    if(text.size()>=2 && text[0]==u':' && text[1]==u':'){gap=0;at=2;}
    else if(!text.empty() && text[0]==u':')return fail();
    while(count<8){
        // 數字後接「.」時改以 IPv4 解析最後兩組。
        std::size_t probe=at;while(probe<text.size() && hex_digit(text[probe]))++probe;
        if(probe<text.size() && text[probe]==u'.' && probe>at){
            if(count>6)return fail();std::size_t stop=0;std::array<std::uint8_t,4> v4{};
            if(!ipv4_parse(text.substr(at),true,stop,v4)){at+=stop;return fail();}
            words[count++]=static_cast<std::uint16_t>((v4[0]<<8)|v4[1]);words[count++]=static_cast<std::uint16_t>((v4[2]<<8)|v4[3]);at+=stop;tail=true;break;
        }
        if(probe==at){if(gap==count && count>0 && text[at-1]==u':')break;if(gap==0 && count==0)break;return fail();}
        if(probe-at>4){at+=4;return fail();}
        std::uint32_t value=0;for(auto i=at;i<probe;++i){const auto c=text[i];value=value*16+static_cast<std::uint32_t>(c<=u'9'?c-u'0':(c|0x20)-u'a'+10);}
        words[count++]=static_cast<std::uint16_t>(value);at=probe;
        if(at<text.size() && text[at]==u':'){
            if(at+1<text.size() && text[at+1]==u':'){if(gap!=8)return fail();gap=count;at+=2;continue;}
            if(count==8)break;
            if(at+1>=text.size() || !hex_digit(text[at+1]))return fail();
            ++at;continue;
        }
        break;
    }
    (void)tail;
    if(gap!=8){if(count==8)return fail();std::array<std::uint16_t,8> expanded{};const auto after=count-gap;
        for(std::size_t i=0;i<gap;++i)expanded[i]=words[i];for(std::size_t i=0;i<after;++i)expanded[8-after+i]=words[gap+i];words=expanded;}
    else if(count!=8)return fail();
    for(unsigned i=0;i<8;++i){address[2*i]=static_cast<std::uint8_t>(words[i]>>8);address[2*i+1]=static_cast<std::uint8_t>(words[i]);}
    end=at;return true;
}
// NtQueryLicenseValue 的產品政策：與 GetProductInfo（Professional，0x30）及 RtlGetDeviceFamilyInfoEnum（Windows.Desktop）同一份虛擬 Windows 設定。
// 型別為登錄值型別（REG_SZ=1、REG_DWORD=4）。未列出的名稱與 Windows 相同回 STATUS_OBJECT_NAME_NOT_FOUND。
struct LicenseValue {std::u16string_view name;std::uint32_t type;std::uint32_t number;std::u16string_view text;};
constexpr LicenseValue license_values[]{
    {u"Kernel-ProductInfo",4,0x30,{}},
    {u"Kernel-EditionName",1,0,u"Professional"},
    {u"Kernel-OneCore-DeviceFamilyID",4,3,{}},
    {u"Security-SPP-GenuineLocalStatus",4,1,{}},
};
}
std::span<const WindowsServiceDescriptor> ntdll_misc_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"ntdll.dll","RtlIpv4AddressToStringA",2},{"ntdll.dll","RtlIpv4AddressToStringW",2},
        {"ntdll.dll","RtlIpv4AddressToStringExA",4},{"ntdll.dll","RtlIpv4AddressToStringExW",4},
        {"ntdll.dll","RtlIpv4StringToAddressA",4},{"ntdll.dll","RtlIpv4StringToAddressW",4},
        {"ntdll.dll","RtlIpv4StringToAddressExA",4},{"ntdll.dll","RtlIpv4StringToAddressExW",4},
        {"ntdll.dll","RtlIpv6AddressToStringA",2},{"ntdll.dll","RtlIpv6AddressToStringW",2},
        {"ntdll.dll","RtlIpv6AddressToStringExA",5},{"ntdll.dll","RtlIpv6AddressToStringExW",5},
        {"ntdll.dll","RtlIpv6StringToAddressA",3},{"ntdll.dll","RtlIpv6StringToAddressW",3},
        {"ntdll.dll","RtlIpv6StringToAddressExA",4},{"ntdll.dll","RtlIpv6StringToAddressExW",4},
        {"ntdll.dll","RtlCompressBuffer",8,0,0,0x1},{"ntdll.dll","RtlDecompressBuffer",6,0,0,0x1},{"ntdll.dll","RtlDecompressFragment",8,0,0,0x1},
        {"ntdll.dll","RtlGetCompressionWorkSpaceSize",3,0,0,0x1},
        {"ntdll.dll","RtlDllShutdownInProgress",0},{"ntdll.dll","NtQueryLicenseValue",5},
        {"ntdll.dll","DbgPrint",1,0,0,0,true},{"ntdll.dll","DbgPrintEx",3,0,0,0,true},{"ntdll.dll","vDbgPrintEx",4},{"ntdll.dll","vDbgPrintExWithPrefix",5},
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_ntdll_misc(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const auto name=service.symbol;
    if(name.starts_with("DbgPrint") || name.starts_with("vDbgPrintEx"))return invoke_debug_print(state,memory,service);
    const WindowsCall arg(state,memory,service);const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto done=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const bool wide=name.ends_with('W');
    const auto read_text=[&](std::uint64_t address){
        if(wide)return read_utf16(memory,GuestAddress{address},256);
        std::u16string text;for(const auto c:read_ansi(memory,address,256))text.push_back(static_cast<unsigned char>(c));return text;
    };
    const auto write_text=[&](std::uint64_t address,std::u16string_view text){
        for(std::size_t i=0;i<text.size();++i){if(wide)memory.store(GuestAddress{address+i*2},16,text[i]);else memory.store(GuestAddress{address+i},8,text[i]);}
        if(wide)memory.store(GuestAddress{address+text.size()*2},16,0);else memory.store(GuestAddress{address+text.size()},8,0);
    };
    const auto format=[&](std::uint64_t address){
        std::array<std::uint8_t,4> bytes{};memory.read(GuestAddress{address},bytes);std::u16string text;
        for(unsigned i=0;i<4;++i){if(i)text+=u'.';for(const auto c:std::to_string(bytes[i]))text+=static_cast<char16_t>(c);}
        return text;
    };
    try{
        if(name=="RtlDllShutdownInProgress")return done(runtime_.exiting?1:0);
        if(name.starts_with("RtlIpv4AddressToString")){
            if(!(name.find("Ex")!=name.npos)){
                // 回傳指向結尾 NUL 的指標，供呼叫者接續寫入。
                const auto text=format(arg(0));write_text(arg(1),text);return done(arg(1)+text.size()*(wide?2:1));
            }
            if(!arg(0) || !arg(3))return done(status_invalid_parameter);
            auto text=format(arg(0));const auto port=static_cast<std::uint16_t>(arg(1));
            if(port){const auto host=static_cast<std::uint16_t>((port>>8)|(port<<8));text+=u':';for(const auto c:std::to_string(host))text+=static_cast<char16_t>(c);}
            const auto needed=static_cast<std::uint32_t>(text.size()+1);
            if(!arg(2) || memory.load(GuestAddress{arg(3)},32)<needed){memory.store(GuestAddress{arg(3)},32,needed);return done(status_invalid_parameter);}
            write_text(arg(2),text);memory.store(GuestAddress{arg(3)},32,needed);return done(0);
        }
        if(name.starts_with("RtlIpv4StringToAddress")){
            const bool ex=(name.find("Ex")!=name.npos);const auto strict=(arg(1)&0xffU)!=0;
            const auto output=arg(ex?2:3),extra=arg(ex?3:2);
            if(!arg(0) || !output || !extra)return done(status_invalid_parameter);
            const auto text=read_text(arg(0));std::size_t terminator=0;std::array<std::uint8_t,4> address{};
            const bool parsed=ipv4_parse(text,strict,terminator,address);
            if(!ex){
                memory.store(GuestAddress{extra},bits,arg(0)+terminator*(wide?2:1));
                if(!parsed)return done(status_invalid_parameter);
                memory.write(GuestAddress{output},address);return done(0);
            }
            if(!parsed)return done(status_invalid_parameter);
            std::uint16_t port=0;
            if(terminator<text.size()){
                if(text[terminator]!=u':')return done(status_invalid_parameter);
                std::size_t at=terminator+1;std::uint32_t value=0;
                if(!ipv4_component(text,at,false,value) || !value || value>0xffffU || at!=text.size())return done(status_invalid_parameter);
                port=static_cast<std::uint16_t>(((value&0xffU)<<8)|(value>>8));
            }
            memory.write(GuestAddress{output},address);memory.store(GuestAddress{extra},16,port);return done(0);
        }
        if(name.starts_with("RtlIpv6AddressToString")){
            std::array<std::uint8_t,16> bytes{};
            if(!(name.find("Ex")!=name.npos)){memory.read(GuestAddress{arg(0)},bytes);const auto text=ipv6_format(bytes);write_text(arg(1),text);return done(arg(1)+text.size()*(wide?2:1));}
            // (Address, ScopeId, Port（網路位元組順序）, AddressString, AddressStringLength*)：有連接埠時為「[位址%範圍]:埠」。
            if(!arg(0) || !arg(4))return done(status_invalid_parameter);memory.read(GuestAddress{arg(0)},bytes);
            auto text=ipv6_format(bytes);const auto scope=static_cast<std::uint32_t>(arg(1));const auto port=static_cast<std::uint16_t>(arg(2));
            if(scope){text+=u'%';for(const auto c:std::to_string(scope))text+=static_cast<char16_t>(c);}
            if(port){text=u"["+text+u"]:";for(const auto c:std::to_string(static_cast<std::uint16_t>((port>>8)|(port<<8))))text+=static_cast<char16_t>(c);}
            const auto needed=static_cast<std::uint32_t>(text.size()+1);
            if(!arg(3) || memory.load(GuestAddress{arg(4)},32)<needed){memory.store(GuestAddress{arg(4)},32,needed);return done(status_invalid_parameter);}
            write_text(arg(3),text);memory.store(GuestAddress{arg(4)},32,needed);return done(0);
        }
        if(name.starts_with("RtlIpv6StringToAddress")){
            const bool ex=name.find("Ex")!=name.npos;std::array<std::uint8_t,16> address{};std::size_t end=0;
            if(!arg(0) || !arg(ex?1:2) || (!ex && !arg(1)))return done(status_invalid_parameter);
            const auto text=read_text(arg(0));const bool parsed=ipv6_parse(text,end,address);
            if(!ex){memory.store(GuestAddress{arg(1)},bits,arg(0)+end*(wide?2:1));if(!parsed)return done(status_invalid_parameter);memory.write(GuestAddress{arg(2)},address);return done(0);}
            // (AddressString, Address*, ScopeId*, Port*)：接受「位址」、「位址%範圍」或「[位址%範圍]:埠」，整個字串都必須被使用。
            if(!arg(2) || !arg(3))return done(status_invalid_parameter);
            const bool bracket=!text.empty() && text[0]==u'[';std::size_t at=bracket?1:0;
            if(!ipv6_parse(std::u16string_view(text).substr(at),end,address))return done(status_invalid_parameter);at+=end;
            std::uint32_t scope=0,port=0;
            const auto number=[&](std::uint32_t& value,std::uint32_t maximum){const auto begin=at;std::uint64_t current=0;while(at<text.size() && text[at]>=u'0' && text[at]<=u'9'){current=current*10+(text[at]-u'0');if(current>maximum)return false;++at;}value=static_cast<std::uint32_t>(current);return at>begin;};
            if(at<text.size() && text[at]==u'%'){++at;if(!number(scope,UINT32_MAX))return done(status_invalid_parameter);}
            if(bracket){if(at>=text.size() || text[at]!=u']')return done(status_invalid_parameter);++at;
                if(at<text.size()){if(text[at]!=u':')return done(status_invalid_parameter);++at;if(!number(port,0xffff) || !port)return done(status_invalid_parameter);}}
            if(at!=text.size())return done(status_invalid_parameter);
            memory.write(GuestAddress{arg(1)},address);memory.store(GuestAddress{arg(2)},32,scope);memory.store(GuestAddress{arg(3)},16,static_cast<std::uint16_t>(((port&0xffU)<<8)|(port>>8)));return done(0);
        }
        if(name=="RtlGetCompressionWorkSpaceSize" || name=="RtlCompressBuffer" || name=="RtlDecompressBuffer" || name=="RtlDecompressFragment"){
            // 格式：LZNT1（2）與 XPRESS（3）；NONE／DEFAULT 為無效參數，XPRESS_HUFF（4）與其他格式回 STATUS_UNSUPPORTED_COMPRESSION。
            // 引擎：STANDARD（0）與 MAXIMUM（0x100）產生相同格式的資料。工作區不使用（宿主端自行配置）。
            const auto raw=static_cast<std::uint32_t>(arg(0));const auto kind=raw&0xffU,engine=raw&0xff00U;
            if(kind<2)return done(status_invalid_parameter);if(kind!=2 && kind!=3)return done(0xc000025fU);
            if(engine!=0 && engine!=0x100)return done(0xc00000bbU);
            if(name=="RtlGetCompressionWorkSpaceSize"){
                if(!arg(1) || !arg(2))return done(status_invalid_parameter);
                memory.store(GuestAddress{arg(1)},32,kind==2?0x10000U:0x20000U);memory.store(GuestAddress{arg(2)},32,kind==2?0x1000U:0U);return done(0);
            }
            const auto input_size=static_cast<std::uint32_t>(arg(name=="RtlCompressBuffer"?2:4));const auto output_size=static_cast<std::uint32_t>(arg(name=="RtlCompressBuffer"?4:2));
            if(input_size>output_limit)return done(0xc0000017U);
            std::vector<std::uint8_t> input(input_size);memory.read(GuestAddress{arg(name=="RtlCompressBuffer"?1:3)},input);
            if(name=="RtlCompressBuffer"){
                const auto output=kind==2?rtl_compression::lznt1_compress(input,output_size):rtl_compression::xpress_compress(input,output_size);
                if(!output)return done(status_buffer_too_small);
                memory.write(GuestAddress{arg(3)},*output);if(arg(6))memory.store(GuestAddress{arg(6)},32,output->size());
                return done(std::all_of(input.begin(),input.end(),[](std::uint8_t b){return !b;})?0x117U:0U); // STATUS_BUFFER_ALL_ZEROS
            }
            // 輸出緩衝不足時只解壓到可容納的長度；片段版本先略過 FragmentOffset 位元組。
            const bool fragment=name=="RtlDecompressFragment";const std::size_t skip=fragment?static_cast<std::uint32_t>(arg(5)):0U;
            const auto limit=skip+output_size;
            const auto output=kind==2?rtl_compression::lznt1_decompress(input,limit):rtl_compression::xpress_decompress(input,limit);
            if(!output)return done(0xc0000242U);
            const auto begin=std::min(skip,output->size());const auto moved=output->size()-begin;
            if(moved)memory.write(GuestAddress{arg(1)},std::span(*output).subspan(begin));
            if(arg(fragment?6:5))memory.store(GuestAddress{arg(fragment?6:5)},32,moved);return done(0);
        }
        if(name=="NtQueryLicenseValue"){
            // (PUNICODE_STRING ValueName, PULONG Type, PVOID Data, ULONG DataSize, PULONG ResultDataSize)
            if(!arg(0) || !arg(4))return done(status_invalid_parameter);
            const auto length=memory.load(GuestAddress{arg(0)},16),buffer=memory.load(GuestAddress{arg(0)+unit},bits);
            if(!length || !buffer)return done(status_invalid_parameter);
            std::u16string query;for(std::uint64_t i=0;i+1<length;i+=2)query.push_back(static_cast<char16_t>(memory.load(GuestAddress{buffer+i},16)));
            static const bool trace=std::getenv("X64ARM_TRACE_LICENSE")!=nullptr;
            if(trace)std::fprintf(stderr,"[license] %s\n",utf16_to_utf8(query).c_str());
            const auto fold=[](std::u16string_view text){std::u16string lower(text);for(auto& c:lower)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);return lower;};
            const auto key=fold(query);
            const auto found=std::find_if(std::begin(license_values),std::end(license_values),[&](const auto& row){return fold(row.name)==key;});
            if(found==std::end(license_values))return done(status_not_found);
            std::vector<std::uint8_t> data;
            if(found->type==4)for(unsigned i=0;i<4;++i)data.push_back(static_cast<std::uint8_t>(found->number>>(8*i)));
            else {for(const auto c:found->text){data.push_back(static_cast<std::uint8_t>(c));data.push_back(static_cast<std::uint8_t>(c>>8));}data.push_back(0);data.push_back(0);}
            memory.store(GuestAddress{arg(4)},32,data.size());if(arg(1))memory.store(GuestAddress{arg(1)},32,found->type);
            if(static_cast<std::uint32_t>(arg(3))<data.size())return done(status_buffer_too_small);
            if(!arg(2))return done(status_invalid_parameter);
            memory.write(GuestAddress{arg(2)},data);return done(0);
        }
    }catch(const MemoryFault&){
        if((name.starts_with("RtlIpv4AddressToString") || name.starts_with("RtlIpv6AddressToString")) && !(name.find("Ex")!=name.npos))throw;
        return done(0xc0000005U);
    }
    throw HostCallFault("未知的 ntdll 雜項服務");
}
// DbgPrint 家族：依偵錯篩選（預設只放行 ERROR 層級，DbgPrint 屬 DEFAULT 元件的 INFO 層級）決定是否輸出；
// 放行且有偵錯器（或開啟 X64ARM_TRACE_DEBUG_STRING）時以 _vsnprintf 格式化到 512 位元組，再交給 OutputDebugStringA，
// 與一般偵錯字串共用偵錯事件與診斷輸出。沒有偵錯器時 Windows 也不留下可觀察的結果，回 STATUS_SUCCESS。
std::optional<std::uint32_t> WindowsServices::invoke_debug_print(GuestState& entry_state,GuestMemory& entry_memory,const WindowsServiceDescriptor& service){
    static const bool trace=std::getenv("X64ARM_TRACE_DEBUG_STRING")!=nullptr;
    const WindowsCall entry_call(entry_state,entry_memory,service);
    const bool plain=service.symbol=="DbgPrint";
    // vDbgPrintExWithPrefix(Prefix, ComponentId, Level, Format, arglist)：輸出前加上前綴字串。
    const bool prefixed=service.symbol=="vDbgPrintExWithPrefix";
    const auto level=plain?3U:static_cast<std::uint32_t>(entry_call(prefixed?2:1));
    const auto mask=level>31?level:(1U<<level);
    if(!(mask&1U) || (!debugger_attached() && !trace)){entry_call.return_to(entry_state,0);return std::nullopt;}
    return service_callback::invoke<std::optional<std::uint32_t>>(
        this,threads_.current,entry_state,entry_memory,"DbgPrint",
        [this](GuestState& state,GuestMemory& memory,std::uint64_t target,std::vector<std::uint64_t> args,std::uint64_t id){
            start_callbacks(state,memory,{{target,std::move(args)}},Continuation::NestedResult,id);
        },
        [this,service,plain,prefixed](GuestState& state,GuestMemory& memory)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(state,memory,service);
            const auto done=[&](std::uint32_t value){call.return_to(state,value);return std::optional<std::uint32_t>{};};
            const unsigned format_index=plain?0U:prefixed?3U:2U,first=format_index+1;
            std::uint64_t list=0;
            if(service.symbol=="vDbgPrintEx")list=call(3);
            else if(prefixed)list=call(4);
            else if(x86_)list=state.reg(Gpr::Rsp)+4+4*first;
            else {
                // 變動參數的暫存器部分寫回呼叫者保留的 home 區，使 va_list 連續。
                const auto sp=state.reg(Gpr::Rsp);const Gpr registers[]{Gpr::Rcx,Gpr::Rdx,Gpr::R8,Gpr::R9};
                for(unsigned i=first;i<4;++i)memory.store(GuestAddress{sp+8+8*i},64,state.reg(registers[i]));
                list=sp+8+8*first;
            }
            const auto buffer=allocate_heap(memory,512);if(!buffer)co_return done(0xc0000017U);
            struct ReleaseBuffer {WindowsServices& self;GuestState& state;GuestMemory& memory;std::uint64_t address;
                ~ReleaseBuffer(){(void)self.free_heap(memory,address,state);}} release_buffer{*this,state,memory,buffer};
            memory.store(GuestAddress{buffer+511},8,0);
            std::size_t prefix=0;
            if(prefixed && call(0)){const auto text=read_ansi(memory,call(0),255);prefix=text.size();
                memory.write(GuestAddress{buffer},std::span(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));}
            (void)co_await service_callback::call(resolve("crt","_vsnprintf").value,{buffer+prefix,511-prefix,call(format_index),list});
            memory.store(GuestAddress{buffer+511},8,0);
            (void)co_await service_callback::call(resolve("kernel32.dll","OutputDebugStringA").value,{buffer});
            co_return done(0);
        });
}
}
