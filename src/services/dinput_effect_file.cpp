#include "x64arm/input.hpp"
#include "dinput_interfaces.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace x64arm::input {
namespace {
// FEdit 的 FORC 檔案使用固定的 32 位元 DIEFFECT，指標欄位代表後續 data 區塊。
// 依實際 FEdit／DirectX 7 SDK 檔案比對格式；沒有使用第三方解析程式。
constexpr Guid format_id{0x5c,0x77,0x7e,0x19,0xba,0x34,0xd3,0x11,0xab,0xd5,0,0xc0,0x4f,0x8e,0xc6,0x27};
constexpr std::size_t limit=16U*1024U*1024U;
struct Reader {
    std::span<const std::uint8_t> bytes;std::size_t offset{};
    std::span<const std::uint8_t> take(std::size_t n){if(n>bytes.size()-offset)throw std::invalid_argument("FFE 區塊截斷");const auto result=bytes.subspan(offset,n);offset+=n;return result;}
    std::uint32_t word(){const auto value=take(4);std::uint32_t result=0;for(unsigned i=0;i<4;++i)result|=static_cast<std::uint32_t>(value[i])<<(i*8);return result;}
    void tag(std::string_view label){const auto actual=take(label.size());if(!std::equal(actual.begin(),actual.end(),label.begin()))throw std::invalid_argument("FFE 區塊標記無效");}
    std::span<const std::uint8_t> data(){tag("data");const auto n=word();const auto result=take(n);if(n&1)take(1);return result;}
};
void word(std::vector<std::uint8_t>& out,std::uint32_t value){for(unsigned i=0;i<4;++i)out.push_back(static_cast<std::uint8_t>(value>>(i*8)));}
void append(std::vector<std::uint8_t>& out,std::span<const std::uint8_t> bytes){if(out.size()>limit || bytes.size()>limit-out.size())throw std::invalid_argument("FFE 檔案過大");out.insert(out.end(),bytes.begin(),bytes.end());}
void tag(std::vector<std::uint8_t>& out,std::string_view label){for(char c:label)out.push_back(static_cast<std::uint8_t>(c));}
void data(std::vector<std::uint8_t>& out,std::span<const std::uint8_t> bytes){tag(out,"data");word(out,static_cast<std::uint32_t>(bytes.size()));append(out,bytes);if(bytes.size()&1)out.push_back(0);}
std::vector<std::uint8_t> words(std::span<const std::uint32_t> values){std::vector<std::uint8_t> out;for(auto value:values)word(out,value);return out;}
}
std::vector<FileEffect> read_effect_file(std::span<const std::uint8_t> bytes){
    if(bytes.size()<28 || bytes.size()>limit)throw std::invalid_argument("FFE 檔案大小無效");
    Reader file{bytes};file.tag("RIFF");const auto size=file.word();if(size && size!=bytes.size()-8)throw std::invalid_argument("FFE RIFF 長度無效");file.tag("FORC");const auto identifier=file.take(16);if(!std::equal(identifier.begin(),identifier.end(),format_id.begin()))throw std::invalid_argument("FFE 格式識別值無效");
    std::vector<FileEffect> effects;
    while(file.offset<bytes.size()){
        if(effects.size()>=4096)throw std::invalid_argument("FFE 效果數量過多");file.tag("LIST");const auto n=file.word();Reader item{file.take(n)};if(n&1)file.take(1);item.tag("efct");FileEffect effect;const auto name=item.take(64);const auto end=std::find(name.begin(),name.end(),0);effect.name.assign(name.begin(),end);
        if(item.word()!=56)throw std::invalid_argument("FFE DIEFFECT 大小無效");auto& p=effect.parameters;p.flags=item.word();if((p.flags&~0x73U) || !std::has_single_bit(p.flags&3U) || !std::has_single_bit(p.flags&0x70U))throw std::invalid_argument("FFE 座標模式無效");
        p.duration=item.word();p.sample_period=item.word();p.gain=item.word();p.trigger=item.word();p.repeat=item.word();const auto count=item.word();if(count>32 || p.gain>10000)throw std::invalid_argument("FFE 參數超過範圍");const auto axes=item.word(),direction=item.word(),envelope=item.word(),specific_size=item.word(),specific=item.word();p.delay=item.word();const auto type=item.take(16);std::copy(type.begin(),type.end(),effect.type.begin());if(item.word()!=1)throw std::invalid_argument("FFE 效果區塊版本無效");
        if(!axes || !direction || specific_size>1048576 || (!specific && specific_size))throw std::invalid_argument("FFE 資料參照無效");
        Reader axis_data{item.data()},direction_data{item.data()};if(axis_data.bytes.size()!=count*4 || direction_data.bytes.size()!=count*4)throw std::invalid_argument("FFE 軸資料長度無效");for(unsigned i=0;i<count;++i){p.axes.push_back(axis_data.word());p.direction.push_back(static_cast<std::int32_t>(direction_data.word()));}
        if(envelope){Reader env{item.data()};if(env.bytes.size()!=20 || env.word()!=20)throw std::invalid_argument("FFE 包絡大小無效");p.envelope=std::array{env.word(),env.word(),env.word(),env.word()};if((*p.envelope)[0]>10000 || (*p.envelope)[2]>10000)throw std::invalid_argument("FFE 包絡振幅無效");}
        if(specific){const auto raw=item.data();if(raw.size()!=specific_size)throw std::invalid_argument("FFE 效果參數長度無效");if(effect.type==dinput::GUID_CustomForce){Reader custom{raw};if(raw.size()!=16)throw std::invalid_argument("FFE 自訂效果大小無效");p.channels=custom.word();p.custom_period=custom.word();const auto samples=custom.word(),pointer=custom.word();if(!p.channels || p.channels>32 || samples>65536 || samples%p.channels || (!pointer && samples))throw std::invalid_argument("FFE 自訂樣本數量無效");if(pointer){Reader values{item.data()};if(values.bytes.size()!=samples*4)throw std::invalid_argument("FFE 自訂樣本截斷");for(unsigned i=0;i<samples;++i)p.samples.push_back(static_cast<std::int32_t>(values.word()));}}
            else p.specific.assign(raw.begin(),raw.end());}
        if(item.offset!=item.bytes.size())throw std::invalid_argument("FFE 效果區塊有未解析資料");effects.push_back(std::move(effect));
    }
    return effects;
}
std::vector<std::uint8_t> write_effect_file(std::span<const FileEffect> effects){
    if(effects.size()>4096)throw std::invalid_argument("FFE 效果數量過多");std::vector<std::uint8_t> out;tag(out,"RIFF");word(out,0);tag(out,"FORC");append(out,format_id);
    for(const auto& effect:effects){const auto& p=effect.parameters;if(p.axes.size()>32 || p.axes.size()!=p.direction.size() || p.specific.size()>1048576 || p.gain>10000 || (p.flags&~0x73U) || !std::has_single_bit(p.flags&3U) || !std::has_single_bit(p.flags&0x70U))throw std::invalid_argument("FFE 效果參數無效");
        if(p.channels && (effect.type!=dinput::GUID_CustomForce || p.channels>32 || p.samples.size()>65536 || p.samples.size()%p.channels))throw std::invalid_argument("FFE 自訂樣本無效");
        std::vector<std::uint8_t> item;tag(item,"efct");std::size_t length=std::min<std::size_t>(effect.name.size(),63);while(length<effect.name.size() && length && (static_cast<unsigned char>(effect.name[length])&0xc0)==0x80)--length;for(std::size_t i=0;i<64;++i)item.push_back(i<length?static_cast<std::uint8_t>(effect.name[i]):0);
        word(item,56);word(item,p.flags);word(item,p.duration);word(item,p.sample_period);word(item,p.gain);word(item,p.trigger);word(item,p.repeat);word(item,static_cast<std::uint32_t>(p.axes.size()));word(item,0x20);word(item,0x40);word(item,p.envelope?0x80U:0U);word(item,p.channels?16U:static_cast<std::uint32_t>(p.specific.size()));word(item,p.channels || !p.specific.empty()?0x100U:0U);word(item,p.delay);append(item,effect.type);word(item,1);
        data(item,words(p.axes));std::vector<std::uint8_t> directions;for(auto value:p.direction)word(directions,static_cast<std::uint32_t>(value));data(item,directions);
        if(p.envelope){std::vector<std::uint8_t> env;word(env,20);for(auto value:*p.envelope)word(env,value);data(item,env);}
        if(p.channels){std::vector<std::uint8_t> custom;word(custom,p.channels);word(custom,p.custom_period);word(custom,static_cast<std::uint32_t>(p.samples.size()));word(custom,0x200);data(item,custom);std::vector<std::uint8_t> samples;for(auto value:p.samples)word(samples,static_cast<std::uint32_t>(value));data(item,samples);}
        else if(!p.specific.empty())data(item,p.specific);
        tag(out,"LIST");word(out,static_cast<std::uint32_t>(item.size()));append(out,item);if(item.size()&1)out.push_back(0);
    }
    return out;
}
}
