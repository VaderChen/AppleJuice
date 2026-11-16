// GDI+ 文字家族：字型家族與集合、字型、StringFormat、DrawString／MeasureString／MeasureCharacterRanges、DriverString 與 AddPathString。
// 字形對應、字寬與輪廓取自共用的原生字型層（native_font_glyph_indices／native_font_glyph_metrics／native_gdi_glyph），缺字經 native_shape_text 取得替代字型；
// 斷行沿用 unicode_text_analysis.hpp 的 ICU 換行分析，輪廓轉成路徑後交給 render 填滿（反鋸齒依 TextRenderingHint）。
constexpr std::uint32_t family_not_found=14,not_truetype=16;
const auto family_of=[&](std::uint64_t handle)->GdiplusState::FontFamily&{const auto found=g.families.find(handle);if(found==g.families.end())throw std::invalid_argument("GDI+ 字型家族無效");return found->second;};
const auto font_of=[&](std::uint64_t handle)->GdiplusState::Font&{const auto found=g.fonts.find(handle);if(found==g.fonts.end())throw std::invalid_argument("GDI+ 字型無效");return found->second;};
const auto format_of=[&](std::uint64_t handle)->GdiplusState::StringFormat&{const auto found=g.formats.find(handle);if(found==g.formats.end())throw std::invalid_argument("GDI+ StringFormat 無效");return found->second;};
const auto collection_of=[&](std::uint64_t handle)->GdiplusState::FontCollection&{const auto found=g.collections.find(handle);if(found==g.collections.end())throw std::invalid_argument("GDI+ 字型集合無效");return found->second;};
// 已安裝字型以名稱查詢；Windows 介面別名（如 Microsoft Sans Serif）改用系統介面字型，但保留要求的名稱。
const auto installed_family=[&](std::u16string_view name)->std::optional<GdiplusState::FontFamily>{
    if(name.empty())return std::nullopt;auto faces=native_font_faces(name);
    if(faces.empty() && windows_ui_font_alias(name))faces=native_font_faces(native_ui_font_family());
    if(faces.empty())return std::nullopt;return GdiplusState::FontFamily{std::u16string(name),std::move(faces)};};
const auto find_family=[&](std::u16string_view name,std::uint64_t collection)->std::optional<GdiplusState::FontFamily>{
    if(collection && !collection_of(collection).installed){for(const auto& family:collection_of(collection).families)if(windows_nls_compare(family.name,name,0x0409,1)==0)return family;return std::nullopt;}
    return installed_family(name);};
// FontStyle：Bold 1、Italic 2、Underline 4、Strikeout 8。最接近的粗細與斜體；缺少時以 DirectWrite 同樣的粗體／斜體模擬。
const auto face_for=[](const GdiplusState::FontFamily& family,unsigned style)->NativeFontFace{
    if(family.faces.empty())throw std::invalid_argument("GDI+ 字型家族沒有字型");const bool bold=style&1U,italic=style&2U;const auto target=bold?700:400;
    const auto score=[&](const NativeFontFace& f){return std::abs(f.weight-target)+(f.italic!=italic?1000:0);};
    auto face=*std::min_element(family.faces.begin(),family.faces.end(),[&](const auto& a,const auto& b){return score(a)<score(b);});
    if(bold && face.weight<600)face.simulations|=1U;if(italic && !face.italic)face.simulations|=2U;return face;};
const auto style_available=[](const GdiplusState::FontFamily& family,unsigned style){
    return std::any_of(family.faces.begin(),family.faces.end(),[&](const auto& f){return (f.weight>=600)==((style&1U)!=0) && f.italic==((style&2U)!=0);});};
// 字型大小換成世界座標：Unit World 0、Pixel 2、Point 3、Inch 4、Document 5、Millimeter 6（Display 不可用於字型）。
const auto unit_pixels=[](unsigned unit){static constexpr double px[]{1,1,1,96.0/72,96,96.0/300,96.0/25.4};return px[std::min(unit,6U)];};
const auto em_world=[&](const GdiplusState::Font& font,std::uint64_t graphics){
    if(font.unit==0)return font.size;const auto factor=graphics?page_factor(graphics_of(graphics)):1.0;return font.size*unit_pixels(font.unit)/factor;};
