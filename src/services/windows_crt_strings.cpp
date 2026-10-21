#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstdlib>

namespace x64arm {
std::span<const WindowsServiceDescriptor> crt_string_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"ntdll.dll","RtlMultiByteToUnicodeN",5},{"ntdll.dll","RtlOemToUnicodeN",5},
        {"ntdll.dll","RtlUnicodeToMultiByteN",5},{"ntdll.dll","RtlUnicodeToOemN",5},
        {"ntdll.dll","RtlMultiByteToUnicodeSize",3},{"ntdll.dll","RtlUnicodeToMultiByteSize",3},
        {"ntdll.dll","RtlInitUnicodeString",2},{"ntdll.dll","RtlInitUnicodeStringEx",2},
        {"ntdll.dll","RtlInitAnsiString",2},{"ntdll.dll","RtlInitAnsiStringEx",2},{"ntdll.dll","RtlInitString",2},
        {"ntdll.dll","RtlCreateUnicodeString",2},{"ntdll.dll","RtlCreateUnicodeStringFromAsciiz",2},
        {"ntdll.dll","RtlAnsiStringToUnicodeString",3},{"ntdll.dll","RtlUnicodeStringToAnsiString",3},
        {"ntdll.dll","RtlFreeUnicodeString",1},{"ntdll.dll","RtlFreeAnsiString",1},{"ntdll.dll","RtlFormatCurrentUserKeyPath",1},
        {"crt","_itoa",3},{"crt","_itoa_s",4},{"crt","_itow",3},{"crt","_itow_s",4},{"crt","_ltoa",3},{"crt","_ltoa_s",4},{"crt","_ltow",3},{"crt","_ltow_s",4},{"crt","_ultoa",3},{"crt","_ultoa_s",4},{"crt","_ultow",3},{"crt","_ultow_s",4},{"crt","_i64toa",3},{"crt","_i64toa_s",4},{"crt","_i64tow",3},{"crt","_i64tow_s",4},{"crt","_ui64toa",3},{"crt","_ui64toa_s",4},{"crt","_ui64tow",3},{"crt","_ui64tow_s",4},{"crt","_wtoi",1},{"crt","_wtol",1},{"crt","_wtoi64",1},{"crt","wcstol",3},{"crt","wcstoul",3},{"crt","wcstoll",3},{"crt","wcstoull",3},{"crt","_wcstoi64",3},{"crt","_wcstoui64",3},{"crt","mbstowcs",3},{"crt","wcstombs",3},
        {"crt","iswdigit",1},{"crt","strpbrk",2},{"crt","wcspbrk",2},{"crt","__strncnt",2},
        {"crt","mbsrtowcs",4},{"crt","wcsrtombs",4},
        {"crt","mbstowcs_s",5},{"crt","wcstombs_s",5},{"crt","mbsrtowcs_s",6},{"crt","wcsrtombs_s",6},
        {"crt","_mbstowcs_l",4},{"crt","_wcstombs_l",4},{"crt","_mbstowcs_s_l",6},{"crt","_wcstombs_s_l",6},
        {"crt","_strlwr",1},{"crt","_strupr",1},{"crt","_wcslwr",1},{"crt","_wcsupr",1},
        {"crt","_strlwr_s",2},{"crt","_strupr_s",2},{"crt","_wcslwr_s",2},{"crt","_wcsupr_s",2},
        {"crt","_strrev",1},{"crt","_wcsrev",1},{"crt","_wcsdup",1},{"crt","wcscpy",2},{"crt","wcsncpy",3},
        {"crt","strcoll",2},{"crt","wcscoll",2},{"crt","wcsxfrm",3},{"crt","strxfrm",3},
        {"crt","strcpy_s",3},{"crt","strncpy_s",4},{"crt","strcat_s",3},{"crt","strncat_s",4},
        {"crt","wcscpy_s",3},{"crt","wcsncpy_s",4},{"crt","wcscat_s",3},{"crt","wcsncat_s",4},
        {"crt","memcpy_s",4},{"crt","memmove_s",4},{"crt","wmemcpy_s",4},{"crt","wmemmove_s",4},
        {"crt","_swab",3},{"crt","swab",3},
        {"crt","_makepath",5},{"crt","_wmakepath",5},{"crt","_makepath_s",6},{"crt","_wmakepath_s",6},
        {"crt","_splitpath",5},{"crt","_wsplitpath",5},{"crt","_splitpath_s",9},{"crt","_wsplitpath_s",9},
        {"crt","__isascii",1},{"crt","_memicmp",3},{"crt","_mbscmp",2},{"crt","_mbstrlen",1},
        {"crt","strtok",2},{"crt","strtok_s",3},{"crt","wcstok_s",3},
        {"crt","sscanf",2},{"crt","sscanf_s",2},{"crt","swscanf",2},{"crt","swscanf_s",2},
        {"crt","vsscanf",3},{"crt","vsscanf_s",3},{"crt","vswscanf",3},{"crt","vswscanf_s",3},
        {"crt","_snscanf",3},{"crt","_snscanf_s",3},{"crt","_snwscanf",3},{"crt","_snwscanf_s",3},
        {"crt","__stdio_common_vsscanf",6,1},{"crt","__stdio_common_vswscanf",6,1},
        {"crt","fscanf",2},{"crt","fscanf_s",2},{"crt","vfscanf",3},{"crt","vfscanf_s",3},
        {"crt","__stdio_common_vfscanf",5,1}
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_crt_string(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;
    if(symbol.find("scanf")!=symbol.npos)return invoke_crt_scan(state,memory,service);
    if(symbol=="mbstowcs" || symbol=="wcstombs" || symbol=="mbsrtowcs" || symbol=="wcsrtombs"
       || symbol=="mbstowcs_s" || symbol=="wcstombs_s" || symbol=="mbsrtowcs_s" || symbol=="wcsrtombs_s"
       || symbol=="_mbstowcs_l" || symbol=="_wcstombs_l" || symbol=="_mbstowcs_s_l" || symbol=="_wcstombs_s_l")
        return invoke_locale(state,memory,service);
    if(service.family=="ntdll.dll"){
        const bool raw=symbol=="RtlMultiByteToUnicodeN" || symbol=="RtlOemToUnicodeN" || symbol=="RtlUnicodeToMultiByteN" || symbol=="RtlUnicodeToOemN" || symbol=="RtlMultiByteToUnicodeSize" || symbol=="RtlUnicodeToMultiByteSize";
        if(raw){
            const bool sizing=symbol.ends_with("Size"),to_unicode=!symbol.starts_with("RtlUnicodeTo");
            const auto source=arg(sizing?1:3);const auto length=static_cast<std::uint32_t>(arg(sizing?2:4));
            const auto count_pointer=arg(sizing?0:2),destination=sizing?0:arg(0);
            const auto capacity=sizing?0U:static_cast<std::uint32_t>(arg(1));
            const auto finish=[&](std::uint32_t status){arg.return_to(state,status);return std::optional<std::uint32_t>{};};
            if((sizing && !count_pointer) || (length && !source) || (!sizing && capacity && !destination) || (!to_unicode && (length&1U)))return finish(0xc000000dU);
            if(count_pointer)memory.check(GuestAddress{count_pointer},4,Access::Write);
            if(length>output_limit)return finish(0xc0000017U);
            // ACP 與 OEMCP 均沿用引擎的 UTF-8 碼頁；精確長度保留內嵌 NUL，不自動附加終止字元。
            try {
                std::vector<std::uint8_t> data;
                if(to_unicode){
                    std::string bytes(length,'\0');memory.read(GuestAddress{source},std::span(reinterpret_cast<std::uint8_t*>(bytes.data()),bytes.size()));
                    auto value=utf8_to_utf16_lossy(bytes);data=utf16_bytes(value);data.resize(data.size()-2);
                }else {
                    auto value=read_utf16_counted(memory,GuestAddress{source},length/2);
                    for(std::size_t i=0;i<value.size();++i){const auto c=value[i];
                        if(c>=0xd800 && c<=0xdbff){if(i+1<value.size() && value[i+1]>=0xdc00 && value[i+1]<=0xdfff)++i;else value[i]=u'\ufffd';}
                        else if(c>=0xdc00 && c<=0xdfff)value[i]=u'\ufffd';
                    }
                    const auto bytes=utf16_to_utf8(value);data.assign(bytes.begin(),bytes.end());
                }
                std::size_t written=data.size();
                if(!sizing){
                    written=std::min<std::size_t>(written,capacity);
                    if(to_unicode){
                        written&=~std::size_t(1);
                        if(written && written<data.size()){
                            const auto last=static_cast<unsigned>(data[written-2])|(static_cast<unsigned>(data[written-1])<<8);
                            if(last>=0xd800 && last<=0xdbff)written-=2;
                        }
                    }else if(written<data.size())while(written && (data[written]&0xc0U)==0x80U)--written;
                    if(written)memory.write(GuestAddress{destination},std::span(data).first(written));
                }
                if(count_pointer)memory.store(GuestAddress{count_pointer},32,written);
                return finish(0);
            }catch(const std::bad_alloc&){return finish(0xc0000017U);}
        }
        if(!symbol.starts_with("RtlInit")){
            const unsigned pointer_size=x86_?4U:8U;const auto width=static_cast<std::uint8_t>(pointer_size*8);
            const auto finish=[&](std::uint32_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
            const auto output=arg(0);if(!output)return finish(symbol.starts_with("RtlCreate")?0U:0xc000000dU);
            memory.check(GuestAddress{output},pointer_size*2U,Access::Write);
            const auto header=[&](std::uint16_t length,std::uint16_t capacity,std::uint64_t buffer){
                memory.store(GuestAddress{output},16,length);memory.store(GuestAddress{output+2},16,capacity);
                memory.store(GuestAddress{output+pointer_size},width,buffer);
            };
            if(symbol.starts_with("RtlFree")){
                const auto buffer=memory.load(GuestAddress{output+pointer_size},width);
                if(!free_heap(memory,buffer,state))throw HostCallFault("RtlFreeString 收到非 heap 配置起始位址");
                header(0,0,0);return finish(0);
            }
            const bool create=symbol.starts_with("RtlCreate"),format=symbol=="RtlFormatCurrentUserKeyPath";
            const bool unicode=symbol!="RtlUnicodeStringToAnsiString",allocate=create || format || (arg(2)&255U);
            std::vector<std::uint8_t> data;
            try {
                std::u16string text;
                if(format){initialize_tokens();text=u"\\Registry\\User\\"+utf8_to_utf16(token_user_sid_string(effective_token_id()));}
                else if(create){
                    if(!arg(1))return finish(0);
                    text=symbol=="RtlCreateUnicodeString"?read_utf16(memory,GuestAddress{arg(1)},32766):utf8_to_utf16_lossy(read_ansi(memory,arg(1),65534));
                }else {
                    if(!arg(1))return finish(0xc000000dU);
                    memory.check(GuestAddress{arg(1)},pointer_size*2U,Access::Read);
                    const auto length=memory.load(GuestAddress{arg(1)},16),capacity=memory.load(GuestAddress{arg(1)+2},16),buffer=memory.load(GuestAddress{arg(1)+pointer_size},width);
                    if(length>capacity || (length && !buffer) || (!unicode && (length&1U)))return finish(0xc000000dU);
                    if(unicode){std::string source(static_cast<std::size_t>(length),'\0');memory.read(GuestAddress{buffer},std::span(reinterpret_cast<std::uint8_t*>(source.data()),source.size()));text=utf8_to_utf16_lossy(source);}
                    else text=read_utf16_counted(memory,GuestAddress{buffer},static_cast<std::size_t>(length/2));
                }
                // 與虛擬 GetACP／Win32 轉碼共用 UTF-8 系統碼頁；counted string 保留內嵌 NUL。
                if(unicode)data=utf16_bytes(text);
                else {
                    for(std::size_t i=0;i<text.size();++i){const auto c=text[i];
                        if(c>=0xd800 && c<=0xdbff){if(i+1<text.size() && text[i+1]>=0xdc00 && text[i+1]<=0xdfff)++i;else text[i]=u'\ufffd';}
                        else if(c>=0xdc00 && c<=0xdfff)text[i]=u'\ufffd';
                    }
                    const auto source=utf16_to_utf8(text);data.assign(source.begin(),source.end());data.push_back(0);
                }
            }catch(const std::invalid_argument&){return finish(create?0U:0xc000000dU);}
             catch(const std::bad_alloc&){return finish(create?0U:0xc0000017U);}
            const unsigned terminator=unicode?2U:1U;
            if(data.size()>UINT16_MAX)return finish(create?0U:0xc000000dU);
            const auto length=static_cast<std::uint16_t>(data.size()-terminator);
            if(allocate){
                const auto buffer=allocate_heap(memory,data.size());if(!buffer)return finish(create?0U:0xc0000017U);
                try{memory.write(GuestAddress{buffer},data);header(length,static_cast<std::uint16_t>(data.size()),buffer);}
                catch(...){(void)free_heap(memory,buffer,state);throw;}
            }else {
                const auto capacity=memory.load(GuestAddress{output+2},16),buffer=memory.load(GuestAddress{output+pointer_size},width);
                if(capacity<data.size()){
                    if(unicode){memory.store(GuestAddress{output},16,length);return finish(0xc0000023U);}
                    if(!capacity){memory.store(GuestAddress{output},16,0);return finish(0x80000005U);}
                    if(!buffer)return finish(0xc000000dU);
                    std::size_t count=static_cast<std::size_t>(capacity-1);
                    while(count && (data[count]&0xc0U)==0x80U)--count;
                    memory.check(GuestAddress{buffer},count+1,Access::Write);memory.write(GuestAddress{buffer},std::span(data).first(count));memory.store(GuestAddress{buffer+count},8,0);
                    memory.store(GuestAddress{output},16,count);return finish(0x80000005U);
                }
                if(!buffer)return finish(0xc000000dU);
                memory.write(GuestAddress{buffer},data);memory.store(GuestAddress{output},16,length);
            }
            return finish(create?1U:0U);
        }
        const bool unicode=symbol.starts_with("RtlInitUnicode"),extended=symbol.ends_with("Ex");const unsigned unit=unicode?2U:1U,pointer_size=x86_?4U:8U;
        const std::uint32_t limit=unicode?0xfffcU:0xfffeU;std::uint32_t length=0;
        if(arg(1))while(memory.load(GuestAddress{arg(1)+length},static_cast<std::uint8_t>(unit*8))){
            if(length==limit){if(extended){arg.return_to(state,0xc000000dU);return std::nullopt;}break;}length+=unit;
        }
        memory.check(GuestAddress{arg(0)},pointer_size*2U,Access::Write);
        memory.store(GuestAddress{arg(0)},16,length);memory.store(GuestAddress{arg(0)+2},16,arg(1)?length+unit:0);
        memory.store(GuestAddress{arg(0)+pointer_size},static_cast<std::uint8_t>(pointer_size*8),arg(1));
        arg.return_to(state,0);return std::nullopt;
    }
    const bool wide=symbol=="_wtoi" || symbol=="_wtol" || symbol=="_wtoi64" || symbol.starts_with("wcs") || symbol.starts_with("_wcs") || symbol.find("tow")!=symbol.npos || symbol.starts_with("wmem") || symbol.starts_with("_wmakepath") || symbol.starts_with("_wsplitpath");
    const unsigned unit=wide?2U:1U;const auto width=static_cast<std::uint8_t>(unit*8);
    constexpr std::uint64_t maximum=1048576;std::uint64_t result=0;
    const auto address=[&](std::uint64_t base,std::uint64_t offset){
        if(offset>(UINT64_MAX-base)/unit)throw HostCallFault("CRT 字串位址溢位");
        return GuestAddress{base+offset*unit};
    };
    const auto text=[&](std::uint64_t pointer){
        if(wide)return read_utf16(memory,GuestAddress{pointer},maximum-1);
        const auto bytes=read_ansi(memory,pointer,maximum-1);std::u16string value;
        for(const auto c:bytes)value.push_back(static_cast<unsigned char>(c));return value;
    };
    const auto bytes=[&](std::u16string_view value,bool nul=true){
        std::vector<std::uint8_t> data;data.reserve((value.size()+(nul?1U:0U))*unit);
        for(auto c:value){data.push_back(static_cast<std::uint8_t>(c));if(wide)data.push_back(static_cast<std::uint8_t>(c>>8));}
        if(nul)data.resize(data.size()+unit);return data;
    };
    const auto invalid=[&](std::uint32_t error,std::optional<std::uint64_t> returned={})->std::optional<std::uint32_t>{
        memory.store(GuestAddress{runtime_.errno_cell},32,error);
        if(current_invalid_parameter_handler()){start_callbacks(state,memory,{{current_invalid_parameter_handler(),{0,0,0,0,0}}},Continuation::Return,returned.value_or(error));return std::nullopt;}
        return 0xc0000417U;
    };
    if(symbol=="_swab" || symbol=="swab"){
        const auto requested=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(2)));
        if(!arg(0) || !arg(1) || requested<0)return invalid(22,0);
        // CRT 的 n 是有號 int；奇數尾端既不讀取也不寫入。
        const auto count=static_cast<std::size_t>(requested)&~std::size_t(1);
        if(count>memory.limit())throw HostCallFault("CRT 位元組交換超過上限");
        if(count){
            memory.check(GuestAddress{arg(0)},count,Access::Read);
            memory.check(GuestAddress{arg(1)},count,Access::Write);
            std::array<std::uint8_t,4096> buffer{};
            for(std::size_t offset=0;offset<count;){
                const auto size=std::min(buffer.size(),count-offset);
                auto block=std::span(buffer).first(size);
                memory.read(GuestAddress{arg(0)+offset},block);
                for(std::size_t i=0;i<size;i+=2)std::swap(block[i],block[i+1]);
                memory.write(GuestAddress{arg(1)+offset},block);offset+=size;
            }
        }
    }else if(symbol=="__strncnt"){
        while(result<arg(1)){
            if(result>=output_limit)throw HostCallFault("CRT 有界字串掃描超過上限");
            if(!memory.load(address(arg(0),result),8))break;++result;
        }
    }else if(symbol=="memcpy_s" || symbol=="memmove_s" || symbol=="wmemcpy_s" || symbol=="wmemmove_s"){
        const auto count=arg(3),capacity=arg(1);const bool move=symbol.find("move")!=symbol.npos;
        if(count){
            if(!arg(0))return invalid(22);
            if(!arg(2) || count>capacity){
                if(!move && capacity){if(capacity>output_limit/unit)throw HostCallFault("CRT 錯誤清理範圍超過上限");memory.fill(GuestAddress{arg(0)},0,static_cast<std::size_t>(capacity*unit));}
                return invalid(!arg(2)?22:34);
            }
            if(count>output_limit/unit)throw HostCallFault("CRT 記憶體複製範圍超過上限");
            memory.move(GuestAddress{arg(0)},GuestAddress{arg(2)},static_cast<std::size_t>(count*unit));
        }
    }else if(symbol=="__isascii")result=static_cast<std::uint32_t>(arg(0))<128;
    else if(symbol=="_memicmp" || symbol=="_mbscmp" || symbol=="_mbstrlen"){
        // 與 setlocale／___mb_cur_max_func 共用目前的 C locale；不誤用宿主 locale 或 Windows ACP。
        if(symbol=="_mbstrlen"){if(!arg(0))return invalid(22,x86_?UINT32_MAX:UINT64_MAX);result=text(arg(0)).size();}
        else {
            if(!arg(0) || !arg(1))return invalid(22,0x7fffffffU);
            const bool raw=symbol=="_memicmp";const auto count=raw?arg(2):maximum;if(count>output_limit)throw HostCallFault("CRT 比較範圍超過上限");
            for(std::uint64_t i=0;i<count;++i){auto left=memory.load(address(arg(0),i),8),right=memory.load(address(arg(1),i),8);
                if(raw){if(left>='A' && left<='Z')left+=32;if(right>='A' && right<='Z')right+=32;}
                if(left!=right){result=left<right?UINT32_MAX:1;break;}if(!raw && !left)break;
                if(!raw && i+1==maximum)throw HostCallFault("CRT 比較找不到終止字元");
            }
        }
    }else if(symbol.find("makepath")!=symbol.npos){
        const bool secure=symbol.ends_with("_s");const auto capacity=secure?arg(1):maximum;const unsigned first=secure?2:1;
        if(!arg(0) || !capacity)return invalid(22,secure?22:0);
        const auto drive=arg(first)?text(arg(first)):std::u16string{},directory=arg(first+1)?text(arg(first+1)):std::u16string{},file=arg(first+2)?text(arg(first+2)):std::u16string{},extension=arg(first+3)?text(arg(first+3)):std::u16string{};
        std::u16string path;if(!drive.empty()){path+=drive.front();path+=u':';}path+=directory;
        if(!directory.empty() && directory.back()!=u'\\' && directory.back()!=u'/')path+=u'\\';path+=file;
        if(!extension.empty()){if(extension.front()!=u'.')path+=u'.';path+=extension;}
        if(path.size()>=capacity){memory.store(GuestAddress{arg(0)},width,0);return invalid(34,secure?34:0);}
        if(path.size()>=maximum)throw HostCallFault("CRT 組合路徑超過上限");memory.write(GuestAddress{arg(0)},bytes(path));
    }else if(symbol.find("splitpath")!=symbol.npos){
        const bool secure=symbol.ends_with("_s");std::array<std::uint64_t,4> outputs{},capacities{};bool bad=!arg(0);
        for(unsigned i=0;i<4;++i){outputs[i]=arg(1+i*(secure?2:1));capacities[i]=secure?arg(2+i*2):maximum;if(secure && static_cast<bool>(outputs[i])!=static_cast<bool>(capacities[i]))bad=true;}
        const auto fail=[&](unsigned error){for(unsigned i=0;i<4;++i)if(outputs[i] && capacities[i])memory.store(GuestAddress{outputs[i]},width,0);return invalid(error,secure?error:0);};
        if(bad)return fail(22);const auto path=text(arg(0));std::array<std::u16string,4> parts;std::size_t start=0;
        if(path.size()>=2 && path[1]==u':'){parts[0]=path.substr(0,2);start=2;}
        const auto slash=path.find_last_of(u"\\/");if(slash!=path.npos && slash>=start){parts[1]=path.substr(start,slash-start+1);start=slash+1;}
        const auto dot=path.find_last_of(u'.');if(dot!=path.npos && dot>=start){parts[2]=path.substr(start,dot-start);parts[3]=path.substr(dot);}else parts[2]=path.substr(start);
        for(unsigned i=0;i<4;++i)if(outputs[i] && parts[i].size()>=capacities[i])return fail(34);
        for(unsigned i=0;i<4;++i)if(outputs[i])memory.check(GuestAddress{outputs[i]},(parts[i].size()+1)*unit,Access::Write);
        for(unsigned i=0;i<4;++i)if(outputs[i])memory.write(GuestAddress{outputs[i]},bytes(parts[i]));
    }else if(symbol.starts_with('_') && (symbol.find("toa")!=symbol.npos || symbol.find("tow")!=symbol.npos)){
        const bool secure=symbol.ends_with("_s"),large=symbol.find("64")!=symbol.npos,signed_value=!symbol.starts_with("_u");
        const unsigned offset=x86_ && large?1U:0U;const auto buffer=arg(1+offset);
        const auto capacity=secure?arg(2+offset):65;const auto radix=static_cast<std::uint32_t>(arg((secure?3U:2U)+offset));
        if(!buffer || !capacity)return invalid(22,secure?22:0);
        memory.store(GuestAddress{buffer},width,0);
        if(radix<2 || radix>36)return invalid(22,secure?22:0);
        const auto mask=large?UINT64_MAX:UINT32_MAX;auto magnitude=(x86_ && large?memory.load(GuestAddress{state.reg(Gpr::Rsp)+4},64):arg(0))&mask;
        const bool negative=signed_value && radix==10 && (magnitude&(UINT64_C(1)<<(large?63:31)));
        if(negative)magnitude=(UINT64_C(0)-magnitude)&mask;
        std::u16string value;constexpr std::u16string_view digits=u"0123456789abcdefghijklmnopqrstuvwxyz";
        do {value.push_back(digits[static_cast<std::size_t>(magnitude%radix)]);magnitude/=radix;}while(magnitude);
        if(negative)value.push_back(u'-');std::reverse(value.begin(),value.end());
        if(value.size()>=capacity)return invalid(22,secure?22:0);
        memory.write(GuestAddress{buffer},bytes(value));result=secure?0:buffer;
    }else if(symbol=="_wtoi" || symbol=="_wtol" || symbol=="_wtoi64" || symbol=="wcstol" || symbol=="wcstoul" || symbol=="wcstoll" || symbol=="wcstoull" || symbol=="_wcstoi64" || symbol=="_wcstoui64"){
        const bool simple=symbol=="_wtoi" || symbol=="_wtol" || symbol=="_wtoi64";
        if(!arg(0))return invalid(22,0);
        const auto value=text(arg(0));std::string source;source.reserve(value.size());for(auto c:value)source.push_back(c<128?static_cast<char>(c):'\x7f');
        const auto base=simple?10:std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(2)));char* end=nullptr;
        if(!simple && arg(1))memory.check(GuestAddress{arg(1)},x86_?4:8,Access::Write);
        const auto previous_errno=errno;errno=0;
        if(base && (base<2 || base>36)){errno=EINVAL;end=source.data();}
        else if(symbol=="wcstoul" || symbol=="wcstoull" || symbol=="_wcstoui64"){
            result=std::strtoull(source.c_str(),&end,base);
            if(symbol=="wcstoul"){
                const auto first=source.find_first_not_of(" \t\r\n\f\v");const bool negative=first!=source.npos && source[first]=='-';
                const auto magnitude=negative && errno!=ERANGE?UINT64_C(0)-result:result;
                if(magnitude>UINT32_MAX){result=UINT32_MAX;errno=ERANGE;}else result&=UINT32_MAX;
            }
        }else {auto parsed=std::strtoll(source.c_str(),&end,base);
            if(symbol=="wcstol" || symbol=="_wtoi" || symbol=="_wtol"){if(parsed<INT32_MIN){parsed=INT32_MIN;errno=ERANGE;}if(parsed>INT32_MAX){parsed=INT32_MAX;errno=ERANGE;}result=static_cast<std::uint32_t>(parsed);}
            else result=static_cast<std::uint64_t>(parsed);
        }
        const auto conversion_errno=errno;errno=previous_errno;
        if(conversion_errno)memory.store(GuestAddress{runtime_.errno_cell},32,conversion_errno==ERANGE?34:22);
        if(!simple && arg(1))memory.store(GuestAddress{arg(1)},x86_?32:64,arg(0)+static_cast<std::uint64_t>(end-source.c_str())*2);
    }else if(symbol=="iswdigit")result=static_cast<std::uint16_t>(arg(0))>=u'0' && static_cast<std::uint16_t>(arg(0))<=u'9';
    else if(symbol=="strpbrk" || symbol=="wcspbrk"){
        const auto source=text(arg(0)),accept=text(arg(1));const auto at=source.find_first_of(accept);
        if(at!=source.npos)result=address(arg(0),at).value;
    }else if(symbol=="_wcsdup"){
        result=crt_duplicate_string(memory,arg(0),true);
    }else if(symbol.starts_with("_")){
        const bool secure=symbol.ends_with("_s");
        if(!arg(0) || (secure && !arg(1)))return invalid(22);
        std::u16string value;
        if(secure){
            // 先有界掃描再修改；缺少終止字元時清空目的字串並走既有 invalid-parameter 流程。
            const auto capacity=arg(1);bool terminated=false;
            for(std::uint64_t i=0;i<capacity;++i){
                if(i>=maximum)throw HostCallFault("CRT 大小寫轉換超過字串上限");
                const auto c=memory.load(address(arg(0),i),width);if(!c){terminated=true;break;}
                value.push_back(static_cast<char16_t>(c));
            }
            if(!terminated){memory.store(GuestAddress{arg(0)},width,0);return invalid(22);}
        }else value=text(arg(0));
        if(symbol=="_wcsrev" || symbol=="_strrev")std::reverse(value.begin(),value.end());
        else {const bool lower=symbol.find("lwr")!=symbol.npos;for(auto& c:value){
            if(lower && c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);
            if(!lower && c>=u'a' && c<=u'z')c=static_cast<char16_t>(c-32);
        }}
        memory.write(GuestAddress{arg(0)},bytes(value));result=secure?0:arg(0);
    }else if(symbol=="wcscoll" || symbol=="strcoll"){
        if(!arg(0) || !arg(1))return invalid(22,INT32_MAX);
        if(wide){const auto left=text(arg(0)),right=text(arg(1));result=left<right?UINT32_MAX:left>right?1:0;}
        else{
            // 已支援的 C／POSIX locale 按 unsigned byte 排序，與 strcmp 共用分段比較。
            const auto order=compare_ansi(memory,arg(0),arg(1),maximum);
            if(!order)(void)ansi_length(memory,arg(0),maximum-1); // 相等仍須確認上限內有 NUL。
            result=order<0?UINT32_MAX:order>0?1:0;
        }
    }else if(symbol=="wcsxfrm" || symbol=="strxfrm"){
        if(!arg(1) || (arg(2) && !arg(0)) || arg(2)>INT32_MAX)return invalid(22,INT32_MAX);
        const auto count=arg(2);
        if(wide){const auto value=text(arg(1));result=value.size();
            if(count)memory.write(GuestAddress{arg(0)},bytes(std::u16string_view(value).substr(0,static_cast<std::size_t>(count)),count>value.size()));
        }else{
            result=ansi_length(memory,arg(1),maximum-1);
            if(count)memory.move(GuestAddress{arg(0)},GuestAddress{arg(1)},static_cast<std::size_t>(std::min(count,result+1)));
        }
    }else if(symbol=="wcscpy" || symbol=="wcsncpy"){
        if(symbol=="wcscpy")memory.write(GuestAddress{arg(0)},bytes(text(arg(1))));
        else {
            const auto count=arg(2);if(count>maximum)throw HostCallFault("CRT 複製長度超過上限");
            std::u16string value(static_cast<std::size_t>(count),0);
            for(std::uint64_t i=0;i<count;++i){const auto c=memory.load(address(arg(1),i),width);if(!c)break;value[static_cast<std::size_t>(i)]=static_cast<char16_t>(c);}
            memory.write(GuestAddress{arg(0)},bytes(value,false));
        }result=arg(0);
    }else if(symbol=="strtok" || symbol=="strtok_s" || symbol=="wcstok_s"){
        const bool secure=symbol!="strtok";const auto pointer_width=static_cast<std::uint8_t>(x86_?32:64);
        if(!arg(1) || (secure && !arg(2)))return invalid(22);
        if(secure)memory.check(GuestAddress{arg(2)},pointer_width/8,Access::Write);
        auto cursor=arg(0)?arg(0):secure?memory.load(GuestAddress{arg(2)},pointer_width):threads_.items.at(threads_.current).strtok_cursor;
        if(!cursor){if(secure)return invalid(22);arg.return_to(state,0);return std::nullopt;}
        const auto delimiters=text(arg(1));std::uint64_t offset=0;
        const auto read=[&]{if(offset>=maximum)throw HostCallFault("CRT 分詞範圍超過上限");return static_cast<char16_t>(memory.load(address(cursor,offset),width));};
        auto c=read();while(c && delimiters.find(c)!=delimiters.npos){++offset;c=read();}
        if(c){result=address(cursor,offset).value;while(c && delimiters.find(c)==delimiters.npos){++offset;c=read();}
            if(c){memory.store(address(cursor,offset),width,0);++offset;}
        }
        cursor=address(cursor,offset).value;
        if(secure)memory.store(GuestAddress{arg(2)},pointer_width,cursor);else threads_.items.at(threads_.current).strtok_cursor=cursor;
    }else {
        // 安全複製／串接以字元數計算容量；_TRUNCATE 保留 NUL 並回傳 STRUNCATE。
        const bool append=symbol.find("cat")!=symbol.npos,bounded=symbol.find("ncpy")!=symbol.npos || symbol.find("ncat")!=symbol.npos;
        const auto capacity=arg(1),count=bounded?arg(3):maximum;
        if(bounded && !count && !arg(0) && !capacity){arg.return_to(state,0);return std::nullopt;}
        if(!arg(0) || !capacity)return invalid(22);
        if(capacity>maximum)throw HostCallFault("CRT 安全字串容量超過上限");
        memory.check(GuestAddress{arg(0)},static_cast<std::size_t>(capacity*unit),Access::Write);
        const auto fail=[&](std::uint32_t error){memory.store(GuestAddress{arg(0)},width,0);return invalid(error);};
        if(!arg(2) && (!bounded || count))return fail(22);
        std::u16string value;
        if(append){for(std::uint64_t i=0;i<capacity;++i){const auto c=memory.load(address(arg(0),i),width);if(!c)break;value.push_back(static_cast<char16_t>(c));}
            if(value.size()==capacity)return fail(22);
        }
        const auto room=capacity-value.size()-1;const bool truncate=bounded && count==(x86_?UINT32_MAX:UINT64_MAX);
        const auto limit=bounded && !truncate?std::min(count,room+1):room+1;
        for(std::uint64_t i=0;i<limit;++i){const auto c=memory.load(address(arg(2),i),width);if(!c)break;
            if(i==room){if(truncate){result=80;break;}return fail(34);}
            value.push_back(static_cast<char16_t>(c));
        }
        memory.write(GuestAddress{arg(0)},bytes(value));
    }
    if(x86_ && (symbol=="_wtoi64" || symbol=="wcstoll" || symbol=="wcstoull" || symbol=="_wcstoi64" || symbol=="_wcstoui64"))state.reg(Gpr::Rdx)=static_cast<std::uint32_t>(result>>32);
    arg.return_to(state,result);return std::nullopt;
}
}
