#pragma once
#include "x64arm/windows_gui.hpp"
#include "x64arm/windows_transform.hpp"
#include "x64arm/windows_geometry.hpp"
#include "x64arm/windows_fonts.hpp"
#include "x64arm/native_font_data.hpp"
#include "x64arm/unicode.hpp"
#include <bit>
namespace x64arm {
inline void emf_put(std::vector<std::uint8_t>& bytes,std::size_t offset,std::uint64_t value,unsigned size=4){if(offset>bytes.size() || size>bytes.size()-offset)throw std::invalid_argument("EMF 欄位越界");for(unsigned i=0;i<size;++i)bytes[offset+i]=static_cast<std::uint8_t>(value>>(8*i));}
inline std::uint32_t emf_get(std::span<const std::uint8_t> bytes,std::size_t offset){if(offset>bytes.size() || 4>bytes.size()-offset)throw std::invalid_argument("EMF 欄位越界");std::uint32_t value=0;for(unsigned i=0;i<4;++i)value|=std::uint32_t{bytes[offset+i]}<<(8*i);return value;}
inline std::vector<std::uint8_t> emf_record(std::uint32_t type,unsigned size){std::vector<std::uint8_t> bytes(size);emf_put(bytes,0,type);emf_put(bytes,4,size);return bytes;}
inline void emf_append(GuiMetafile& meta,const std::vector<std::uint8_t>& record){if(meta.bytes.size()+record.size()>16*1024*1024 || meta.records>=65535)throw std::length_error("EMF 記錄超過上限");meta.bytes.insert(meta.bytes.end(),record.begin(),record.end());++meta.records;}
inline void emf_scalar(GuiMetafile& meta,unsigned type,std::uint32_t value){auto r=emf_record(type,12);emf_put(r,8,value);emf_append(meta,r);}
inline void emf_rect(GuiMetafile& meta,unsigned type,const GuiRect& rect){auto r=emf_record(type,24);for(unsigned i=0;i<4;++i)emf_put(r,8+i*4,static_cast<std::uint32_t>(rect[i]));emf_append(meta,r);}
inline void emf_begin(GuiMetafile& meta){meta.bytes=emf_record(1,108);meta.records=1;if(!meta.description.empty()){const auto text=utf16_bytes(meta.description);emf_put(meta.bytes,60,text.size()/2);emf_put(meta.bytes,64,108);meta.bytes.insert(meta.bytes.end(),text.begin(),text.end());meta.bytes.resize((meta.bytes.size()+3)&~std::size_t{3});emf_put(meta.bytes,4,meta.bytes.size());}}
inline void emf_finish(GuiMetafile& meta){auto end=emf_record(14,20);emf_put(end,16,20);emf_append(meta,end);const auto bounds=meta.bounds.value_or(GuiRect{0,0,0,0});auto frame=meta.frame.value_or(GuiRect{});if(!meta.frame)for(unsigned i=0;i<4;++i)frame[i]=static_cast<std::int32_t>(std::round(bounds[i]*2540.0/96));
    for(unsigned i=0;i<4;++i){emf_put(meta.bytes,8+i*4,static_cast<std::uint32_t>(bounds[i]));emf_put(meta.bytes,24+i*4,static_cast<std::uint32_t>(frame[i]));}
    emf_put(meta.bytes,40,0x464d4520);emf_put(meta.bytes,44,0x10000);emf_put(meta.bytes,48,meta.bytes.size());emf_put(meta.bytes,52,meta.records);emf_put(meta.bytes,56,4,2);emf_put(meta.bytes,72,1440);emf_put(meta.bytes,76,900);emf_put(meta.bytes,80,381);emf_put(meta.bytes,84,238);emf_put(meta.bytes,100,381000);emf_put(meta.bytes,104,238125);
}
inline std::vector<std::pair<std::size_t,std::size_t>> emf_validate(const std::vector<std::uint8_t>& bytes){
    if(bytes.size()<88 || bytes.size()>16*1024*1024 || emf_get(bytes,0)!=1 || emf_get(bytes,4)<88 || emf_get(bytes,40)!=0x464d4520 || emf_get(bytes,48)!=bytes.size())throw std::invalid_argument("EMF 標頭不合法");
    const auto header=emf_get(bytes,4),description=emf_get(bytes,60),offset=emf_get(bytes,64);if(description && (offset<88 || offset>header || description>(header-offset)/2))throw std::invalid_argument("EMF 描述越界");
    std::vector<std::pair<std::size_t,std::size_t>> records;bool eof=false;
    for(std::size_t at=0;at<bytes.size();){const auto type=emf_get(bytes,at),size=emf_get(bytes,at+4);if(eof || size<8 || size%4 || size>bytes.size()-at || !type || records.size()>=65536)throw std::invalid_argument("EMF 記錄越界");records.emplace_back(at,size);eof=type==14;if(eof && size<20)throw std::invalid_argument("EMF EOF 不完整");at+=size;}
    if(!eof || records.size()!=emf_get(bytes,52))throw std::invalid_argument("EMF 記錄數不符");return records;
}
inline void emf_draw(GuiMetafile& meta,const GuiDraw& draw){
    if(draw.pattern_brush || draw.hatch>=0 || draw.kind==GuiDraw::Kind::Icon)throw std::invalid_argument("EMF 圖樣或圖示記錄尚未支援");
    auto changed=meta; // 驗證、配置失敗不留下半筆記錄。
    emf_append(changed,emf_record(33,8));emf_scalar(changed,17,1);emf_scalar(changed,20,draw.rop2);
    auto transform=emf_record(35,32);for(unsigned i=0;i<6;++i)emf_put(transform,8+i*4,std::bit_cast<std::uint32_t>(static_cast<float>(draw.transform[i])));emf_append(changed,transform);
    // 裁切矩形先在 device 空間套用，再切回繪圖矩陣。
    if(draw.clipped || !draw.excluded.empty()){
        auto identity=emf_record(35,32);emf_put(identity,8,0x3f800000);emf_put(identity,20,0x3f800000);emf_append(changed,identity);
        if(draw.clipped)emf_rect(changed,30,draw.clip);for(const auto& rect:draw.excluded)emf_rect(changed,29,rect);emf_append(changed,transform);
    }
    GuiRect bounds{draw.x,draw.y,draw.right,draw.bottom};
    if(draw.kind==GuiDraw::Kind::Text){
        auto font=emf_record(82,332);emf_put(font,8,1);emf_put(font,12,static_cast<std::uint32_t>(draw.font.height));emf_put(font,16,static_cast<std::uint32_t>(draw.font.width));emf_put(font,28,static_cast<std::uint32_t>(draw.font.weight));font[32]=draw.font.italic;font[33]=draw.font.underline;font[34]=draw.font.strikeout;font[35]=draw.font.charset;font[39]=draw.font.pitch_family;const auto face=utf16_bytes(draw.font.face.substr(0,31));std::copy(face.begin(),face.end(),font.begin()+40);emf_append(changed,font);emf_scalar(changed,37,1);emf_scalar(changed,24,draw.color);emf_scalar(changed,18,1);emf_scalar(changed,22,0);
        const auto characters=static_cast<unsigned>(draw.text.size()),string_bytes=characters*2,dx_offset=(76+string_bytes+3)&~3U,step=draw.advances_y.empty()?4U:8U;auto record=emf_record(84,dx_offset+(draw.advances.empty()?0:characters*step));emf_put(record,24,2);emf_put(record,28,0x3f800000);emf_put(record,32,0x3f800000);emf_put(record,36,static_cast<std::uint32_t>(draw.x));emf_put(record,40,static_cast<std::uint32_t>(draw.y));emf_put(record,44,characters);emf_put(record,48,76);emf_put(record,52,(draw.glyph_indices?16U:draw.rtl_reading?128U:0U)|(draw.advances_y.empty()?0U:8192U));
        if(!draw.advances_y.empty() && (draw.advances_y.size()!=characters || std::find(draw.advances_y.begin(),draw.advances_y.end(),INT32_MIN)!=draw.advances_y.end()))throw std::invalid_argument("EMF 垂直字距數目不符");
        for(unsigned i=0;i<characters;++i)emf_put(record,76+i*2,draw.text[i],2);if(!draw.advances.empty()){if(draw.advances.size()!=characters)throw std::invalid_argument("EMF 字距數目不符");emf_put(record,72,dx_offset);for(unsigned i=0;i<characters;++i){emf_put(record,dx_offset+i*step,static_cast<std::uint32_t>(draw.advances[i]));if(step==8)emf_put(record,dx_offset+i*step+4,static_cast<std::uint32_t>(-static_cast<std::int64_t>(draw.advances_y[i])));}}
        const auto coordinate=[](std::int64_t value){if(value<INT32_MIN || value>INT32_MAX)throw std::invalid_argument("EMF 文字邊界溢位");return static_cast<std::int32_t>(value);};
        if(draw.glyph_indices){
            // 使用實際字形範圍，保留負字距、垂直位移及字型外伸部分。
            const auto raster=native_gdi_glyph_raster(draw);
            for(unsigned i=0;i<4;++i)bounds[i]=coordinate(static_cast<std::int64_t>(i%2?draw.y:draw.x)+raster.bounds[i]);
        }else {
            const auto height=std::max<std::int64_t>(1,std::abs(static_cast<std::int64_t>(draw.font.height))),cell=std::max<std::int64_t>(1,height/2);
            bounds[2]=coordinate(static_cast<std::int64_t>(draw.x)+characters*cell);bounds[3]=coordinate(static_cast<std::int64_t>(draw.y)+height);
            if(!draw.advances.empty()){std::int64_t x=draw.x,y=draw.y;for(unsigned i=0;i<characters;++i){bounds[0]=std::min(bounds[0],coordinate(x));bounds[1]=std::min(bounds[1],coordinate(y));bounds[2]=std::max(bounds[2],coordinate(x+std::max<std::int64_t>(cell,draw.advances[i])));bounds[3]=std::max(bounds[3],coordinate(y+height));x+=draw.advances[i];if(!draw.advances_y.empty())y+=draw.advances_y[i];}}
        }
        const auto device=gui_transform_bounds(draw.transform,bounds);for(unsigned i=0;i<4;++i)emf_put(record,8+i*4,static_cast<std::uint32_t>(device[i]));emf_append(changed,record);emf_scalar(changed,37,0x8000000d);emf_scalar(changed,40,1);
    }else {
        auto pen=emf_record(38,28);emf_put(pen,8,2);emf_put(pen,12,draw.kind==GuiDraw::Kind::Fill || !draw.pen_width?5:0);emf_put(pen,16,static_cast<unsigned>(draw.pen_width));emf_put(pen,24,draw.color);emf_append(changed,pen);emf_scalar(changed,37,2);
        auto brush=emf_record(39,24);emf_put(brush,8,3);emf_put(brush,12,draw.kind==GuiDraw::Kind::Fill || draw.filled?0:1);emf_put(brush,16,draw.kind==GuiDraw::Kind::Fill?draw.color:draw.fill_color);emf_append(changed,brush);emf_scalar(changed,37,3);
        if(draw.kind==GuiDraw::Kind::Line){auto move=emf_record(27,16);emf_put(move,8,static_cast<unsigned>(draw.x));emf_put(move,12,static_cast<unsigned>(draw.y));emf_append(changed,move);auto line=emf_record(54,16);emf_put(line,8,static_cast<unsigned>(draw.right));emf_put(line,12,static_cast<unsigned>(draw.bottom));emf_append(changed,line);bounds={std::min(draw.x,draw.right),std::min(draw.y,draw.bottom),std::max(draw.x,draw.right)+1,std::max(draw.y,draw.bottom)+1};}
        else emf_rect(changed,draw.kind==GuiDraw::Kind::Ellipse?42:43,bounds);
        emf_scalar(changed,37,0x80000007);emf_scalar(changed,37,0x80000000);emf_scalar(changed,40,2);emf_scalar(changed,40,3);
    }
    const auto area=gui_transform_bounds(draw.transform,bounds);if(changed.bounds){auto& b=*changed.bounds;for(unsigned i=0;i<4;++i)b[i]=i<2?std::min(b[i],area[i]):std::max(b[i],area[i]);}else changed.bounds=area;
    emf_scalar(changed,34,UINT32_MAX);changed.draws.push_back(draw);meta=std::move(changed);
}
struct EmfDecoder {
    struct State {GuiTransform transform{gui_identity};GuiFont font;std::uint32_t text{},pen{},brush{0xffffff},rop2{13},pen_object{0x80000007},brush_object{0x80000000},font_object{0x80000011},mode{2},alignment{};int width{1};bool filled{true};std::array<int,2> point{};std::optional<GuiRegion> clip;};
    struct Object {unsigned type{};GuiFont font;std::uint32_t color{};int width{1};bool empty{};};
    State current;std::vector<State> saved;std::map<unsigned,Object> objects;
};
inline std::vector<GuiDraw> emf_decode_record(EmfDecoder& decoder,std::span<const std::uint8_t> record){
    auto& current=decoder.current;auto& saved=decoder.saved;auto& objects=decoder.objects;std::vector<GuiDraw> draws;const auto length=record.size();
    do{const auto get=[&](unsigned at){return emf_get(record,at);};const auto integer=[&](unsigned at){return static_cast<std::int32_t>(get(at));};const auto rectangle=[&](unsigned at){return GuiRect{integer(at),integer(at+4),integer(at+8),integer(at+12)};};const auto type=get(0);
        if(length<8 || length%4 || get(4)!=length)throw std::invalid_argument("EMF 記錄長度不符");
        if(type==1 || type==14)continue;
        if(type==70){const auto count=get(8);if(count>length-12)throw std::invalid_argument("EMF 註解越界");if(count>=4 && get(12)==0x2b464d45)throw std::domain_error("EMF+ 繪圖尚未支援");if(count>=8 && get(12)==0x43494447 && get(16)!=2 && get(16)!=3)throw std::domain_error("EMF 公開嵌入格式尚未支援");continue;}
        if(type==33){if(saved.size()>=4096)throw std::invalid_argument("EMF 狀態堆疊過深");saved.push_back(current);continue;}
        if(type==34){const auto level=integer(8);const auto index=level>0?static_cast<std::int64_t>(level)-1:static_cast<std::int64_t>(saved.size())+level;if(!level || index<0 || static_cast<std::uint64_t>(index)>=saved.size())throw std::invalid_argument("EMF RestoreDC 不合法");current=saved[static_cast<std::size_t>(index)];saved.resize(static_cast<std::size_t>(index));continue;}
        if(type==17){if(get(8)!=1)throw std::domain_error("EMF 非 MM_TEXT 映射尚未支援");continue;}
        if(type==35){for(unsigned i=0;i<6;++i)current.transform[i]=std::bit_cast<float>(get(8+i*4));if(!gui_valid_transform(current.transform) || !gui_inverse(current.transform))throw std::invalid_argument("EMF 矩陣不合法");continue;}
        if(type==29 || type==30){const auto region=gui_transform_region_rect(current.transform,rectangle(8));if(type==30)current.clip=current.clip?gui_region_combine(*current.clip,region,1):region;else {if(!current.clip)throw std::domain_error("EMF 無界排除裁切尚未支援");current.clip=gui_region_combine(*current.clip,region,4);}continue;}
        if(type==38 || type==39 || type==82){const auto handle=get(8);if(!handle || handle>65535 || objects.contains(handle))throw std::invalid_argument("EMF 物件索引不合法");EmfDecoder::Object object;object.type=type;
            if(type==38){if(get(12)!=0 && get(12)!=5)throw std::domain_error("EMF 畫筆樣式尚未支援");object.empty=get(12)==5;object.width=integer(16);object.color=get(24);}
            else if(type==39){if(get(12)>1)throw std::domain_error("EMF 圖樣筆刷尚未支援");object.empty=get(12)==1;object.color=get(16);}
            else {if(length<104)throw std::invalid_argument("EMF 字型記錄不足");auto& font=object.font;font.height=integer(12);font.width=integer(16);if(get(20) || get(24))throw std::domain_error("EMF 字型角度尚未支援");font.weight=integer(28);font.italic=record[32]!=0;font.underline=record[33]!=0;font.strikeout=record[34]!=0;font.charset=record[35];font.pitch_family=record[39];for(unsigned i=0;i<32;++i){const auto c=static_cast<char16_t>(record[40+i*2]|(static_cast<unsigned>(record[41+i*2])<<8));if(!c)break;font.face.push_back(c);}}
            objects.emplace(handle,std::move(object));continue;
        }
        if(type==37){const auto handle=get(8);if(handle&0x80000000U){const auto id=handle&0x7fffffffU;if(id<=5)current.brush_object=handle;else if(id<=8)current.pen_object=handle;else current.font_object=handle;}else{const auto found=objects.find(handle);if(found!=objects.end()){if(found->second.type==38)current.pen_object=handle;else if(found->second.type==39)current.brush_object=handle;else current.font_object=handle;}}if(handle&0x80000000U){switch(handle&0x7fffffffU){case 0:current.brush=0xffffff;current.filled=true;break;case 4:current.brush=0;current.filled=true;break;case 5:current.filled=false;break;case 6:current.pen=0xffffff;current.width=1;break;case 7:current.pen=0;current.width=1;break;case 8:current.width=0;break;case 10:case 11:case 12:case 13:case 14:case 16:case 17:current.font={};break;default:throw std::domain_error("EMF stock 物件尚未支援");}}
            else {const auto found=objects.find(handle);if(found==objects.end())throw std::invalid_argument("EMF 選取未知物件");const auto& object=found->second;if(object.type==38){current.pen=object.color;current.width=object.empty?0:object.width;}else if(object.type==39){current.brush=object.color;current.filled=!object.empty;}else current.font=object.font;}continue;
        }
        if(type==40){if(!objects.erase(get(8)))throw std::invalid_argument("EMF 刪除未知物件");continue;}
        if(type==20){if(get(8)<1 || get(8)>16)throw std::invalid_argument("EMF ROP2 不合法");current.rop2=get(8);continue;}
        if(type==24){current.text=get(8);continue;}
        if(type==18 || type==22){if(get(8)!=(type==18?1U:0U))throw std::domain_error("EMF 文字狀態尚未支援");if(type==18)current.mode=get(8);else current.alignment=get(8);continue;}
        if(type==27){current.point={integer(8),integer(12)};continue;}
        GuiDraw draw;draw.rop2=current.rop2;draw.transform=current.transform;draw.font=current.font;draw.color=current.pen;draw.pen_width=current.width;draw.filled=current.filled;draw.fill_color=current.brush;
        if(current.clip){draw.clipped=true;draw.clip=gui_region_bounds(*current.clip);draw.excluded=gui_region_combine(GuiRegion{draw.clip},*current.clip,4);}
        if(type==42 || type==43){const auto rect=rectangle(8);draw.kind=type==42?GuiDraw::Kind::Ellipse:GuiDraw::Kind::Rectangle;draw.x=rect[0];draw.y=rect[1];draw.right=rect[2];draw.bottom=rect[3];}
        else if(type==54){draw.kind=GuiDraw::Kind::Line;draw.x=current.point[0];draw.y=current.point[1];draw.right=integer(8);draw.bottom=integer(12);current.point={draw.right,draw.bottom};}
        else if(type==84){const auto flags=get(52);if(flags&~0x2090U)throw std::domain_error("EMF 延伸文字旗標尚未支援");const auto count=get(44),string=get(48),dx=get(72),step=(flags&8192U)?8U:4U;if(count>65536 || string<76 || string>length || count>(length-string)/2 || (dx && (dx<76 || dx>length || count>(length-dx)/step)))throw std::invalid_argument("EMF 文字範圍不合法");draw.kind=GuiDraw::Kind::Text;draw.glyph_indices=(flags&16U)!=0;draw.rtl_reading=!draw.glyph_indices && (flags&128U);draw.color=current.text;draw.x=integer(36);draw.y=integer(40);for(unsigned i=0;i<count;++i){draw.text.push_back(static_cast<char16_t>(record[string+i*2]|(static_cast<unsigned>(record[string+i*2+1])<<8)));if(dx){draw.advances.push_back(integer(dx+i*step));if(step==8){const auto dy=integer(dx+i*step+4);if(dy==INT32_MIN)throw std::invalid_argument("EMF 垂直字距溢位");draw.advances_y.push_back(-dy);}}}}
        else throw std::domain_error("EMF 記錄種類尚未支援");
        draws.push_back(std::move(draw));
    }while(false);
    return draws;
}
inline std::vector<GuiDraw> emf_decode(const std::vector<std::uint8_t>& bytes){
    EmfDecoder decoder;std::vector<GuiDraw> draws;for(const auto& [offset,size]:emf_validate(bytes)){auto records=emf_decode_record(decoder,std::span(bytes).subspan(offset,size));draws.insert(draws.end(),std::make_move_iterator(records.begin()),std::make_move_iterator(records.end()));}return draws;
}
}
