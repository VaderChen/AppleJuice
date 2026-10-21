#pragma once
#include <iomanip>
#include <locale>
namespace x64arm::d3d9_xfile {
class Writer {
    unsigned format_;std::vector<std::uint8_t> bytes_;std::ostringstream text_;
    void raw(std::uint32_t value,unsigned width){for(unsigned i=0;i<width;++i)bytes_.push_back(static_cast<std::uint8_t>(value>>(i*8)));}
public:
    explicit Writer(unsigned format):format_(format){if(format>3)throw std::invalid_argument("X-file 輸出格式無效");text_.imbue(std::locale::classic());text_<<std::setprecision(9);}
    void name(std::string_view value){if(format_&1)text_<<value<<' ';else {raw(1,2);raw(static_cast<std::uint32_t>(value.size()),4);bytes_.insert(bytes_.end(),value.begin(),value.end());}}
    void punct(char value){if(format_&1)text_<<value<<(value==';'||value=='}'?'\n':' ');else {const auto token=value=='{'?10U:value=='}'?11U:value=='['?14U:value==']'?15U:value==','?19U:20U;raw(token,2);}}
    void integer(unsigned value){if(format_&1)text_<<value;else{raw(3,2);raw(value,4);}}
    void real(float value){if(!std::isfinite(value))throw std::invalid_argument("X-file 不能保存非有限數值");if(format_&1)text_<<value;else{raw(7,2);raw(1,4);raw(std::bit_cast<std::uint32_t>(value),4);}}
    void string(std::string_view value){if(format_&1){text_<<'"';for(auto c:value){if(c=='"'||c=='\\')text_<<'\\';text_<<c;}text_<<'"';}else{raw(2,2);raw(static_cast<std::uint32_t>(value.size()),4);bytes_.insert(bytes_.end(),value.begin(),value.end());raw(20,2);}}
    void begin(std::string_view type){name(type);punct('{');}void end(){punct('}');}
    void templates(std::string_view value){
        if(format_&1){text_<<value<<'\n';return;}
        const std::string source="xof 0303txt 0032"+std::string(value);const Parser parser(std::span(reinterpret_cast<const std::uint8_t*>(source.data()),source.size()));
        const std::map<std::string,unsigned> keywords{{"template",31},{"DWORD",41},{"FLOAT",42},{"STRING",51},{"array",52}};
        for(const auto& token:parser.token_stream()){
            if(token.uuid){raw(5,2);bytes_.insert(bytes_.end(),token.guid.begin(),token.guid.end());}
            else if(token.numeric)integer(static_cast<unsigned>(token.number));
            else if(auto it=keywords.find(token.text);it!=keywords.end())raw(it->second,2);
            else if(token.text=="..."){raw(18,2);raw(18,2);raw(18,2);}
            else if(token.text.size()==1 && std::strchr("{}[];,",token.text[0]))punct(token.text[0]);else name(token.text);
        }
    }
    std::vector<std::uint8_t> finish(){
        if(format_&1){const auto value=text_.str();bytes_.assign(value.begin(),value.end());}
        const std::string header=(format_&1)?"xof 0303txt 0032":"xof 0303bin 0032";std::vector<std::uint8_t> out(header.begin(),header.end());
        if(!(format_&2)){out.insert(out.end(),bytes_.begin(),bytes_.end());return out;}
        if(bytes_.size()>UINT32_MAX-16)throw std::bad_alloc{};std::memcpy(out.data()+8,(format_&1)?"tzip":"bzip",4);
        const auto add=[&](unsigned value,unsigned count){for(unsigned i=0;i<count;++i)out.push_back(static_cast<std::uint8_t>(value>>(i*8)));};add(static_cast<unsigned>(bytes_.size()+16),4);
        for(std::size_t at=0;at<bytes_.size();at+=32768){const auto count=std::min<std::size_t>(32768,bytes_.size()-at);std::vector<std::uint8_t> compressed(compressBound(count));z_stream stream{};
            if(deflateInit2(&stream,Z_DEFAULT_COMPRESSION,Z_DEFLATED,-MAX_WBITS,8,Z_DEFAULT_STRATEGY)!=Z_OK)throw std::bad_alloc{};
            if(at && deflateSetDictionary(&stream,bytes_.data()+at-std::min<std::size_t>(at,32768),static_cast<uInt>(std::min<std::size_t>(at,32768)))!=Z_OK){deflateEnd(&stream);throw std::invalid_argument("X-file 壓縮字典失敗");}
            stream.next_in=bytes_.data()+at;stream.avail_in=static_cast<uInt>(count);stream.next_out=compressed.data();stream.avail_out=static_cast<uInt>(compressed.size());const auto status=deflate(&stream,Z_FINISH);const auto written=stream.total_out;deflateEnd(&stream);
            if(status!=Z_STREAM_END || written+2>65535)throw std::invalid_argument("X-file 壓縮失敗");add(static_cast<unsigned>(count),2);add(static_cast<unsigned>(written+2),2);out.push_back('C');out.push_back('K');out.insert(out.end(),compressed.begin(),compressed.begin()+static_cast<std::ptrdiff_t>(written));
        }return out;
    }
};
}
