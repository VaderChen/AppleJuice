// 將沒有標準 OpenType 輪廓表的原生字型實例輸出為 CID-keyed CFF。
// glyph ID、cmap 與排版表保持一致；CoreText 的二次曲線精確轉成三次曲線。
using FontTables=std::vector<std::pair<std::uint32_t,std::vector<std::uint8_t>>>;
void font_be(std::vector<std::uint8_t>& bytes,std::uint32_t value,unsigned count){for(unsigned i=count;i;--i)bytes.push_back(static_cast<std::uint8_t>(value>>(8*(i-1))));}
void cff_integer(std::vector<std::uint8_t>& bytes,std::int32_t value){
    if(value>=-107 && value<=107)bytes.push_back(static_cast<std::uint8_t>(value+139));
    else if(value>=108 && value<=1131){const auto n=value-108;bytes.push_back(static_cast<std::uint8_t>(247+n/256));bytes.push_back(static_cast<std::uint8_t>(n));}
    else if(value<=-108 && value>=-1131){const auto n=-value-108;bytes.push_back(static_cast<std::uint8_t>(251+n/256));bytes.push_back(static_cast<std::uint8_t>(n));}
    else if(value>=INT16_MIN && value<=INT16_MAX){bytes.push_back(28);font_be(bytes,static_cast<std::uint32_t>(value),2);}
    else throw std::invalid_argument("CFF charstring 整數超出範圍");
}
void cff_number(std::vector<std::uint8_t>& bytes,double value){
    if(!std::isfinite(value) || value<INT16_MIN || value>INT16_MAX)throw std::invalid_argument("CFF charstring 座標超出範圍");
    const auto whole=std::round(value);if(std::abs(value-whole)<1.0/65536){cff_integer(bytes,static_cast<std::int32_t>(whole));return;}
    bytes.push_back(255);font_be(bytes,static_cast<std::uint32_t>(static_cast<std::int32_t>(std::round(value*65536))),4);
}
std::vector<std::uint8_t> cff_index(const std::vector<std::vector<std::uint8_t>>& objects){
    if(objects.size()>65535)throw std::invalid_argument("CFF INDEX 項目過多");std::vector<std::uint8_t> out;font_be(out,static_cast<std::uint32_t>(objects.size()),2);if(objects.empty())return out;
    std::uint64_t total=1;for(const auto& object:objects)total+=object.size();if(total>UINT32_MAX)throw std::bad_alloc{};
    const unsigned width=total<=255?1:total<=65535?2:total<=16777215?3:4;out.push_back(static_cast<std::uint8_t>(width));std::uint32_t offset=1;
    out.reserve(3+(objects.size()+1)*width+static_cast<std::size_t>(total-1));
    for(const auto& object:objects){font_be(out,offset,width);offset+=static_cast<std::uint32_t>(object.size());}font_be(out,offset,width);
    for(const auto& object:objects)out.insert(out.end(),object.begin(),object.end());return out;
}
void export_native_outlines(const NativeFontFace& face,CTFontRef font,FontTables& tables){
    const auto glyph_count=CTFontGetGlyphCount(font);if(glyph_count<=0 || glyph_count>65535)throw std::invalid_argument("原生字型 glyph count 不合法");
    std::vector<std::uint16_t> glyphs(static_cast<std::size_t>(glyph_count));for(std::size_t i=0;i<glyphs.size();++i)glyphs[i]=static_cast<std::uint16_t>(i);
    const auto started=std::chrono::steady_clock::now();
    const auto metrics=native_font_glyph_metrics(face,glyphs);
    const auto measured=std::chrono::steady_clock::now();
    std::vector<std::vector<std::uint8_t>> programs(glyphs.size());
    struct GlyphBounds {std::array<std::int32_t,4> value{};bool present=false;};
    std::vector<GlyphBounds> glyph_bounds(glyphs.size());
    // CTFont 不可變；每個工作只寫自己的 glyph，彙整固定依 glyph ID 進行。
    // 大型字型最多兩個工作，降低共用 CoreText 資源的競爭；不占滿主機核心。
    const auto processors=static_cast<std::size_t>([[NSProcessInfo processInfo] activeProcessorCount]);
    auto workers=font_export_detail::acceleration_enabled() && glyphs.size()>=1024?std::min<std::size_t>(2,processors>1?processors-1:1):1;
    // 診斷／效能比對可降低並行度，但不可超出主機與預設上限。
    if(const auto* value=std::getenv("X64ARM_FONT_EXPORT_WORKERS")){
        const std::string_view input(value);std::size_t requested=0;
        const auto parsed=std::from_chars(input.data(),input.data()+input.size(),requested);
        if(parsed.ec==std::errc{} && parsed.ptr==input.data()+input.size() && requested)workers=std::min(workers,requested);
    }
    std::vector<std::exception_ptr> errors(workers);
    const auto encode=[&](std::uint16_t glyph){
        const auto outline=native_gdi_glyph(face,glyph,{1,0,0,1},0);std::vector<std::uint8_t> program;cff_number(program,metrics[glyph][1]);std::array<double,2> current{};
        for(const auto& contour:outline.contours){
            cff_number(program,contour.start[0]-current[0]);cff_number(program,contour.start[1]-current[1]);program.push_back(21);current=contour.start;
            for(const auto& segment:contour.segments){
                if(segment.kind==1){for(unsigned axis=0;axis<2;++axis)cff_number(program,segment.points[0][axis]-current[axis]);program.push_back(5);current=segment.points[0];continue;}
                auto points=segment.points;
                if(segment.kind==2)for(unsigned axis=0;axis<2;++axis){points[2][axis]=segment.points[1][axis];points[0][axis]=current[axis]+(segment.points[0][axis]-current[axis])*2/3;points[1][axis]=points[2][axis]+(segment.points[0][axis]-points[2][axis])*2/3;}
                for(unsigned i=0;i<3;++i){for(unsigned axis=0;axis<2;++axis)cff_number(program,points[i][axis]-current[axis]);current=points[i];}program.push_back(8);
            }
        }
        program.push_back(14);programs[glyph]=std::move(program);
        glyph_bounds[glyph]={outline.bounds,!outline.contours.empty()};
    };
    const auto work=[&](std::size_t worker){
        @autoreleasepool {
            try {
                constexpr std::size_t batch=128;
                for(std::size_t first=worker*batch;first<glyphs.size();first+=workers*batch)
                    for(std::size_t i=first;i<std::min(first+batch,glyphs.size());++i)encode(glyphs[i]);
            }catch(...){errors[worker]=std::current_exception();}
        }
    };
    if(workers==1)work(0);
    else dispatch_apply_f(workers,dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),const_cast<void*>(static_cast<const void*>(&work)),[](void* context,std::size_t worker){
        (*static_cast<const decltype(work)*>(context))(worker);
    });
    // 不讓 C++ 例外跨越 libdispatch 的 C callback 邊界。
    for(const auto& error:errors)if(error)std::rethrow_exception(error);
    const auto encoded=std::chrono::steady_clock::now();
    if(const auto* trace=std::getenv("X64ARM_FONT_CACHE_TRACE");trace && std::string_view(trace)=="1")
        std::fprintf(stderr,"font_outline glyphs=%zu workers=%zu metrics_ms=%.3f encode_ms=%.3f\n",glyphs.size(),workers,
            std::chrono::duration<double,std::milli>(measured-started).count(),std::chrono::duration<double,std::milli>(encoded-measured).count());
    std::vector<std::uint8_t> horizontal,vertical;horizontal.reserve(glyphs.size()*4);vertical.reserve(glyphs.size()*4);
    std::array<std::int32_t,4> bounds{INT16_MAX,INT16_MAX,INT16_MIN,INT16_MIN};bool has_bounds=false;
    for(const auto glyph:glyphs){
        const auto& box=glyph_bounds[glyph];
        if(box.present){has_bounds=true;for(unsigned axis=0;axis<2;++axis){bounds[axis]=std::min(bounds[axis],box.value[axis]);bounds[axis+2]=std::max(bounds[axis+2],box.value[axis+2]);}}
        const auto& m=metrics[glyph];if(m[1]<0 || m[1]>UINT16_MAX || m[4]<0 || m[4]>UINT16_MAX || m[0]<INT16_MIN || m[0]>INT16_MAX || m[3]<INT16_MIN || m[3]>INT16_MAX)throw std::invalid_argument("原生字型 advance／bearing 超出範圍");
        font_be(horizontal,static_cast<std::uint32_t>(m[1]),2);font_be(horizontal,static_cast<std::uint32_t>(m[0]),2);font_be(vertical,static_cast<std::uint32_t>(m[4]),2);font_be(vertical,static_cast<std::uint32_t>(m[3]),2);
    }
    if(!has_bounds)bounds={};
    std::vector<std::uint8_t> postscript;for(const auto ch:face.postscript_name){if(ch<33 || ch>126)throw std::invalid_argument("原生字型 PostScript 名稱不合法");postscript.push_back(static_cast<std::uint8_t>(ch));}if(postscript.empty())throw std::invalid_argument("原生字型缺少 PostScript 名稱");
    const auto charstrings=cff_index(programs);const auto names=cff_index({postscript}),strings=cff_index({{'A','d','o','b','e'},{'I','d','e','n','t','i','t','y'}}),globals=cff_index({});
    std::vector<std::uint8_t> charset{0};for(std::uint32_t i=1;i<glyphs.size();++i)font_be(charset,i,2);
    std::vector<std::uint8_t> fdselect{3,0,1,0,0,0};font_be(fdselect,static_cast<std::uint32_t>(glyphs.size()),2);
    // 單一 FD，空 Private DICT，width 由每個 charstring 明確提供。
    const auto fdarray=cff_index({{139,139,18}});
    const auto dict=[&](std::uint32_t charset_at,std::uint32_t chars_at,std::uint32_t fd_at,std::uint32_t select_at){
        std::vector<std::uint8_t> out;const auto integer=[&](std::int32_t value){out.push_back(29);font_be(out,static_cast<std::uint32_t>(value),4);};
        integer(391);integer(392);integer(0);out.insert(out.end(),{12,30});integer(static_cast<std::int32_t>(glyphs.size()));out.insert(out.end(),{12,34});
        for(const auto value:bounds)integer(value);out.push_back(5);
        const auto real=[&](double value){char text[64];std::snprintf(text,sizeof(text),"%.12f",value);std::vector<unsigned> nibbles;for(const char* p=text;*p;++p)nibbles.push_back(*p=='.'?10U:*p=='-'?14U:static_cast<unsigned>(*p-'0'));nibbles.push_back(15);if(nibbles.size()%2)nibbles.push_back(15);out.push_back(30);for(std::size_t i=0;i<nibbles.size();i+=2)out.push_back(static_cast<std::uint8_t>(nibbles[i]*16+nibbles[i+1]));};
        real(1.0/face.em);integer(0);integer(0);real(1.0/face.em);integer(0);integer(0);out.insert(out.end(),{12,7});
        integer(static_cast<std::int32_t>(charset_at));out.push_back(15);integer(static_cast<std::int32_t>(chars_at));out.push_back(17);integer(static_cast<std::int32_t>(fd_at));out.insert(out.end(),{12,36});integer(static_cast<std::int32_t>(select_at));out.insert(out.end(),{12,37});return out;
    };
    const auto top_size=cff_index({dict(0,0,0,0)}).size();const auto charset_at=static_cast<std::uint32_t>(4+names.size()+top_size+strings.size()+globals.size());
    const auto select_at=charset_at+static_cast<std::uint32_t>(charset.size()),fd_at=select_at+static_cast<std::uint32_t>(fdselect.size()),chars_at=fd_at+static_cast<std::uint32_t>(fdarray.size());
    std::vector<std::uint8_t> cff{1,0,4,4};for(const auto* part:{&names})cff.insert(cff.end(),part->begin(),part->end());
    const auto top=cff_index({dict(charset_at,chars_at,fd_at,select_at)});const std::array<const std::vector<std::uint8_t>*,7> parts{&top,&strings,&globals,&charset,&fdselect,&fdarray,&charstrings};for(const auto* part:parts)cff.insert(cff.end(),part->begin(),part->end());
    // 已固化為目前實例；移除原生專用、variation 與舊 TrueType hint 資料。
    std::erase_if(tables,[](const auto& entry){switch(entry.first){case 0x6876676c:case 0x63696467:case 0x66766172:case 0x61766172:case 0x67766172:case 0x48564152:case 0x56564152:case 0x4d564152:case 0x63766172:case 0x53544154:case 0x63767420:case 0x6670676d:case 0x70726570:case 0x67617370:case 0x68646d78:case 0x4c545348:case 0x56444d58:case 0x564f5247:return true;default:return false;}});
    const auto replace=[&](std::uint32_t tag,std::vector<std::uint8_t> bytes){const auto at=std::find_if(tables.begin(),tables.end(),[&](const auto& entry){return entry.first==tag;});if(at==tables.end())tables.emplace_back(tag,std::move(bytes));else at->second=std::move(bytes);};
    const auto patch=[&](std::uint32_t tag,unsigned offset,std::uint32_t value,unsigned size){const auto at=std::find_if(tables.begin(),tables.end(),[&](const auto& entry){return entry.first==tag;});if(at==tables.end() || offset+size>at->second.size())throw std::invalid_argument("原生字型 metrics table 不完整");for(unsigned i=0;i<size;++i)at->second[offset+i]=static_cast<std::uint8_t>(value>>(8*(size-1-i)));};
    std::vector<std::uint8_t> maxp{0,0,0x50,0};font_be(maxp,static_cast<std::uint32_t>(glyphs.size()),2);replace(0x6d617870,std::move(maxp));replace(0x43464620,std::move(cff));replace(0x686d7478,std::move(horizontal));patch(0x68686561,34,static_cast<std::uint32_t>(glyphs.size()),2);
    if(std::any_of(tables.begin(),tables.end(),[](const auto& entry){return entry.first==0x76686561;})){replace(0x766d7478,std::move(vertical));patch(0x76686561,34,static_cast<std::uint32_t>(glyphs.size()),2);}
    patch(0x68656164,50,0,2);for(unsigned i=0;i<4;++i)patch(0x68656164,36+2*i,static_cast<std::uint32_t>(bounds[i]),2);
    auto post=table(font,kCTFontTablePost);post.resize(32);post[0]=0;post[1]=3;post[2]=post[3]=0;replace(0x706f7374,std::move(post));
    // 沒有 fvar 的靜態實例不保留 OpenType layout 的 variation 分支。
    for(auto& [tag,bytes]:tables)if((tag==0x47535542 || tag==0x47504f53) && bytes.size()>=14 && dword(bytes,0)==0x10001)std::fill(bytes.begin()+10,bytes.begin()+14,0);
}
