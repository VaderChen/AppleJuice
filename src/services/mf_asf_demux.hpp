#pragma once
#include "service_callback_task.hpp"
#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace x64arm::mf {
// ASF（Advanced Systems Format）解多工：只解析容器標頭、資料封包與 Simple Index，不含解碼與客體 ABI。
// 時間一律換算為 100 ns 並扣除 Preroll；同步（宿主檔案）與協程（客體 byte stream）讀取共用同一份流程。
struct AsfStream {
    std::uint8_t number{};                  // ASF 串流編號（1～127），也是 stream descriptor 的識別碼。
    bool video{},audio{};
    // 視訊：BITMAPINFOHEADER；extra 是標頭之後的編碼器私有資料。
    std::uint32_t width{},height{},fourcc{},par_num{1},par_den{1};
    std::uint64_t frame_time{};             // Extended Stream Properties 的平均每幀時間（100 ns），沒有時為 0。
    // 音訊：WAVEFORMATEX；extra 是 cbSize 指定的額外資料。
    std::uint16_t format_tag{},channels{},block_align{},bits{};
    std::uint32_t sample_rate{},bytes_per_second{};
    std::vector<std::uint8_t> extra;
    std::uint32_t bitrate{};                // Stream Bitrate Properties；沒有時取 Extended Stream Properties 的 Data Bitrate。
};
struct AsfMovie {
    std::vector<AsfStream> streams;         // 依串流編號排序；索引即 presentation 內的串流索引。
    std::uint64_t file_size{},play_duration{},preroll{}; // play_duration 為 100 ns、preroll 為毫秒。
    std::uint32_t flags{},packet_size{},max_bitrate{};
    std::uint64_t packet_count{},data_offset{};          // 可讀取的封包數與第一個封包的檔案位置。
    std::int64_t duration{};                // Play Duration 減 Preroll。
    std::uint64_t index_interval{};         // Simple Index 的項目間隔（100 ns）；index 為空表示沒有索引。
    std::vector<std::uint32_t> index;       // 各項目對應的封包編號。
    const AsfStream* find(std::uint8_t number)const{for(const auto& s:streams)if(s.number==number)return &s;return nullptr;}
};
struct AsfObject {std::uint8_t stream{};bool key{};std::int64_t time{};std::vector<std::uint8_t> bytes;};
// 同一來源的解多工同時只能有一個讀取在進行（客體 byte stream 讀取會跨越服務續行）。
struct AsfBusy:std::logic_error {AsfBusy():std::logic_error("ASF 解多工正在使用中"){}};
namespace asf_detail {
[[noreturn]] inline void bad(const char* what){throw std::invalid_argument(what);}
inline std::uint64_t le(std::span<const std::uint8_t> bytes,std::size_t at,unsigned width){
    if(at>bytes.size() || width>bytes.size()-at)bad("ASF 欄位截斷");
    std::uint64_t value=0;for(unsigned n=width;n--;)value=(value<<8)|bytes[at+n];return value;
}
inline std::uint32_t le32(std::span<const std::uint8_t> bytes,std::size_t at){return static_cast<std::uint32_t>(le(bytes,at,4));}
inline std::uint16_t le16(std::span<const std::uint8_t> bytes,std::size_t at){return static_cast<std::uint16_t>(le(bytes,at,2));}
using Id=std::array<std::uint8_t,16>;
constexpr Id id(std::uint32_t a,std::uint16_t b,std::uint16_t c,std::array<std::uint8_t,8> d){
    return {static_cast<std::uint8_t>(a),static_cast<std::uint8_t>(a>>8),static_cast<std::uint8_t>(a>>16),static_cast<std::uint8_t>(a>>24),static_cast<std::uint8_t>(b),static_cast<std::uint8_t>(b>>8),static_cast<std::uint8_t>(c),static_cast<std::uint8_t>(c>>8),d[0],d[1],d[2],d[3],d[4],d[5],d[6],d[7]};
}
constexpr Id header_object=id(0x75b22630,0x668e,0x11cf,{0xa6,0xd9,0x00,0xaa,0x00,0x62,0xce,0x6c});
constexpr Id data_object=id(0x75b22636,0x668e,0x11cf,{0xa6,0xd9,0x00,0xaa,0x00,0x62,0xce,0x6c});
constexpr Id simple_index_object=id(0x33000890,0xe5b1,0x11cf,{0x89,0xf4,0x00,0xa0,0xc9,0x03,0x49,0xcb});
constexpr Id file_properties=id(0x8cabdca1,0xa947,0x11cf,{0x8e,0xe4,0x00,0xc0,0x0c,0x20,0x53,0x65});
constexpr Id stream_properties=id(0xb7dc0791,0xa9b7,0x11cf,{0x8e,0xe6,0x00,0xc0,0x0c,0x20,0x53,0x65});
constexpr Id header_extension=id(0x5fbf03b5,0xa92e,0x11cf,{0x8e,0xe3,0x00,0xc0,0x0c,0x20,0x53,0x65});
constexpr Id stream_bitrate_properties=id(0x7bf875ce,0x468d,0x11d1,{0x8d,0x82,0x00,0x60,0x97,0xc9,0xa2,0xb2});
constexpr Id extended_stream_properties=id(0x14e6a5cb,0xc672,0x4332,{0x83,0x99,0xa9,0x69,0x52,0x06,0x5b,0x5a});
constexpr Id metadata_object=id(0xc5f8cbea,0x5baf,0x4877,{0x84,0x67,0xaa,0x8c,0x44,0xfa,0x4c,0xca});
constexpr Id audio_media=id(0xf8699e40,0x5b4d,0x11cf,{0xa8,0xfd,0x00,0x80,0x5f,0x5c,0x44,0x2b});
constexpr Id audio_spread=id(0xbfc3cd50,0x618f,0x11cf,{0x8b,0xb2,0x00,0xaa,0x00,0xb4,0xe2,0x20});
constexpr Id video_media=id(0xbc19efc0,0x5b4d,0x11cf,{0xa8,0xfd,0x00,0x80,0x5f,0x5c,0x44,0x2b});
inline bool is(std::span<const std::uint8_t> bytes,std::size_t at,const Id& expected){
    if(at>bytes.size() || 16>bytes.size()-at)bad("ASF GUID 截斷");return std::equal(expected.begin(),expected.end(),bytes.begin()+static_cast<std::ptrdiff_t>(at));
}
// 走訪一段連續的 ASF 物件（16-byte GUID＋64-bit 大小）；大小不足標頭或超出範圍即視為損毀。
template<class F> void each_object(std::span<const std::uint8_t> bytes,F&& visit){
    for(std::size_t at=0;bytes.size()-at>=24;){
        const auto size=le(bytes,at+16,8);if(size<24 || size>bytes.size()-at)bad("ASF 物件大小無效");
        visit(bytes.subspan(at,16),bytes.subspan(at+24,static_cast<std::size_t>(size)-24));at+=static_cast<std::size_t>(size);
    }
}
struct StreamExtra {std::uint64_t frame_time{};std::uint32_t bitrate{},average{},par_x{},par_y{};};
struct HeaderState {AsfMovie movie;bool file{};std::map<std::uint8_t,StreamExtra> extra;};
inline void parse_stream_properties(std::span<const std::uint8_t> body,HeaderState& state){
    if(body.size()<54)bad("ASF Stream Properties 截斷");
    const std::uint64_t type_size=le32(body,40),correction_size=le32(body,44);const auto flags=le16(body,48);
    // Error Correction 資料不解讀，但長度必須落在物件內。
    if(type_size+correction_size>body.size()-54)bad("ASF Stream Properties 長度無效");
    AsfStream stream;stream.number=static_cast<std::uint8_t>(flags&0x7fU);if(!stream.number)bad("ASF 串流編號無效");
    if(flags&0x8000U)bad("尚未支援加密的 ASF 串流");
    const auto data=body.subspan(54,static_cast<std::size_t>(type_size));
    if(is(body,0,audio_media)){
        if(data.size()<16)bad("ASF 音訊格式截斷");stream.audio=true;
        stream.format_tag=le16(data,0);stream.channels=le16(data,2);stream.sample_rate=le32(data,4);stream.bytes_per_second=le32(data,8);stream.block_align=le16(data,12);stream.bits=le16(data,14);
        if(data.size()>=18){const std::size_t extra=le16(data,16);if(extra>data.size()-18)bad("ASF 音訊額外資料長度無效");stream.extra.assign(data.begin()+18,data.begin()+18+static_cast<std::ptrdiff_t>(extra));}
        if(!stream.channels || !stream.sample_rate)bad("ASF 音訊格式無效");
    }else if(is(body,0,video_media)){
        if(data.size()<11)bad("ASF 視訊格式截斷");stream.video=true;
        const std::size_t format=le16(data,9);if(format<40 || format>data.size()-11)bad("ASF 視訊格式長度無效");
        const auto header=data.subspan(11,format);
        const auto width=le32(header,4);const auto height=static_cast<std::int32_t>(le32(header,8));
        stream.width=width?width:le32(data,0);stream.height=height?static_cast<std::uint32_t>(height<0?-static_cast<std::int64_t>(height):height):le32(data,4);stream.fourcc=le32(header,16);
        stream.extra.assign(header.begin()+40,header.end());
        if(!stream.width || !stream.height)bad("ASF 視訊尺寸無效");
    }else return; // 指令、檔案傳輸等其他串流類型不產生媒體串流。
    // Audio Spread 的 span 大於 1 時 payload 內容經過交錯，需要反交錯才是原始資料；尚未實作，不交出錯誤內容。
    if(is(body,16,audio_spread) && correction_size && le(body,54+static_cast<std::size_t>(type_size),1)>1)bad("尚未支援交錯的 ASF 音訊（Audio Spread span>1）");
    if(!state.movie.find(stream.number))state.movie.streams.push_back(std::move(stream));
}
inline void parse_extended_stream_properties(std::span<const std::uint8_t> body,HeaderState& state){
    if(body.size()<64)bad("ASF Extended Stream Properties 截斷");
    const auto number=static_cast<std::uint8_t>(le16(body,48)&0x7fU);auto& extra=state.extra[number];
    extra.bitrate=le32(body,16);extra.frame_time=le(body,52,8);
    std::size_t at=64;
    for(unsigned n=le16(body,60);n--;){const std::size_t length=le16(body,at+2);if(length>body.size()-at-4)bad("ASF 串流名稱長度無效");at+=4+length;}
    for(unsigned n=le16(body,62);n--;){const std::uint64_t length=le32(body,at+18);if(length>body.size()-at-22)bad("ASF payload 延伸系統長度無效");at+=22+static_cast<std::size_t>(length);}
    // 之後可內嵌一個 Stream Properties Object。
    if(body.size()-at>=24)each_object(body.subspan(at),[&](std::span<const std::uint8_t> type,std::span<const std::uint8_t> inner){if(is(type,0,stream_properties))parse_stream_properties(inner,state);});
}
inline void parse_metadata(std::span<const std::uint8_t> body,HeaderState& state){
    std::size_t at=2;
    for(unsigned n=le16(body,0);n--;){
        const auto number=static_cast<std::uint8_t>(le16(body,at+2)&0x7fU);const std::size_t name_size=le16(body,at+4);const auto type=le16(body,at+6);const std::uint64_t data_size=le32(body,at+8);at+=12;
        if(name_size>body.size()-at || data_size>body.size()-at-name_size)bad("ASF Metadata 長度無效");
        std::u16string name;for(std::size_t i=0;i+1<name_size;i+=2){const auto unit=static_cast<char16_t>(le16(body,at+i));if(!unit)break;name.push_back(unit);}
        const auto data=body.subspan(at+name_size,static_cast<std::size_t>(data_size));at+=name_size+static_cast<std::size_t>(data_size);
        if(type==3 && data.size()>=4){if(name==u"AspectRatioX")state.extra[number].par_x=le32(data,0);else if(name==u"AspectRatioY")state.extra[number].par_y=le32(data,0);}
    }
}
inline void parse_header_object(std::span<const std::uint8_t> type,std::span<const std::uint8_t> body,HeaderState& state){
    if(is(type,0,file_properties)){
        if(body.size()<80)bad("ASF File Properties 截斷");auto& movie=state.movie;state.file=true;
        movie.packet_count=le(body,32,8);movie.play_duration=le(body,40,8);movie.preroll=le(body,56,8);movie.flags=le32(body,64);
        const auto minimum=le32(body,68),maximum=le32(body,72);movie.max_bitrate=le32(body,76);
        // 封包以固定大小排列才能直接定位；規格要求兩者相等。
        if(!minimum || minimum!=maximum)bad("ASF 封包大小不固定");if(minimum<16 || minimum>16U*1024U*1024U)bad("ASF 封包大小無效");movie.packet_size=minimum;
    }else if(is(type,0,stream_properties))parse_stream_properties(body,state);
    else if(is(type,0,stream_bitrate_properties)){
        const std::size_t count=le16(body,0);if(count*6>body.size()-2)bad("ASF Stream Bitrate Properties 截斷");
        for(std::size_t n=0;n<count;++n)state.extra[static_cast<std::uint8_t>(le16(body,2+n*6)&0x7fU)].average=le32(body,4+n*6);
    }else if(is(type,0,header_extension)){
        if(body.size()<22)bad("ASF Header Extension 截斷");const std::uint64_t size=le32(body,18);if(size>body.size()-22)bad("ASF Header Extension 長度無效");
        each_object(body.subspan(22,static_cast<std::size_t>(size)),[&](std::span<const std::uint8_t> inner_type,std::span<const std::uint8_t> inner){
            if(is(inner_type,0,extended_stream_properties))parse_extended_stream_properties(inner,state);
            else if(is(inner_type,0,metadata_object))parse_metadata(inner,state);
        });
    }
    // 其餘物件（Codec List、Content Description、Padding 等）依大小跳過。
}
// 封包標頭：Error Correction Data 與 Payload Parsing Information。
struct PacketHeader {std::size_t at{},end{};bool multiple{};std::uint8_t property{};std::uint32_t send_time{};};
inline std::uint32_t field(std::span<const std::uint8_t> packet,std::size_t& at,unsigned type){
    constexpr unsigned widths[]{0,1,2,4};const auto width=widths[type&3U];const auto value=static_cast<std::uint32_t>(le(packet,at,width));at+=width;return value;
}
inline PacketHeader packet_header(std::span<const std::uint8_t> packet,std::size_t packet_size){
    PacketHeader header;std::size_t at=0;const auto first=static_cast<std::uint8_t>(le(packet,0,1));
    if(first&0x80U){
        // 只支援規格定義的形式：長度型別 0、沒有 opaque 資料；資料本身略過。
        if(first&0x70U)bad("尚未支援的 ASF Error Correction 形式");at=1U+(first&0x0fU);
    }
    const auto flags=static_cast<std::uint8_t>(le(packet,at,1));header.property=static_cast<std::uint8_t>(le(packet,at+1,1));at+=2;
    const auto length_type=(flags>>5)&3U;const auto length=field(packet,at,length_type);(void)field(packet,at,(flags>>1)&3U);const auto padding=field(packet,at,(flags>>3)&3U);
    header.send_time=le32(packet,at);at+=6;header.multiple=flags&1U;
    const std::size_t total=length_type?length:packet_size;
    if(total>packet_size || at>total || padding>total-at)bad("ASF 封包長度無效");
    header.at=at;header.end=total-padding;return header;
}
struct Payload {std::uint8_t stream{};bool key{},whole{},sized{};std::uint32_t object{},offset{},size{},time{};std::span<const std::uint8_t> data;};
// 逐一交出封包內的 payload；壓縮 payload（Replicated Data Length＝1）的每個子 payload 是一個完整媒體物件。
template<class F> void each_payload(std::span<const std::uint8_t> packet,F&& visit){
    const auto header=packet_header(packet,packet.size());std::size_t at=header.at;const auto end=header.end;
    unsigned count=1,length_type=0;
    if(header.multiple){const auto flags=static_cast<std::uint8_t>(le(packet,at,1));++at;count=flags&0x3fU;length_type=(flags>>6)&3U;if(!length_type)bad("ASF 多重 payload 缺少長度欄位");}
    for(unsigned n=0;n<count;++n){
        Payload payload;const auto number=static_cast<std::uint8_t>(le(packet,at,1));++at;payload.stream=number&0x7fU;payload.key=number&0x80U;
        payload.object=field(packet,at,(header.property>>4)&3U);payload.offset=field(packet,at,(header.property>>2)&3U);
        const std::size_t replicated=field(packet,at,header.property&3U);
        if(at>end || replicated>end-at)bad("ASF Replicated Data 長度無效");
        std::uint8_t delta=0;
        if(replicated==1){delta=packet[at];payload.whole=true;}
        else if(replicated>=8){payload.size=le32(packet,at);payload.time=le32(packet,at+4);payload.sized=true;}
        else if(replicated)bad("ASF Replicated Data 長度無效");
        else payload.time=header.send_time; // 沒有 Replicated Data 時只能以封包送出時間近似。
        at+=replicated;
        const std::size_t length=header.multiple?field(packet,at,length_type):end-std::min(at,end);
        if(at>end || length>end-at)bad("ASF payload 長度無效");
        if(payload.whole){
            // 壓縮 payload 的 Offset 欄位是第一個子 payload 的呈現時間，之後每個遞增 delta。
            const auto base=payload.offset;const auto first=payload.object;payload.offset=0;payload.sized=true;
            for(std::size_t cursor=at,index=0;cursor<at+length;++index){
                const std::size_t size=packet[cursor++];if(size>at+length-cursor)bad("ASF 壓縮 payload 長度無效");
                payload.object=first+static_cast<std::uint32_t>(index);payload.time=base+static_cast<std::uint32_t>(index)*delta;payload.size=static_cast<std::uint32_t>(size);payload.data=packet.subspan(cursor,size);
                visit(payload);cursor+=size;
            }
        }else{payload.data=packet.subspan(at,length);visit(payload);}
        at+=length;
    }
}
}
constexpr std::size_t asf_max_object=256U*1024U*1024U;
constexpr std::uint32_t asf_invalid_data=0xc00d3a9aU; // MF_E_ASF_INVALIDDATA：標頭或封包損毀時回報的 HRESULT。
inline bool asf_signature(std::span<const std::uint8_t> head){return head.size()>=16 && std::equal(asf_detail::header_object.begin(),asf_detail::header_object.end(),head.begin());}
// 平均每幀時間換成幀率；常見的 NTSC／整數幀率（容許 1 個 100 ns 單位的捨入）回報標準分數。
inline std::pair<std::uint32_t,std::uint32_t> asf_frame_rate(std::uint64_t frame_time){
    if(!frame_time || frame_time>10000000U*100U)return {0,0};
    constexpr std::array<std::array<std::uint32_t,3>,8> known{{{417083,24000,1001},{416667,24,1},{400000,25,1},{333667,30000,1001},{333333,30,1},{200000,50,1},{166833,60000,1001},{166667,60,1}}};
    for(const auto& item:known)if(frame_time+1>=item[0] && frame_time<=item[0]+1U)return {item[1],item[2]};
    return {10000000U,static_cast<std::uint32_t>(frame_time)};
}
// Header Object（含 24-byte 物件標頭）→ 串流描述；Data Object 的位置由 read_asf_movie 補上。
inline std::shared_ptr<AsfMovie> parse_asf_header(std::span<const std::uint8_t> header,std::uint64_t file_size){
    using namespace asf_detail;
    if(header.size()<30 || !is(header,0,header_object) || le(header,16,8)!=header.size())bad("ASF Header Object 無效");
    HeaderState state;state.movie.file_size=file_size;
    each_object(header.subspan(30),[&](std::span<const std::uint8_t> type,std::span<const std::uint8_t> body){parse_header_object(type,body,state);});
    if(!state.file)bad("ASF 缺少 File Properties");if(state.movie.streams.empty())bad("ASF 沒有可播放串流");
    auto movie=std::make_shared<AsfMovie>(std::move(state.movie));
    std::sort(movie->streams.begin(),movie->streams.end(),[](const AsfStream& a,const AsfStream& b){return a.number<b.number;});
    for(auto& stream:movie->streams)if(const auto found=state.extra.find(stream.number);found!=state.extra.end()){
        const auto& extra=found->second;stream.bitrate=extra.average?extra.average:extra.bitrate;
        if(stream.video){stream.frame_time=extra.frame_time;if(extra.par_x && extra.par_y){stream.par_num=extra.par_x;stream.par_den=extra.par_y;}}
    }
    // 廣播（bit 0）檔案的 Play Duration 無效。
    const auto preroll=movie->preroll<=UINT64_MAX/10000U?movie->preroll*10000U:UINT64_MAX;
    if(!(movie->flags&1U) && movie->play_duration>preroll && movie->play_duration-preroll<=static_cast<std::uint64_t>(INT64_MAX))movie->duration=static_cast<std::int64_t>(movie->play_duration-preroll);
    return movie;
}
// Simple Index Object 的內容（不含 24-byte 物件標頭）。
inline void parse_asf_simple_index(std::span<const std::uint8_t> body,AsfMovie& movie){
    using namespace asf_detail;
    if(body.size()<32)bad("ASF Simple Index 截斷");const auto interval=le(body,16,8);const std::uint64_t count=le32(body,28);
    if(count*6>body.size()-32)bad("ASF Simple Index 長度無效");if(!interval || !count)return;
    movie.index_interval=interval;movie.index.resize(static_cast<std::size_t>(count));
    for(std::size_t n=0;n<movie.index.size();++n)movie.index[n]=le32(body,32+n*6);
}
// 讀取 Header Object、Data Object 標頭與其後的 Simple Index；同步與客體 byte stream 共用同一份流程。
template<class Read> service_callback::Task<std::shared_ptr<const AsfMovie>> read_asf_movie_async(std::uint64_t size,Read read){
    using namespace asf_detail;
    if(size<30)bad("ASF 檔案截斷");
    std::array<std::uint8_t,30> head{};co_await read(0,std::span<std::uint8_t>(head));
    const auto header_size=le(head,16,8);
    if(!is(head,0,header_object) || header_size<30 || header_size>size)bad("ASF Header Object 大小無效");
    if(header_size>64U*1024U*1024U)bad("ASF Header Object 過大");
    std::vector<std::uint8_t> header(static_cast<std::size_t>(header_size));co_await read(0,std::span<std::uint8_t>(header));
    auto movie=parse_asf_header(header,size);
    if(size-header_size<50)bad("ASF 缺少 Data Object");
    std::array<std::uint8_t,50> data{};co_await read(header_size,std::span<std::uint8_t>(data));
    if(!is(data,0,data_object))bad("ASF 缺少 Data Object");
    const auto data_size=le(data,16,8),total=le(data,40,8);
    movie->data_offset=header_size+50;
    // 廣播或被截斷的檔案以實際檔案長度為準；封包數取標頭宣告與實際可讀數量的較小者。
    const auto data_end=data_size>=50 && data_size<=size-header_size?header_size+data_size:size;
    const auto available=(data_end-movie->data_offset)/movie->packet_size;
    const auto declared=total?total:movie->packet_count;
    movie->packet_count=declared?std::min(declared,available):available;
    for(std::uint64_t at=data_end;size-at>=24 && movie->index.empty();){
        std::array<std::uint8_t,24> object{};co_await read(at,std::span<std::uint8_t>(object));
        const auto object_size=le(object,16,8);if(object_size<24 || object_size>size-at)break; // 檔尾索引損毀不影響播放，只是不能用索引跳轉。
        if(is(object,0,simple_index_object) && object_size<=64U*1024U*1024U){
            std::vector<std::uint8_t> body(static_cast<std::size_t>(object_size-24));if(!body.empty())co_await read(at+24,std::span<std::uint8_t>(body));
            parse_asf_simple_index(body,*movie);
        }
        at+=object_size;
    }
    co_return movie;
}
// 解多工狀態：跨封包重組中的媒體物件與已完成、尚未取走的物件（依檔案順序）。
struct AsfDemux {
    struct Partial {bool active{},key{},sized{};std::uint32_t object{},size{},time{};std::vector<std::uint8_t> bytes;};
    std::uint64_t next_packet{};
    std::map<std::uint8_t,Partial> partial;
    std::deque<AsfObject> ready;
    std::bitset<128> selected,wait_key;     // wait_key：跳轉後該串流要等到 keyframe 才開始交付。
    bool flushed{},busy{};
    void reset(std::uint64_t packet){next_packet=packet;partial.clear();ready.clear();flushed=false;wait_key.reset();}
    void select(std::uint8_t stream,bool enabled){
        selected.set(stream&0x7fU,enabled);if(enabled)return;
        partial.erase(stream);std::erase_if(ready,[&](const AsfObject& object){return object.stream==stream;});
    }
    void emit(const AsfMovie& movie,std::uint8_t stream,bool key,std::uint32_t time,std::vector<std::uint8_t> bytes){
        if(wait_key.test(stream)){if(!key)return;wait_key.reset(stream);}
        AsfObject object;object.stream=stream;object.key=key;object.bytes=std::move(bytes);
        const auto ms=static_cast<std::int64_t>(time)-static_cast<std::int64_t>(std::min<std::uint64_t>(movie.preroll,UINT32_MAX));
        object.time=std::max<std::int64_t>(ms,0)*10000;ready.push_back(std::move(object));
    }
    void finish(const AsfMovie& movie,std::uint8_t stream,Partial& item){
        // 知道物件大小卻沒收齊代表資料遺失，不交付殘缺物件。
        if(item.active && (!item.sized || item.bytes.size()==item.size))emit(movie,stream,item.key,item.time,std::move(item.bytes));
        item.active=false;item.bytes.clear();
    }
    void feed(const AsfMovie& movie,std::span<const std::uint8_t> packet){
        asf_detail::each_payload(packet,[&](const asf_detail::Payload& payload){
            if(!selected.test(payload.stream) || !movie.find(payload.stream))return;
            auto& item=partial[payload.stream];
            if(payload.whole){finish(movie,payload.stream,item);emit(movie,payload.stream,payload.key,payload.time,{payload.data.begin(),payload.data.end()});return;}
            if(item.active && item.object!=payload.object)finish(movie,payload.stream,item);
            if(!item.active){
                if(payload.offset)return; // 跳轉或遺失後從物件中段開始的片段略過。
                if(payload.sized && payload.size>asf_max_object)asf_detail::bad("ASF 媒體物件過大");
                item.active=true;item.key=payload.key;item.sized=payload.sized;item.object=payload.object;item.size=payload.size;item.time=payload.time;item.bytes.clear();
                if(item.sized)item.bytes.reserve(std::min<std::size_t>(item.size,4U*1024U*1024U));
            }else if(payload.offset!=item.bytes.size()){item.active=false;item.bytes.clear();return;}
            if(item.sized?payload.data.size()>item.size-item.bytes.size():payload.data.size()>asf_max_object-item.bytes.size())asf_detail::bad("ASF payload 超出媒體物件大小");
            item.bytes.insert(item.bytes.end(),payload.data.begin(),payload.data.end());
            if(item.sized && item.bytes.size()==item.size)finish(movie,payload.stream,item);
        });
    }
    void finish_all(const AsfMovie& movie){for(auto& [stream,item]:partial)finish(movie,stream,item);flushed=true;}
};
struct AsfBusyScope {
    AsfDemux& demux;
    explicit AsfBusyScope(AsfDemux& d):demux(d){if(demux.busy)throw AsfBusy{};demux.busy=true;}
    AsfBusyScope(const AsfBusyScope&)=delete;
    ~AsfBusyScope(){demux.busy=false;}
};
// 取出指定串流（stream＝0 表示任一已選取串流）的下一個完整媒體物件；讀到結尾回空值。
// 每個封包處理後游標即前進：損毀封包只回報一次錯誤，下一次讀取從其後繼續。
template<class Read> service_callback::Task<std::optional<AsfObject>> asf_next_object_async(const AsfMovie& movie,AsfDemux& demux,std::uint8_t stream,Read read){
    AsfBusyScope scope(demux);
    for(;;){
        const auto found=std::find_if(demux.ready.begin(),demux.ready.end(),[&](const AsfObject& object){return !stream || object.stream==stream;});
        if(found!=demux.ready.end()){auto object=std::move(*found);demux.ready.erase(found);co_return object;}
        if(demux.next_packet>=movie.packet_count){if(demux.flushed)co_return std::nullopt;demux.finish_all(movie);continue;}
        const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(movie.packet_count-demux.next_packet,std::max<std::uint32_t>(65536U/movie.packet_size,1)));
        std::vector<std::uint8_t> chunk(count*movie.packet_size);
        co_await read(movie.data_offset+demux.next_packet*movie.packet_size,std::span<std::uint8_t>(chunk));
        for(std::size_t n=0;n<count;++n){++demux.next_packet;demux.feed(movie,std::span<const std::uint8_t>(chunk).subspan(n*movie.packet_size,movie.packet_size));}
    }
}
// 跳轉到 time（100 ns，已扣除 Preroll 的呈現時間）。
// 有 Simple Index：直接取該時間所在項目的封包（內含最近的前一個 keyframe）。
// 沒有索引：以封包送出時間二分搜尋最後一個不晚於目標的封包。
// 兩者之後都從該封包重新解多工，視訊串流等到第一個 keyframe 才交付、其他串流從下一個完整物件開始。
template<class Read> service_callback::Task<bool> asf_seek_async(const AsfMovie& movie,AsfDemux& demux,std::int64_t time,Read read){
    AsfBusyScope scope(demux);
    std::uint64_t packet=0;
    if(time>0 && movie.packet_count){
        if(!movie.index.empty()){
            const auto preroll=static_cast<std::uint64_t>(std::min<std::uint64_t>(movie.preroll,UINT32_MAX))*10000U;
            const auto entry=std::min<std::uint64_t>((static_cast<std::uint64_t>(time)+preroll)/movie.index_interval,movie.index.size()-1);
            packet=std::min<std::uint64_t>(movie.index[static_cast<std::size_t>(entry)],movie.packet_count-1);
        }else{
            const auto target=static_cast<std::uint64_t>(time)/10000U;
            std::uint64_t low=0,high=movie.packet_count;
            while(high-low>1){
                const auto middle=low+(high-low)/2;std::array<std::uint8_t,48> head{};
                const auto size=std::min<std::size_t>(head.size(),movie.packet_size);
                co_await read(movie.data_offset+middle*movie.packet_size,std::span<std::uint8_t>(head).first(size));
                if(asf_detail::packet_header(std::span<const std::uint8_t>(head).first(size),movie.packet_size).send_time<=target)low=middle;else high=middle;
            }
            packet=low;
        }
    }
    demux.reset(packet);
    if(packet)for(const auto& stream:movie.streams)if(stream.video)demux.wait_key.set(stream.number);
    co_return true;
}
// 宿主檔案的讀取立即完成；以同一份協程流程同步執行。
struct AsfReadReady {
    bool await_ready()const noexcept{return true;}
    void await_suspend(std::coroutine_handle<>)const noexcept{}
    void await_resume()const noexcept{}
};
template<class T> T asf_run(service_callback::Task<T> task){task.address().resume();return task.take();}
template<class Read> std::shared_ptr<const AsfMovie> read_asf_movie(std::uint64_t size,Read read){
    return asf_run(read_asf_movie_async(size,[&](std::uint64_t at,std::span<std::uint8_t> bytes){read(at,bytes);return AsfReadReady{};}));
}
}
