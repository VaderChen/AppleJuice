        if(o.kind==Kind::FontFallbackBuilder){
            if(method=="AddMapping"){
                if(!arg(1) || !arg(2) || !arg(3) || !arg(4) || arg(2)>65536 || arg(4)>65536)return finish(invalid);FontMapping mapping;mapping.scale=floating(8);if(!std::isfinite(mapping.scale) || mapping.scale<=0)return finish(invalid);mapping.fonts=system();
                if(arg(5)){const auto collection=bridge.objects.find(arg(5));if(collection==bridge.objects.end() || collection->second.kind!=Kind::FontCollection)return finish(invalid);mapping.fonts=collection->second.fonts;}
                if(arg(6))mapping.locale=folded(read_utf16(memory,GuestAddress{arg(6)},85));if(arg(7))mapping.base=folded(read_utf16(memory,GuestAddress{arg(7)},32768));
                for(std::size_t i=0;i<arg(2);++i){std::array<std::uint32_t,2> range{};for(unsigned j=0;j<2;++j)range[j]=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)+i*8+j*4},32));if(range[0]>range[1] || range[1]>0x10ffff)return finish(invalid);mapping.ranges.push_back(range);}
                for(std::size_t i=0;i<arg(4);++i){const auto at=memory.load(GuestAddress{arg(3)+i*unit},bits);if(!at)return finish(invalid);mapping.families.push_back(read_utf16(memory,GuestAddress{at},32768));}o.mappings.push_back(std::move(mapping));return finish(0);
            }
            if(method=="AddMappings"){const auto fallback=bridge.objects.find(arg(1));if(fallback==bridge.objects.end() || fallback->second.kind!=Kind::FontFallback)return finish(invalid);o.mappings.insert(o.mappings.end(),fallback->second.mappings.begin(),fallback->second.mappings.end());return finish(0);}
            if(method=="CreateFontFallback"){if(!arg(1))return finish(pointer_error);Object value;value.kind=Kind::FontFallback;value.mappings=o.mappings;out(1,create(std::move(value)));return finish(0);}
        }
        if(o.kind==Kind::FontFallback && method=="MapCharacters"){
            if(!arg(9) || !arg(10) || !arg(11))return finish(pointer_error);memory.check(GuestAddress{arg(9)},4,Access::Write);memory.check(GuestAddress{arg(10)},unit,Access::Write);memory.check(GuestAddress{arg(11)},4,Access::Write);
            const auto complete=[&](std::uint32_t status){
                if(std::getenv("X64ARM_TRACE_FONT_FALLBACK")){
                    const auto saved=errno;const auto font=memory.load(GuestAddress{arg(10)},bits);
                    std::fprintf(stderr,"[font_fallback] pid=%u status=%x requested=%llu mapped=%llu font=%llx",process_id(),status,static_cast<unsigned long long>(arg(3)),static_cast<unsigned long long>(memory.load(GuestAddress{arg(9)},32)),static_cast<unsigned long long>(font));
                    if(const auto at=bridge.objects.find(font);at!=bridge.objects.end() && at->second.fonts && !at->second.indices.empty())std::fprintf(stderr," family=%s",utf16_to_utf8(at->second.fonts->faces.at(at->second.indices[0]).family).c_str());
                    std::fputc('\n',stderr);errno=saved;
                }
                if(resuming){const auto at=bridge.fallbacks.find(resuming);if(at!=bridge.fallbacks.end()){bridge.deferred.push_back(font_callback(memory,unit,at->second.object,2));bridge.deferred.push_back(font_callback(memory,unit,at->second.source,2));(void)free_heap(memory,at->second.scratch,state);bridge.fallbacks.erase(at);}}return finish(status);
            };
            if(!resuming){
                memory.store(GuestAddress{arg(9)},32,0);out(10,0);memory.store(GuestAddress{arg(11)},32,std::bit_cast<std::uint32_t>(1.0f));if(!arg(1) || arg(3)>1048576 || arg(2)>UINT32_MAX-arg(3) || arg(6)<1 || arg(6)>999 || arg(7)>2 || arg(8)<1 || arg(8)>9)return finish(invalid);if(!arg(3))return finish(0);
                if(arg(4)){const auto collection=bridge.objects.find(arg(4));if(collection==bridge.objects.end() || collection->second.kind!=Kind::FontCollection)return finish(invalid);}
                FallbackOperation operation;operation.service=service;operation.object=arg(0);operation.source=arg(1);operation.scratch=allocate_heap(memory,64);if(!operation.scratch)throw std::bad_alloc{};memory.write(GuestAddress{operation.scratch},std::array<std::uint8_t,64>{});const auto scratch=operation.scratch,token=bridge.next_operation++;++o.references;bridge.fallbacks.emplace(token,std::move(operation));
                start_callbacks(state,memory,{font_callback(memory,unit,arg(1),1),font_callback(memory,unit,arg(1),3,{arg(2),scratch,scratch+16})},Continuation::DWriteOperation,token);return std::nullopt;
            }
            auto& operation=bridge.fallbacks.at(resuming);const auto callback_result=static_cast<std::uint32_t>(state.reg(Gpr::Rax));if(callback_result&0x80000000U)return complete(callback_result);
            if(operation.step==0){
                const auto at=memory.load(GuestAddress{operation.scratch},bits),available=memory.load(GuestAddress{operation.scratch+16},32),count=std::min(arg(3),available);if(!count)return complete(0);if(!at)return complete(pointer_error);operation.text.resize(static_cast<std::size_t>(count));for(std::size_t i=0;i<count;++i)operation.text[i]=static_cast<char16_t>(memory.load(GuestAddress{at+i*2},16));operation.step=1;
                start_callbacks(state,memory,{font_callback(memory,unit,arg(1),6,{arg(2),operation.scratch+24,operation.scratch+32})},Continuation::DWriteOperation,resuming);return std::nullopt;
            }
            const auto locale_at=memory.load(GuestAddress{operation.scratch+32},bits);if(locale_at)operation.locale=folded(read_utf16(memory,GuestAddress{locale_at},85));const auto locale_length=memory.load(GuestAddress{operation.scratch+24},32);if(locale_length && locale_length<operation.text.size())operation.text.resize(static_cast<std::size_t>(locale_length));
            const auto& text=operation.text;const auto decode=[&](std::size_t index){std::uint32_t code=text[index];if(code>=0xd800 && code<=0xdbff && index+1<text.size() && text[index+1]>=0xdc00 && text[index+1]<=0xdfff)code=0x10000+((code-0xd800)<<10)+(text[index+1]-0xdc00);return code;};const auto first=decode(0);const auto first_length=first>0xffff?2U:1U;
            const auto base=arg(5)?read_utf16(memory,GuestAddress{arg(5)},32768):u"";std::shared_ptr<FontSet> selected_set;std::optional<std::size_t> selected;float scale=1;
            const auto choose=[&](const std::shared_ptr<FontSet>& fonts,std::u16string_view family){
                std::optional<std::size_t> best;const auto rank=[&](std::size_t i){const auto& f=fonts->faces[i];return std::tuple{std::abs(static_cast<int>(font_stretch(f))-static_cast<int>(arg(8))),f.italic!=(arg(7)!=0),std::abs(f.weight-static_cast<int>(arg(6)))};};
                for(std::size_t i=0;i<fonts->faces.size();++i){const auto& f=fonts->faces[i];if(folded(f.family)!=folded(std::u16string(family)) || !native_font_glyph_indices(f,std::array<std::uint32_t,1>{first})[0])continue;if(!best || rank(i)<rank(*best))best=i;}return best;
            };
            if(!base.empty()){auto fonts=arg(4)?bridge.objects.at(arg(4)).fonts:system();if(auto index=choose(fonts,base)){selected_set=fonts;selected=index;}}
            std::size_t mapping_limit=text.size();
            if(!selected)for(const auto& mapping:o.mappings){
                if(!mapping.base.empty() && mapping.base!=folded(base))continue;if(!mapping.locale.empty() && !operation.locale.starts_with(mapping.locale))continue;
                if(mapping.system){
                    GuiFont font;font.face=base;font.weight=static_cast<int>(arg(6));font.italic=arg(7)!=0;auto physical=native_resolve_font(font);if(!physical && !mapping.fonts->faces.empty())physical=mapping.fonts->faces[0];if(!physical)continue;
                    NativeLayoutOptions options;options.width=1e6f;options.height=1e6f;options.wrapping=1;const std::array spans{NativeLayoutSpan{0,static_cast<std::uint32_t>(text.size()),*physical,12}};const auto layout=native_text_layout(text,spans,options);const auto run=std::find_if(layout.runs.begin(),layout.runs.end(),[](const auto& r){return r.start==0;});if(run==layout.runs.end() || std::any_of(run->glyphs.indices.begin(),run->glyphs.indices.end(),[](auto g){return g==0;}))continue;
                    if(auto index=choose(mapping.fonts,run->face.family)){selected_set=mapping.fonts;selected=index;}
                    else{selected_set=std::make_shared<FontSet>();selected_set->faces={run->face};complete_set(*selected_set);selected=0;}
                    mapping_limit=run->length;break;
                }
                const auto in_range=[&](std::uint32_t code){return std::any_of(mapping.ranges.begin(),mapping.ranges.end(),[&](const auto& r){return code>=r[0] && code<=r[1];});};if(!in_range(first))continue;
                for(const auto& family:mapping.families)if(auto index=choose(mapping.fonts,family)){selected_set=mapping.fonts;selected=index;scale=mapping.scale;break;}
                if(selected){mapping_limit=0;while(mapping_limit<text.size() && in_range(decode(mapping_limit)))mapping_limit+=decode(mapping_limit)>0xffff?2:1;break;}
            }
            std::size_t mapped=first_length;if(selected){mapped=0;const auto& face=selected_set->faces[*selected];while(mapped<text.size() && mapped<mapping_limit){const auto code=decode(mapped);if(!native_font_glyph_indices(face,std::array<std::uint32_t,1>{code})[0])break;mapped+=code>0xffff?2:1;}if(mapped){out(10,create(font_object(Kind::Font,selected_set,*selected)));memory.store(GuestAddress{arg(11)},32,std::bit_cast<std::uint32_t>(scale));}else mapped=first_length;}
            memory.store(GuestAddress{arg(9)},32,mapped);return complete(0);
        }
