// 此檔納入 DirectWrite 共用派送流程，不另建與既有字型／COM 不相容的後端。
const auto float_result=[&](float value){
    const auto raw=std::bit_cast<std::uint32_t>(value);
    if(x86_){const auto top=static_cast<unsigned>(((state.x87_status>>11)-1)&7);int exponent=0;const double fraction=std::frexp(std::abs(value),&exponent);
        state.x87[top]={static_cast<std::uint64_t>(fraction*0x1p64),static_cast<std::uint16_t>((value==0?0:exponent+16382)|(std::signbit(value)?0x8000:0))};
        state.x87_status=static_cast<std::uint16_t>((state.x87_status&~0x3800U)|(top<<11));state.x87_tag=static_cast<std::uint16_t>((state.x87_tag&~(3U<<(top*2)))|((value==0?1U:0U)<<(top*2)));
    }else {state.xmm[0]={};for(unsigned i=0;i<4;++i)state.xmm[0][i]=static_cast<std::uint8_t>(raw>>(i*8));}
    return finish(0);
};
const auto text_references=[&](Object& target){
    std::set<std::uint64_t> next;
    const auto add=[&](const TextStyle& style){if(style.collection)next.insert(style.collection);if(style.effect)next.insert(style.effect);};
    add(target.text_data->format);for(const auto& [position,style]:target.text_data->ranges){(void)position;add(style);}
    for(auto pointer:next)if(!target.retained.contains(pointer))bridge.deferred.push_back(font_callback(memory,unit,pointer,1));
    for(auto pointer:target.retained)if(!next.contains(pointer))bridge.deferred.push_back(font_callback(memory,unit,pointer,2));
    target.retained=std::move(next);
};
if(o.kind==Kind::Factory && method=="CreateTextFormat"){
    if(!arg(8))return finish(pointer_error);out(8,0);
    const float size=floating(6);
    if(!arg(1) || !arg(7) || arg(3)<1 || arg(3)>999 || arg(4)>2 || arg(5)<1 || arg(5)>9 || !std::isfinite(size) || size<=0)return finish(invalid);
    auto collection=arg(2);
    if(collection){const auto it=bridge.objects.find(collection);if(it==bridge.objects.end() || it->second.kind!=Kind::FontCollection)return finish(invalid);}
    auto data=std::make_shared<TextData>();data->format.family=read_utf16(memory,GuestAddress{arg(1)},256);data->format.locale=read_utf16(memory,GuestAddress{arg(7)},85);
    if(data->format.family.empty())return finish(invalid);
    data->format.weight=static_cast<std::uint32_t>(arg(3));data->format.style=static_cast<std::uint32_t>(arg(4));data->format.stretch=static_cast<std::uint32_t>(arg(5));data->format.size=size;data->options.tab=size*4;
    Object value;value.kind=Kind::TextFormat;value.text_data=data;
    if(!collection){Object fonts;fonts.kind=Kind::FontCollection;fonts.fonts=system();collection=create(std::move(fonts));value.retained.insert(collection);}
    data->format.collection=collection;const auto at=create(std::move(value));text_references(bridge.objects.at(at));out(8,at);return finish(0);
}
if(o.kind==Kind::Factory && (method=="CreateTextLayout" || method=="CreateGdiCompatibleTextLayout")){
    const bool gdi=method=="CreateGdiCompatibleTextLayout";const unsigned output=gdi?9:6;
    if(!arg(output))return finish(pointer_error);out(output,0);
    const float width=floating(4),height=floating(5),ppd=gdi?floating(6):1;
    const auto source=bridge.objects.find(arg(3));
    if(!arg(1) || source==bridge.objects.end() || !source->second.text_data || !std::isfinite(width) || width<0 || !std::isfinite(height) || height<0 || !std::isfinite(ppd) || ppd<=0)return finish(invalid);
    const auto count=static_cast<std::uint32_t>(arg(2));if(count>4*1024*1024)return finish(0x8007000eU);
    if(gdi && arg(7)){for(unsigned i=0;i<6;++i)if(!std::isfinite(std::bit_cast<float>(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(7)+i*4},32)))))return finish(invalid);}
    Object value;value.kind=Kind::TextLayout;value.text_data=std::make_shared<TextData>(*source->second.text_data);auto& data=*value.text_data;
    data.layout.reset();data.input.resize(count);if(count)memory.read(GuestAddress{arg(1)},std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(data.input.data()),count*2));
    data.options.width=width;data.options.height=height;data.options.pixels_per_dip=ppd;data.options.measuring=gdi?(arg(8)?2:1):0;
    data.ranges={{0,data.format}};const auto at=create(std::move(value));text_references(bridge.objects.at(at));out(output,at);return finish(0);
}
if(o.kind==Kind::TextFormat || o.kind==Kind::TextLayout){
    auto& data=*o.text_data;
    const auto write_float=[&](std::uint64_t at,float value){memory.store(GuestAddress{at},32,std::bit_cast<std::uint32_t>(value));};
    const auto string_out=[&](std::uint64_t at,std::uint32_t capacity,const std::u16string& text){if(!at)return pointer_error;if(capacity<=text.size())return insufficient;memory.write(GuestAddress{at},utf16_bytes(text));return 0U;};
    if(method=="SetTextAlignment" || method=="SetParagraphAlignment" || method=="SetWordWrapping" || method=="SetReadingDirection" || method=="SetFlowDirection"){
        const auto value=static_cast<std::uint32_t>(arg(1));
        if(method=="SetTextAlignment"){if(value>3)return finish(invalid);if(value==3)return finish(0x80004001U);data.options.alignment=value;}
        if(method=="SetParagraphAlignment"){if(value>2)return finish(invalid);data.options.paragraph=value;}
        if(method=="SetWordWrapping"){if(value>4)return finish(invalid);if(value==3)return finish(0x80004001U);data.options.wrapping=value;}
        if(method=="SetReadingDirection"){if(value>3)return finish(invalid);if(value>1)return finish(0x80004001U);data.options.reading=value;}
        if(method=="SetFlowDirection"){if(value>3)return finish(invalid);if(value)return finish(0x80004001U);data.flow=value;}
        data.layout.reset();return finish(0);
    }
    if(method=="GetTextAlignment")return finish(data.options.alignment);
    if(method=="GetParagraphAlignment")return finish(data.options.paragraph);
    if(method=="GetWordWrapping")return finish(data.options.wrapping);
    if(method=="GetReadingDirection")return finish(data.options.reading);
    if(method=="GetFlowDirection")return finish(data.flow);
    if(method=="SetIncrementalTabStop"){const auto value=floating(1);if(!std::isfinite(value) || value<=0)return finish(invalid);data.options.tab=value;data.layout.reset();return finish(0);}
    if(method=="GetIncrementalTabStop")return float_result(data.options.tab);
    if(method=="SetLineSpacing"){
        const auto spacing=floating(2),baseline=floating(3);if(arg(1)>1 || !std::isfinite(spacing) || spacing<0 || !std::isfinite(baseline) || baseline<0)return finish(invalid);
        data.options.spacing=static_cast<unsigned>(arg(1));data.options.line_spacing=spacing;data.options.baseline=baseline;data.layout.reset();return finish(0);
    }
    if(method=="GetLineSpacing"){for(unsigned i=1;i<4;++i){if(!arg(i))return finish(pointer_error);memory.check(GuestAddress{arg(i)},4,Access::Write);}memory.store(GuestAddress{arg(1)},32,data.options.spacing);write_float(arg(2),data.options.line_spacing);write_float(arg(3),data.options.baseline);return finish(0);}
    if(method=="SetTrimming"){
        if(!arg(1))return finish(pointer_error);std::array<std::uint32_t,3> trim{};for(unsigned i=0;i<3;++i)trim[i]=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)+i*4},32));
        if(trim[0]>2 || trim[1]>0x10ffff)return finish(invalid);if(trim[0] || arg(2))return finish(0x80004001U);
        std::copy(trim.begin(),trim.end(),data.trimming);data.layout.reset();return finish(0);
    }
    if(method=="GetTrimming"){if(!arg(1) || !arg(2))return finish(pointer_error);memory.check(GuestAddress{arg(1)},12,Access::Write);memory.check(GuestAddress{arg(2)},unit,Access::Write);for(unsigned i=0;i<3;++i)memory.store(GuestAddress{arg(1)+i*4},32,data.trimming[i]);out(2,0);return finish(0);}
    const bool suffix=method.ends_with("At");const bool at=suffix || method=="GetUnderline" || method=="GetStrikethrough" || method=="GetDrawingEffect" || method=="GetInlineObject" || method=="GetTypography";const auto property=suffix?method.substr(0,method.size()-2):method;
    if(method.starts_with("Get") && (property=="GetFontCollection" || property.starts_with("GetFontFamilyName") || property=="GetFontWeight" || property=="GetFontStyle" || property=="GetFontStretch" || property=="GetFontSize" || property.starts_with("GetLocaleName") || property=="GetUnderline" || property=="GetStrikethrough" || property=="GetDrawingEffect" || property=="GetInlineObject" || property=="GetTypography")){
        const auto position=static_cast<std::uint32_t>(arg(1));auto selected=data.ranges.empty()?data.ranges.end():std::prev(data.ranges.upper_bound(position));const auto& style=at?selected->second:data.format;
        const unsigned output=at?2:1;const bool str=property=="GetFontFamilyName" || property=="GetLocaleName";const unsigned range_index=str?4:3;
        if(at && arg(range_index)){memory.check(GuestAddress{arg(range_index)},8,Access::Write);const auto start=selected->first;auto next=std::next(selected);memory.store(GuestAddress{arg(range_index)},32,start);memory.store(GuestAddress{arg(range_index)+4},32,next==data.ranges.end()?UINT32_MAX-start:next->first-start);}
        if(property=="GetFontFamilyName" || property=="GetLocaleName")return finish(string_out(arg(output),static_cast<std::uint32_t>(arg(output+1)),property=="GetFontFamilyName"?style.family:style.locale));
        const bool length=property.ends_with("Length");
        if(property=="GetFontCollection" || property=="GetDrawingEffect" || property=="GetInlineObject" || property=="GetTypography"){
            if(!arg(output))return finish(pointer_error);const auto pointer=property=="GetFontCollection"?style.collection:property=="GetDrawingEffect"?style.effect:0;
            out(output,pointer);if(pointer)bridge.deferred.push_back(font_callback(memory,unit,pointer,1));return finish(0);
        }
        if(property=="GetFontSize"){if(at){if(!arg(output))return finish(pointer_error);write_float(arg(output),style.size);return finish(0);}return float_result(style.size);}
        const std::uint32_t value=length?static_cast<std::uint32_t>(property.starts_with("GetFontFamilyName")?style.family.size():style.locale.size()):property=="GetFontWeight"?style.weight:property=="GetFontStyle"?style.style:property=="GetFontStretch"?style.stretch:property=="GetUnderline"?style.underline:style.strike;
        if(at){if(!arg(output))return finish(pointer_error);memory.store(GuestAddress{arg(output)},32,value);return finish(0);}return finish(value);
    }
    if(o.kind==Kind::TextLayout){
        if(method=="SetMaxWidth" || method=="SetMaxHeight"){const auto value=floating(1);if(!std::isfinite(value) || value<0)return finish(invalid);(method=="SetMaxWidth"?data.options.width:data.options.height)=value;data.layout.reset();return finish(0);}
        if(method=="GetMaxWidth")return float_result(data.options.width);if(method=="GetMaxHeight")return float_result(data.options.height);
        if(method.starts_with("SetFont") || method=="SetLocaleName" || method=="SetUnderline" || method=="SetStrikethrough" || method=="SetDrawingEffect" || method=="SetInlineObject" || method=="SetTypography"){
            const auto packed=arg(2);const auto start=static_cast<std::uint32_t>(packed),length=static_cast<std::uint32_t>(packed>>32);
            if(length>UINT32_MAX-start)return finish(invalid);if(!length)return finish(0);
            TextStyle replacement;
            if(method=="SetFontFamilyName" || method=="SetLocaleName"){if(!arg(1))return finish(pointer_error);replacement.family=read_utf16(memory,GuestAddress{arg(1)},method=="SetLocaleName"?85:256);if(method=="SetFontFamilyName" && replacement.family.empty())return finish(invalid);}
            if(method=="SetFontWeight" && (arg(1)<1 || arg(1)>999))return finish(invalid);if(method=="SetFontStyle" && arg(1)>2)return finish(invalid);if(method=="SetFontStretch" && (arg(1)<1 || arg(1)>9))return finish(invalid);
            if(method=="SetFontSize" && (!std::isfinite(floating(1)) || floating(1)<=0))return finish(invalid);
            if(method=="SetFontCollection"){const auto found_collection=bridge.objects.find(arg(1));if(found_collection==bridge.objects.end() || found_collection->second.kind!=Kind::FontCollection)return finish(invalid);}
            if((method=="SetInlineObject" || method=="SetTypography") && arg(1))return finish(0x80004001U);
            const auto split=[&](std::uint32_t point){if(!data.ranges.contains(point))data.ranges.emplace(point,std::prev(data.ranges.upper_bound(point))->second);};split(start);split(start+length);
            for(auto i=data.ranges.lower_bound(start);i!=data.ranges.end() && i->first<start+length;++i){auto& style=i->second;
                if(method=="SetFontFamilyName")style.family=replacement.family;if(method=="SetLocaleName")style.locale=replacement.family;
                if(method=="SetFontCollection")style.collection=arg(1);if(method=="SetFontWeight")style.weight=static_cast<std::uint32_t>(arg(1));if(method=="SetFontStyle")style.style=static_cast<std::uint32_t>(arg(1));if(method=="SetFontStretch")style.stretch=static_cast<std::uint32_t>(arg(1));
                if(method=="SetFontSize")style.size=floating(1);if(method=="SetUnderline")style.underline=arg(1)!=0;if(method=="SetStrikethrough")style.strike=arg(1)!=0;if(method=="SetDrawingEffect")style.effect=arg(1);
            }
            data.layout.reset();text_references(o);return finish(0);
        }
        const auto get_layout=[&]() -> const NativeTextLayout& {
            if(!data.layout){std::vector<NativeLayoutSpan> spans;
                for(auto at=data.ranges.begin();at!=data.ranges.end();++at){if(at->first>data.input.size())break;const auto next=std::next(at);const auto end=std::min<std::size_t>(data.input.size(),next==data.ranges.end()?data.input.size():next->first);const auto& style=at->second;
                    const auto& fonts=bridge.objects.at(style.collection).fonts;std::optional<std::size_t> best;
                    const auto family=fonts->system_collection && windows_ui_font_alias(style.family)?native_ui_font_family():style.family;
                    for(std::size_t i=0;i<fonts->faces.size();++i){const auto& face=fonts->faces[i];if(folded(face.family)!=folded(family) && folded(face.full_name)!=folded(family))continue;
                        const auto rank=[&](std::size_t index){const auto& f=fonts->faces[index];return std::pair{f.italic!=(style.style!=0),std::abs(f.weight-static_cast<int>(style.weight))};};if(!best || rank(i)<rank(*best))best=i;}
                    NativeFontFace face;if(best)face=fonts->faces[*best];else {GuiFont font;font.face=style.family;font.height=-static_cast<std::int32_t>(std::ceil(style.size));font.weight=static_cast<int>(style.weight);font.italic=style.style!=0;const auto fallback=native_resolve_font(font);if(!fallback)throw std::bad_alloc{};face=*fallback;}
                    spans.push_back({at->first,static_cast<std::uint32_t>(end-at->first),std::move(face),style.size,style.locale});
                }data.layout=native_text_layout(data.input,spans,data.options);
            }return *data.layout;
        };
        const auto hit_bytes=[&](const NativeLayoutCluster& c,float x,float y){const auto& line=get_layout().lines[c.line];std::array<std::uint32_t,9> value{c.start,c.length,std::bit_cast<std::uint32_t>(c.x+x),std::bit_cast<std::uint32_t>(line.top+y),std::bit_cast<std::uint32_t>(c.width),std::bit_cast<std::uint32_t>(line.height),c.rtl?1U:0U,1,0};return value;};
        const auto write_hit=[&](std::uint64_t address,const NativeLayoutCluster& c,float x=0,float y=0){const auto bytes=hit_bytes(c,x,y);memory.write(GuestAddress{address},std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()),36));};
        if(method=="GetMetrics"){
            if(!arg(1))return finish(pointer_error);const auto& layout=get_layout();const std::array<std::uint32_t,9> metrics{std::bit_cast<std::uint32_t>(layout.left),std::bit_cast<std::uint32_t>(layout.top),std::bit_cast<std::uint32_t>(layout.width),std::bit_cast<std::uint32_t>(layout.width_trailing),std::bit_cast<std::uint32_t>(layout.height),std::bit_cast<std::uint32_t>(data.options.width),std::bit_cast<std::uint32_t>(data.options.height),layout.bidi_depth,static_cast<std::uint32_t>(layout.lines.size())};
            memory.write(GuestAddress{arg(1)},std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(metrics.data()),36));return finish(0);
        }
        if(method=="GetOverhangMetrics"){if(!arg(1))return finish(pointer_error);const auto& layout=get_layout();for(unsigned i=0;i<4;++i)write_float(arg(1)+4*i,layout.overhang[i]);return finish(0);}
        if(method=="DetermineMinWidth"){if(!arg(1))return finish(pointer_error);write_float(arg(1),get_layout().minimum);return finish(0);}
        if(method=="GetLineMetrics" || method=="GetClusterMetrics"){
            if(!arg(3))return finish(pointer_error);const auto& layout=get_layout();const bool line=method=="GetLineMetrics";const auto count=line?layout.lines.size():layout.clusters.size();const auto capacity=static_cast<std::uint32_t>(arg(2));const auto used=std::min<std::size_t>(count,capacity);const unsigned size=line?24:8;
            if(used && !arg(1))return finish(pointer_error);memory.check(GuestAddress{arg(3)},4,Access::Write);if(used)memory.check(GuestAddress{arg(1)},used*size,Access::Write);
            memory.store(GuestAddress{arg(3)},32,count);
            for(std::size_t i=0;i<used;++i){const auto dest=arg(1)+i*size;
                if(line){const auto& m=layout.lines[i];const std::array<std::uint32_t,6> bytes{m.length,m.trailing,m.newline,std::bit_cast<std::uint32_t>(m.height),std::bit_cast<std::uint32_t>(m.baseline),0};memory.write(GuestAddress{dest},std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()),24));}
                else {const auto& c=layout.clusters[i];write_float(dest,c.width);memory.store(GuestAddress{dest+4},16,c.length);memory.store(GuestAddress{dest+6},16,(c.wrap?1:0)|(c.whitespace?2:0)|(c.newline?4:0)|(data.input[c.start]==u'\u00ad'?8:0)|(c.rtl?16:0));}
            }return finish(capacity<count?insufficient:0);
        }
        if(method=="HitTestTextPosition" || method=="HitTestPoint"){
            const auto& layout=get_layout();const bool point=method=="HitTestPoint";const unsigned metric=point?5:5;
            for(unsigned i=3;i<=metric;++i)if(!arg(i))return finish(pointer_error);
            memory.check(GuestAddress{arg(3)},4,Access::Write);memory.check(GuestAddress{arg(4)},4,Access::Write);memory.check(GuestAddress{arg(5)},36,Access::Write);
            NativeLayoutCluster empty;empty.line=static_cast<std::uint32_t>(layout.lines.size()-1);empty.start=static_cast<std::uint32_t>(data.input.size());empty.x=layout.lines.back().x+layout.lines.back().width_trailing;
            const NativeLayoutCluster* selected=&empty;bool trailing=false,inside=false;
            if(point){const auto x=floating(1),y=floating(2);if(!std::isfinite(x) || !std::isfinite(y))return finish(invalid);float best=INFINITY;
                for(const auto& c:layout.clusters){const auto& line=layout.lines[c.line];const float dx=x<c.x?c.x-x:x>c.x+c.width?x-c.x-c.width:0,dy=y<line.top?line.top-y:y>line.top+line.height?y-line.top-line.height:0;const float distance=dx*dx+dy*dy;if(distance<best){best=distance;selected=&c;inside=dx==0 && dy==0;}}
                trailing=(x>selected->x+selected->width*0.5f)!=selected->rtl;memory.store(GuestAddress{arg(3)},32,trailing);memory.store(GuestAddress{arg(4)},32,inside);
            }else {const auto pos=static_cast<std::uint32_t>(arg(1));if(pos>data.input.size())return finish(invalid);for(const auto& c:layout.clusters)if(pos>=c.start && pos<c.start+c.length){selected=&c;break;}trailing=arg(2)!=0;write_float(arg(3),selected->x+((trailing!=selected->rtl)?selected->width:0));write_float(arg(4),layout.lines[selected->line].top);}
            write_hit(arg(5),*selected);return finish(0);
        }
        if(method=="HitTestTextRange"){
            if(!arg(7))return finish(pointer_error);const auto start=static_cast<std::uint32_t>(arg(1)),length=static_cast<std::uint32_t>(arg(2));if(start>data.input.size() || length>data.input.size()-start || !std::isfinite(floating(3)) || !std::isfinite(floating(4)))return finish(invalid);
            const auto& layout=get_layout();std::vector<NativeLayoutCluster> hits;for(const auto& c:layout.clusters)if(c.start<start+length && c.start+c.length>start){
                if(!hits.empty() && hits.back().line==c.line && hits.back().rtl==c.rtl && hits.back().start+hits.back().length==c.start && std::abs(hits.back().x+hits.back().width-c.x)<0.01f){hits.back().length+=c.length;hits.back().width+=c.width;}else hits.push_back(c);}
            const auto capacity=static_cast<std::uint32_t>(arg(6));const auto used=std::min<std::size_t>(hits.size(),capacity);if(used && !arg(5))return finish(pointer_error);memory.check(GuestAddress{arg(7)},4,Access::Write);if(used)memory.check(GuestAddress{arg(5)},used*36,Access::Write);memory.store(GuestAddress{arg(7)},32,hits.size());for(std::size_t i=0;i<used;++i)write_hit(arg(5)+i*36,hits[i],floating(3),floating(4));return finish(capacity<hits.size()?insufficient:0);
        }
        if(method=="Draw"){
            if(!arg(2))return finish(pointer_error);if(!std::isfinite(floating(3)) || !std::isfinite(floating(4)))return finish(invalid);const auto& layout=get_layout();
            TextDrawing drawing;drawing.service=service;drawing.layout=arg(0);std::size_t bytes=66+data.input.size()*2;
            for(const auto& run:layout.runs)bytes+=384+run.length*2+run.glyphs.indices.size()*14;
            if(bytes>64*1024*1024)return finish(0x8007000eU);drawing.scratch=allocate_heap(memory,bytes);if(!drawing.scratch)return finish(0x8007000eU);
            const auto base=drawing.scratch;memory.write(GuestAddress{base},std::array<std::uint8_t,64>{});write_float(base+4,1);write_float(base+16,1);write_float(base+28,1);
            drawing.callbacks={font_callback(memory,unit,arg(2),3,{arg(1),base}),font_callback(memory,unit,arg(2),4,{arg(1),base+4}),font_callback(memory,unit,arg(2),5,{arg(1),base+28})};
            std::uint64_t cursor=base+64;const auto input=cursor;auto input_bytes=utf16_bytes(data.input);memory.write(GuestAddress{input},input_bytes);cursor+=input_bytes.size();
            try {for(const auto& run:layout.runs){
                const auto& style=std::prev(data.ranges.upper_bound(run.start))->second;Object face;face.kind=Kind::FontFace;face.fonts=std::make_shared<FontSet>();face.fonts->faces={run.face};face.indices={0};const auto font=create(std::move(face));drawing.faces.push_back(font);
                const auto aligned=[&](unsigned alignment){cursor=(cursor+alignment-1)&~static_cast<std::uint64_t>(alignment-1);};aligned(8);const auto glyph_run=cursor;cursor+=unit==8?48:32;const auto description=cursor;cursor+=unit==8?40:20;
                const auto indices=cursor;cursor+=run.glyphs.indices.size()*2;aligned(4);const auto advances=cursor;cursor+=run.glyphs.advances.size()*4;const auto offsets=cursor;cursor+=run.glyphs.offsets.size()*8;const auto clusters=cursor;cursor+=run.clusters.size()*2;const auto locale=cursor;const auto locale_bytes=utf16_bytes(style.locale);cursor+=locale_bytes.size();
                if(cursor>base+bytes)throw std::bad_alloc{};memory.write(GuestAddress{locale},locale_bytes);memory.write(GuestAddress{indices},std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(run.glyphs.indices.data()),run.glyphs.indices.size()*2));memory.write(GuestAddress{advances},std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(run.glyphs.advances.data()),run.glyphs.advances.size()*4));memory.write(GuestAddress{offsets},std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(run.glyphs.offsets.data()),run.glyphs.offsets.size()*8));memory.write(GuestAddress{clusters},std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(run.clusters.data()),run.clusters.size()*2));
                put(glyph_run,font);write_float(glyph_run+unit,run.glyphs.em);memory.store(GuestAddress{glyph_run+unit+4},32,run.glyphs.indices.size());put(glyph_run+(unit==8?16:12),indices);put(glyph_run+(unit==8?24:16),advances);put(glyph_run+(unit==8?32:20),offsets);memory.store(GuestAddress{glyph_run+(unit==8?40:24)},32,0);memory.store(GuestAddress{glyph_run+(unit==8?44:28)},32,run.glyphs.rtl);
                put(description,locale);put(description+unit,input+run.start*2);memory.store(GuestAddress{description+2*unit},32,run.length);put(description+(unit==8?24:12),clusters);memory.store(GuestAddress{description+(unit==8?32:16)},32,run.start);
                const auto x=std::bit_cast<std::uint32_t>(run.glyphs.baseline_x+floating(3)),y=std::bit_cast<std::uint32_t>(run.glyphs.baseline_y+floating(4));
                auto callback=font_callback(memory,unit,arg(2),6,{arg(1),x,y,data.options.measuring,glyph_run,description,style.effect});callback.floating_arguments=12;drawing.callbacks.push_back(std::move(callback));
                for(unsigned decoration=0;decoration<2;++decoration)if(decoration?style.strike:style.underline){aligned(8);const auto record=cursor;cursor+=40;write_float(record,run.width);const float scale=style.size/static_cast<float>(run.face.em);write_float(record+4,std::max(1.0f,static_cast<float>(run.face.design_metrics[decoration?9:7])*scale));write_float(record+8,-static_cast<float>(run.face.design_metrics[decoration?8:6])*scale);
                    const unsigned direction=decoration?12:16; if(!decoration)write_float(record+12,style.size);memory.store(GuestAddress{record+direction},32,data.options.reading);memory.store(GuestAddress{record+direction+4},32,data.flow);const unsigned locale_offset=unit==8?24:direction+8;put(record+locale_offset,locale);memory.store(GuestAddress{record+locale_offset+unit},32,data.options.measuring);
                    auto decoration_call=font_callback(memory,unit,arg(2),decoration?8:7,{arg(1),x,y,record,style.effect});decoration_call.floating_arguments=12;drawing.callbacks.push_back(std::move(decoration_call));}
            }}catch(...){for(auto face:drawing.faces)bridge.deferred.push_back(font_callback(memory,unit,face,2));(void)free_heap(memory,base,state);throw;}
            ++o.references;const auto token=bridge.next_operation++;bridge.drawings.emplace(token,std::move(drawing));start_callbacks(state,memory,{bridge.drawings.at(token).callbacks[0]},Continuation::DWriteOperation,token);return std::nullopt;
        }
    }
}