const auto line_spacing=[](const NativeFontFace& face){return static_cast<double>(face.ascent+face.descent+face.line_gap);};
// 設計單位、Y 向上的字形輪廓（二次曲線轉成三次）。
const auto glyph_outline=[&](const NativeFontFace& face,std::uint16_t glyph)->const std::vector<GuiPathPoint>&{
    const auto key=std::tuple{face.postscript_name,face.simulations,glyph};if(const auto found=g.outlines.find(key);found!=g.outlines.end())return found->second;
    if(g.outlines.size()>4096)g.outlines.clear();
    std::vector<GuiPathPoint> points;const auto native=native_gdi_glyph(face,glyph,{1,0,0,1},0);
    for(const auto& contour:native.contours){
        points.push_back({contour.start[0],contour.start[1],6});auto last=contour.start;
        for(const auto& segment:contour.segments){
            if(segment.kind==1){points.push_back({segment.points[0][0],segment.points[0][1],2});last=segment.points[0];}
            else if(segment.kind==2){const auto& q=segment.points[0];const auto& e=segment.points[1];
                points.push_back({last[0]+(q[0]-last[0])*2/3,last[1]+(q[1]-last[1])*2/3,4});points.push_back({e[0]+(q[0]-e[0])*2/3,e[1]+(q[1]-e[1])*2/3,4});points.push_back({e[0],e[1],4});last=e;}
            else {for(unsigned i=0;i<3;++i)points.push_back({segment.points[i][0],segment.points[i][1],4});last=segment.points[2];}
        }
        points.back().type|=1;
    }
    return g.outlines.emplace(key,std::move(points)).first->second;
};
// 文字對應到字形：主要字型沒有的字元以 native_shape_text 的替代字型補上。
struct PlacedGlyph {NativeFontFace face;std::uint16_t glyph{};double advance{};};
const auto map_glyphs=[&](const NativeFontFace& primary,std::u16string_view text,bool fallback)->std::vector<PlacedGlyph>{
    std::vector<PlacedGlyph> out(text.size());std::vector<std::uint32_t> codepoints;std::vector<std::size_t> starts;
    for(std::size_t i=0;i<text.size();){const auto begin=i;UChar32 cp;U16_NEXT(text.data(),i,text.size(),cp);codepoints.push_back(static_cast<std::uint32_t>(cp));starts.push_back(begin);}
    const auto apply=[&](const NativeFontFace& face,std::span<const std::size_t> indices){
        std::vector<std::uint32_t> cps;for(const auto i:indices)cps.push_back(codepoints[i]);const auto glyphs=native_font_glyph_indices(face,cps);
        std::vector<std::uint16_t> present;for(const auto glyph:glyphs)present.push_back(glyph);const auto metrics=native_font_glyph_metrics(face,present);
        const auto scale=1.0/static_cast<double>(face.em);
        for(std::size_t k=0;k<indices.size();++k)if(glyphs[k] || &face==&primary)out[starts[indices[k]]]={face,glyphs[k],metrics[k][1]*scale};};
    std::vector<std::size_t> all(codepoints.size());for(std::size_t i=0;i<all.size();++i)all[i]=i;apply(primary,all);
    if(fallback){
        for(std::size_t i=0;i<codepoints.size();){
            if(out[starts[i]].glyph || codepoints[i]<0x20 || codepoints[i]==0x2028 || codepoints[i]==0x2029){++i;continue;}
            auto end=i;while(end<codepoints.size() && !out[starts[end]].glyph && codepoints[end]>=0x20)++end;
            const auto begin_unit=starts[i],end_unit=end<starts.size()?starts[end]:text.size();
            GuiFont model;model.face=primary.family;model.height=-16;model.weight=primary.weight;model.italic=primary.italic;
            const auto shape=native_shape_text(model,text.substr(begin_unit,end_unit-begin_unit),false,true);std::size_t cursor=i;
            for(const auto& run:shape.runs){const auto face=native_resolve_font(run.font);std::size_t units=0;std::vector<std::size_t> indices;
                while(cursor<end && units<run.text.size()){indices.push_back(cursor);units+=codepoints[cursor]>0xffff?2:1;++cursor;}
                if(face && !indices.empty())apply(*face,indices);}
            i=end;
        }
    }
    return out;
};
// 排版結果：每個字形的基線原點（世界座標）、每個來源字元的格子、裝飾線、外框與實際放入的字元／行數。
struct LaidGlyph {NativeFontFace face;std::uint16_t glyph{};double x{},y{},scale{};};
struct TextLayout {std::vector<LaidGlyph> glyphs;std::vector<std::array<double,4>> cells,decorations;std::array<double,4> box{};std::int32_t fitted{},lines{};};
const auto layout_text=[&](std::u16string_view source,const GdiplusState::FontFamily& family,unsigned style,double em,std::array<double,4> rect,const GdiplusState::StringFormat& format)->TextLayout{
    TextLayout result;result.cells.assign(source.size(),{0,0,0,0});const auto face=face_for(family,style);const auto unit=em/static_cast<double>(face.em);
    const auto ascent=face.ascent*unit,height=line_spacing(face)*unit,padding=format.typographic?0.0:em/6;
    // 熱鍵前綴：Hide 移除 &、Show 另在下一字元畫底線；&& 為字面 &。
    std::u16string text;std::vector<std::size_t> origin;std::vector<std::size_t> hotkeys;
    for(std::size_t i=0;i<source.size();++i){
        if(format.hotkey && source[i]==u'&'){if(i+1<source.size() && source[i+1]==u'&'){text.push_back(u'&');origin.push_back(i);++i;continue;}if(format.hotkey==1 && i+1<source.size())hotkeys.push_back(text.size());continue;}
        text.push_back(source[i]);origin.push_back(i);}
    const auto glyphs=map_glyphs(face,text,!(format.flags&0x400U));
    const auto breaks=text_analysis::line_breaks(text,"");const bool wrap=!(format.flags&0x1000U) && rect[2]>0;const auto available=rect[2]>0?rect[2]-2*padding:INFINITY;
    const auto advance_of=[&](std::size_t i,double x)->double{
        if(text[i]==u'\t'){double stop=format.tab_first;std::size_t k=0;const auto tab=format.tabs.empty()?4*em/2:0.0;
            while(stop<=x+1e-9){stop+=format.tabs.empty()?tab:format.tabs[std::min(k,format.tabs.size()-1)];++k;if(format.tabs.empty() && k>4096)break;}return stop-x;}
        if(text[i]<0x20 || (text[i]>=0xdc00 && text[i]<=0xdfff))return 0;return glyphs[i].advance*em;};
    struct Line {std::size_t begin{},end{};double width{};};std::vector<Line> lines;
    for(std::size_t start=0;start<=text.size();){
        auto stop=start;while(stop<text.size() && text[stop]!=u'\n' && text[stop]!=u'\r' && text[stop]!=0x2028)++stop;
        // 段落內貪婪換行：在換行機會處斷開，沒有機會時在字元處斷開。
        std::size_t line_begin=start;double x=0;std::size_t last_break=std::string::npos;double break_x=0;
        for(std::size_t i=start;i<stop;++i){
            if(i>line_begin && (breaks[i]&3U))  {last_break=i;break_x=x;}
            const auto w=advance_of(i,x);
            if(wrap && x+w>available && i>line_begin && !u_isUWhiteSpace(text[i])){
                const auto at=last_break!=std::string::npos && last_break>line_begin?last_break:i;lines.push_back({line_begin,at,at==i?x:break_x});
                line_begin=at;x=0;last_break=std::string::npos;for(auto k=line_begin;k<i;++k)x+=advance_of(k,x);
            }
            x+=w;
        }
        lines.push_back({line_begin,stop,x});
        if(stop>=text.size())break;start=stop+1;if(text[stop]==u'\r' && start<text.size() && text[start]==u'\n')++start;
    }
    // 行寬不含行尾空白（MeasureTrailingSpaces 例外）；LineLimit 只保留完整可見的行，沒有時至少保留部分可見者。
    for(auto& line:lines){if(format.flags&0x800U)continue;auto end=line.end;while(end>line.begin && u_isUWhiteSpace(text[end-1]))--end;double w=0;for(auto k=line.begin;k<end;++k)w+=advance_of(k,w);line.width=w;}
    std::size_t visible=lines.size();
    if(rect[3]>0){const auto limit=(format.flags&0x2000U)?std::floor(rect[3]/height+1e-9):std::ceil(rect[3]/height-1e-9);visible=std::min<std::size_t>(lines.size(),static_cast<std::size_t>(std::max(0.0,limit)));}
    // 修剪：不換行或有未顯示的行時，最後一行依修剪方式截斷；省略號樣式加上「…」。
    std::vector<std::pair<std::size_t,std::size_t>> shown;for(std::size_t i=0;i<visible;++i)shown.push_back({lines[i].begin,lines[i].end});
    bool ellipsis=false;
    if(!shown.empty() && format.trimming && (visible<lines.size() || (!wrap && rect[2]>0 && lines[visible-1].width>available))){
        auto& [b,e]=shown.back();const bool dots=format.trimming>=3;const auto dot_width=dots?map_glyphs(face,u"…",true)[0].advance*em:0.0;double x=0;std::size_t cut=b,word=b;
        for(auto k=b;k<e;++k){const auto w=advance_of(k,x);if(x+w+dot_width>available+1e-9)break;x+=w;cut=k+1;if(breaks[k]&3U)word=k;}
        if(visible<lines.size() || cut<e){e=(format.trimming==2 || format.trimming==4) && word>b?word:cut;ellipsis=dots;}
        double w=0;for(auto k=b;k<e;++k)w+=advance_of(k,w);lines[visible-1].width=w+(ellipsis?dot_width:0);
    }
    const auto block=static_cast<double>(visible)*height;double box_left=INFINITY,box_right=-INFINITY;
    const bool rtl=format.flags&1U;const auto align=rtl && format.align!=1?2-format.align:format.align;
    const auto top=format.line_align==0 || rect[3]<=0?(format.line_align==0?rect[1]:format.line_align==1?rect[1]-block/2:rect[1]-block):format.line_align==1?rect[1]+(rect[3]-block)/2:rect[1]+rect[3]-block;
    for(std::size_t i=0;i<visible;++i){
        const auto [b,e]=shown[i];const auto w=lines[i].width;
        const auto x0=rect[2]>0?(align==0?rect[0]+padding:align==1?rect[0]+(rect[2]-w)/2:rect[0]+rect[2]-padding-w):(align==0?rect[0]+padding:align==1?rect[0]-w/2:rect[0]-padding-w);
        const auto y=top+static_cast<double>(i)*height;double x=x0;box_left=std::min(box_left,x0);box_right=std::max(box_right,x0+w);
        for(auto k=b;k<e;++k){
            const auto w_k=advance_of(k,x-x0);
            if(text[k]>=0x20 && !(text[k]>=0xdc00 && text[k]<=0xdfff) && glyphs[k].glyph)result.glyphs.push_back({glyphs[k].face,glyphs[k].glyph,x,y+ascent,em/static_cast<double>(glyphs[k].face.em)});
            if(std::find(hotkeys.begin(),hotkeys.end(),k)!=hotkeys.end())result.decorations.push_back({x,y+ascent+em*0.08,w_k,std::max(em/14,0.5)});
            result.cells[origin[k]]={x,y,w_k,height};x+=w_k;result.fitted=static_cast<std::int32_t>(origin[k]+1);
        }
        if(ellipsis && i+1==visible){const auto dot=map_glyphs(face,u"…",true)[0];if(dot.glyph)result.glyphs.push_back({dot.face,dot.glyph,x,y+ascent,em/static_cast<double>(dot.face.em)});}
        if(style&4U)result.decorations.push_back({x0,y+ascent+em*0.1,w,std::max(em/14,0.5)});
        if(style&8U)result.decorations.push_back({x0,y+ascent-em*0.3,w,std::max(em/14,0.5)});
    }
    if(visible==lines.size() && !lines.empty())result.fitted=static_cast<std::int32_t>(source.size());
    result.lines=static_cast<std::int32_t>(visible);
    if(visible)result.box={box_left-padding,top,box_right-box_left+2*padding,block};else result.box={rect[0],rect[1],0,0};return result;
};
// 排版結果轉成世界座標路徑（字形輪廓與裝飾線）。
const auto layout_path=[&](const TextLayout& layout,GdiplusState::Path& path){
    for(const auto& item:layout.glyphs)for(auto p:glyph_outline(item.face,item.glyph)){p.x=item.x+p.x*item.scale;p.y=item.y-p.y*item.scale;path.points.push_back(p);}
    for(const auto& r:layout.decorations)add_rectangle(path,r[0],r[1],r[2],r[3]);path.new_figure=true;};
