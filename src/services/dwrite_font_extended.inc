        if(o.kind==Kind::Font || o.kind==Kind::FontFace){
            const auto face=effective_face(o.fonts->faces.at(o.indices.at(0)),o.simulations);
            const auto write_metrics=[&](std::uint64_t at,double sx=1,double sy=1){memory.check(GuestAddress{at},48,Access::Write);auto metrics=font_metrics1(face);for(unsigned i=0;i<22;++i){auto value=metrics[i];if(i && sx!=1){const bool horizontal=i==10 || i==12 || i==14 || i==16 || i==18 || i==20;const auto scale=horizontal?sx:sy;value=static_cast<std::int32_t>(std::round(std::round(value*scale)/scale));}memory.store(GuestAddress{at+i*2},16,static_cast<std::uint16_t>(value));}memory.store(GuestAddress{at+44},32,static_cast<std::uint32_t>(metrics[22]));};
            if(method=="GetMetrics1"){if(!arg(1))return finish(pointer_error);write_metrics(arg(1));return finish(0);}
            if(method=="GetGdiCompatibleMetrics1"){
                if(!arg(4))return finish(pointer_error);double sx=static_cast<double>(floating(1))*floating(2)/face.em,sy=sx;if(!std::isfinite(sx) || sx<=0)return finish(invalid);if(arg(3)){const auto read=[&](unsigned i){return std::bit_cast<float>(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)+i*4},32)));};for(unsigned i=0;i<6;++i)if(!std::isfinite(read(i)))return finish(invalid);if(read(1)!=0 || read(2)!=0)return finish(0x80004001U);sx*=std::abs(read(0));sy*=std::abs(read(3));if(sx<=0 || sy<=0)return finish(invalid);}write_metrics(arg(4),sx,sy);return finish(0);
            }
            if(method=="GetCaretMetrics"){
                if(!arg(1))return finish(pointer_error);const auto data=native_font_table(face,0x68686561U);memory.check(GuestAddress{arg(1)},6,Access::Write);for(unsigned i=0;i<3;++i)memory.store(GuestAddress{arg(1)+i*2},16,data.size()>=24?font_be(data,18+i*2,2):i==0?1:0);return finish(0);
            }
            if(method=="GetPanose"){
                if(!arg(1))return finish(pointer_error);const auto data=native_font_table(face,0x4f532f32U);std::array<std::uint8_t,10> panose{};if(data.size()>=42)std::copy_n(data.begin()+32,10,panose.begin());memory.write(GuestAddress{arg(1)},panose);return finish(0);
            }
            if(method=="GetUnicodeRanges"){
                if(!arg(3))return finish(pointer_error);if(arg(1)>1048576 || (arg(1) && !arg(2)))return finish(invalid);const auto ranges=font_unicode_ranges(face);memory.check(GuestAddress{arg(3)},4,Access::Write);if(arg(1))memory.check(GuestAddress{arg(2)},static_cast<std::size_t>(arg(1))*8,Access::Write);
                memory.store(GuestAddress{arg(3)},32,ranges.size());for(std::size_t i=0;i<std::min<std::size_t>(ranges.size(),arg(1));++i)for(unsigned j=0;j<2;++j)memory.store(GuestAddress{arg(2)+i*8+j*4},32,ranges[i][j]);return finish(arg(1)<ranges.size()?insufficient:0);
            }
            if(method=="IsMonospacedFont")return finish(face.fixed_pitch);
            if(method=="GetFontFaceReference"){
                if(!arg(1))return finish(pointer_error);auto value=font_object(Kind::FontFaceReference,o.fonts,o.indices.at(0));value.blob=o.blob;value.file_index=o.file_index;value.simulations=o.simulations;out(1,create(std::move(value)));return finish(0);
            }
            if(method=="Equals"){const auto other=bridge.objects.find(arg(1));return finish(other!=bridge.objects.end() && other->second.kind==Kind::Font && same_font(o,other->second));}
            if(method=="HasCharacter3" || (o.kind==Kind::FontFace && method=="HasCharacter"))return finish(arg(1)<=0x10ffff && native_font_glyph_indices(face,std::array<std::uint32_t,1>{static_cast<std::uint32_t>(arg(1))})[0]!=0);
            if(method=="GetFamilyNames"){if(!arg(1))return finish(pointer_error);auto values=native_font_names(face,16);if(values.empty())values=native_font_names(face,1);if(values.empty())values.push_back({u"en-us",face.family});Object value;value.kind=Kind::LocalizedStrings;value.localized=std::move(values);out(1,create(std::move(value)));return finish(0);}
            if(method=="GetLocality")return finish(o.blob && !o.blob->loaded?0:2);
            if(method=="IsCharacterLocal" || method=="IsGlyphLocal")return finish(!o.blob || o.blob->loaded);
            if(method=="AreCharactersLocal" || method=="AreGlyphsLocal"){
                if(!arg(4))return finish(pointer_error);if(arg(2)>1048576 || (arg(2) && !arg(1)))return finish(invalid);if(arg(2))memory.check(GuestAddress{arg(1)},static_cast<std::size_t>(arg(2))*2,Access::Read);memory.store(GuestAddress{arg(4)},32,!o.blob || o.blob->loaded);return finish(0);
            }
            if(method=="HasKerningPairs")return finish(!font_kern_pairs(face).empty());
            if(method=="HasVerticalGlyphVariants")return finish(!font_vertical(face).empty());
            if(method=="GetKerningPairAdjustments" || method=="GetVerticalGlyphVariants"){
                const auto count=static_cast<unsigned>(arg(1));if(count>1048576 || (count && (!arg(2) || !arg(3))))return finish(invalid);const bool vertical=method=="GetVerticalGlyphVariants";if(count){memory.check(GuestAddress{arg(2)},count*2U,Access::Read);memory.check(GuestAddress{arg(3)},count*(vertical?2U:4U),Access::Write);}
                const auto pairs=vertical?std::map<std::uint32_t,std::int32_t>{}:font_kern_pairs(face);const auto variants=vertical?font_vertical(face):std::map<std::uint16_t,std::uint16_t>{};
                for(unsigned i=0;i<count;++i){const auto glyph=static_cast<std::uint16_t>(memory.load(GuestAddress{arg(2)+i*2},16));std::int32_t value=vertical?glyph:0;if(vertical){const auto at=variants.find(glyph);if(at!=variants.end())value=at->second;}else if(i+1<count){const auto next=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)+(i+1)*2},16));const auto at=pairs.find((static_cast<std::uint32_t>(glyph)<<16)|next);if(at!=pairs.end())value=at->second;}memory.store(GuestAddress{arg(3)+i*(vertical?2U:4U)},vertical?16:32,static_cast<std::uint32_t>(value));}return finish(0);
            }
            if(method=="GetDesignGlyphAdvances" || method=="GetGdiCompatibleGlyphAdvances"){
                const bool gdi=method=="GetGdiCompatibleGlyphAdvances";const auto count=static_cast<unsigned>(arg(gdi?6:1));const auto input=arg(gdi?7:2),output=arg(gdi?8:3);if(count>1048576 || (count && (!input || !output)))return finish(invalid);if(count){memory.check(GuestAddress{input},count*2U,Access::Read);memory.check(GuestAddress{output},count*4U,Access::Write);}double scale=1;const bool sideways=arg(gdi?5:4)!=0;
                if(gdi){const double em=floating(1),ppd=floating(2);scale=em*ppd/face.em;if(!std::isfinite(scale) || em<=0 || ppd<=0)return finish(invalid);if(arg(3)){const auto read=[&](unsigned i){return std::bit_cast<float>(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)+i*4},32)));};for(unsigned i=0;i<6;++i)if(!std::isfinite(read(i)))return finish(invalid);if(read(1)!=0 || read(2)!=0)return finish(0x80004001U);scale*=std::abs(read(sideways?3:0));if(scale<=0)return finish(invalid);}if(arg(4) && !sideways)scale*=3;}
                std::vector<std::uint16_t> indices(count);for(unsigned i=0;i<count;++i)indices[i]=static_cast<std::uint16_t>(memory.load(GuestAddress{input+i*2},16));const auto metrics=native_font_glyph_metrics(face,indices);for(unsigned i=0;i<count;++i){const auto value=metrics[i][sideways?4:1];memory.store(GuestAddress{output+i*4},32,static_cast<std::uint32_t>(gdi?static_cast<std::int32_t>(std::round(std::round(value*scale)/scale)):value));}return finish(0);
            }
            if(method=="GetRecommendedRenderingMode1" || method=="GetRecommendedRenderingMode2" || method=="GetRecommendedRenderingMode3"){
                const bool params=method!="GetRecommendedRenderingMode1";const unsigned output=params?9:8;if(!arg(output) || (params && !arg(10)))return finish(pointer_error);const double em=floating(1),dx=floating(2),dy=floating(3);if(!std::isfinite(em) || !std::isfinite(dx) || !std::isfinite(dy) || em<=0 || dx<=0 || dy<=0 || arg(6)>1 || arg(7)>2)return finish(invalid);if(arg(4))for(unsigned i=0;i<6;++i)if(!std::isfinite(std::bit_cast<float>(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(4)+i*4},32)))))return finish(invalid);
                unsigned mode=em*std::max(dx,dy)/96>=(arg(6)?350:100)?6:arg(7)==1?2:arg(7)==2?3:4,grid=arg(7)?2:1;
                if(params && arg(8)){const auto rendering=bridge.objects.find(arg(8));if(rendering==bridge.objects.end() || rendering->second.kind!=Kind::RenderingParams)return finish(invalid);if(rendering->second.rendering_mode)mode=rendering->second.rendering_mode;if(rendering->second.grid_fit)grid=rendering->second.grid_fit;}
                memory.check(GuestAddress{arg(output)},4,Access::Write);if(params)memory.check(GuestAddress{arg(10)},4,Access::Write);memory.store(GuestAddress{arg(output)},32,mode);if(params)memory.store(GuestAddress{arg(10)},32,grid);return finish(0);
            }
            if(method=="IsColorFont")return finish(!native_font_table(face,0x434f4c52U).empty() && !native_font_table(face,0x4350414cU).empty());
            if(method=="GetColorPaletteCount" || method=="GetPaletteEntryCount"){const auto data=native_font_table(face,0x4350414cU);return finish(data.size()>=12?font_be(data,method=="GetColorPaletteCount"?4:2,2):0);}
            if(method=="GetPaletteEntries"){
                if(arg(3)>65536 || (arg(3) && !arg(4)))return finish(invalid);const auto colors=font_palette(face,static_cast<unsigned>(arg(1)));if(arg(2)>colors.size() || arg(3)>colors.size()-arg(2))return finish(invalid);if(arg(3))memory.check(GuestAddress{arg(4)},static_cast<std::size_t>(arg(3))*16,Access::Write);for(std::size_t i=0;i<arg(3);++i)for(unsigned j=0;j<4;++j)memory.store(GuestAddress{arg(4)+i*16+j*4},32,std::bit_cast<std::uint32_t>(colors[i+arg(2)][j]));return finish(0);
            }
        }
