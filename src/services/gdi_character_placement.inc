// GCP 共用目前選取的原生字型與 ICU 雙向分析；A/W 僅在 guest 字串／ABI 邊界轉換。
if(name=="GetFontLanguageInfo"){
    unsigned value=0;const auto scripts=face->signature[4];
    if(scripts&0x003e0000U)value|=1U;
    if(scripts&0x60U)value|=2U|0x100U;
    if(!native_font_table(*face,0x6b65726eU).empty() || !native_font_table(*face,0x47504f53U).empty())value|=8U;
    if(!native_font_table(*face,0x47535542U).empty())value|=0x40000U|0x10U|0x20U;
    return finish(value);
}
if(name.starts_with("GetCharacterPlacement")){
    try {
        const auto count=static_cast<std::int32_t>(arg(2)),limit=static_cast<std::int32_t>(arg(3));
        const auto flags=static_cast<std::uint32_t>(arg(5));const auto result=arg(4);
        constexpr unsigned reorder=2,kern=8,shape=16,ligate=32,justify=0x10000,classin=0x80000,maxextent=0x100000;
        if(count<0 || count>65535 || (count && !arg(1)) || ((flags&maxextent) && limit<0) || (flags&~0x0ff9053bU))return fail(87);
        if((flags&justify) && !(flags&maxextent))return fail(87);
        // GCP_JUSTIFYIN 不是公開 GCP 契約；Kashida 插入不能以調整空白冒充。
        if(flags&(0x400U|0x200000U))return fail(50);
        if((flags&0x0c000000U)==0x0c000000U)return fail(87);
        if(dc==gui_.contexts.end())return fail(6);
        const unsigned unit=x86_?4:8,base=x86_?4:8,glyph_offset=base+6*unit,structure=glyph_offset+8;
        std::array<std::uint64_t,6> arrays{};unsigned capacity=static_cast<unsigned>(count);
        if(result){
            memory.check(GuestAddress{result},structure,Access::Read);
            if(memory.load(GuestAddress{result},32)!=structure)return fail(87);
            for(unsigned i=0;i<6;++i)arrays[i]=memory.load(GuestAddress{result+base+i*unit},static_cast<std::uint8_t>(unit*8));
            capacity=static_cast<unsigned>(memory.load(GuestAddress{result+glyph_offset},32));
            memory.check(GuestAddress{result+glyph_offset},8,Access::Write);
            if((flags&(justify|kern)) && !arrays[2] && !arrays[3])return fail(87);
            if((flags&classin) && !arrays[4])return fail(87);
            // 非零限制及跨呼叫字詞連接需要額外的上下文；不可悄悄回傳錯誤字形。
            if((flags&(shape|ligate)) && arrays[5] && capacity && memory.load(GuestAddress{arrays[5]},16))return fail(50);
        }
        std::u16string text;std::vector<unsigned> source_begin,source_end;
        const auto source_count=std::min(static_cast<unsigned>(count),capacity);
        if(wide){
            memory.check(GuestAddress{arg(1)},static_cast<std::size_t>(count)*2,Access::Read);
            for(unsigned i=0;i<source_count;++i){text.push_back(static_cast<char16_t>(memory.load(GuestAddress{arg(1)+i*2},16)));source_begin.push_back(i);source_end.push_back(i+1);}
            if(source_count<static_cast<unsigned>(count) && !text.empty() && text.back()>=0xd800 && text.back()<=0xdbff){text.pop_back();source_begin.pop_back();source_end.pop_back();}
        }else{
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count));memory.read(GuestAddress{arg(1)},bytes);
            for(int at=0;at<count;){const auto begin=at;UChar32 cp;U8_NEXT_OR_FFFD(bytes.data(),at,count,cp);if(static_cast<unsigned>(at)>source_count)break;
                if(cp>0xffff){const auto value=static_cast<unsigned>(cp)-0x10000;text.push_back(static_cast<char16_t>(0xd800+(value>>10)));source_begin.push_back(static_cast<unsigned>(begin));source_end.push_back(static_cast<unsigned>(at));cp=static_cast<UChar32>(0xdc00+(value&1023));}
                text.push_back(static_cast<char16_t>(cp));source_begin.push_back(static_cast<unsigned>(begin));source_end.push_back(static_cast<unsigned>(at));}
        }
        if(flags&justify){while(!text.empty() && u_isUWhiteSpace(text.back())){text.pop_back();source_begin.pop_back();source_end.pop_back();}if(text.empty())return fail(87);}
        std::vector<std::uint8_t> classes(text.size()),presets(text.size());
        for(std::size_t i=0;i<text.size();++i){const auto direction=u_charDirection(text[i]);
            classes[i]=direction==U_RIGHT_TO_LEFT || direction==U_RIGHT_TO_LEFT_ARABIC?2:direction==U_ARABIC_NUMBER?4:direction==U_EUROPEAN_NUMBER?5:direction==U_EUROPEAN_NUMBER_TERMINATOR?6:direction==U_EUROPEAN_NUMBER_SEPARATOR?7:direction==U_COMMON_NUMBER_SEPARATOR?8:direction==U_LEFT_TO_RIGHT?1:3;
            if((flags&classin) && arrays[4]){const auto preset=static_cast<std::uint8_t>(memory.load(GuestAddress{arrays[4]+source_begin[i]},8));if((preset&15U)>8 || (i && (preset&0xf0U)))return fail(87);presets[i]=preset;if(preset&15U)classes[i]=preset;else classes[i]|=preset&0xf0U;}
            if(flags&0x0c000000U){const auto digit=u_charDigitValue(text[i]);if(digit>=0 && digit<10){const auto zero=(flags&0x04000000U)?u'0':model.charset==178?u'\u0660':model.charset==222?u'\u0e50':u'0';text[i]=static_cast<char16_t>(zero+digit);}}
        }
        struct Layout {
            std::u16string visual;std::vector<unsigned> order,glyph_order,cluster_end;
            std::vector<std::uint16_t> glyphs;std::vector<std::int32_t> dx,caret;
            std::vector<bool> rtl;int width{};
        };
        const auto layout=[&](std::size_t length){
            Layout out;out.order.resize(length);out.glyph_order.resize(length);out.caret.resize(length);out.rtl.resize(length);out.cluster_end.resize(length);
            if(!length)return out;
            UErrorCode error=U_ZERO_ERROR;std::unique_ptr<UBiDi,decltype(&ubidi_close)> bidi(ubidi_openSized(static_cast<int>(length),0,&error),ubidi_close);
            if(!bidi || U_FAILURE(error))throw std::bad_alloc();
            auto analysis=text.substr(0,length);const bool base_rtl=(dc->second.alignment&0x100U)!=0;
            for(std::size_t i=0;i<length;++i){auto cls=classes[i]&15U;bool overridden=(presets[i]&15U)!=0;
                if(((flags&0x1000000U) && (cls==4 || cls==5)) || ((flags&0x2000000U) && cls==3)){cls=base_rtl?2U:1U;overridden=true;}
                if(overridden){constexpr char16_t representatives[]{0,u'A',u'\u05d0',u' ',u'\u0660',u'0',u'$',u'+',u','};analysis[i]=representatives[cls];}}
            char16_t before=0,after=0;const auto boundary=classes[0]&0xf0U;
            if((boundary&0xc0U)==0xc0U || (boundary&0x30U)==0x30U)throw std::invalid_argument("GCP 邊界方向互斥");
            if(boundary&0xc0U)before=(boundary&0x40U)?u'\u05d0':u'A';if(boundary&0x30U)after=(boundary&0x10U)?u'\u05d0':u'A';
            ubidi_setContext(bidi.get(),&before,before?1:0,&after,after?1:0,&error);
            ubidi_setPara(bidi.get(),analysis.data(),static_cast<int>(length),base_rtl?1:UBIDI_DEFAULT_LTR,nullptr,&error);
            if(U_FAILURE(error))throw std::invalid_argument("GCP 雙向分析失敗");
            const auto runs=(flags&reorder)?ubidi_countRuns(bidi.get(),&error):1;
            if(U_FAILURE(error))throw std::invalid_argument("GCP 視覺分段失敗");
            double advance=0;std::vector<double> widths;
            for(int run=0;run<runs;++run){int begin=0,size=static_cast<int>(length);const bool rtl=(flags&reorder) && ubidi_getVisualRun(bidi.get(),run,&begin,&size)==UBIDI_RTL;
                auto part=text.substr(static_cast<std::size_t>(begin),static_cast<std::size_t>(size));
                auto native=native_gdi_placement(model,part,rtl,flags);
                if(rtl && !(flags&0x800000U))for(auto& ch:part)ch=static_cast<char16_t>(u_charMirror(ch));
                const auto start=static_cast<unsigned>(out.glyphs.size());out.glyphs.insert(out.glyphs.end(),native.glyphs.begin(),native.glyphs.end());widths.insert(widths.end(),native.advances.begin(),native.advances.end());
                // lpOutString 保留組合字元／surrogate；lpOrder 的 glyph 與文字索引各自建立。
                std::unique_ptr<UBreakIterator,decltype(&ubrk_close)> breaks(ubrk_open(UBRK_CHARACTER,"root",part.data(),size,&error),ubrk_close);
                if(!breaks || U_FAILURE(error))throw std::bad_alloc();std::vector<std::pair<int,int>> chunks;
                for(int a=ubrk_first(breaks.get()),b=ubrk_next(breaks.get());b!=UBRK_DONE;a=b,b=ubrk_next(breaks.get()))chunks.emplace_back(a,b);
                if(rtl)std::reverse(chunks.begin(),chunks.end());
                for(const auto [a,b]:chunks)for(int i=a;i<b;++i){out.order[static_cast<std::size_t>(begin+i)]=static_cast<unsigned>(out.visual.size());out.visual.push_back(part[static_cast<std::size_t>(i)]);}
                for(int i=0;i<size;++i){const auto at=static_cast<std::size_t>(begin+i);out.glyph_order[at]=start+native.clusters[static_cast<std::size_t>(i)];out.rtl[at]=rtl;}
            }
            if(out.glyphs.size()>capacity)throw std::length_error("GCP 字形超過輸出容量");
            std::map<unsigned,unsigned> cluster_last;
            for(unsigned i=0;i<length;++i)cluster_last[out.glyph_order[i]]=i+1;
            std::vector<int> positions(widths.size()+1);
            for(std::size_t i=0;i<widths.size();++i){advance+=widths[i];if(!std::isfinite(advance) || advance<0 || advance>INT32_MAX)throw std::invalid_argument("GCP 字距超出範圍");positions[i+1]=static_cast<int>(std::lround(advance));out.dx.push_back(positions[i+1]-positions[i]);}
            out.width=positions.back();
            for(unsigned i=0;i<length;++i){const auto glyph=out.glyph_order[i];const auto next=cluster_last.upper_bound(glyph);const auto end=next==cluster_last.end()?static_cast<unsigned>(widths.size()):next->first;
                out.caret[i]=positions[out.rtl[i]?end:glyph];out.cluster_end[i]=cluster_last.at(glyph);}
            return out;
        };
        auto placed=layout(text.size());std::size_t fitted=text.size();
        if(flags&maxextent){
            for(;;){std::int64_t width=0;std::size_t fit=0;std::map<unsigned,int> sums;
                for(unsigned i=0;i<placed.glyph_order.size();++i)sums.try_emplace(placed.glyph_order[i],0);
                for(auto it=sums.begin();it!=sums.end();++it){auto next=std::next(it);const auto end=next==sums.end()?placed.dx.size():next->first;it->second=std::accumulate(placed.dx.begin()+it->first,placed.dx.begin()+static_cast<std::ptrdiff_t>(end),0);}
                for(std::size_t i=0;i<fitted;){const auto end=placed.cluster_end[i];if(width+sums.at(placed.glyph_order[i])>limit)break;width+=sums.at(placed.glyph_order[i]);fit=end;i=end;}
                if(fit==fitted)break;fitted=fit;placed=layout(fitted);
            }
        }
        if((flags&justify) && fitted && placed.width<limit){
            std::vector<unsigned> gaps;for(unsigned i=0;i<fitted;++i)if(text[i]==u' ' && (i==0 || placed.glyph_order[i]!=placed.glyph_order[i-1]))gaps.push_back(placed.glyph_order[i]);
            if(gaps.empty())for(unsigned i=0;i+1<placed.dx.size();++i)gaps.push_back(i);
            if(gaps.empty() && placed.dx.size()==1)gaps.push_back(0);
            if(!gaps.empty()){const auto extra=limit-placed.width;for(unsigned i=0;i<gaps.size();++i)placed.dx[gaps[i]]+=extra/static_cast<int>(gaps.size())+(i<static_cast<unsigned>(extra)%gaps.size());placed.width=limit;
                std::vector<int> positions(placed.dx.size()+1);std::partial_sum(placed.dx.begin(),placed.dx.end(),positions.begin()+1);std::set<unsigned> starts(placed.glyph_order.begin(),placed.glyph_order.end());
                for(unsigned i=0;i<fitted;++i){const auto glyph=placed.glyph_order[i];const auto next=starts.upper_bound(glyph);placed.caret[i]=positions[placed.rtl[i]?(next==starts.end()?placed.dx.size():*next):glyph];}}
        }
        const auto metrics=native_gui_?native_gui_->font_metrics(model).value_or(fallback_font_metrics(model)):fallback_font_metrics(model);
        if(result){
            const auto units=fitted?source_end[fitted-1]:0;
            const auto visual_bytes=utf16_to_utf8(placed.visual);if(!wide && visual_bytes.size()>capacity)return fail(122);
            // 全部輸出範圍驗證完才開始寫入；無效尾端不留下半套 GCP_RESULTS。
            const std::array<std::size_t,6> sizes{wide?placed.visual.size()*2:visual_bytes.size(),units*4U,placed.dx.size()*4,units*4U,units,placed.glyphs.size()*2};
            for(unsigned i=0;i<6;++i)if(arrays[i])memory.check(GuestAddress{arrays[i]},sizes[i],Access::Write);
            if(arrays[0]){if(wide){for(std::size_t i=0;i<placed.visual.size();++i)memory.store(GuestAddress{arrays[0]+i*2},16,placed.visual[i]);}else memory.write(GuestAddress{arrays[0]},{reinterpret_cast<const std::uint8_t*>(visual_bytes.data()),visual_bytes.size()});}
            std::vector<unsigned> visual_offsets(placed.visual.size());if(!wide){unsigned bytes=0;for(unsigned i=0;i<placed.visual.size();){const auto begin=i;UChar32 cp;U16_NEXT(placed.visual.data(),i,static_cast<unsigned>(placed.visual.size()),cp);for(unsigned j=begin;j<i;++j)visual_offsets[j]=bytes;bytes+=static_cast<unsigned>(utf16_to_utf8(std::u16string_view(placed.visual).substr(begin,i-begin)).size());}}
            for(std::size_t i=0;i<fitted;++i)for(unsigned j=source_begin[i];j<source_end[i];++j){
                if(arrays[1])memory.store(GuestAddress{arrays[1]+j*4},32,arrays[5]?placed.glyph_order[i]:wide?placed.order[i]:visual_offsets[placed.order[i]]);
                if(arrays[3])memory.store(GuestAddress{arrays[3]+j*4},32,static_cast<std::uint32_t>(placed.caret[i]));
                if(arrays[4])memory.store(GuestAddress{arrays[4]+j},8,classes[i]);}
            for(std::size_t i=0;i<placed.glyphs.size();++i){if(arrays[2])memory.store(GuestAddress{arrays[2]+i*4},32,static_cast<std::uint32_t>(placed.dx[i]));if(arrays[5])memory.store(GuestAddress{arrays[5]+i*2},16,placed.glyphs[i]);}
            memory.store(GuestAddress{result+glyph_offset},32,placed.glyphs.size());memory.store(GuestAddress{result+glyph_offset+4},32,units);
        }
        return finish((static_cast<unsigned>(metrics.height)&65535U)<<16 | (static_cast<unsigned>(placed.width)&65535U));
    }catch(const std::bad_alloc&){return fail(8);}catch(const std::length_error&){return fail(122);}catch(const std::invalid_argument&){return fail(87);}catch(const std::domain_error&){return fail(50);}
}