const auto read_text=[&](std::uint64_t at,std::int32_t length)->std::u16string{
    if(!at)throw std::invalid_argument("GDI+ 文字為 NULL");if(length<0)return read_utf16(memory,GuestAddress{at});
    std::u16string out(static_cast<std::size_t>(length),u'\0');for(std::size_t i=0;i<out.size();++i)out[i]=static_cast<char16_t>(memory.load(GuestAddress{at+i*2},16));return out;};
const GdiplusState::StringFormat default_format{};
const auto format_or_default=[&](std::uint64_t handle)->const GdiplusState::StringFormat&{return handle?format_of(handle):default_format;};
// 文字填滿：TextRenderingHint 0、3、4、5 以 4 倍超取樣反鋸齒；版面矩形有大小且未設 NoClip 時裁到矩形。
const auto fill_text=[&](std::uint64_t graphics,std::uint64_t brush,GdiplusState::Path path,const std::array<double,4>& rect,bool clip)->std::uint32_t{
    path.fill=1;const auto hint=graphics_of(graphics).text_hint;const unsigned scale=hint==1 || hint==2?1U:4U;
    if(!clip || !(rect[2]>0) || !(rect[3]>0))return render(graphics,true,brush,std::move(path),scale);
    auto& state_info=graphics_of(graphics);const auto saved=state_info.clip;combine_clip(state_info.clip,device_region(graphics,[&]{GdiplusState::Path box;add_rectangle(box,rect[0],rect[1],rect[2],rect[3]);return box;}()),1);
    const auto status=render(graphics,true,brush,std::move(path),scale);graphics_of(graphics).clip=saved;return status;};
