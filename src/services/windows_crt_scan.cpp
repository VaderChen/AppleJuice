#include "x64arm/windows_services.hpp"
#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstdlib>
#include <functional>
#include <limits>

namespace x64arm {
namespace {
bool scan_space(char16_t c){return c==u' ' || (c>=u'\t' && c<=u'\r');}
// 格式解析器僅要求按需讀取；字串與 FILE 共用所有轉換與客體 ABI。
struct ScanInput {
    std::function<std::optional<char16_t>()> read;
    std::u16string data;
    std::size_t capacity{};
    bool ended{};
    bool has(std::size_t index){
        while(!ended && data.size()<=index){
            if(data.size()>=capacity)throw HostCallFault("CRT 掃描輸入超出容量");
            if(const auto value=read())data.push_back(*value);else ended=true;
        }
        return index<data.size();
    }
    char16_t operator[](std::size_t index)const{return data[index];}
};
}
std::optional<std::uint32_t> WindowsServices::invoke_crt_scan(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const bool common=name.starts_with("__stdio_common_"),bounded=name.starts_with("_sn"),list=common || name.starts_with('v');
    const bool stream=name=="__stdio_common_vfscanf" || name.starts_with("fscanf") || name.starts_with("vfscanf");
    const bool wide=name.find("wscanf")!=name.npos;
    const auto options=common?arg(0):UINT64_C(6)|(name.ends_with("_s")?1U:0U);
    const bool secure=options&1U;const unsigned unit=wide?2U:1U;
    const auto source=arg(common?1:0),format=arg(common?(stream?2:3):bounded?2:1);
    const auto limit=common && !stream?arg(2):bounded?arg(1):UINT64_MAX;
    unsigned next=bounded?3U:2U;auto va=list?arg(common?(stream?4:5):2):0;
    std::function<void()> restore=[]{};
    const auto argument=[&](){if(!list)return arg(next++);const auto value=memory.load(GuestAddress{va},x86_?32:64);va+=x86_?4:8;return value;};
    const auto finish=[&](std::uint32_t result,std::uint32_t error=0)->std::optional<std::uint32_t>{
        restore();if(error)memory.store(GuestAddress{runtime_.errno_cell},32,error);arg.return_to(state,result);return std::nullopt;
    };
    const auto invalid=[&]()->std::optional<std::uint32_t>{
        restore();memory.store(GuestAddress{runtime_.errno_cell},32,22);
        if(const auto handler=current_invalid_parameter_handler()){start_callbacks(state,memory,{{handler,{0,0,0,0,0}}},Continuation::Return,UINT32_MAX);return std::nullopt;}
        return 0xc0000417U;
    };
    // CRT 目前提供 C locale；不將未知 locale 物件或未知選項當成有效設定。
    if(!source || !format || (options&~UINT64_C(7)) || (common && arg(stream?3:4)))return invalid();
    const auto read=[&](std::uint64_t address,std::uint64_t count){
        std::u16string result;
        for(std::uint64_t i=0;i<count;++i){
            if(i>=output_limit || i>(UINT64_MAX-address)/unit)throw HostCallFault("CRT 掃描輸入超出容量或位址範圍");
            const auto c=memory.load(GuestAddress{address+i*unit},static_cast<std::uint8_t>(unit*8));if(!c)break;
            result.push_back(static_cast<char16_t>(c));
        }return result;
    };
    const auto pattern=read(format,UINT64_MAX);
    std::size_t cursor=0;std::uint32_t assigned=0;bool converted=false;
    ScanInput input;input.capacity=output_limit;std::uint64_t offset=0;
    struct Checkpoint {std::uint64_t position;std::size_t pushback;bool eof,lowio_eof,has_read;};
    std::vector<Checkpoint> checkpoints;std::vector<std::uint8_t> original_pushback;
    CrtFile* file=nullptr;std::shared_ptr<void> position_guard;
    const CrtIoScope scope(*this,state,memory,stream?crt_stream(source):0);if(!scope)return std::nullopt;
    if(stream){
        const auto pointer=crt_stream(source),base=runtime_base_.value+0x800;
        if(pointer==base && standard_open_[0])file=&runtime_.standard_input;
        else if(const auto found=runtime_.streams.find(pointer);found!=runtime_.streams.end())file=&found->second;
        if(!file || !import_crt_lowio(memory,*file))return finish(UINT32_MAX,9);
        if(!wide && file->text && (file->lowio_extended&3U))return invalid();
        if(!file->readable){file->error=true;return finish(UINT32_MAX,9);}
        // 即時 stdin 需要可暫停的解析狀態；本批不以暫時沒有資料冒充 EOF。
        if(file->standard_handle==0x100 && live_stdio_[0]>=0)return finish(UINT32_MAX,129);
        position_guard=crt_file_position(*file).lock();original_pushback=file->pushback;
        const auto checkpoint=[&](){return Checkpoint{crt_stream_position(memory,*file,false),file->pushback.size(),file->eof,file->lowio_eof,file->has_read};};
        checkpoints.push_back(checkpoint());
        input.read=[&,checkpoint]()->std::optional<char16_t>{
            const auto bytes=read_crt_file(memory,*file,1,true);if(bytes.empty())return std::nullopt;
            checkpoints.push_back(checkpoint());return bytes[0];
        };
        // 回復未消耗的 lookahead 與其實體 CRLF 位置，不能只減去字元數。
        restore=[&](){if(cursor<input.data.size()){
            const auto& saved=checkpoints.at(cursor);crt_file_position(*file)=saved.position;
            file->buffer.used=file->buffer.cursor=0;file->buffer.direction=CrtFile::Buffer::Direction::None;crt_buffer_fields(memory,*file);
            file->pushback.assign(original_pushback.begin(),original_pushback.begin()+static_cast<std::ptrdiff_t>(saved.pushback));
            file->eof=saved.eof;file->lowio_eof=saved.lowio_eof;file->has_read=saved.has_read;publish_crt_lowio(memory,*file);
        }};
    }else input.read=[&]()->std::optional<char16_t>{
        if(offset==limit)return std::nullopt;
        if(offset>(UINT64_MAX-source)/unit)throw HostCallFault("CRT 掃描輸入位址溢位");
        const auto value=memory.load(GuestAddress{source+offset++*unit},static_cast<std::uint8_t>(unit*8));
        return value?std::optional{static_cast<char16_t>(value)}:std::nullopt;
    };
    const auto failed=[&](bool eof=false){return finish(eof && !converted?UINT32_MAX:assigned);};
    for(std::size_t f=0;f<pattern.size();){
        auto c=pattern[f++];
        if(scan_space(c)){while(input.has(cursor) && scan_space(input[cursor]))++cursor;continue;}
        if(c!=u'%'){if(!input.has(cursor))return failed(true);if(input[cursor]!=c)return failed();++cursor;continue;}
        if(f==pattern.size())return invalid();
        if(pattern[f]==u'%'){++f;if(!input.has(cursor))return failed(true);if(input[cursor]!=u'%')return failed();++cursor;continue;}
        bool suppress=false;if(pattern[f]==u'*'){suppress=true;++f;}
        std::size_t width=0;bool has_width=false;
        while(f<pattern.size() && pattern[f]>=u'0' && pattern[f]<=u'9'){
            has_width=true;width=std::min<std::size_t>(output_limit,width*10+static_cast<unsigned>(pattern[f++]-u'0'));
        }
        if(has_width && !width)return failed();
        std::u16string length;
        if(f<pattern.size() && std::u16string_view(u"hlLjztIw").find(pattern[f])!=std::u16string_view::npos){
            length.push_back(pattern[f++]);
            if(f<pattern.size() && (length==u"h" || length==u"l") && pattern[f]==length[0])length.push_back(pattern[f++]);
            if(length==u"I" && f+1<pattern.size() && (pattern.substr(f,2)==u"64" || pattern.substr(f,2)==u"32")){length+=pattern.substr(f,2);f+=2;}
        }
        if(f==pattern.size())return invalid();c=pattern[f++];
        const bool chars=c==u'c' || c==u'C',string=c==u's' || c==u'S',set=c==u'[';
        std::vector<std::pair<char16_t,char16_t>> ranges;bool invert=false;
        if(set){
            if(f<pattern.size() && pattern[f]==u'^'){invert=true;++f;}
            if(f<pattern.size() && pattern[f]==u']'){ranges.emplace_back(u']',u']');++f;}
            while(f<pattern.size() && pattern[f]!=u']'){
                auto first=pattern[f++],last=first;
                if(f+1<pattern.size() && pattern[f]==u'-' && pattern[f+1]!=u']'){last=pattern[f+1];f+=2;}
                ranges.emplace_back(std::min(first,last),std::max(first,last));
            }
            if(f==pattern.size())return invalid();++f;
        }
        const bool integer=std::u16string_view(u"diouxXp").find(c)!=std::u16string_view::npos;
        const bool floating=std::u16string_view(u"aAeEfFgG").find(c)!=std::u16string_view::npos;
        if(!chars && !string && !set && !integer && !floating && c!=u'n')return invalid();
        const auto target=suppress?0:argument();if(!suppress && !target)return invalid();
        const std::uint8_t bits=c==u'p'?(x86_?32:64):length==u"hh"?8:length==u"h"?16:
            length==u"ll" || length==u"I64" || length==u"j"?64:length==u"I" || length==u"z" || length==u"t"?(x86_?32:64):32;
        if(c==u'n'){if(!suppress)memory.store(GuestAddress{target},bits,cursor);converted=true;continue;}
        const auto capacity=secure && !suppress && (chars || string || set)?static_cast<std::uint32_t>(argument()):UINT32_MAX;
        if(secure && !suppress && (chars || string || set) && !capacity)return invalid();
        if(!chars && !set)while(input.has(cursor) && scan_space(input[cursor]))++cursor;
        if(!input.has(cursor))return failed(true);
        const auto start=cursor,extent=has_width?width:chars?1U:output_limit;
        const auto end=cursor+extent;
        if(chars || string || set){
            while(cursor<end && input.has(cursor)){
                const auto value=input[cursor];
                if(string && scan_space(value))break;
                if(set){const bool contained=std::any_of(ranges.begin(),ranges.end(),[&](const auto& range){return value>=range.first && value<=range.second;});if(contained==invert)break;}
                ++cursor;
            }
            if(cursor==start)return failed();
            if(chars && cursor-start<extent)return failed(true);
            const bool output_wide=length==u"l" || length==u"w" || (length!=u"h" && (c==u'C' || c==u'S'?!wide:wide && (options&2U)));
            if(!suppress){
                const auto count=cursor-start+(chars?0U:1U);if(count>capacity)return finish(assigned,34);
                std::vector<std::uint8_t> bytes;bytes.reserve(count*(output_wide?2U:1U));
                for(auto i=start;i<cursor;++i){const auto value=input[i];if(!output_wide && value>255)return finish(assigned,42);
                    bytes.push_back(static_cast<std::uint8_t>(value));if(output_wide)bytes.push_back(static_cast<std::uint8_t>(value>>8));}
                if(!chars)bytes.resize(bytes.size()+(output_wide?2U:1U));memory.write(GuestAddress{target},bytes);++assigned;
            }
        }else if(integer){
            // 數值轉換共用宿主 C runtime；Windows 的輸出寬度與客體寫回仍由相容層控制。
            const int radix=c==u'i'?0:c==u'o'?8:c==u'x' || c==u'X' || c==u'p'?16:10;
            std::string token;auto i=cursor;
            if(i<end && input.has(i) && (input[i]==u'+' || input[i]==u'-'))token.push_back(static_cast<char>(input[i++]));
            const auto prefix=token.size();int base=radix?radix:10;
            if(i<end && input.has(i) && input[i]==u'0'){
                token.push_back('0');++i;if(!radix)base=8;
                if((radix==0 || radix==16) && i<end && input.has(i) && (input[i]==u'x' || input[i]==u'X')){token.push_back('x');++i;base=16;}
            }
            while(i<end && input.has(i)){
                const auto value=input[i];const unsigned digit=value>=u'0' && value<=u'9'?value-u'0':value>=u'a' && value<=u'f'?value-u'a'+10:value>=u'A' && value<=u'F'?value-u'A'+10:255;
                if(digit>=static_cast<unsigned>(base))break;token.push_back(static_cast<char>(value));++i;
            }
            char* tail=nullptr;errno=0;const auto value=std::strtoull(token.c_str(),&tail,radix);
            const auto consumed=static_cast<std::size_t>(tail-token.c_str());if(!consumed){cursor+=token.size();return failed();}
            // 不完整的 0x 前綴是 scanf 比對失敗，不能採用 strtoull 回退所得的 0。
            if((radix==0 || radix==16) && consumed==prefix+1 && consumed<token.size() && token[prefix]=='0' && (token[consumed]=='x' || token[consumed]=='X')){cursor+=token.size();return failed();}
            cursor+=consumed;
            if(!suppress){memory.store(GuestAddress{target},bits,value);++assigned;}
        }else {
            std::string token;auto i=cursor;
            const auto lower=[](char16_t value){return value>=u'A' && value<=u'Z'?static_cast<char16_t>(value+32):value;};
            const auto take=[&](){token.push_back(static_cast<char>(input[i++]));};
            if(i<end && input.has(i) && (input[i]==u'+' || input[i]==u'-'))take();
            if(i<end && input.has(i) && (lower(input[i])==u'i' || lower(input[i])==u'n')){
                const bool infinity=lower(input[i])==u'i';const std::u16string_view word=infinity?u"infinity":u"nan";
                std::size_t matched=0;for(;matched<word.size() && i<end && input.has(i) && lower(input[i])==word[matched];++matched)take();
                if(!infinity && matched==3 && i<end && input.has(i) && input[i]==u'('){
                    take();while(i<end && input.has(i)){
                        const auto value=lower(input[i]);
                        if(value==u')'){take();break;}
                        if(!(value>=u'a' && value<=u'z') && !(value>=u'0' && value<=u'9') && value!=u'_')break;take();
                    }
                }
            }else {
                bool hex=false;
                if(i<end && input.has(i) && input[i]==u'0'){
                    take();if(i<end && input.has(i) && lower(input[i])==u'x'){take();hex=true;}
                }
                bool dot=false;
                while(i<end && input.has(i)){
                    const auto value=lower(input[i]);
                    if((value>=u'0' && value<=u'9') || (hex && value>=u'a' && value<=u'f'))take();
                    else if(!dot && value==u'.'){take();dot=true;}else break;
                }
                if(i<end && input.has(i) && lower(input[i])==(hex?u'p':u'e')){
                    take();if(i<end && input.has(i) && (input[i]==u'+' || input[i]==u'-'))take();
                    while(i<end && input.has(i) && input[i]>=u'0' && input[i]<=u'9')take();
                }
            }
            char* tail=nullptr;errno=0;const double value=std::strtod(token.c_str(),&tail);const auto consumed=static_cast<std::size_t>(tail-token.c_str());
            // scanf 消耗整個輸入項目；不完整的指數／前綴不能採用 strtod 回退結果。
            if(!consumed || consumed!=token.size()){cursor+=token.size();return failed();}
            cursor+=consumed;
            if(!suppress){if(length==u"l" || length==u"L")memory.store(GuestAddress{target},64,std::bit_cast<std::uint64_t>(value));
                else memory.store(GuestAddress{target},32,std::bit_cast<std::uint32_t>(static_cast<float>(value)));++assigned;}
        }
        converted=true;
    }
    return finish(assigned);
}
}
