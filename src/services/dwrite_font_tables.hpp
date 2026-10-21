NativeFontFace effective_face(const NativeFontFace& face,unsigned simulations){auto out=face;out.simulations=simulations;return out;}
// 派生 DirectWrite 介面共用實體 OpenType 資料；所有偏移先檢查再讀取。
std::uint32_t font_be(std::span<const std::uint8_t> data,std::size_t at,unsigned width){
    if(at>data.size() || width>data.size()-at)throw std::invalid_argument("OpenType 表格偏移無效");
    std::uint32_t value=0;for(unsigned i=0;i<width;++i)value=(value<<8)|data[at+i];return value;
}
int font_signed(std::span<const std::uint8_t> data,std::size_t at){return static_cast<std::int16_t>(font_be(data,at,2));}
unsigned font_stretch(const NativeFontFace& face){const auto table=native_font_table(face,0x4f532f32U);return table.size()>=8?std::clamp(font_be(table,6,2),1U,9U):5;}
std::array<std::int32_t,23> font_metrics1(const NativeFontFace& face){
    std::array<std::int32_t,23> out{};std::copy(face.design_metrics.begin(),face.design_metrics.end(),out.begin());
    const auto head=native_font_table(face,0x68656164U),os=native_font_table(face,0x4f532f32U);
    if(head.size()>=44){out[10]=font_signed(head,36);out[11]=font_signed(head,42);out[12]=font_signed(head,40);out[13]=font_signed(head,38);}
    if(os.size()>=26){out[14]=font_signed(os,14);out[15]=-font_signed(os,16);out[16]=font_signed(os,10);out[17]=font_signed(os,12);out[18]=font_signed(os,22);out[19]=font_signed(os,24);out[20]=font_signed(os,18);out[21]=font_signed(os,20);}
    out[22]=os.size()>=74 && (font_be(os,62,2)&0x80);return out;
}
std::vector<std::array<std::uint32_t,2>> font_unicode_ranges(const NativeFontFace& face){
    const auto data=native_font_table(face,0x636d6170U);std::set<std::uint32_t> supported;
    if(data.size()<4)return {};std::size_t chosen=0;unsigned format=0;
    for(unsigned i=0;i<font_be(data,2,2);++i){const auto at=4+i*8;const auto off=font_be(data,at+4,4);const auto f=font_be(data,off,2);
        if(f==12 || (f==4 && format!=12)){chosen=off;format=f;}}
    std::vector<std::array<std::uint32_t,2>> ranges;
    const auto append=[&](std::uint32_t a,std::uint32_t b){if(a>b || b>0x10ffff)return;if(!ranges.empty() && a<=ranges.back()[1]+1)ranges.back()[1]=std::max(ranges.back()[1],b);else ranges.push_back({a,b});};
    if(format==12){const auto count=font_be(data,chosen+12,4);if(count>(data.size()-chosen)/12)throw std::invalid_argument("cmap 群組超限");
        for(unsigned i=0;i<count;++i){const auto at=chosen+16+i*12;auto a=font_be(data,at,4);const auto b=font_be(data,at+4,4),glyph=font_be(data,at+8,4);if(!glyph)++a;append(a,b);}return ranges;}
    if(format==4){const auto count=font_be(data,chosen+6,2)/2;const auto ends=chosen+14,starts=ends+count*2+2,deltas=starts+count*2,offsets=deltas+count*2;
        for(unsigned i=0;i<count;++i){const auto a=font_be(data,starts+i*2,2),b=font_be(data,ends+i*2,2),delta=font_be(data,deltas+i*2,2),offset=font_be(data,offsets+i*2,2);
            for(auto c=a;c<=b && c<0xffff;++c){const auto glyph=offset?font_be(data,offsets+i*2+offset+(c-a)*2,2):c;const auto mapped=(glyph+(offset && !glyph?0:delta))&0xffff;if(mapped)append(c,c);}}}
    return ranges;
}
std::map<std::uint32_t,std::int32_t> font_kern_pairs(const NativeFontFace& face){
    std::map<std::uint32_t,std::int32_t> out;const auto data=native_font_table(face,0x6b65726eU);if(data.size()<4 || font_be(data,0,2)!=0)return out;
    std::size_t at=4;for(unsigned t=0;t<font_be(data,2,2);++t){const auto length=font_be(data,at+2,2),coverage=font_be(data,at+4,2);if(length<6 || length>data.size()-at)throw std::invalid_argument("kern 表格長度無效");
        if((coverage>>8)==0 && (coverage&1) && !(coverage&6)){const auto count=font_be(data,at+6,2);if(14+count*6>length)throw std::invalid_argument("kern 配對長度無效");for(unsigned i=0;i<count;++i){const auto p=at+14+i*6;out[font_be(data,p,4)]+=font_signed(data,p+4);}}at+=length;}
    return out;
}
std::vector<std::uint16_t> font_coverage(std::span<const std::uint8_t> data,std::size_t at){
    std::vector<std::uint16_t> out;const auto format=font_be(data,at,2),count=font_be(data,at+2,2);
    if(format==1)for(unsigned i=0;i<count;++i)out.push_back(static_cast<std::uint16_t>(font_be(data,at+4+i*2,2)));
    else if(format==2)for(unsigned i=0;i<count;++i){const auto p=at+4+i*6;const auto a=font_be(data,p,2),b=font_be(data,p+2,2),index=font_be(data,p+4,2);if(a>b || index!=out.size())throw std::invalid_argument("GSUB coverage 無效");for(auto g=a;g<=b;++g)out.push_back(static_cast<std::uint16_t>(g));}
    return out;
}
std::map<std::uint16_t,std::uint16_t> font_vertical(const NativeFontFace& face){
    std::map<std::uint16_t,std::uint16_t> out;const auto data=native_font_table(face,0x47535542U);if(data.size()<10)return out;
    const auto features=font_be(data,6,2),lookups=font_be(data,8,2);std::set<unsigned> selected;
    for(unsigned i=0;i<font_be(data,features,2);++i){const auto p=features+2+i*6,tag=font_be(data,p,4);if(tag!=0x76657274 && tag!=0x76727432)continue;const auto f=features+font_be(data,p+4,2);for(unsigned j=0;j<font_be(data,f+2,2);++j)selected.insert(font_be(data,f+4+j*2,2));}
    for(auto id:selected){if(id>=font_be(data,lookups,2))throw std::invalid_argument("GSUB lookup 無效");const auto lookup=lookups+font_be(data,lookups+2+id*2,2),type=font_be(data,lookup,2);
        for(unsigned i=0;i<font_be(data,lookup+4,2);++i){auto sub=lookup+font_be(data,lookup+6+i*2,2);auto actual=type;if(type==7){actual=font_be(data,sub+2,2);sub+=font_be(data,sub+4,4);}if(actual!=1)continue;
            const auto format=font_be(data,sub,2);const auto glyphs=font_coverage(data,sub+font_be(data,sub+2,2));
            for(unsigned j=0;j<glyphs.size();++j){if(format==1)out[glyphs[j]]=static_cast<std::uint16_t>(glyphs[j]+font_be(data,sub+4,2));else if(format==2){if(j>=font_be(data,sub+4,2))throw std::invalid_argument("GSUB substitution 無效");out[glyphs[j]]=static_cast<std::uint16_t>(font_be(data,sub+6+j*2,2));}}}}
    return out;
}
std::vector<std::array<float,4>> font_palette(const NativeFontFace& face,unsigned palette){
    const auto data=native_font_table(face,0x4350414cU);if(data.size()<12)return {};const auto entries=font_be(data,2,2),count=font_be(data,4,2),records=font_be(data,6,2),offset=font_be(data,8,4);
    if(palette>=count)throw std::invalid_argument("CPAL palette 無效");const auto first=font_be(data,12+palette*2,2);if(first+entries>records)throw std::invalid_argument("CPAL entry 無效");
    std::vector<std::array<float,4>> out;for(unsigned i=0;i<entries;++i){const auto p=offset+(first+i)*4;const auto color=font_be(data,p,4);out.push_back({static_cast<float>((color>>8)&255)/255,static_cast<float>((color>>16)&255)/255,static_cast<float>((color>>24)&255)/255,static_cast<float>(color&255)/255});}return out;
}