// ---- 字型集合 ----
if(symbol=="GdipNewInstalledFontCollection"){
    if(!arg(0))return finish(invalid_parameter);
    if(!g.installed_collection){const auto handle=new_handle();if(!handle)return finish(out_of_memory);GdiplusState::FontCollection collection;collection.installed=true;g.collections[handle]=std::move(collection);g.installed_collection=handle;}
    memory.store(GuestAddress{arg(0)},pointer_bits,g.installed_collection);return finish(ok);
}
if(symbol=="GdipNewPrivateFontCollection"){const auto handle=new_object(arg(0));if(!handle)return finish(out_of_memory);g.collections[handle]={};memory.store(GuestAddress{arg(0)},pointer_bits,handle);return finish(ok);}
if(symbol=="GdipDeletePrivateFontCollection"){
    if(!arg(0))return finish(invalid_parameter);const auto handle=memory.load(GuestAddress{arg(0)},pointer_bits);const auto found=g.collections.find(handle);
    if(found==g.collections.end() || found->second.installed)return finish(invalid_parameter);g.collections.erase(found);release_handle(handle);memory.store(GuestAddress{arg(0)},pointer_bits,0);return finish(ok);
}
if(symbol=="GdipPrivateAddFontFile" || symbol=="GdipPrivateAddMemoryFont"){
    auto& collection=collection_of(arg(0));if(collection.installed)return finish(invalid_parameter);std::vector<std::uint8_t> bytes;
    if(symbol=="GdipPrivateAddMemoryFont"){if(!arg(1) || s32(2)<=0 || static_cast<std::size_t>(s32(2))>native_font_file_limit)return finish(invalid_parameter);bytes.resize(u32(2));memory.read(GuestAddress{arg(1)},bytes);}
    else {if(!arg(1))return finish(invalid_parameter);const auto path=read_utf16(memory,GuestAddress{arg(1)});
        const auto opened=filesystem_.open_reader(utf16_to_utf8(normalized_windows_path(path,runtime_.current_directory)));if(opened.error)return finish(opened.error==2 || opened.error==3?10:opened.error==5?12:7);
        const auto file_info=opened.file->information();if(file_info.error || file_info.metadata.size>native_font_file_limit)return finish(7);bytes.resize(file_info.metadata.size);
        const auto io=opened.file->read(bytes,0);if(io.error || io.bytes!=bytes.size())return finish(7);}
    std::vector<NativeFontFace> faces;try{faces=native_load_font_faces(bytes);}catch(const std::exception&){return finish(generic_error);}if(faces.empty())return finish(generic_error);
    for(auto& face:faces){auto found=std::find_if(collection.families.begin(),collection.families.end(),[&](const auto& f){return f.name==face.family;});
        if(found==collection.families.end())collection.families.push_back({face.family,{face}});else found->faces.push_back(face);}
    return finish(ok);
}
if(symbol=="GdipGetFontCollectionFamilyCount" || symbol=="GdipGetFontCollectionFamilyList"){
    auto& collection=collection_of(arg(0));
    if(collection.installed && collection.families.empty()){
        std::vector<std::u16string> names;for(const auto& face:native_font_faces(u""))if(!face.family.empty())names.push_back(face.family);
        std::sort(names.begin(),names.end());names.erase(std::unique(names.begin(),names.end()),names.end());for(auto& name:names)if(auto family=installed_family(name))collection.families.push_back(std::move(*family));
    }
    if(symbol=="GdipGetFontCollectionFamilyCount"){if(!arg(1))return finish(invalid_parameter);memory.store(GuestAddress{arg(1)},32,collection.families.size());return finish(ok);}
    // (collection, numSought, families[], numFound)：回傳的家族由呼叫端以 GdipDeleteFontFamily 釋放。
    if(!arg(2) || !arg(3) || s32(1)<0)return finish(invalid_parameter);const auto count=std::min<std::size_t>(static_cast<std::size_t>(s32(1)),collection.families.size());
    for(std::size_t i=0;i<count;++i){const auto handle=new_handle();if(!handle)return finish(out_of_memory);g.families[handle]=collection.families[i];memory.store(GuestAddress{arg(2)+i*(pointer_bits/8)},pointer_bits,handle);}
    memory.store(GuestAddress{arg(3)},32,count);return finish(ok);
}
// ---- 字型家族 ----
if(symbol=="GdipCreateFontFamilyFromName"){
    if(!arg(0) || !arg(2))return finish(invalid_parameter);auto family=find_family(read_utf16(memory,GuestAddress{arg(0)}),arg(1));if(!family)return finish(family_not_found);
    const auto handle=new_object(arg(2));if(!handle)return finish(out_of_memory);g.families[handle]=std::move(*family);memory.store(GuestAddress{arg(2)},pointer_bits,handle);return finish(ok);
}
if(symbol=="GdipGetGenericFontFamilySansSerif" || symbol=="GdipGetGenericFontFamilySerif" || symbol=="GdipGetGenericFontFamilyMonospace"){
    const auto name=symbol.ends_with("SansSerif")?u"Microsoft Sans Serif":symbol.ends_with("Serif")?u"Times New Roman":u"Courier New";
    auto family=installed_family(name);if(!family)family=installed_family(native_ui_font_family());if(!family)return finish(family_not_found);
    const auto handle=new_object(arg(0));if(!handle)return finish(out_of_memory);g.families[handle]=std::move(*family);memory.store(GuestAddress{arg(0)},pointer_bits,handle);return finish(ok);
}
if(symbol=="GdipCloneFontFamily"){const auto copy=family_of(arg(0));const auto handle=new_object(arg(1));if(!handle)return finish(out_of_memory);g.families[handle]=copy;memory.store(GuestAddress{arg(1)},pointer_bits,handle);return finish(ok);}
if(symbol=="GdipDeleteFontFamily"){if(!g.families.erase(arg(0)))return finish(invalid_parameter);release_handle(arg(0));return finish(ok);}
if(symbol=="GdipGetFamilyName"){
    // name 為 LF_FACESIZE（32）個 WCHAR。
    const auto& family=family_of(arg(0));if(!arg(1))return finish(invalid_parameter);auto name=family.name.substr(0,31);name.push_back(0);
    for(std::size_t i=0;i<name.size();++i)memory.store(GuestAddress{arg(1)+i*2},16,name[i]);return finish(ok);
}
if(symbol=="GdipIsStyleAvailable"){const auto& family=family_of(arg(0));if(!arg(2))return finish(invalid_parameter);memory.store(GuestAddress{arg(2)},32,style_available(family,u32(1))?1U:0U);return finish(ok);}
if(symbol=="GdipGetEmHeight" || symbol=="GdipGetCellAscent" || symbol=="GdipGetCellDescent" || symbol=="GdipGetLineSpacing"){
    // (family, style, UINT16*)：設計單位。
    const auto& family=family_of(arg(0));if(!arg(2))return finish(invalid_parameter);const auto face=face_for(family,u32(1));
    const auto value=symbol=="GdipGetEmHeight"?face.em:symbol=="GdipGetCellAscent"?face.ascent:symbol=="GdipGetCellDescent"?face.descent:static_cast<std::int32_t>(line_spacing(face));
    memory.store(GuestAddress{arg(2)},16,static_cast<std::uint16_t>(std::clamp(value,0,65535)));return finish(ok);
}
// ---- 字型 ----
const auto store_font=[&](GdiplusState::Font font,std::uint64_t output)->std::uint32_t{const auto handle=new_object(output);if(!handle)return out_of_memory;g.fonts[handle]=std::move(font);memory.store(GuestAddress{output},pointer_bits,handle);return ok;};
// LOGFONT：高度為負時為 em 像素，為正時為字元格高度；粗細 600 以上視為粗體。
const auto font_from_logical=[&](const GuiFont& logical,std::uint64_t output)->std::uint32_t{
    const auto face_name=logical.face.empty()?native_ui_font_family():logical.face;auto family=installed_family(face_name);if(!family)return not_truetype;
    const auto face=face_for(*family,0);auto size=static_cast<double>(std::abs(logical.height));
    if(logical.height>0 && face.ascent+face.descent>0)size=size*face.em/(face.ascent+face.descent);if(!logical.height)size=13;
    GdiplusState::Font font{std::move(*family),size,(logical.weight>=600?1U:0U)|(logical.italic?2U:0U)|(logical.underline?4U:0U)|(logical.strikeout?8U:0U),0};return store_font(std::move(font),output);};
