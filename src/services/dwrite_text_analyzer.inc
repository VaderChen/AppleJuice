// 使用既有 CoreText 排版結果；FontFace、COM lifetime 與 buffer 驗證沿用同一橋接。
if(o.kind==Kind::Factory && method=="CreateTextAnalyzer"){
    if(!arg(1))return finish(pointer_error);out(1,0);Object value;value.kind=Kind::TextAnalyzer;out(1,create(std::move(value)));return finish(0);
}
if(o.kind==Kind::TextAnalyzer && (method=="GetGlyphs" || method=="GetGlyphPlacements" || method=="GetGdiCompatibleGlyphPlacements")){
    const bool shaping=method=="GetGlyphs",gdi=method=="GetGdiCompatibleGlyphPlacements";
    const unsigned length=static_cast<unsigned>(arg(shaping?2:4)),glyph_count=shaping?0:static_cast<unsigned>(arg(7));
    const unsigned face_index=shaping?3:8,side_index=shaping?4:gdi?13:10,rtl_index=shaping?5:gdi?14:11;
    const unsigned script_index=shaping?6:gdi?15:12,features_index=shaping?9:gdi?17:14,feature_count_index=shaping?11:gdi?19:16;
    if(length>65535 || glyph_count>65535 || !arg(face_index) || !arg(script_index) || (length && !arg(1)))return finish(invalid);
    if(static_cast<std::uint32_t>(arg(side_index)) || static_cast<std::uint32_t>(arg(feature_count_index)))return finish(0x80004001U);
    if(arg(features_index) || arg(features_index+1))return finish(invalid);
    const auto font=bridge.objects.find(arg(face_index));if(font==bridge.objects.end() || font->second.kind!=Kind::FontFace || !font->second.fonts || font->second.indices.empty())return finish(invalid);
    auto face=font->second.fonts->faces.at(font->second.indices[0]);face.simulations=font->second.simulations;
    const auto shape_mode=memory.load(GuestAddress{arg(script_index)+4},32);if(shape_mode>1)return finish(invalid);
    if(shaping && arg(8)){
        const auto sub=bridge.objects.find(arg(8));if(sub==bridge.objects.end() || sub->second.kind!=Kind::NumberSubstitution)return finish(invalid);
        if(sub->second.number_method!=1)return finish(0x80004001U);
    }
    std::u16string text(length,u'\0');for(unsigned i=0;i<length;++i)text[i]=static_cast<char16_t>(memory.load(GuestAddress{arg(1)+i*2},16));
    if(!shape_mode && text.find_first_of(u"\r\n\t")!=text.npos)return finish(0x80004001U);
    const float em=shaping?64.0f:floating(9),scale=gdi?floating(10):1.0f;
    if(!std::isfinite(em) || em<=0 || !std::isfinite(scale) || scale<=0)return finish(invalid);
    if(gdi && arg(11)){
        constexpr std::array<float,6> identity{1,0,0,1,0,0};
        for(unsigned i=0;i<6;++i)if(std::bit_cast<float>(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(11)+i*4},32)))!=identity[i])return finish(0x80004001U);
    }
    NativeGlyphRun glyphs;std::vector<std::uint16_t> clusters(length);
    if(length && !shape_mode){
        NativeLayoutOptions options;options.width=1000000;options.height=1000000;options.wrapping=1;options.reading=static_cast<std::uint32_t>(arg(rtl_index))?1:0;options.pixels_per_dip=scale;options.measuring=gdi && !static_cast<std::uint32_t>(arg(12))?1:0;
        const unsigned locale_index=shaping?7:gdi?16:13;const auto locale=arg(locale_index)?read_utf16(memory,GuestAddress{arg(locale_index)},85):u"";
        if(!locale.empty() && !windows_locale_name_valid(locale))return finish(invalid);
        const NativeLayoutSpan span{0,length,face,em,locale};const auto layout=native_text_layout(text,std::span(&span,1),options);
        for(const auto& run:layout.runs){
            // 字形索引必須屬於指定 FontFace；不把 CoreText fallback 的另一張字型混入。
            if(run.face.postscript_name!=face.postscript_name)return finish(0x88985000U);
            const auto offset=glyphs.indices.size();if(offset+run.glyphs.indices.size()>65535)return finish(invalid);
            glyphs.indices.insert(glyphs.indices.end(),run.glyphs.indices.begin(),run.glyphs.indices.end());glyphs.advances.insert(glyphs.advances.end(),run.glyphs.advances.begin(),run.glyphs.advances.end());glyphs.offsets.insert(glyphs.offsets.end(),run.glyphs.offsets.begin(),run.glyphs.offsets.end());
            for(unsigned i=0;i<run.length;++i)clusters[run.start+i]=static_cast<std::uint16_t>(offset+run.clusters[i]);
        }
    }
    if(shaping){
        if(!arg(17))return finish(pointer_error);memory.check(GuestAddress{arg(17)},4,Access::Write);memory.store(GuestAddress{arg(17)},32,0);
        if(glyphs.indices.size()>static_cast<std::uint32_t>(arg(12)))return finish(insufficient);
        if((length && (!arg(13) || !arg(14))) || (!glyphs.indices.empty() && (!arg(15) || !arg(16))))return finish(pointer_error);
        memory.check(GuestAddress{arg(13)},length*2,Access::Write);memory.check(GuestAddress{arg(14)},length*2,Access::Write);memory.check(GuestAddress{arg(15)},glyphs.indices.size()*2,Access::Write);memory.check(GuestAddress{arg(16)},glyphs.indices.size()*2,Access::Write);
        std::vector<bool> starts(glyphs.indices.size());for(auto cluster:clusters)if(cluster<starts.size())starts[cluster]=true;
        for(unsigned i=0;i<length;++i){memory.store(GuestAddress{arg(13)+i*2},16,clusters[i]);memory.store(GuestAddress{arg(14)+i*2},16,shape_mode?1:0);}
        for(unsigned i=0;i<glyphs.indices.size();++i){memory.store(GuestAddress{arg(15)+i*2},16,glyphs.indices[i]);memory.store(GuestAddress{arg(16)+i*2},16,(starts[i]?16:0)|(glyphs.advances[i]==0?64:0));}
        memory.store(GuestAddress{arg(17)},32,glyphs.indices.size());return finish(0);
    }
    const unsigned advances_index=gdi?20:17,offsets_index=gdi?21:18;
    if((length && (!arg(2) || !arg(3))) || (glyph_count && (!arg(5) || !arg(6) || !arg(advances_index) || !arg(offsets_index))))return finish(pointer_error);
    memory.check(GuestAddress{arg(3)},length*2,Access::Read);memory.check(GuestAddress{arg(6)},glyph_count*2,Access::Read);
    if(glyph_count!=glyphs.indices.size())return finish(invalid);
    for(unsigned i=0;i<length;++i)if(memory.load(GuestAddress{arg(2)+i*2},16)!=clusters[i])return finish(invalid);
    for(unsigned i=0;i<glyph_count;++i)if(memory.load(GuestAddress{arg(5)+i*2},16)!=glyphs.indices[i])return finish(invalid);
    memory.check(GuestAddress{arg(advances_index)},glyph_count*4,Access::Write);memory.check(GuestAddress{arg(offsets_index)},glyph_count*8,Access::Write);
    for(unsigned i=0;i<glyph_count;++i){memory.store(GuestAddress{arg(advances_index)+i*4},32,std::bit_cast<std::uint32_t>(glyphs.advances[i]));for(unsigned j=0;j<2;++j)memory.store(GuestAddress{arg(offsets_index)+i*8+j*4},32,std::bit_cast<std::uint32_t>(glyphs.offsets[i][j]));}
    return finish(0);
}
if(o.kind==Kind::TextAnalyzer && method.starts_with("Analyze")){
    if(!arg(1) || !arg(4))return finish(pointer_error);
    const auto start=static_cast<std::uint32_t>(arg(2)),length=static_cast<std::uint32_t>(arg(3));if(length>65535 || length>UINT32_MAX-start)return finish(invalid);if(!length)return finish(0);
    if(bridge.analyses.size()>=128)return finish(0x8007000eU);
    TextAnalysisOperation operation;operation.service=service;operation.object=arg(0);operation.source=arg(1);operation.sink=arg(4);operation.start=static_cast<unsigned>(start);operation.length=static_cast<unsigned>(length);
    operation.scratch=allocate_heap(memory,64+length);if(!operation.scratch)return finish(0x8007000eU);
    const auto token=bridge.next_operation++;bridge.analyses.emplace(token,std::move(operation));++o.references;
    return analysis_next(token,TextAnalysisStep::RetainSource,arg(1),1);
}
