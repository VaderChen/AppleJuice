#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_geometry.hpp"
#include "x64arm/windows_transform.hpp"
#include <unicode/ubrk.h>
#include <algorithm>
#include <bit>
#include <functional>
#include <memory>

namespace x64arm {
namespace {
constexpr unsigned single=0x20,expand=0x40,tabstop=0x80,noclip=0x100,leading=0x200,calculate=0x400;
constexpr unsigned noprefix=0x800,internal=0x1000,edit=0x2000,path_ellipsis=0x4000,end_ellipsis=0x8000;
constexpr unsigned modify=0x10000,rtl=0x20000,word_ellipsis=0x40000,no_fullwidth=0x80000,hideprefix=0x100000,prefixonly=0x200000;
std::int32_t coordinate(std::int64_t value){
    if(value<INT32_MIN || value>INT32_MAX)throw HostCallFault("GDI 文字座標超出 INT 範圍");
    return static_cast<std::int32_t>(value);
}
struct InputText {std::u16string text;std::vector<std::size_t> source_end;std::size_t units{};};
InputText read_text(GuestMemory& memory,std::uint64_t address,int count,bool wide){
    InputText result;std::string bytes;bool terminated=count>=0;
    for(unsigned i=0;i<(count<0?65536U:static_cast<unsigned>(count));++i){
        const auto c=memory.load(GuestAddress{address+std::uint64_t{i}*(wide?2:1)},wide?16:8);
        if(count<0 && !c){terminated=true;break;}
        if(wide){result.text.push_back(static_cast<char16_t>(c));result.source_end.push_back(++result.units);}
        else bytes.push_back(static_cast<char>(c));
    }
    if(!terminated)throw HostCallFault("GDI 文字缺少結尾 NUL 或超過 65536 個單位");
    if(wide)return result;
    result.units=bytes.size();
    // ACP 是 UTF-8；另保留每個 UTF-16 單位對應的原始 byte 結尾，供 uiLengthDrawn 使用。
    for(std::size_t at=0;at<bytes.size();){
        const auto first=static_cast<unsigned char>(bytes[at]);unsigned size=1;std::uint32_t code=first;
        if(first>=0xc2 && first<=0xdf){size=2;code=first&31U;}
        else if(first>=0xe0 && first<=0xef){size=3;code=first&15U;}
        else if(first>=0xf0 && first<=0xf4){size=4;code=first&7U;}
        else if(first>=128)code=0xfffd;
        bool valid=at+size<=bytes.size();
        for(unsigned j=1;valid && j<size;++j){const auto next=static_cast<unsigned char>(bytes[at+j]);valid=(next&0xc0)==0x80;code=(code<<6)|(next&63U);}
        valid=valid && !(size==3 && (code<0x800 || (code>=0xd800 && code<=0xdfff))) && !(size==4 && (code<0x10000 || code>0x10ffff));
        if(!valid){size=1;code=0xfffd;}at+=size;
        if(code>0xffff){code-=0x10000;result.text.push_back(static_cast<char16_t>(0xd800+(code>>10)));result.source_end.push_back(at);code=0xdc00+(code&1023);}
        result.text.push_back(static_cast<char16_t>(code));result.source_end.push_back(at);
    }
    return result;
}
std::vector<std::size_t> boundaries(std::u16string_view text,UBreakIteratorType type){
    UErrorCode error=U_ZERO_ERROR;
    std::unique_ptr<UBreakIterator,decltype(&ubrk_close)> iterator(ubrk_open(type,"root",reinterpret_cast<const UChar*>(text.data()),static_cast<int>(text.size()),&error),ubrk_close);
    if(U_FAILURE(error) || !iterator)throw HostCallFault("ICU 文字斷行器建立失敗");
    std::vector<std::size_t> result;
    for(auto at=ubrk_first(iterator.get());at!=UBRK_DONE;at=ubrk_next(iterator.get()))result.push_back(static_cast<std::size_t>(at));
    return result;
}
void clip_text(GuiDraw& draw,const GuiDc& dc,const std::optional<GuiRect>& rect){
    draw.clipped=dc.clipped;draw.clip=dc.clip;draw.excluded=dc.excluded;
    if(!rect)return;
    auto region=gui_transform_region_rect(gui_device_transform(dc),*rect);
    if(dc.clipped)region=gui_region_combine(region,GuiRegion{dc.clip},1);
    if(!dc.excluded.empty())region=gui_region_combine(region,dc.excluded,4);
    draw.clipped=true;draw.clip=gui_region_bounds(region);draw.excluded=gui_region_combine(GuiRegion{draw.clip},region,4);
}
using Measure=std::function<std::int32_t(std::u16string_view)>;
struct TextRun {std::u16string text;std::size_t begin{};std::int32_t x{},width{};};
struct TabLayout {std::vector<TextRun> runs;std::int32_t width{};};
TabLayout tab_layout(std::u16string_view text,std::int32_t x,std::int32_t origin,const std::vector<std::int32_t>& stops,std::int32_t interval,const Measure& measure,bool expand_tabs){
    TabLayout result;std::int64_t cursor=x;std::size_t begin=0;
    while(begin<text.size()){
        auto end=expand_tabs?text.find(u'\t',begin):text.size();if(end==text.npos)end=text.size();
        TextRun run{std::u16string(text.substr(begin,end-begin)),begin,coordinate(cursor-x),0};run.width=measure(run.text);cursor+=run.width;coordinate(cursor);
        if(!run.text.empty())result.runs.push_back(std::move(run));
        if(end==text.size())break;
        bool found=false;
        for(auto stop:stops){const auto next=static_cast<std::int64_t>(origin)+stop;if(next>cursor){cursor=next;found=true;break;}}
        if(!found){const auto relative=cursor-origin;auto quotient=relative/interval;if(relative<0 && relative%interval)--quotient;cursor=static_cast<std::int64_t>(origin)+(quotient+1)*interval;}
        coordinate(cursor);begin=end+1;
    }
    result.width=coordinate(cursor-x);return result;
}
struct DisplayText {std::u16string text;std::vector<std::size_t> raw_end,source_end;std::vector<bool> marked;};
DisplayText display_text(const InputText& input,unsigned flags){
    DisplayText result;bool mark=false;
    for(std::size_t i=0;i<input.text.size();++i){
        auto c=input.text[i];
        if(c==u'&' && !(flags&noprefix)){
            if(i+1>=input.text.size())break;
            if(input.text[i+1]==u'&'){++i;c=u'&';}else {mark=true;continue;}
        }
        if(c==u'\r'){if(i+1<input.text.size() && input.text[i+1]==u'\n')++i;c=u'\n';}
        if(c==u'\n' && (flags&single))c=u' ';
        result.text.push_back(c);result.raw_end.push_back(i+1);result.source_end.push_back(input.source_end[i]);result.marked.push_back(mark);mark=false;
    }
    return result;
}
struct TextLine {std::size_t begin{},end{},next{};};
std::vector<TextLine> wrap_lines(const DisplayText& text,unsigned flags,std::int64_t available,const std::function<int(std::u16string_view)>& width){
    std::vector<TextLine> lines;
    for(std::size_t paragraph=0;paragraph<text.text.size();){
        auto end=text.text.find(u'\n',paragraph);if(end==text.text.npos)end=text.text.size();
        if(paragraph==end)lines.push_back({paragraph,end,end+1});
        else if(!(flags&0x10) || (flags&single))lines.push_back({paragraph,end,end+(end<text.text.size())});
        else {
            auto breaks=boundaries(std::u16string_view(text.text).substr(paragraph,end-paragraph),UBRK_LINE);
            if(flags&no_fullwidth)std::erase_if(breaks,[&](auto at){return at && at<end-paragraph && text.text[paragraph+at-1]!=u' ' && text.text[paragraph+at-1]!=u'\t' && text.text[paragraph+at-1]!=u'-';});
            for(auto begin=paragraph;begin<end;){
                auto chosen=end;bool fitting=false;
                for(auto offset:breaks){const auto candidate=paragraph+offset;if(candidate<=begin)continue;
                    auto trimmed=candidate;while(trimmed>begin && (text.text[trimmed-1]==u' ' || text.text[trimmed-1]==u'\t'))--trimmed;
                    if(width(std::u16string_view(text.text).substr(begin,trimmed-begin))<=available){chosen=candidate;fitting=true;continue;}
                    if(!fitting)chosen=candidate;break;
                }
                auto next=chosen;while(next<end && (text.text[next]==u' ' || text.text[next]==u'\t'))++next;
                auto trimmed=chosen;while(trimmed>begin && (text.text[trimmed-1]==u' ' || text.text[trimmed-1]==u'\t'))--trimmed;
                lines.push_back({begin,trimmed,next==end?end+(end<text.text.size()):next});begin=next;
            }
        }
        paragraph=end+(end<text.text.size());
    }
    return lines;
}
struct Ellipsis {std::u16string text;std::vector<std::size_t> indices;bool changed{};};
Ellipsis ellipsize(std::u16string_view text,int available,unsigned flags,const Measure& measure){
    Ellipsis result{std::u16string(text),{},false};for(std::size_t i=0;i<text.size();++i)result.indices.push_back(i);
    if(measure(text)<=available || !(flags&(end_ellipsis|path_ellipsis|word_ellipsis)))return result;
    const auto clusters=boundaries(text,UBRK_CHARACTER);std::size_t head=0,tail=text.size();
    if(flags&path_ellipsis){const auto slash=text.find_last_of(u'\\');if(slash!=text.npos)tail=slash;
        while(tail<text.size() && measure(u"..."+std::u16string(text.substr(tail)))>available){const auto it=std::upper_bound(clusters.begin(),clusters.end(),tail);tail=it==clusters.end()?text.size():*it;}
    }
    for(auto at:clusters){if(at>tail)break;if(measure(std::u16string(text.substr(0,at))+u"..."+std::u16string(text.substr(tail)))>available)break;head=at;}
    if((flags&word_ellipsis) && head<text.size()){const auto space=text.substr(0,head).find_last_of(u" \t");if(space!=text.npos)head=space;}
    while(head && (text[head-1]==u' ' || text[head-1]==u'\t'))--head;
    result.text=std::u16string(text.substr(0,head))+u"..."+std::u16string(text.substr(tail));result.indices.clear();
    for(std::size_t i=0;i<head;++i)result.indices.push_back(i);for(unsigned i=0;i<3;++i)result.indices.push_back(SIZE_MAX);for(auto i=tail;i<text.size();++i)result.indices.push_back(i);
    result.changed=true;return result;
}
}

std::optional<std::uint32_t> WindowsServices::invoke_gdi_text(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;
    const auto integer=[&](unsigned index){return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(index)));};
    const auto finish=[&](std::uint32_t value,std::uint32_t error=0)->std::optional<std::uint32_t>{if(error)last_error_=error;arg.return_to(state,value);return std::nullopt;};
    const auto context=gui_.contexts.find(arg(0));if(context==gui_.contexts.end())return finish(0,6);auto& dc=context->second;
    const bool formatted=symbol.starts_with("DrawText"),extent=symbol.starts_with("GetTabbed"),wide=symbol.ends_with('W');
    const auto count=integer(extent?2:formatted?2:4);const auto pointer=arg(extent || formatted?1:3);
    if(count<(formatted?-1:0) || count>65536 || (!pointer && count))return finish(0,87);
    auto flags=formatted?static_cast<std::uint32_t>(arg(4)):0U;const bool extended=symbol.starts_with("DrawTextEx");
    std::int32_t tabs=8,left_margin=0,right_margin=0;const auto parameters=extended?arg(5):0;
    if(formatted){
        if(!arg(3) || (flags&~0x3fffffU) || (dc.alignment&~0x100U))return finish(0,87);
        if(parameters){memory.check(GuestAddress{parameters},20,Access::Read);memory.check(GuestAddress{parameters+16},4,Access::Write);
            if(memory.load(GuestAddress{parameters},32)!=20)return finish(0,87);
            const auto field=[&](unsigned offset){return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{parameters+offset},32)));};
            tabs=field(4);left_margin=field(8);right_margin=field(12);if(left_margin<0 || right_margin<0 || ((flags&tabstop) && tabs<=0))return finish(0,87);
        }
        if(!extended && (flags&tabstop)){tabs=static_cast<int>((flags>>8)&255);if(!tabs)tabs=8;flags&=~0xff00U;}
        if((flags&expand) && (flags&(end_ellipsis|path_ellipsis|word_ellipsis)))return finish(0,87);
        if(const auto font=gui_.fonts.find(dc.font);font!=gui_.fonts.end()){
            const auto& bytes=font->second;if(bytes.size()<16)return finish(0,87);for(unsigned i=8;i<16;++i)if(bytes[i])return finish(0,50);
        }
    }
    const auto font=gui_font(gui_,dc.font),metrics_font=(flags&internal)?gui_font(gui_,0xb00d):font;
    const auto metrics=native_gui_?native_gui_->font_metrics(metrics_font).value_or(fallback_font_metrics(metrics_font)):fallback_font_metrics(metrics_font);
    const Measure measure=[&](std::u16string_view text){if(native_gui_)if(const auto width=native_gui_->text_width(font,std::u16string(text)))return *width;return coordinate(static_cast<std::int64_t>(text.size())*metrics.average_width);};
    const auto input=read_text(memory,pointer,count,wide);
    const bool drawing=!extent && !(flags&calculate);
    if(drawing && !input.text.empty() && !gui_.emf_recorders.contains(arg(0)) && !raster_surface(memory,arg(0)))return finish(0,50);
    const auto emit=[&](GuiDraw draw,const std::optional<GuiRect>& rect){clip_text(draw,dc,rect);if(!raster_draw(memory,arg(0),draw))throw HostCallFault("GDI 文字沒有可用的繪製表面");};
    const auto draw_line=[&](std::u16string_view text,std::int32_t x,std::int32_t y,const TabLayout& layout,const std::optional<GuiRect>& rect,bool only_prefix){
        if(dc.mode==2 && !only_prefix && layout.width>0){GuiDraw background;background.kind=GuiDraw::Kind::Fill;background.x=x;background.y=y;background.right=coordinate(static_cast<std::int64_t>(x)+layout.width);background.bottom=coordinate(static_cast<std::int64_t>(y)+metrics.height);background.color=dc.background;emit(background,rect);}
        if(only_prefix)return;
        for(const auto& run:layout.runs){GuiDraw draw;draw.kind=GuiDraw::Kind::Text;draw.font=font;draw.text=run.text;draw.color=dc.color;draw.x=coordinate(static_cast<std::int64_t>(x)+run.x);draw.y=y;if(metrics.fixed_pitch)draw.cell_width=static_cast<std::uint32_t>(metrics.average_width);
            if((flags&rtl) && native_gui_){const auto shape=native_shape_text(font,run.text,true,true);for(const auto& item:shape.runs){auto physical=draw;physical.font=item.font;physical.text=item.text;physical.x=coordinate(static_cast<std::int64_t>(draw.x)+item.x);physical.y=coordinate(static_cast<std::int64_t>(y)+item.top);emit(physical,rect);}}
            else emit(draw,rect);
        }
        (void)text;
    };
    if(!formatted){
        const auto n=integer(extent?3:5);const auto table=arg(extent?4:6);if(n<0 || n>65536 || (n && !table))return finish(0,87);
        std::vector<std::int32_t> stops;auto interval=coordinate(static_cast<std::int64_t>(std::max(1,metrics.average_width))*8);
        for(int i=0;i<n;++i){const auto value=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{table+4ULL*static_cast<unsigned>(i)},32)));if(n==1){if(value<=0)return finish(0,87);interval=value;}else{if(!stops.empty() && value<=stops.back())return finish(0,87);stops.push_back(value);}}
        const auto x=extent?0:(dc.alignment&1)?dc.x:integer(1),y=extent?0:(dc.alignment&1)?dc.y:integer(2),origin=extent?0:integer(7);
        const auto layout=tab_layout(input.text,x,origin,stops,interval,measure,true);
        const auto next=coordinate(static_cast<std::int64_t>(x)+layout.width);
        if(drawing && !input.text.empty())draw_line(input.text,x,y,layout,std::nullopt,false);
        if(drawing && (dc.alignment&1)){dc.x=next;dc.y=y;}
        return finish((static_cast<std::uint32_t>(metrics.height)&65535U)<<16 | (static_cast<std::uint32_t>(layout.width)&65535U));
    }
    memory.check(GuestAddress{arg(3)},16,Access::Read);if(flags&calculate)memory.check(GuestAddress{arg(3)},16,Access::Write);
    GuiRect rect{};for(unsigned i=0;i<4;++i)rect[i]=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)+i*4},32)));
    const auto margin_left=static_cast<std::int64_t>(left_margin)*metrics.average_width,margin_right=static_cast<std::int64_t>(right_margin)*metrics.average_width;
    const auto left=coordinate(static_cast<std::int64_t>(rect[0])+margin_left),right=coordinate(static_cast<std::int64_t>(rect[2])-margin_right);
    const auto available=static_cast<std::int64_t>(right)-left;const auto interval=coordinate(static_cast<std::int64_t>((flags&tabstop)?tabs:8)*std::max(1,metrics.average_width));
    const auto display=display_text(input,flags);
    const auto layout_for=[&](std::u16string_view text){return tab_layout(text,0,0,{},interval,measure,flags&expand);};
    const auto lines=wrap_lines(display,flags,available,[&](auto text){return layout_for(text).width;});
    const auto line_height=coordinate(static_cast<std::int64_t>(metrics.height)+((flags&leading)?metrics.external_leading:0));
    const auto total=coordinate(static_cast<std::int64_t>(line_height)*static_cast<std::int64_t>(lines.size()));
    if(flags&calculate){std::int32_t width=0;for(const auto& line:lines)width=std::max(width,layout_for(std::u16string_view(display.text).substr(line.begin,line.end-line.begin)).width);
        const auto calculated_right=coordinate(static_cast<std::int64_t>(rect[0])+width+margin_left+margin_right),bottom=coordinate(static_cast<std::int64_t>(rect[1])+total);
        memory.store(GuestAddress{arg(3)+8},32,static_cast<std::uint32_t>(calculated_right));memory.store(GuestAddress{arg(3)+12},32,static_cast<std::uint32_t>(bottom));if(parameters)memory.store(GuestAddress{parameters+16},32,input.units);return finish(static_cast<std::uint32_t>(total));
    }
    auto y=rect[1];if(flags&single){if(flags&4)y=coordinate(static_cast<std::int64_t>(y)+(static_cast<std::int64_t>(rect[3])-y-total)/2);else if(flags&8)y=coordinate(static_cast<std::int64_t>(rect[3])-total);}
    const std::optional<GuiRect> clipping=(flags&noclip)?std::nullopt:std::optional<GuiRect>{{left,rect[1],right,rect[3]}};
    std::size_t consumed=0;std::int32_t bottom=rect[1];std::u16string modified=input.text;std::int64_t adjustment=0;
    struct Prepared {TextLine line;Ellipsis value;TabLayout layout;std::int32_t x{},y{};};std::vector<Prepared> prepared;
    for(const auto& line:lines){
        if(!(flags&noclip) && (y>=rect[3] || ((flags&edit) && static_cast<std::int64_t>(y)+metrics.height>rect[3])))break;
        const auto value=ellipsize(std::u16string_view(display.text).substr(line.begin,line.end-line.begin),coordinate(std::max<std::int64_t>(0,available)),flags,measure);const auto layout=layout_for(value.text);
        auto x=left;if(flags&1)x=coordinate(static_cast<std::int64_t>(left)+(available-layout.width)/2);else if(flags&2)x=coordinate(static_cast<std::int64_t>(right)-layout.width);
        if((flags&modify) && (flags&(end_ellipsis|path_ellipsis)) && value.changed){
            const auto begin=line.begin?display.raw_end[line.begin-1]:0,end=line.end?display.raw_end[line.end-1]:begin;
            std::u16string replacement;auto previous=begin;bool dots=false;
            for(auto index:value.indices){if(index==SIZE_MAX){if(!dots){replacement+=u"...";dots=true;}continue;}
                const auto current=line.begin+index,raw_end=display.raw_end[current];auto raw_begin=current?display.raw_end[current-1]:0;if(!dots)raw_begin=previous;
                replacement+=input.text.substr(raw_begin,raw_end-raw_begin);previous=raw_end;
            }
            modified.replace(static_cast<std::size_t>(static_cast<std::int64_t>(begin)+adjustment),end-begin,replacement);adjustment+=static_cast<std::int64_t>(replacement.size())-static_cast<std::int64_t>(end-begin);
        }
        prepared.push_back({line,value,layout,x,y});consumed=line.next>=display.text.size()?input.units:display.source_end[line.next-1];bottom=coordinate(static_cast<std::int64_t>(y)+line_height);y=bottom;
    }
    // 先驗證寫回範圍，避免無效 MODIFYSTRING 緩衝區在部分繪製後才失敗。
    std::vector<std::uint8_t> replacement;
    if(modified!=input.text){if(wide)replacement=utf16_bytes(modified);else{const auto bytes=utf16_to_utf8(modified);replacement.assign(bytes.begin(),bytes.end());replacement.push_back(0);}if(replacement.size()>(input.units+4)*(wide?2U:1U))return finish(0,50);memory.check(GuestAddress{pointer},replacement.size(),Access::Write);}
    for(const auto& item:prepared){draw_line(item.value.text,item.x,item.y,item.layout,clipping,flags&prefixonly);
        if(!(flags&(noprefix|hideprefix))){for(const auto& run:item.layout.runs){
            const auto shape=native_gui_?native_shape_text(font,run.text,flags&rtl,true):NativeTextShape{};
            for(std::size_t i=0;i<run.text.size();++i){const auto index=item.value.indices[run.begin+i];if(index==SIZE_MAX || !display.marked[item.line.begin+index])continue;
                auto end=i+1;if(run.text[i]>=0xd800 && run.text[i]<=0xdbff && end<run.text.size())++end;
                const auto a=shape.leading.empty()?measure(std::u16string_view(run.text).substr(0,i)):shape.leading[i],b=shape.trailing.empty()?measure(std::u16string_view(run.text).substr(0,end)):shape.trailing[end-1];
                GuiDraw underline;underline.kind=GuiDraw::Kind::Fill;underline.color=dc.color;underline.x=coordinate(static_cast<std::int64_t>(item.x)+run.x+std::min(a,b));underline.right=coordinate(static_cast<std::int64_t>(item.x)+run.x+std::max(a,b));underline.y=coordinate(static_cast<std::int64_t>(item.y)+metrics.ascent+1);underline.bottom=coordinate(static_cast<std::int64_t>(underline.y)+1);emit(underline,clipping);
            }
        }}
    }
    if(!replacement.empty())memory.write(GuestAddress{pointer},replacement);if(parameters)memory.store(GuestAddress{parameters+16},32,consumed);
    return finish(static_cast<std::uint32_t>(coordinate(static_cast<std::int64_t>(bottom)-rect[1])));
}
}