if(symbol=="GdipCreateFont"){
    // (family, emSize, style, unit, font**)
    const auto& family=family_of(arg(0));const auto size=real(1);if(!arg(4) || !(size>0) || u32(3)==1 || u32(3)>6 || u32(2)>15)return finish(invalid_parameter);
    return finish(store_font({family,size,u32(2),u32(3)},arg(4)));
}
if(symbol=="GdipCreateFontFromLogfontW" || symbol=="GdipCreateFontFromLogfontA"){
    if(!arg(1) || !arg(2))return finish(invalid_parameter);const bool wide=symbol.ends_with('W');std::vector<std::uint8_t> bytes(wide?92:60);memory.read(GuestAddress{arg(1)},bytes);
    // LOGFONT：高度 0、寬度 4、粗細 16、斜體 20、底線 21、刪除線 22、字型名稱 28（W 為 32 個 WCHAR，A 為 32 個字元）。
    GuiFont logical;const auto integer=[&](std::size_t at){std::uint32_t v=0;for(unsigned i=0;i<4;++i)v|=std::uint32_t{bytes[at+i]}<<(8*i);return std::bit_cast<std::int32_t>(v);};
    logical.height=integer(0);logical.width=integer(4);logical.weight=integer(16);logical.italic=bytes[20]!=0;logical.underline=bytes[21]!=0;logical.strikeout=bytes[22]!=0;
    if(wide){for(std::size_t i=28;i+1<92;i+=2){const auto c=static_cast<char16_t>(bytes[i]|(bytes[i+1]<<8));if(!c)break;logical.face.push_back(c);}}
    else {std::string name;for(std::size_t i=28;i<60 && bytes[i];++i)name.push_back(static_cast<char>(bytes[i]));logical.face=utf8_to_utf16_lossy(name);}
    return finish(font_from_logical(logical,arg(2)));
}
if(symbol=="GdipCreateFontFromDC"){const auto dc=gui_.contexts.find(arg(0));if(dc==gui_.contexts.end() || !arg(1))return finish(invalid_parameter);return finish(font_from_logical(gui_font(gui_,dc->second.font),arg(1)));}
if(symbol=="GdipCloneFont"){const auto copy=font_of(arg(0));return finish(store_font(copy,arg(1)));}
if(symbol=="GdipDeleteFont"){if(!g.fonts.erase(arg(0)))return finish(invalid_parameter);release_handle(arg(0));return finish(ok);}
if(symbol=="GdipGetFamily"){const auto& font=font_of(arg(0));const auto handle=new_object(arg(1));if(!handle)return finish(out_of_memory);g.families[handle]=font.family;memory.store(GuestAddress{arg(1)},pointer_bits,handle);return finish(ok);}
if(symbol=="GdipGetFontSize"){const auto& font=font_of(arg(0));if(!arg(1))return finish(invalid_parameter);store_real(arg(1),font.size);return finish(ok);}
if(symbol=="GdipGetFontStyle" || symbol=="GdipGetFontUnit"){const auto& font=font_of(arg(0));if(!arg(1))return finish(invalid_parameter);memory.store(GuestAddress{arg(1)},32,symbol=="GdipGetFontStyle"?font.style:font.unit);return finish(ok);}
if(symbol=="GdipGetFontHeight" || symbol=="GdipGetFontHeightGivenDPI"){
    // GetFontHeight：Graphics 的世界單位（NULL 時為 96 DPI 像素）；GivenDPI：指定 DPI 的像素。
    const auto& font=font_of(arg(0));if(!arg(2))return finish(invalid_parameter);const auto face=face_for(font.family,font.style);
    double em=0;if(symbol=="GdipGetFontHeight")em=arg(1)?em_world(font,arg(1)):font.size*unit_pixels(font.unit);
    else {const auto dpi=real(1);if(!(dpi>0))return finish(invalid_parameter);em=font.unit<=2?font.size:font.size*unit_pixels(font.unit)*dpi/96;}
    store_real(arg(2),line_spacing(face)*em/static_cast<double>(face.em));return finish(ok);
}
if(symbol=="GdipGetLogFontW" || symbol=="GdipGetLogFontA"){
    const auto& font=font_of(arg(0));if(!arg(2))return finish(invalid_parameter);const bool wide=symbol.ends_with('W');
    const auto pixels=em_world(font,arg(1))*(arg(1)?page_factor(graphics_of(arg(1))):1.0);std::vector<std::uint8_t> bytes(wide?92:60);
    const auto put=[&](std::size_t at,std::uint32_t v){for(unsigned i=0;i<4;++i)bytes[at+i]=static_cast<std::uint8_t>(v>>(8*i));};
    put(0,static_cast<std::uint32_t>(-static_cast<std::int32_t>(std::lround(pixels))));put(16,(font.style&1U)?700U:400U);
    bytes[20]=(font.style&2U)?1:0;bytes[21]=(font.style&4U)?1:0;bytes[22]=(font.style&8U)?1:0;bytes[23]=1;
    if(wide){const auto name=font.family.name.substr(0,31);for(std::size_t i=0;i<name.size();++i){bytes[28+i*2]=static_cast<std::uint8_t>(name[i]);bytes[29+i*2]=static_cast<std::uint8_t>(name[i]>>8);}}
    else {auto name=utf16_to_utf8_lossy(font.family.name);name.resize(std::min<std::size_t>(name.size(),31));std::copy(name.begin(),name.end(),bytes.begin()+28);}
    memory.write(GuestAddress{arg(2)},bytes);return finish(ok);
}
// ---- StringFormat ----
const auto store_format=[&](GdiplusState::StringFormat format,std::uint64_t output)->std::uint32_t{const auto handle=new_object(output);if(!handle)return out_of_memory;g.formats[handle]=std::move(format);memory.store(GuestAddress{output},pointer_bits,handle);return ok;};
if(symbol=="GdipCreateStringFormat"){GdiplusState::StringFormat format;format.flags=u32(0);format.language=static_cast<std::uint16_t>(u32(1));return finish(store_format(std::move(format),arg(2)));}
if(symbol=="GdipStringFormatGetGenericDefault" || symbol=="GdipStringFormatGetGenericTypographic"){
    // 共用的靜態物件；Typographic 為 NoClip｜FitBlackBox｜LineLimit、不修剪且不加左右留白。
    const bool typographic=symbol.ends_with("Typographic");auto& handle=typographic?g.generic_typographic:g.generic_default;if(!arg(0))return finish(invalid_parameter);
    if(!handle){handle=new_handle();if(!handle)return finish(out_of_memory);GdiplusState::StringFormat format;if(typographic){format.flags=0x6004U;format.trimming=0;format.typographic=true;}g.formats[handle]=format;}
    memory.store(GuestAddress{arg(0)},pointer_bits,handle);return finish(ok);
}
if(symbol=="GdipCloneStringFormat"){auto copy=format_of(arg(0));return finish(store_format(std::move(copy),arg(1)));}
if(symbol=="GdipDeleteStringFormat"){
    format_of(arg(0));if(arg(0)==g.generic_default || arg(0)==g.generic_typographic)return finish(ok);g.formats.erase(arg(0));release_handle(arg(0));return finish(ok);}
{
    struct Field{std::string_view name;std::uint32_t maximum;unsigned GdiplusState::StringFormat::* field;};
    static constexpr Field fields[]{{"Align",2,&GdiplusState::StringFormat::align},{"LineAlign",2,&GdiplusState::StringFormat::line_align},{"Trimming",5,&GdiplusState::StringFormat::trimming},{"HotkeyPrefix",2,&GdiplusState::StringFormat::hotkey}};
    for(const auto& field:fields){
        if(symbol.size()<=19 || symbol.substr(19)!=field.name || !(symbol.starts_with("GdipSetStringFormat") || symbol.starts_with("GdipGetStringFormat")))continue;
        auto& format=format_of(arg(0));
        if(symbol.starts_with("GdipGet")){if(!arg(1))return finish(invalid_parameter);memory.store(GuestAddress{arg(1)},32,format.*field.field);return finish(ok);}
        if(u32(1)>field.maximum)return finish(invalid_parameter);format.*field.field=u32(1);return finish(ok);
    }
}
if(symbol=="GdipSetStringFormatFlags"){format_of(arg(0)).flags=u32(1);return finish(ok);}
if(symbol=="GdipGetStringFormatFlags"){const auto& format=format_of(arg(0));if(!arg(1))return finish(invalid_parameter);memory.store(GuestAddress{arg(1)},32,format.flags);return finish(ok);}
if(symbol=="GdipSetStringFormatTabStops"){
    // (format, firstTabOffset, count, tabStops*)
    auto& format=format_of(arg(0));const auto count=s32(2);if(count<0 || count>4096 || (count && !arg(3)))return finish(invalid_parameter);
    format.tab_first=real(1);format.tabs=count?real_array(arg(3),static_cast<std::size_t>(count)):std::vector<double>{};return finish(ok);
}
if(symbol=="GdipGetStringFormatTabStopCount"){const auto& format=format_of(arg(0));if(!arg(1))return finish(invalid_parameter);memory.store(GuestAddress{arg(1)},32,format.tabs.size());return finish(ok);}
if(symbol=="GdipGetStringFormatTabStops"){
    const auto& format=format_of(arg(0));if(!arg(2) || !arg(3) || s32(1)<static_cast<std::int32_t>(format.tabs.size()))return finish(invalid_parameter);
    store_real(arg(2),format.tab_first);for(std::size_t i=0;i<format.tabs.size();++i)store_real(arg(3)+i*4,format.tabs[i]);return finish(ok);
}
if(symbol=="GdipSetStringFormatDigitSubstitution"){auto& format=format_of(arg(0));if(u32(2)>3)return finish(invalid_parameter);format.digit_language=static_cast<std::uint16_t>(u32(1));format.digit_method=u32(2);return finish(ok);}
if(symbol=="GdipGetStringFormatDigitSubstitution"){
    const auto& format=format_of(arg(0));if(arg(1))memory.store(GuestAddress{arg(1)},16,format.digit_language);if(arg(2))memory.store(GuestAddress{arg(2)},32,format.digit_method);return finish(ok);}
