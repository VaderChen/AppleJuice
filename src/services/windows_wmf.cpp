// Windows 中繼檔（WMF）家族（gdi32）：CreateMetaFileA／W、CloseMetaFile、PlayMetaFile、PlayMetaFileRecord、EnumMetaFile、
// GetWinMetaFileBits（EMF→WMF，含內嵌 EMF 註解）、SetWinMetaFileBits（WMF→EMF）、GdiIsMetaFileDC、GdiIsMetaPrintDC、GdiIsPlayMetafileDC。
// 錄製沿用 EMF 錄製 DC（emf_recorders）的 GuiDraw 串流；播放依 MS-WMF 記錄呼叫既有 GDI 服務，物件 handle 存在呼叫端的 handle table。
#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "windows_locale_data.hpp"
#include "windows_wmf.hpp"
#include <algorithm>
#include <cmath>

namespace x64arm {
bool valid_windows_metafile(std::span<const std::uint8_t> bytes){
    if(bytes.size()<18)return false;const auto words=wmf_dword(bytes,6);
    const auto type=wmf_word(bytes,0),version=wmf_word(bytes,4);
    return (type==1 || type==2) && wmf_word(bytes,2)==9 && (version==0x100 || version==0x300) && words>=9 && std::uint64_t{words}*2<=bytes.size();
}
std::size_t windows_metafile_size(std::span<const std::uint8_t> bytes){return 2*static_cast<std::size_t>(wmf_dword(bytes,6));}
std::vector<std::pair<std::size_t,std::size_t>> wmf_records(std::span<const std::uint8_t> bytes){
    if(!valid_windows_metafile(bytes))throw std::invalid_argument("WMF 標頭不合法");
    const auto size=windows_metafile_size(bytes);std::vector<std::pair<std::size_t,std::size_t>> records;
    // 有些寫入端省略 EOF：資料結束即視為結尾。
    for(std::size_t at=18;at+6<=size;){
        const auto words=wmf_dword(bytes,at);if(words<3 || std::uint64_t{words}*2>size-at || records.size()>=(1U<<22))throw std::invalid_argument("WMF 記錄越界");
        records.emplace_back(at,static_cast<std::size_t>(words)*2);if(!wmf_word(bytes,at+4))break;at+=static_cast<std::size_t>(words)*2;
    }
    return records;
}
namespace {
using Words=std::vector<std::uint16_t>;
constexpr std::uint16_t meta_savedc=0x001e,meta_restoredc=0x0127,meta_setrop2=0x0104,meta_setbkmode=0x0102,meta_setbkcolor=0x0201,meta_settextcolor=0x0209,
    meta_settextalign=0x012e,meta_intersectcliprect=0x0416,meta_excludecliprect=0x0415,meta_createpenindirect=0x02fa,meta_createbrushindirect=0x02fc,
    meta_createfontindirect=0x02fb,meta_selectobject=0x012d,meta_deleteobject=0x01f0,meta_rectangle=0x041b,meta_ellipse=0x0418,meta_moveto=0x0214,
    meta_lineto=0x0213,meta_patblt=0x061d,meta_exttextout=0x0a32,meta_escape=0x0626;
std::uint16_t s16(double value){
    const auto rounded=std::round(value);if(!std::isfinite(rounded) || rounded<-32768 || rounded>32767)throw std::range_error("WMF 座標超出 16 位元");
    return static_cast<std::uint16_t>(static_cast<std::int16_t>(rounded));
}
void push_dword(Words& words,std::uint32_t value){words.push_back(static_cast<std::uint16_t>(value));words.push_back(static_cast<std::uint16_t>(value>>16));}
void push_bytes(Words& words,std::span<const std::uint8_t> bytes){for(std::size_t i=0;i<bytes.size();i+=2)words.push_back(static_cast<std::uint16_t>(bytes[i]|(i+1<bytes.size()?static_cast<unsigned>(bytes[i+1])<<8:0U)));}
// ROP2（1～16）是 P、D 的真值表（位元索引 P*2+D）；PatBlt 的三元運算碼以 P*4+S*2+D 為索引，低位字為標準 RPN 編碼。
std::uint32_t patblt_rop(std::uint32_t rop2){
    static constexpr std::pair<std::uint8_t,std::uint16_t> codes[]{{0x00,0x0042},{0x05,0x00a9},{0x0a,0x0329},{0x0f,0x0001},{0x50,0x0325},{0x55,0x0009},{0x5a,0x0049},{0x5f,0x00e9},
        {0xa0,0x00c9},{0xa5,0x0065},{0xaa,0x0029},{0xaf,0x0229},{0xf0,0x0021},{0xf5,0x0225},{0xfa,0x0089},{0xff,0x0062}};
    if(rop2<1 || rop2>16)throw std::invalid_argument("WMF ROP2 不合法");
    const auto table=rop2-1;std::uint32_t index=0;for(unsigned i=0;i<8;++i)if((table>>(((i>>2)&1U)*2+(i&1U)))&1U)index|=1U<<i;
    for(const auto& [code,low]:codes)if(code==index)return index<<16|low;
    throw std::invalid_argument("WMF ROP2 不合法");
}
// UTF-16 文字依字型碼頁編成位元組；字距依 A 版契約放在每個字元的第一個位元組，其餘位元組為 0。
struct EncodedText {std::vector<std::uint8_t> bytes;std::vector<std::int32_t> dx;};
EncodedText encode_text(std::u16string_view text,std::uint32_t page,std::span<const std::int32_t> advances){
    EncodedText out;
    for(std::size_t i=0;i<text.size();){
        const std::size_t units=(text[i]>=0xd800 && text[i]<0xdc00 && i+1<text.size() && text[i+1]>=0xdc00 && text[i+1]<0xe000)?2:1;std::vector<std::uint8_t> bytes;
        if(!page){const auto c=text[i];bytes.push_back(static_cast<std::uint8_t>(c>=0xf000 && c<=0xf0ff?c&0xff:c<256?c:u'?'));}
        else if(const auto encoded=windows_nls_encode(text.substr(i,units),page,false,"?");encoded && !encoded->bytes.empty())bytes=encoded->bytes;
        else bytes.push_back('?');
        std::int64_t advance=0;if(!advances.empty())for(std::size_t k=0;k<units;++k)advance+=advances[i+k];
        for(std::size_t k=0;k<bytes.size();++k){out.bytes.push_back(bytes[k]);out.dx.push_back(k?0:static_cast<std::int32_t>(std::clamp<std::int64_t>(advance,INT32_MIN,INT32_MAX)));}
        i+=units;
    }
    return out;
}
// 位元組依字型碼頁解成 UTF-16；每個字元的位元組字距加總後放在第一個 UTF-16 單位。
struct DecodedText {std::u16string text;std::vector<std::int32_t> dx;};
DecodedText decode_text(std::span<const std::uint8_t> bytes,std::uint32_t page,std::span<const std::int32_t> dx){
    DecodedText out;
    for(std::size_t at=0;at<bytes.size();){
        char32_t character=bytes[at];std::size_t used=1;
        if(page){const auto decoded=windows_crt_decode(bytes.subspan(at),page);if(decoded.status==CrtDecodedCharacter::Status::Complete && decoded.bytes){character=decoded.character;used=decoded.bytes;}else character=u'?';}
        std::int64_t advance=0;if(!dx.empty())for(std::size_t k=at;k<at+used && k<dx.size();++k)advance+=dx[k];
        const auto clamped=static_cast<std::int32_t>(std::clamp<std::int64_t>(advance,INT32_MIN,INT32_MAX));
        if(character>0xffff){out.text.push_back(static_cast<char16_t>(0xd800+((character-0x10000)>>10)));out.text.push_back(static_cast<char16_t>(0xdc00+((character-0x10000)&0x3ff)));if(!dx.empty()){out.dx.push_back(clamped);out.dx.push_back(0);}}
        else {out.text.push_back(static_cast<char16_t>(character));if(!dx.empty())out.dx.push_back(clamped);}
        at+=used;
    }
    return out;
}
// DIB（BITMAPINFO＋色表＋像素）各部分的位移；像素長度不足時視為不合法。
struct DibLayout {std::size_t bits{},size{};};
std::optional<DibLayout> dib_layout(std::span<const std::uint8_t> dib,std::uint32_t usage){
    if(dib.size()<12)return {};const auto header=wmf_dword(dib,0);std::int64_t width=0,height=0;std::uint32_t bpp=0,compression=0,image=0,used=0;
    if(header==12){width=wmf_word(dib,4);height=wmf_word(dib,6);bpp=wmf_word(dib,10);}
    else if(header>=40 && header<=dib.size()){width=static_cast<std::int32_t>(wmf_dword(dib,4));height=static_cast<std::int32_t>(wmf_dword(dib,8));bpp=wmf_word(dib,14);compression=wmf_dword(dib,16);image=wmf_dword(dib,20);used=wmf_dword(dib,32);}
    else return {};
    if(width<=0 || !height || !bpp || bpp>32 || used>65536)return {};
    const std::uint64_t entries=used?used:bpp<=8?1U<<bpp:0,entry=usage==1?2:header==12?3:4;
    const std::uint64_t masks=header==40 && compression==3?12:header==40 && compression==6?16:0;
    const auto bits=header+entries*entry+masks;
    const std::uint64_t stride=(static_cast<std::uint64_t>(width)*bpp+31)/32*4,rows=static_cast<std::uint64_t>(height<0?-height:height);
    const auto pixels=(compression==0 || compression==3 || compression==6)?stride*rows:image;
    if(bits>dib.size() || pixels>dib.size()-bits)return {};
    return DibLayout{static_cast<std::size_t>(bits),static_cast<std::size_t>(bits+pixels)};
}
}
void wmf_record(GuiMetafile& meta,std::uint16_t function,std::span<const std::uint16_t> parameters){
    const auto bytes=6+parameters.size()*2;if(meta.bytes.size()+bytes>windows_metafile_limit)throw std::length_error("WMF 超過大小上限");
    const auto words=static_cast<std::uint32_t>(3+parameters.size());
    for(const auto value:{static_cast<std::uint16_t>(words),static_cast<std::uint16_t>(words>>16),function}){meta.bytes.push_back(static_cast<std::uint8_t>(value));meta.bytes.push_back(static_cast<std::uint8_t>(value>>8));}
    for(const auto value:parameters){meta.bytes.push_back(static_cast<std::uint8_t>(value));meta.bytes.push_back(static_cast<std::uint8_t>(value>>8));}
    ++meta.records;
}
void wmf_begin(GuiMetafile& meta,bool disk){
    meta.bytes.assign(18,0);meta.bytes[0]=disk?2:1;meta.bytes[2]=9;meta.bytes[5]=3;meta.records=0;meta.wmf=true;
}
void wmf_finish(GuiMetafile& meta){
    wmf_record(meta,0,{});
    // NumberOfObjects：依建立與刪除記錄模擬同時存在的最大物件數；MaxRecord 為最大記錄的 WORD 數。
    std::uint32_t largest=0;unsigned live=0,peak=0;
    for(std::size_t at=18;at+6<=meta.bytes.size();){
        const auto words=wmf_dword(meta.bytes,at);const auto function=wmf_word(meta.bytes,at+4);largest=std::max(largest,words);
        if(function==0x02fa || function==0x02fb || function==0x02fc || function==0x00f7 || function==0x01f9 || function==0x0142 || function==0x06ff)peak=std::max(peak,++live);
        else if(function==meta_deleteobject && live)--live;
        at+=static_cast<std::size_t>(words)*2;
    }
    const auto size=static_cast<std::uint32_t>(meta.bytes.size()/2);
    const auto put=[&](std::size_t at,std::uint16_t value){meta.bytes[at]=static_cast<std::uint8_t>(value);meta.bytes[at+1]=static_cast<std::uint8_t>(value>>8);};
    put(6,static_cast<std::uint16_t>(size));put(8,static_cast<std::uint16_t>(size>>16));put(10,static_cast<std::uint16_t>(std::min(peak,65535U)));
    put(12,static_cast<std::uint16_t>(largest));put(14,static_cast<std::uint16_t>(largest>>16));put(16,0);
}
void wmf_draw(GuiMetafile& meta,const GuiDraw& draw,const GuiTransform& output){
    if(draw.kind==GuiDraw::Kind::Icon || draw.pattern_brush)throw std::domain_error("WMF 圖示與圖樣筆刷記錄尚未支援");
    const auto m=draw.device_coordinates?output:gui_multiply(draw.transform,output);
    if(m[1]!=0 || m[2]!=0 || !gui_valid_transform(m))throw std::domain_error("WMF 無法表示旋轉或斜切");
    // 先在本地組完整筆記錄，驗證或溢位失敗時不留下半筆。
    std::vector<std::pair<std::uint16_t,Words>> out;const auto add=[&](std::uint16_t function,Words words){out.emplace_back(function,std::move(words));};
    const auto point=[&](double x,double y){const auto p=gui_point(m,x,y);return std::array<std::uint16_t,2>{s16(p[0]),s16(p[1])};};
    const auto color=[](std::uint32_t value){Words words;push_dword(words,value);return words;};
    add(meta_savedc,{});
    if(draw.kind!=GuiDraw::Kind::Fill)add(meta_setrop2,{static_cast<std::uint16_t>(draw.rop2)});
    // 裁切為來源裝置空間，只套用輸出轉換。
    const auto clip_rect=[&](std::uint16_t function,const GuiRect& rect){const auto a=gui_point(output,rect[0],rect[1]),b=gui_point(output,rect[2],rect[3]);add(function,{s16(b[1]),s16(b[0]),s16(a[1]),s16(a[0])});};
    if(draw.clipped)clip_rect(meta_intersectcliprect,draw.clip);for(const auto& rect:draw.excluded)clip_rect(meta_excludecliprect,rect);
    std::vector<std::uint16_t> objects;
    const auto brush=[&](std::uint32_t value,bool filled){
        Words words{static_cast<std::uint16_t>(!filled?1:draw.hatch>=0?2:0)};push_dword(words,value&0x00ffffffU);words.push_back(static_cast<std::uint16_t>(draw.hatch>=0?draw.hatch:0));
        if(filled && draw.hatch>=0){add(meta_setbkcolor,color(draw.pattern_background));add(meta_setbkmode,{static_cast<std::uint16_t>(draw.pattern_opaque?2:1)});}
        const auto index=static_cast<std::uint16_t>(objects.size());add(meta_createbrushindirect,std::move(words));add(meta_selectobject,{index});objects.push_back(index);
    };
    const auto p0=point(draw.x,draw.y),p1=point(draw.right,draw.bottom);
    if(draw.kind==GuiDraw::Kind::Text){
        if(draw.glyph_indices)throw std::domain_error("WMF 無法表示字形索引");
        if(std::any_of(draw.advances_y.begin(),draw.advances_y.end(),[](std::int32_t value){return value!=0;}))throw std::domain_error("WMF 無法表示垂直字距");
        const auto page=gdi_charset_code_page(draw.font.charset);std::vector<std::int32_t> scaled;
        for(const auto advance:draw.advances)scaled.push_back(static_cast<std::int16_t>(s16(advance*m[0])));
        const auto text=encode_text(draw.text,page,scaled);if(text.bytes.size()>0x7fff)throw std::range_error("WMF 文字過長");
        const auto height=draw.font.height?static_cast<std::int16_t>(s16(draw.font.height*std::abs(m[3]))):std::int16_t{0},width=static_cast<std::int16_t>(s16(draw.font.width*std::abs(m[0])));
        std::vector<std::uint8_t> font(50);
        const auto put=[&](std::size_t at,std::int32_t value){font[at]=static_cast<std::uint8_t>(value);font[at+1]=static_cast<std::uint8_t>(value>>8);};
        put(0,height?height:static_cast<std::int16_t>(draw.font.height>0?1:draw.font.height<0?-1:0));put(2,width);put(8,draw.font.weight);
        font[10]=draw.font.italic;font[11]=draw.font.underline;font[12]=draw.font.strikeout;font[13]=draw.font.charset;font[17]=draw.font.pitch_family;
        const auto face=encode_text(std::u16string_view(draw.font.face).substr(0,31),page,{});std::copy_n(face.bytes.begin(),std::min<std::size_t>(face.bytes.size(),31),font.begin()+18);
        add(meta_settextcolor,color(draw.color));add(meta_setbkmode,{1});add(meta_settextalign,{0});
        Words font_words;push_bytes(font_words,font);const auto index=static_cast<std::uint16_t>(objects.size());add(meta_createfontindirect,std::move(font_words));add(meta_selectobject,{index});objects.push_back(index);
        Words words{p0[1],p0[0],static_cast<std::uint16_t>(text.bytes.size()),static_cast<std::uint16_t>(draw.rtl_reading?0x80:0)};push_bytes(words,text.bytes);
        if(!draw.advances.empty())for(const auto value:text.dx)words.push_back(static_cast<std::uint16_t>(value));
        add(meta_exttextout,std::move(words));
    }else if(draw.kind==GuiDraw::Kind::Fill){
        // 填色等同 FillRect／PatBlt：以筆刷與等價的三元運算碼記錄。
        brush(draw.color,true);Words words;push_dword(words,patblt_rop(draw.rop2));
        const auto h=s16(static_cast<double>(static_cast<std::int16_t>(p1[1]))-static_cast<std::int16_t>(p0[1])),w=s16(static_cast<double>(static_cast<std::int16_t>(p1[0]))-static_cast<std::int16_t>(p0[0]));
        words.insert(words.end(),{h,w,p0[1],p0[0]});add(meta_patblt,std::move(words));
    }else {
        Words pen{static_cast<std::uint16_t>(draw.pen_width?0:5),static_cast<std::uint16_t>(draw.pen_width?std::max<std::int32_t>(1,static_cast<std::int16_t>(s16(draw.pen_width*std::abs(m[0])))):0),0};
        push_dword(pen,draw.color&0x00ffffffU);const auto index=static_cast<std::uint16_t>(objects.size());add(meta_createpenindirect,std::move(pen));add(meta_selectobject,{index});objects.push_back(index);
        if(draw.kind==GuiDraw::Kind::Line){add(meta_moveto,{p0[1],p0[0]});add(meta_lineto,{p1[1],p1[0]});}
        else {brush(draw.fill_color,draw.filled);add(draw.kind==GuiDraw::Kind::Ellipse?meta_ellipse:meta_rectangle,{p1[1],p1[0],p0[1],p0[0]});}
    }
    // RestoreDC 先取消選取，再刪除本筆建立的物件。
    add(meta_restoredc,{0xffff});for(const auto index:objects)add(meta_deleteobject,{index});
    std::size_t bytes=0;for(const auto& [function,words]:out){(void)function;bytes+=6+words.size()*2;}
    if(meta.bytes.size()+bytes>windows_metafile_limit)throw std::length_error("WMF 超過大小上限");
    for(const auto& [function,words]:out)wmf_record(meta,function,words);
}
void wmf_embed_emf(GuiMetafile& meta,std::span<const std::uint8_t> emf){
    std::uint16_t sum=0;for(std::size_t i=0;i+1<emf.size();i+=2)sum^=static_cast<std::uint16_t>(emf[i]|static_cast<unsigned>(emf[i+1])<<8);
    const auto checksum=static_cast<std::uint16_t>(~sum);const auto total=static_cast<std::uint32_t>(emf.size());const auto count=std::max<std::uint32_t>(1,(total+8191)/8192);
    for(std::uint32_t index=0,offset=0;index<count;++index){
        const auto chunk=std::min<std::uint32_t>(8192,total-offset);std::vector<std::uint8_t> data(34);
        const auto put=[&](std::size_t at,std::uint32_t value,unsigned size){for(unsigned i=0;i<size;++i)data[at+i]=static_cast<std::uint8_t>(value>>(8*i));};
        put(0,0x43464d57,4);put(4,1,4);put(8,0x10000,4);put(12,checksum,2);put(14,0,4);put(18,count,4);put(22,chunk,4);put(26,total-offset-chunk,4);put(30,total,4);
        data.insert(data.end(),emf.begin()+offset,emf.begin()+offset+chunk);
        Words words{15,static_cast<std::uint16_t>(34+chunk)};push_bytes(words,data);wmf_record(meta,meta_escape,words);offset+=chunk;
    }
}
std::optional<std::vector<std::uint8_t>> wmf_embedded_emf(std::span<const std::uint8_t> wmf){
    std::vector<std::uint8_t> emf;std::uint32_t expected=0,total=0,checksum=0,seen=0;
    for(const auto& [offset,size]:wmf_records(wmf)){
        const auto record=wmf.subspan(offset,size);
        const bool comment=size>=50 && wmf_word(record,4)==meta_escape && wmf_word(record,6)==15 && wmf_dword(record,10)==0x43464d57 && wmf_dword(record,14)==1;
        if(!comment){if(seen)break;continue;}
        const auto count=wmf_dword(record,28),chunk=wmf_dword(record,32),remaining=wmf_dword(record,36),size_total=wmf_dword(record,40);
        if(chunk>8192 || wmf_word(record,8)<34+chunk || 44+std::size_t{chunk}>size || size_total>16*1024*1024)return {};
        if(!seen){expected=count;total=size_total;checksum=wmf_word(record,22);}
        else if(count!=expected || size_total!=total || wmf_word(record,22)!=checksum)return {};
        if(emf.size()+chunk+remaining!=total)return {};
        emf.insert(emf.end(),record.begin()+44,record.begin()+44+chunk);if(++seen==expected)break;
    }
    if(!seen || seen!=expected || emf.size()!=total)return {};
    std::uint16_t sum=0;for(std::size_t i=0;i+1<emf.size();i+=2)sum^=static_cast<std::uint16_t>(emf[i]|static_cast<unsigned>(emf[i+1])<<8);
    if(static_cast<std::uint16_t>(~sum)!=checksum)return {};
    return emf;
}

std::span<const WindowsServiceDescriptor> windows_metafile_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"gdi32.dll","CreateMetaFileA",1},{"gdi32.dll","CreateMetaFileW",1},{"gdi32.dll","CloseMetaFile",1},{"gdi32.dll","PlayMetaFile",2},
        {"gdi32.dll","PlayMetaFileRecord",4},{"gdi32.dll","EnumMetaFile",4},{"gdi32.dll","GetWinMetaFileBits",5},{"gdi32.dll","SetWinMetaFileBits",4},
        {"gdi32.dll","GdiIsMetaFileDC",1},{"gdi32.dll","GdiIsMetaPrintDC",1},{"gdi32.dll","GdiIsPlayMetafileDC",1},
    };return table;
}
bool WindowsServices::write_guest_file(GuestState& state,GuestMemory& memory,std::u16string_view path,std::span<const std::uint8_t> bytes){
    // 磁碟 metafile 經一般檔案服務寫入，沿用路徑映射、權限與共用模式。
    GuestScratch scratch(*this,memory,state);const auto name=scratch.text(path),count=scratch.allocate(8);if(!name || !count)throw std::bad_alloc();
    const auto file=nested_service(state,memory,"kernel32.dll","CreateFileW",{name,0x40000000U,0,0,2,0x80,0});
    if(!file || file==(x86_?std::uint64_t{UINT32_MAX}:UINT64_MAX))return false;
    bool written=true;
    if(!bytes.empty()){const auto buffer=scratch.allocate(bytes.size());if(!buffer){(void)nested_service(state,memory,"kernel32.dll","CloseHandle",{file});throw std::bad_alloc();}
        memory.write(GuestAddress{buffer},bytes);written=nested_service(state,memory,"kernel32.dll","WriteFile",{file,buffer,bytes.size(),count,0}) && memory.load(GuestAddress{count},32)==bytes.size();}
    return nested_service(state,memory,"kernel32.dll","CloseHandle",{file}) && written;
}
std::optional<std::vector<std::uint8_t>> WindowsServices::read_guest_file(GuestState& state,GuestMemory& memory,std::u16string_view path,std::size_t minimum,std::size_t limit){
    // GetMetaFile 與 GetEnhMetaFile 共用：經一般檔案服務讀入；長度不在格式範圍時回 ERROR_INVALID_DATA。
    GuestScratch scratch(*this,memory,state);const auto name=scratch.text(path),count=scratch.allocate(8);if(!name || !count)throw std::bad_alloc();
    const auto file=nested_service(state,memory,"kernel32.dll","CreateFileW",{name,0x80000000U,1,0,3,0x80,0});
    if(!file || file==(x86_?std::uint64_t{UINT32_MAX}:UINT64_MAX))return std::nullopt;
    struct Close {WindowsServices& self;GuestState& state;GuestMemory& memory;std::uint64_t handle;~Close(){try{(void)self.nested_service(state,memory,"kernel32.dll","CloseHandle",{handle});}catch(...){}}} close{*this,state,memory,file};
    if(!nested_service(state,memory,"kernel32.dll","GetFileSizeEx",{file,count}))return std::nullopt;
    const auto size=memory.load(GuestAddress{count},64);if(size<minimum || size>limit){last_error_=13;return std::nullopt;}
    const auto buffer=scratch.allocate(static_cast<std::size_t>(size));if(!buffer)throw std::bad_alloc();
    if(!nested_service(state,memory,"kernel32.dll","ReadFile",{file,buffer,size,count,0}) || memory.load(GuestAddress{count},32)!=size)return std::nullopt;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));memory.read(GuestAddress{buffer},bytes);return bytes;
}
std::array<std::uint64_t,4> WindowsServices::windows_metafile_selection(GuestState& state,GuestMemory& memory,std::uint64_t dc){
    std::array<std::uint64_t,4> selected{};if(!dc)return selected;
    // OBJ_PEN、OBJ_BRUSH、OBJ_FONT、OBJ_PAL：播放結束後還原原本選取的物件。
    constexpr std::array<std::uint32_t,4> types{1,2,6,5};for(unsigned i=0;i<4;++i)selected[i]=nested_service(state,memory,"gdi32.dll","GetCurrentObject",{dc,types[i]});return selected;
}
void WindowsServices::finish_windows_metafile_playback(GuestState& state,GuestMemory& memory,std::uint64_t dc,std::uint64_t table,unsigned handles,const std::array<std::uint64_t,4>& selected){
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    try{
        if(dc){for(unsigned i=0;i<3;++i)if(selected[i])(void)nested_service(state,memory,"gdi32.dll","SelectObject",{dc,selected[i]});
            if(selected[3])(void)nested_service(state,memory,"gdi32.dll","SelectPalette",{dc,selected[3],0});}
        // handle table 中仍存在的物件由播放端建立，結束時刪除。
        for(unsigned i=0;i<handles && table;++i){const auto handle=memory.load(GuestAddress{table+i*unit},bits);if(!handle)continue;memory.store(GuestAddress{table+i*unit},bits,0);(void)nested_service(state,memory,"gdi32.dll","DeleteObject",{handle});}
    }catch(const MemoryFault&){}
}
bool WindowsServices::play_windows_metafile(GuestState& state,GuestMemory& memory,std::uint64_t dc,std::span<const std::uint8_t> bytes){
    const auto records=wmf_records(bytes);const unsigned handles=wmf_word(bytes,10),unit=x86_?4U:8U;
    const auto table=allocate_heap(memory,std::max<std::uint64_t>(1,handles)*unit);if(!table)throw std::bad_alloc();
    struct Release {WindowsServices& self;GuestMemory& memory;const GuestState& state;std::uint64_t pointer;~Release(){(void)self.free_heap(memory,pointer,state);}} release{*this,memory,state,table};
    memory.write(GuestAddress{table},std::vector<std::uint8_t>(std::max<std::size_t>(1,handles)*unit));
    const auto selected=windows_metafile_selection(state,memory,dc);
    try{for(const auto& [offset,size]:records){const auto record=bytes.subspan(offset,size);if(!wmf_word(record,4))break;(void)play_windows_metafile_record(state,memory,dc,table,handles,record);}}
    catch(...){finish_windows_metafile_playback(state,memory,dc,table,handles,selected);throw;}
    finish_windows_metafile_playback(state,memory,dc,table,handles,selected);return true;
}
bool WindowsServices::play_windows_metafile_record(GuestState& state,GuestMemory& memory,std::uint64_t dc,std::uint64_t table,unsigned handles,std::span<const std::uint8_t> record){
    if(record.size()<6 || std::uint64_t{wmf_dword(record,0)}*2!=record.size())throw std::invalid_argument("WMF 記錄長度不符");
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);const auto function=wmf_word(record,4);const std::size_t count=(record.size()-6)/2;
    const auto u=[&](std::size_t i)->std::uint32_t{if(i>=count)throw std::invalid_argument("WMF 參數不足");return wmf_word(record,6+i*2);};
    const auto p=[&](std::size_t i){return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int16_t>(u(i))));};
    const auto dw=[&](std::size_t i){return u(i)|u(i+1)<<16;};
    const auto bytes_at=[&](std::size_t first,std::size_t length){const auto at=6+first*2;if(at>record.size() || length>record.size()-at)throw std::invalid_argument("WMF 資料越界");return record.subspan(at,length);};
    const auto gdi=[&](std::string_view name,std::initializer_list<std::uint64_t> arguments){return nested_service(state,memory,"gdi32.dll",name,arguments);};
    GuestScratch scratch(*this,memory,state);
    const auto copy=[&](std::span<const std::uint8_t> bytes){const auto address=scratch.allocate(bytes.size());if(!address)throw std::bad_alloc();memory.write(GuestAddress{address},bytes);return address;};
    const auto object=[&](std::size_t index){if(index>=handles || !table)throw std::invalid_argument("WMF 物件索引超出 handle table");return memory.load(GuestAddress{table+index*unit},bits);};
    // 新物件放在 handle table 第一個空位；沒有空位時刪除並回報失敗。
    const auto keep=[&](std::uint64_t handle){
        if(!handle)return false;
        for(unsigned i=0;i<handles && table;++i)if(!memory.load(GuestAddress{table+i*unit},bits)){memory.store(GuestAddress{table+i*unit},bits,handle);return true;}
        (void)gdi("DeleteObject",{handle});last_error_=8;return false;
    };
    const auto points=[&](std::size_t first,std::size_t n){
        if(n>65535 || first+n*2>count)throw std::invalid_argument("WMF 頂點超出記錄");const auto address=scratch.allocate(std::max<std::size_t>(n,1)*8);if(!address)throw std::bad_alloc();
        for(std::size_t i=0;i<n;++i){memory.store(GuestAddress{address+i*8},32,static_cast<std::uint32_t>(p(first+i*2)));memory.store(GuestAddress{address+i*8+4},32,static_cast<std::uint32_t>(p(first+i*2+1)));}
        return address;
    };
    const auto charset=[&]{const auto found=gui_.contexts.find(dc);return found==gui_.contexts.end()?std::uint8_t{1}:gui_font(gui_,found->second.font).charset;};
    const auto wide_text=[&](const DecodedText& text){const auto address=scratch.allocate(std::max<std::size_t>(text.text.size(),1)*2);if(!address)throw std::bad_alloc();for(std::size_t i=0;i<text.text.size();++i)memory.store(GuestAddress{address+i*2},16,text.text[i]);return address;};
    const auto ok=[](std::uint64_t value){return value!=0;};
    const auto valid=[](std::uint64_t value){return static_cast<std::uint32_t>(value)!=UINT32_MAX;};
    const auto current_palette=[&]{return gdi("GetCurrentObject",{dc,5});};
    // Bitmap16（Type、Width、Height、WidthBytes、Planes、BitsPixel）→ 相容點陣圖。
    const auto bitmap16=[&](std::span<const std::uint8_t> header,std::span<const std::uint8_t> pixels){
        const auto width=wmf_word(header,2),height=wmf_word(header,4),stride=wmf_word(header,6);const auto planes=header[8],depth=header[9];
        if(!width || !height || stride%2 || std::size_t{stride}*height>pixels.size())throw std::invalid_argument("WMF Bitmap16 不合法");
        return gdi("CreateBitmap",{width,height,planes,depth,copy(pixels.first(std::size_t{stride}*height))});
    };
    const auto blit_bitmap16=[&](std::span<const std::uint8_t> target,std::uint32_t rop,std::uint64_t x,std::uint64_t y,std::uint64_t w,std::uint64_t h,std::uint64_t sx,std::uint64_t sy,std::optional<std::array<std::uint64_t,2>> source_size){
        if(target.size()<10)throw std::invalid_argument("WMF Bitmap16 不足");const auto bitmap=bitmap16(target,target.subspan(10));if(!bitmap)return false;
        const auto memory_dc=gdi("CreateCompatibleDC",{0});if(!memory_dc){(void)gdi("DeleteObject",{bitmap});return false;}
        const auto previous=gdi("SelectObject",{memory_dc,bitmap});
        const auto result=source_size?gdi("StretchBlt",{dc,x,y,w,h,memory_dc,sx,sy,(*source_size)[0],(*source_size)[1],rop}):gdi("BitBlt",{dc,x,y,w,h,memory_dc,sx,sy,rop});
        if(previous)(void)gdi("SelectObject",{memory_dc,previous});(void)gdi("DeleteDC",{memory_dc});(void)gdi("DeleteObject",{bitmap});return ok(result);
    };
    const auto dib=[&](std::size_t first,std::uint32_t usage)->std::optional<std::pair<std::uint64_t,std::uint64_t>>{
        const auto data=bytes_at(first,record.size()-6-first*2);const auto layout=dib_layout(data,usage);if(!layout)return {};
        const auto address=copy(data.first(layout->size));return std::pair{address,address+layout->bits};
    };
    switch(function){
    case 0x0000:return true;
    case 0x001e:return ok(gdi("SaveDC",{dc}));
    case 0x0127:return ok(gdi("RestoreDC",{dc,p(0)}));
    case 0x0201:return valid(gdi("SetBkColor",{dc,dw(0)}));
    case 0x0209:return valid(gdi("SetTextColor",{dc,dw(0)}));
    case 0x0102:return ok(gdi("SetBkMode",{dc,u(0)}));
    case 0x0103:return ok(gdi("SetMapMode",{dc,u(0)}));
    case 0x0104:return ok(gdi("SetROP2",{dc,u(0)}));
    case 0x0105:return ok(gdi("SetRelAbs",{dc,u(0)}));
    case 0x0106:return ok(gdi("SetPolyFillMode",{dc,u(0)}));
    case 0x0107:return ok(gdi("SetStretchBltMode",{dc,u(0)}));
    case 0x0108:return static_cast<std::uint32_t>(gdi("SetTextCharacterExtra",{dc,p(0)}))!=0x80000000U;
    case 0x012e:return valid(gdi("SetTextAlign",{dc,u(0)}));
    case 0x0231:return valid(gdi("SetMapperFlags",{dc,dw(0)}));
    case 0x0149:return valid(gdi("SetLayout",{dc,u(0)}));
    case 0x020a:return ok(gdi("SetTextJustification",{dc,p(1),p(0)}));
    case 0x020b:return ok(gdi("SetWindowOrgEx",{dc,p(1),p(0),0}));
    case 0x020c:return ok(gdi("SetWindowExtEx",{dc,p(1),p(0),0}));
    case 0x020d:return ok(gdi("SetViewportOrgEx",{dc,p(1),p(0),0}));
    case 0x020e:return ok(gdi("SetViewportExtEx",{dc,p(1),p(0),0}));
    case 0x020f:return ok(gdi("OffsetWindowOrgEx",{dc,p(1),p(0),0}));
    case 0x0211:return ok(gdi("OffsetViewportOrgEx",{dc,p(1),p(0),0}));
    case 0x0410:return ok(gdi("ScaleWindowExtEx",{dc,p(3),p(2),p(1),p(0),0}));
    case 0x0412:return ok(gdi("ScaleViewportExtEx",{dc,p(3),p(2),p(1),p(0),0}));
    case 0x0213:return ok(gdi("LineTo",{dc,p(1),p(0)}));
    case 0x0214:return ok(gdi("MoveToEx",{dc,p(1),p(0),0}));
    case 0x0415:return ok(gdi("ExcludeClipRect",{dc,p(3),p(2),p(1),p(0)}));
    case 0x0416:return ok(gdi("IntersectClipRect",{dc,p(3),p(2),p(1),p(0)}));
    case 0x0220:return ok(gdi("OffsetClipRgn",{dc,p(1),p(0)}));
    case 0x0418:return ok(gdi("Ellipse",{dc,p(3),p(2),p(1),p(0)}));
    case 0x041b:return ok(gdi("Rectangle",{dc,p(3),p(2),p(1),p(0)}));
    case 0x061c:return ok(gdi("RoundRect",{dc,p(5),p(4),p(3),p(2),p(1),p(0)}));
    case 0x0817:case 0x081a:case 0x0830:
        return ok(gdi(function==0x0817?"Arc":function==0x081a?"Pie":"Chord",{dc,p(7),p(6),p(5),p(4),p(3),p(2),p(1),p(0)}));
    case 0x0419:return ok(gdi("FloodFill",{dc,p(3),p(2),dw(0)}));
    case 0x0548:return ok(gdi("ExtFloodFill",{dc,p(4),p(3),dw(1),u(0)}));
    case 0x041f:return valid(gdi("SetPixel",{dc,p(3),p(2),dw(0)}));
    case 0x061d:return ok(gdi("PatBlt",{dc,p(5),p(4),p(3),p(2),dw(0)}));
    case 0x0324:case 0x0325:{const auto n=u(0);return ok(gdi(function==0x0324?"Polygon":"Polyline",{dc,points(1,n),n}));}
    case 0x0538:{
        const auto polygons=u(0);if(!polygons || 1+std::size_t{polygons}>count)return false;std::size_t total=0;
        const auto counts=scratch.allocate(std::size_t{polygons}*4);if(!counts)throw std::bad_alloc();
        for(unsigned i=0;i<polygons;++i){const auto n=u(1+i);total+=n;memory.store(GuestAddress{counts+i*4},32,n);}
        return ok(gdi("PolyPolygon",{dc,points(1+polygons,total),counts,polygons}));
    }
    case 0x0521:{
        const auto length=u(0);const auto text=decode_text(bytes_at(1,length),gdi_charset_code_page(charset()),{});const auto after=1+(std::size_t{length}+1)/2;
        return ok(gdi("TextOutW",{dc,p(after+1),p(after),wide_text(text),text.text.size()}));
    }
    case 0x0a32:{
        // {Y, X, StringLength, fwOpts, [Rect], String, [Dx]}：Rect 只在 ETO_OPAQUE／ETO_CLIPPED 時存在；Dx 依位元組逐一記錄。
        const auto length=u(2),options=u(3)&0x0096U;std::size_t at=4;std::uint64_t rect=0;
        if(u(3)&6U){rect=scratch.allocate(16);if(!rect)throw std::bad_alloc();for(unsigned i=0;i<4;++i)memory.store(GuestAddress{rect+i*4},32,static_cast<std::uint32_t>(p(4+i)));at=8;}
        const auto string=bytes_at(at,length);const auto dx_at=at+(std::size_t{length}+1)/2;std::vector<std::int32_t> dx;
        if(length && dx_at+length<=count)for(std::size_t i=0;i<length;++i)dx.push_back(static_cast<std::int16_t>(u(dx_at+i)));
        DecodedText text;
        if(options&16U){for(const auto c:string)text.text.push_back(c);text.dx=dx;}
        else text=decode_text(string,gdi_charset_code_page(charset()),dx);
        std::uint64_t advances=0;if(!text.dx.empty()){advances=scratch.allocate(text.dx.size()*4);if(!advances)throw std::bad_alloc();for(std::size_t i=0;i<text.dx.size();++i)memory.store(GuestAddress{advances+i*4},32,static_cast<std::uint32_t>(text.dx[i]));}
        return ok(gdi("ExtTextOutW",{dc,p(1),p(0),options,rect,wide_text(text),text.text.size(),advances}));
    }
    case 0x0626:{
        // MFCOMMENT 是播放端忽略的註解；其他 escape 交給裝置。
        const auto escape=u(0),length=u(1);if(escape==15)return true;const auto data=bytes_at(2,length);
        return static_cast<std::int32_t>(gdi("Escape",{dc,escape,length,length?copy(data):0,0}))>0;
    }
    case 0x012d:{const auto result=gdi("SelectObject",{dc,object(u(0))});return result && result!=(x86_?std::uint64_t{UINT32_MAX}:UINT64_MAX);}
    case 0x012c:return ok(gdi("SelectClipRgn",{dc,object(u(0))}));
    case 0x0228:return ok(gdi("FillRgn",{dc,object(u(0)),object(u(1))}));
    case 0x0429:return ok(gdi("FrameRgn",{dc,object(u(0)),object(u(1)),p(3),p(2)}));
    case 0x012a:return ok(gdi("InvertRgn",{dc,object(u(0))}));
    case 0x012b:return ok(gdi("PaintRgn",{dc,object(u(0))}));
    case 0x0234:return ok(gdi("SelectPalette",{dc,object(u(0)),0}));
    case 0x0035:return valid(gdi("RealizePalette",{dc}));
    case 0x0037:case 0x0436:{
        const auto start=u(0),n=u(1);const auto entries=copy(bytes_at(2,std::size_t{n}*4));
        return ok(gdi(function==0x0037?"SetPaletteEntries":"AnimatePalette",{current_palette(),start,n,entries}));
    }
    case 0x0139:return ok(gdi("ResizePalette",{current_palette(),u(0)}));
    case 0x01f0:{const auto index=u(0);const auto handle=object(index);memory.store(GuestAddress{table+index*unit},bits,0);return handle && ok(gdi("DeleteObject",{handle}));}
    case 0x00f7:{const auto n=u(1);std::vector<std::uint8_t> palette{0x00,0x03};const auto entries=bytes_at(2,std::size_t{n}*4);palette.push_back(static_cast<std::uint8_t>(n));palette.push_back(static_cast<std::uint8_t>(n>>8));palette.insert(palette.end(),entries.begin(),entries.end());
        return keep(gdi("CreatePalette",{copy(palette)}));}
    case 0x02fa:{
        // LogPen16 {Style, Width(POINTS), ColorRef} → LOGPEN。
        const auto pen=scratch.allocate(16);if(!pen)throw std::bad_alloc();memory.store(GuestAddress{pen},32,u(0));memory.store(GuestAddress{pen+4},32,static_cast<std::uint32_t>(p(1)));memory.store(GuestAddress{pen+12},32,dw(3));
        return keep(gdi("CreatePenIndirect",{pen}));
    }
    case 0x02fc:{
        // LogBrush16 {Style, ColorRef, Hatch}：只有 BS_SOLID、BS_NULL、BS_HATCHED。CreateBrushIndirect 以客體呼叫轉送，宿主端直接建立對應筆刷。
        const auto style=u(0);
        if(style==0)return keep(gdi("CreateSolidBrush",{dw(1)}));
        if(style==1)return keep(gdi("GetStockObject",{5}));
        if(style==2)return keep(gdi("CreateHatchBrush",{u(3),dw(1)}));
        last_error_=87;return false;
    }
    case 0x02fb:{
        // Font16 {Height, Width, Escapement, Orientation, Weight, 8 個位元組欄位, FaceName[32]（字型碼頁）} → LOGFONTW。
        const auto data=bytes_at(0,std::min<std::size_t>(record.size()-6,50));if(data.size()<18)throw std::invalid_argument("WMF 字型記錄不足");
        const auto font=scratch.allocate(92);if(!font)throw std::bad_alloc();for(unsigned i=0;i<5;++i)memory.store(GuestAddress{font+i*4},32,static_cast<std::uint32_t>(p(i)));
        for(unsigned i=0;i<8;++i)memory.store(GuestAddress{font+20+i},8,data[10+i]);
        auto face=data.subspan(18);if(const auto end=std::find(face.begin(),face.end(),0);end!=face.end())face=face.first(static_cast<std::size_t>(end-face.begin()));
        const auto name=decode_text(face,gdi_charset_code_page(data[13]),{}).text.substr(0,31);for(std::size_t i=0;i<name.size();++i)memory.store(GuestAddress{font+28+i*2},16,name[i]);
        return keep(gdi("CreateFontIndirectW",{font}));
    }
    case 0x06ff:{
        // Region {nextInChain, ObjectType, ObjectCount, RegionSize, ScanCount, maxScan, Bounds, Scans[]}：每條掃描線 {Count, Top, Bottom, (Left,Right)×Count/2, Count2}。
        const auto data=bytes_at(0,record.size()-6);if(data.size()<22)throw std::invalid_argument("WMF 區域記錄不足");const auto scans=wmf_word(data,10);
        std::vector<std::array<std::int32_t,4>> rects;std::size_t at=22;
        for(unsigned scan=0;scan<scans;++scan){
            const auto pairs=wmf_word(data,at)/2U;const auto top=static_cast<std::int16_t>(wmf_word(data,at+2)),bottom=static_cast<std::int16_t>(wmf_word(data,at+4));at+=6;
            for(unsigned i=0;i<pairs;++i){rects.push_back({static_cast<std::int16_t>(wmf_word(data,at)),top,static_cast<std::int16_t>(wmf_word(data,at+2)),bottom});at+=4;if(rects.size()>65536)throw std::invalid_argument("WMF 區域過大");}
            at+=2;
        }
        if(rects.empty())return keep(gdi("CreateRectRgn",{0,0,0,0}));
        std::vector<std::uint8_t> region(32+rects.size()*16);const auto put=[&](std::size_t offset,std::uint32_t value){for(unsigned i=0;i<4;++i)region[offset+i]=static_cast<std::uint8_t>(value>>(8*i));};
        put(0,32);put(4,1);put(8,static_cast<std::uint32_t>(rects.size()));put(12,static_cast<std::uint32_t>(rects.size()*16));
        std::array<std::int32_t,4> bound{INT32_MAX,INT32_MAX,INT32_MIN,INT32_MIN};
        for(std::size_t i=0;i<rects.size();++i){for(unsigned k=0;k<4;++k)put(32+i*16+k*4,static_cast<std::uint32_t>(rects[i][k]));bound={std::min(bound[0],rects[i][0]),std::min(bound[1],rects[i][1]),std::max(bound[2],rects[i][2]),std::max(bound[3],rects[i][3])};}
        for(unsigned k=0;k<4;++k)put(16+k*4,static_cast<std::uint32_t>(bound[k]));
        return keep(gdi("ExtCreateRegion",{0,region.size(),copy(region)}));
    }
    case 0x01f9:{
        // {Bitmap16（14 位元組）, Reserved[18], Pattern}。
        const auto data=bytes_at(0,record.size()-6);if(data.size()<32)throw std::invalid_argument("WMF 圖樣筆刷記錄不足");
        const auto bitmap=bitmap16(data,data.subspan(32));if(!bitmap)return false;const auto brush=gdi("CreatePatternBrush",{bitmap});(void)gdi("DeleteObject",{bitmap});return keep(brush);
    }
    case 0x0142:{
        // {Style, ColorUsage, Target}：BS_PATTERN 為 Bitmap16，其餘為 DIB。
        const auto style=u(0),usage=u(1);
        if(style==3){const auto data=bytes_at(2,record.size()-10);if(data.size()<10)throw std::invalid_argument("WMF 圖樣筆刷記錄不足");const auto bitmap=bitmap16(data,data.subspan(10));if(!bitmap)return false;const auto brush=gdi("CreatePatternBrush",{bitmap});(void)gdi("DeleteObject",{bitmap});return keep(brush);}
        const auto packed=dib(2,usage);if(!packed)return false;return keep(gdi("CreateDIBPatternBrushPt",{packed->first,usage}));
    }
    case 0x0922:case 0x0b23:{
        // 沒有點陣圖的變體（RecordSize == (RecordFunction>>8)+3）只用 pattern 與目的地，等同 PatBlt。
        const bool stretch=function==0x0b23;const bool plain=record.size()/2==(function>>8)+3U;const auto rop=dw(0);
        if(plain)return stretch?ok(gdi("PatBlt",{dc,p(10),p(9),p(8),p(7),rop})):ok(gdi("PatBlt",{dc,p(8),p(7),p(6),p(5),rop}));
        if(stretch)return blit_bitmap16(bytes_at(10,record.size()-26),rop,p(9),p(8),p(7),p(6),p(5),p(4),std::array<std::uint64_t,2>{p(3),p(2)});
        return blit_bitmap16(bytes_at(8,record.size()-22),rop,p(7),p(6),p(5),p(4),p(3),p(2),std::nullopt);
    }
    case 0x0940:case 0x0b41:{
        const bool stretch=function==0x0b41;const bool plain=record.size()/2==(function>>8)+3U;const auto rop=dw(0);
        if(plain)return stretch?ok(gdi("PatBlt",{dc,p(10),p(9),p(8),p(7),rop})):ok(gdi("PatBlt",{dc,p(8),p(7),p(6),p(5),rop}));
        const auto packed=dib(stretch?10:8,0);if(!packed)return false;const auto [info,pixels]=*packed;
        if(stretch)return ok(gdi("StretchDIBits",{dc,p(9),p(8),p(7),p(6),p(5),p(4),p(3),p(2),pixels,info,0,rop}));
        return ok(gdi("StretchDIBits",{dc,p(7),p(6),p(5),p(4),p(3),p(2),p(5),p(4),pixels,info,0,rop}));
    }
    case 0x0f43:{
        const auto usage=u(2);const auto packed=dib(11,usage);if(!packed)return false;const auto [info,pixels]=*packed;
        return ok(gdi("StretchDIBits",{dc,p(10),p(9),p(8),p(7),p(6),p(5),p(4),p(3),pixels,info,usage,dw(0)}));
    }
    case 0x0d33:{
        const auto usage=u(0);const auto packed=dib(9,usage);if(!packed)return false;const auto [info,pixels]=*packed;
        return ok(gdi("SetDIBitsToDevice",{dc,p(8),p(7),p(6),p(5),p(4),p(3),u(2),u(1),pixels,info,usage}));
    }
    default:last_error_=87;return false;
    }
}
std::optional<std::uint32_t> WindowsServices::invoke_windows_metafile(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const unsigned unit=x86_?4U:8U;
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto fail=[&](std::uint32_t error){last_error_=error;return finish(0);};
    const auto valid_dc=[&](std::uint64_t h){return gui_.contexts.contains(h) || gui_.dc_objects.contains(h) || (h>=0x10000000 && gui_.windows.contains(h-0x10000000));};
    const auto wmf_recorder=[&](std::uint64_t h){const auto found=gui_.emf_recorders.find(h);return found!=gui_.emf_recorders.end() && found->second.wmf?&found->second:nullptr;};
    try{
        if(name=="GdiIsMetaFileDC")return finish(gui_.emf_recorders.contains(arg(0)));
        // 本引擎的列印 DC 直接輸出頁面，沒有以 EMF 排存的列印 DC。
        if(name=="GdiIsMetaPrintDC")return finish(0);
        if(name=="GdiIsPlayMetafileDC"){
            bool playing=false;
            if(metafile_playback_){for(const auto& [table,operation]:metafile_playback_->enumerations){(void)table;playing|=operation.dc && operation.dc==arg(0);}
                for(const auto& [table,operation]:metafile_playback_->windows_enumerations){(void)table;playing|=operation.dc && operation.dc==arg(0);}}
            return finish(playing);
        }
        if(name.starts_with("CreateMetaFile")){
            GuiMetafile meta;wmf_begin(meta,arg(0)!=0);
            if(arg(0)){meta.file=name.ends_with('W')?read_utf16(memory,GuestAddress{arg(0)}):utf8_to_utf16(read_ansi(memory,arg(0)));if(meta.file.empty())return fail(3);
                // 與 Windows 相同，建立時即建立（截斷）檔案，路徑錯誤立即失敗。
                if(!write_guest_file(state,memory,meta.file,{}))return finish(0);}
            const auto handle=gui_.next_object++;gui_.emf_recorders.emplace(handle,std::move(meta));
            try{gui_.contexts.emplace(handle,GuiDc{});}catch(...){gui_.emf_recorders.erase(handle);throw;}return finish(handle);
        }
        if(name=="CloseMetaFile"){
            const auto recorder=wmf_recorder(arg(0));if(!recorder)return fail(6);auto meta=std::move(*recorder);
            gui_.emf_recorders.erase(arg(0));gui_.contexts.erase(arg(0));gui_.dc_objects.erase(arg(0));gui_.saved_contexts.erase(arg(0));
            wmf_finish(meta);if(!meta.file.empty() && !write_guest_file(state,memory,meta.file,meta.bytes))return finish(0);
            const auto handle=gui_.next_object++;gui_.windows_metafiles.emplace(handle,std::move(meta.bytes));return finish(handle);
        }
        if(name=="PlayMetaFileRecord"){
            // 單筆記錄可在列舉外播放；handle table 由呼叫端提供。
            if(!arg(2))return fail(87);const auto words=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)},32));const auto handles=static_cast<unsigned>(arg(3)&0xffff);
            if(words<3 || std::uint64_t{words}*2>windows_metafile_limit || (handles && !arg(1)))return fail(87);
            std::vector<std::uint8_t> record(static_cast<std::size_t>(words)*2);memory.read(GuestAddress{arg(2)},record);
            return finish(play_windows_metafile_record(state,memory,arg(0),arg(1),handles,record));
        }
        if(name=="SetWinMetaFileBits"){
            const auto size=static_cast<std::uint32_t>(arg(0));if(!arg(1) || size<18 || size>windows_metafile_limit)return fail(87);
            std::vector<std::uint8_t> bytes(size);memory.read(GuestAddress{arg(1)},bytes);if(!valid_windows_metafile(bytes))return fail(13);bytes.resize(windows_metafile_size(bytes));
            // GetWinMetaFileBits 內嵌的原始 EMF 完整且 checksum 相符時直接還原。
            if(auto emf=wmf_embedded_emf(bytes))if(const auto handle=import_enhanced_metafile(std::move(*emf)))return finish(*handle);
            // 否則播放到 EMF 錄製 DC：METAFILEPICT {mm, xExt, yExt, hMF}；等比／非等比模式的正尺寸為 0.01 mm 畫框，未提供時對應整個參考裝置。
            std::uint32_t mode=8;std::int32_t x_ext=0,y_ext=0;
            if(arg(3)){mode=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)},32));x_ext=static_cast<std::int32_t>(memory.load(GuestAddress{arg(3)+4},32));y_ext=static_cast<std::int32_t>(memory.load(GuestAddress{arg(3)+8},32));}
            if(mode<1 || mode>8)return fail(87);const bool scalable=mode==7 || mode==8,framed=scalable && x_ext>0 && y_ext>0;
            GuestScratch scratch(*this,memory,state);std::uint64_t frame=0;
            if(framed){frame=scratch.allocate(16);if(!frame)return fail(8);memory.store(GuestAddress{frame+8},32,static_cast<std::uint32_t>(x_ext));memory.store(GuestAddress{frame+12},32,static_cast<std::uint32_t>(y_ext));}
            const auto recorder=nested_service(state,memory,"gdi32.dll","CreateEnhMetaFileW",{arg(2),0,frame,0});if(!recorder)return finish(0);
            bool played=false;
            try{
                (void)nested_service(state,memory,"gdi32.dll","SetMapMode",{recorder,mode});
                if(scalable){
                    std::int64_t width=0,height=0;
                    if(framed){width=std::max<std::int64_t>(1,std::llround(x_ext*96.0/2540));height=std::max<std::int64_t>(1,std::llround(y_ext*96.0/2540));}
                    else {const auto reference=arg(2);const auto screen=reference?reference:nested_service(state,memory,"user32.dll","GetDC",{0});
                        width=static_cast<std::int32_t>(nested_service(state,memory,"gdi32.dll","GetDeviceCaps",{screen,8}));height=static_cast<std::int32_t>(nested_service(state,memory,"gdi32.dll","GetDeviceCaps",{screen,10}));
                        if(!reference)(void)nested_service(state,memory,"user32.dll","ReleaseDC",{0,screen});}
                    (void)nested_service(state,memory,"gdi32.dll","SetViewportExtEx",{recorder,static_cast<std::uint64_t>(std::max<std::int64_t>(1,width)),static_cast<std::uint64_t>(std::max<std::int64_t>(1,height)),0});
                }
                played=play_windows_metafile(state,memory,recorder,bytes);
            }catch(...){if(const auto closed=nested_service(state,memory,"gdi32.dll","CloseEnhMetaFile",{recorder}))(void)nested_service(state,memory,"gdi32.dll","DeleteEnhMetaFile",{closed});throw;}
            const auto handle=nested_service(state,memory,"gdi32.dll","CloseEnhMetaFile",{recorder});
            if(!played && handle){(void)nested_service(state,memory,"gdi32.dll","DeleteEnhMetaFile",{handle});return fail(13);}
            return finish(handle);
        }
        if(name=="GetWinMetaFileBits"){
            const auto found=gui_.metafiles.find(arg(0));if(found==gui_.metafiles.end())return fail(6);const auto mode=static_cast<std::uint32_t>(arg(3));if(mode<1 || mode>8)return fail(87);
            const auto& emf=found->second;const auto draws=emf_decode(emf.bytes);
            // 參考裝置像素（96 DPI）→ 對應模式的邏輯單位；固定度量模式 Y 向上；等比／非等比模式以 0.01 mm 畫框為視窗。
            static constexpr double scale[]{1,254.0/96,2540.0/96,100.0/96,1000.0/96,1440.0/96,2540.0/96,2540.0/96};
            const double factor=scale[mode-1];const bool upward=mode>=2 && mode<=6;const GuiTransform output{factor,0,0,upward?-factor:factor,0,0};
            GuiMetafile out;wmf_begin(out,false);wmf_embed_emf(out,emf.bytes);
            const std::uint16_t map_mode[]{static_cast<std::uint16_t>(mode)};wmf_record(out,0x0103,map_mode);
            if(mode>=7){
                GuiRect frame{};for(unsigned i=0;i<4;++i)frame[i]=static_cast<std::int32_t>(emf_get(emf.bytes,24+i*4));
                const std::uint16_t origin[]{s16(frame[1]),s16(frame[0])},extent[]{s16(std::max(1.0,static_cast<double>(frame[3])-frame[1])),s16(std::max(1.0,static_cast<double>(frame[2])-frame[0]))};
                wmf_record(out,0x020b,origin);wmf_record(out,0x020c,extent);
            }
            for(const auto& draw:draws)wmf_draw(out,draw,output);
            wmf_finish(out);if(!arg(2))return finish(out.bytes.size());if(static_cast<std::uint32_t>(arg(1))<out.bytes.size())return fail(122);
            memory.write(GuestAddress{arg(2)},out.bytes);return finish(out.bytes.size());
        }
        const auto found=gui_.windows_metafiles.find(arg(1));if(found==gui_.windows_metafiles.end())return fail(6);const auto bytes=found->second;
        if(name=="PlayMetaFile"){if(!valid_dc(arg(0)))return fail(6);return finish(play_windows_metafile(state,memory,arg(0),bytes));}
        if(name=="EnumMetaFile"){
            // 回呼 EnumMetaFileProc(hdc, HANDLETABLE*, METARECORD*, nObj, lParam) 依序收到每筆記錄（含 EOF）；回 0 即停止。
            if(!arg(2))return fail(87);if(arg(0) && !valid_dc(arg(0)))return fail(6);
            const auto records=wmf_records(bytes);const unsigned handles=wmf_word(bytes,10);const auto table_size=std::max<std::uint64_t>(1,handles)*unit;
            const auto allocation=allocate_heap(memory,table_size+bytes.size());if(!allocation)return fail(8);
            try{
                if(!metafile_playback_)metafile_playback_=std::make_shared<MetafileState>();if(metafile_playback_->windows_enumerations.size()>=256)throw std::length_error("WMF 巢狀列舉過深");
                memory.write(GuestAddress{allocation},std::vector<std::uint8_t>(static_cast<std::size_t>(table_size)));const auto data=allocation+table_size;memory.write(GuestAddress{data},bytes);
                metafile_playback_->windows_enumerations.emplace(allocation,MetafileState::WindowsEnumeration{arg(0),handles,windows_metafile_selection(state,memory,arg(0))});
                if(records.empty()){finish_metafile_enumeration(state,memory,allocation);(void)free_heap(memory,allocation,state);return finish(1);}
                std::vector<GuestCallback> callbacks;callbacks.reserve(records.size());
                for(const auto& [offset,size]:records){(void)size;callbacks.push_back({arg(2),{arg(0),allocation,data+offset,handles,arg(3)}});}
                start_callbacks(state,memory,std::move(callbacks),Continuation::MetafileEnumeration,allocation);return std::nullopt;
            }catch(...){finish_metafile_enumeration(state,memory,allocation);(void)free_heap(memory,allocation,state);throw;}
        }
        return fail(50);
    }catch(const std::domain_error&){return fail(50);}catch(const std::range_error&){return fail(87);}catch(const std::bad_alloc&){return fail(8);}catch(const std::length_error&){return fail(8);}catch(const std::invalid_argument&){return fail(13);}catch(const MemoryFault&){return fail(998);}
}
}