if(symbol=="GdipSetStringFormatMeasurableCharacterRanges"){
    // CharacterRange 為（First, Length）；最多 32 個。
    auto& format=format_of(arg(0));const auto count=s32(1);if(count<0 || count>32 || (count && !arg(2)))return finish(invalid_parameter);
    format.ranges.clear();for(std::int32_t i=0;i<count;++i){const auto at=arg(2)+static_cast<std::uint64_t>(i)*8;format.ranges.push_back({static_cast<std::int32_t>(memory.load(GuestAddress{at},32)),static_cast<std::int32_t>(memory.load(GuestAddress{at+4},32))});}return finish(ok);
}
if(symbol=="GdipGetStringFormatMeasurableCharacterRangeCount"){const auto& format=format_of(arg(0));if(!arg(1))return finish(invalid_parameter);memory.store(GuestAddress{arg(1)},32,format.ranges.size());return finish(ok);}
// ---- 繪製與量測 ----
if(symbol=="GdipDrawString"){
    // (graphics, string, length, font, layoutRect*, format, brush)
    graphics_of(arg(0));const auto& font=font_of(arg(3));if(!arg(4) || !g.brushes.contains(arg(6)))return finish(invalid_parameter);
    const auto& format=format_or_default(arg(5));const auto rect=rect_array(arg(4),1,false)[0];
    const auto layout=layout_text(read_text(arg(1),s32(2)),font.family,font.style,em_world(font,arg(0)),rect,format);GdiplusState::Path path;layout_path(layout,path);
    return finish(fill_text(arg(0),arg(6),std::move(path),rect,!(format.flags&0x4000U)));
}
if(symbol=="GdipMeasureString"){
    // (graphics, string, length, font, layoutRect*, format, boundingBox*, codepointsFitted*, linesFilled*)
    graphics_of(arg(0));const auto& font=font_of(arg(3));if(!arg(4) || !arg(6))return finish(invalid_parameter);
    const auto layout=layout_text(read_text(arg(1),s32(2)),font.family,font.style,em_world(font,arg(0)),rect_array(arg(4),1,false)[0],format_or_default(arg(5)));
    store_rect(arg(6),layout.box[0],layout.box[1],layout.box[2],layout.box[3],false);if(arg(7))memory.store(GuestAddress{arg(7)},32,static_cast<std::uint32_t>(layout.fitted));
    if(arg(8))memory.store(GuestAddress{arg(8)},32,static_cast<std::uint32_t>(layout.lines));return finish(ok);
}
if(symbol=="GdipMeasureCharacterRanges"){
    // (graphics, string, length, font, layoutRect*, format, regionCount, regions[])：每個範圍的字元格聯集寫入呼叫端提供的 Region。
    graphics_of(arg(0));const auto& font=font_of(arg(3));const auto& format=format_of(arg(5));if(!arg(4) || !arg(7) || s32(6)!=static_cast<std::int32_t>(format.ranges.size()))return finish(invalid_parameter);
    const auto text=read_text(arg(1),s32(2));const auto layout=layout_text(text,font.family,font.style,em_world(font,arg(0)),rect_array(arg(4),1,false)[0],format);
    for(std::size_t r=0;r<format.ranges.size();++r){
        auto& target=region_of(memory.load(GuestAddress{arg(7)+r*(pointer_bits/8)},pointer_bits));const auto [first,length]=format.ranges[r];
        if(first<0 || length<0 || static_cast<std::size_t>(first)+static_cast<std::size_t>(length)>text.size())return finish(invalid_parameter);
        std::vector<RegionPtr> leaves;for(auto i=static_cast<std::size_t>(first);i<static_cast<std::size_t>(first+length);++i){const auto& c=layout.cells[i];if(c[2]>0)leaves.push_back(region_box(c[0],c[1],c[2],c[3]));}
        target=union_tree(union_tree,leaves,0,leaves.size());
    }
    return finish(ok);
}
if(symbol=="GdipDrawDriverString" || symbol=="GdipMeasureDriverString"){
    // Draw(graphics, text, length, font, brush, positions, flags, matrix)；Measure(graphics, text, length, font, positions, flags, matrix, box)。
    // flags：CmapLookup 1（text 為字元）、Vertical 2、RealizedAdvance 4（只用第一個位置，其餘依字寬）、LimitSubpixel 8。
    const bool draw=symbol=="GdipDrawDriverString";graphics_of(arg(0));const auto& font=font_of(arg(3));const unsigned at=draw?5U:4U;
    const auto flags=u32(at+1);const auto matrix=arg(at+2);if(!arg(1) || !arg(at) || (draw && !g.brushes.contains(arg(4))) || (!draw && !arg(7)))return finish(invalid_parameter);
    if(flags&2U)return finish(not_implemented);
    auto text=read_text(arg(1),s32(2));const auto face=face_for(font.family,font.style);const auto em=em_world(font,arg(0));const auto scale=em/static_cast<double>(face.em);
    std::vector<std::uint16_t> glyphs;if(flags&1U){for(const auto& item:map_glyphs(face,text,false))glyphs.push_back(item.glyph);}else glyphs.assign(text.begin(),text.end());
    const auto metrics=native_font_glyph_metrics(face,glyphs);const auto positions=point_array(arg(at),(flags&4U)?1:static_cast<std::int64_t>(glyphs.size()),false);
    GdiplusState::Path path;double l=INFINITY,t=INFINITY,r=-INFINITY,b=-INFINITY;double pen_x=positions[0][0];
    const auto transform=matrix?std::optional<GuiTransform>{matrix_of(matrix)}:std::nullopt;
    for(std::size_t i=0;i<glyphs.size();++i){
        const auto origin=(flags&4U)?std::array<double,2>{pen_x,positions[0][1]}:positions[i];pen_x=origin[0]+metrics[i][1]*scale;
        l=std::min(l,origin[0]);r=std::max(r,origin[0]+metrics[i][1]*scale);t=std::min(t,origin[1]-face.ascent*scale);b=std::max(b,origin[1]+face.descent*scale);
        if(!draw || !glyphs[i])continue;
        for(auto p:glyph_outline(face,glyphs[i])){double x=p.x*scale,y=-p.y*scale;if(transform){const auto q=gui_point(*transform,x,y);x=q[0];y=q[1];}p.x=origin[0]+x;p.y=origin[1]+y;path.points.push_back(p);}
    }
    if(!draw){if(glyphs.empty())store_rect(arg(7),0,0,0,0,false);else store_rect(arg(7),l,t,r-l,b-t,false);return finish(ok);}
    return finish(fill_text(arg(0),arg(4),std::move(path),{0,0,0,0},false));
}
if(symbol=="GdipAddPathString" || symbol=="GdipAddPathStringI"){
    // (path, string, length, family, style, emSize, layoutRect*, format)：emSize 為路徑座標單位。
    auto& path=path_of(arg(0));const auto& family=family_of(arg(3));const auto em=real(5);if(!arg(6) || !(em>0))return finish(invalid_parameter);
    const auto layout=layout_text(read_text(arg(1),s32(2)),family,u32(4),em,rect_array(arg(6),1,symbol.ends_with('I'))[0],format_or_default(arg(7)));
    GdiplusState::Path added;layout_path(layout,added);path.points.insert(path.points.end(),added.points.begin(),added.points.end());path.new_figure=true;return finish(ok);
}
