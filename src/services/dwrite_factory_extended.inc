        // Factory 1～3 共用本機字型資料、私有 SFNT 與相同 COM identity。
        const auto complete_set=[](FontSet& set){std::set<std::u16string> names;for(const auto& face:set.faces)names.insert(face.family);set.families.assign(names.begin(),names.end());};
        const auto blob_for=[&](Object& value){
            if(!value.blob){const auto& face=value.fonts->faces.at(value.indices.at(0));auto blob=native_font_blob(face);blob->key=bridge.next_file++;bridge.files.emplace(blob->key,blob);value.blob=std::move(blob);value.file_index=0;}
            return value.blob;
        };
        const auto font_object=[&](Kind kind,std::shared_ptr<FontSet> set,std::size_t index){Object value;value.kind=kind;value.fonts=std::move(set);value.indices={index};if(index<value.fonts->simulations.size())value.simulations=value.fonts->simulations[index];return value;};
        const auto same_font=[](const Object& a,const Object& b){
            if(!a.fonts || !b.fonts || a.indices.empty() || b.indices.empty())return false;
            const auto& af=a.fonts->faces.at(a.indices[0]);const auto& bf=b.fonts->faces.at(b.indices[0]);
            if(a.blob && b.blob)return a.file_index==b.file_index && a.simulations==b.simulations &&
                (a.blob==b.blob || a.blob->contents()==b.blob->contents());
            return af.native==bf.native && a.simulations==b.simulations;
        };
        const auto open_font=[&](std::uint64_t path_at,std::uint64_t time_at)->std::pair<std::shared_ptr<FontBlob>,std::uint32_t>{
            if(!path_at)return {{},invalid};const auto path=utf16_to_utf8(read_utf16(memory,GuestAddress{path_at},32768));const auto meta=filesystem_.information(path);
            if(meta.error)return {{},0x80070000U|meta.error};if((meta.metadata.attributes&16) || meta.metadata.size>native_font_file_limit)return {{},invalid};const auto source=read_font_file(filesystem_,path);if(source.error)return {{},0x80070000U|source.error};
            auto blob=std::make_shared<FontBlob>();blob->bytes=*source.bytes;blob->fonts=std::make_shared<FontSet>();blob->fonts->faces=native_load_font_faces(blob->bytes);complete_set(*blob->fonts);blob->loaded=true;blob->key=bridge.next_file++;blob->modified=time_at?memory.load(GuestAddress{time_at},64):meta.metadata.modified;blob->path=utf8_to_utf16(source.key);std::replace(blob->path.begin(),blob->path.end(),u'/',u'\\');bridge.files.emplace(blob->key,blob);return {blob,0};
        };
        const auto property_values=[&](const FontSet& set,std::size_t index,unsigned id){
            if(index<set.properties.size()){const auto at=set.properties[index].find(id);if(at!=set.properties[index].end())return at->second;}
            const auto& face=set.faces.at(index);std::vector<NativeFontName> values;
            constexpr std::array<std::uint16_t,14> ids{UINT16_MAX,21,16,22,4,1,6,UINT16_MAX,UINT16_MAX,UINT16_MAX,UINT16_MAX,UINT16_MAX,UINT16_MAX,17};
            if(id<ids.size() && ids[id]!=UINT16_MAX)values=native_font_names(face,ids[id]);
            if(values.empty()){std::u16string text;if(id==1 || id==2 || id==5)text=face.family;else if(id==3 || id==13)text=face.style;else if(id==4)text=face.full_name;else if(id==6)text=face.postscript_name;else if(id>=10 && id<=12)text=utf8_to_utf16(std::to_string(id==10?face.weight:id==11?static_cast<int>(font_stretch(face)):face.italic?2:0));if(!text.empty())values.push_back({u"en-us",std::move(text)});}
            return values;
        };
        const auto read_properties=[&](std::uint64_t at,unsigned count){
            if(count>65536 || (count && !at))throw std::invalid_argument("字型屬性陣列無效");std::map<unsigned,std::vector<NativeFontName>> values;
            const unsigned stride=unit==8?24:12;for(unsigned i=0;i<count;++i){const auto p=at+i*stride;const auto id=static_cast<unsigned>(memory.load(GuestAddress{p},32));if(!id || id>13)throw std::invalid_argument("字型屬性識別無效");const auto text=memory.load(GuestAddress{p+unit},bits),locale=memory.load(GuestAddress{p+unit*2},bits);if(!text)throw std::invalid_argument("字型屬性文字無效");values[id].push_back({locale?read_utf16(memory,GuestAddress{locale},85):u"",read_utf16(memory,GuestAddress{text},32768)});}return values;
        };
        if(o.kind==Kind::Factory){
            if(method=="CreateCustomRenderingParams1" || method=="CreateCustomRenderingParams2" || method=="CreateCustomRenderingParams3"){
                const unsigned output=method.ends_with('1')?7:8;if(!arg(output))return finish(pointer_error);out(output,0);Object value;value.kind=Kind::RenderingParams;value.blend={floating(1),floating(2),floating(4)};value.grayscale_contrast=floating(3);value.pixel_geometry=static_cast<unsigned>(arg(5));value.rendering_mode=static_cast<unsigned>(arg(6));value.grid_fit=output==8?static_cast<unsigned>(arg(7)):0;
                if(std::any_of(value.blend.begin(),value.blend.end(),[](float v){return !std::isfinite(v);}) || value.blend[0]<=0 || value.blend[1]<0 || !std::isfinite(value.grayscale_contrast) || value.grayscale_contrast<0 || value.blend[2]<0 || value.blend[2]>1 || value.pixel_geometry>2 || value.grid_fit>2 || value.rendering_mode>(method.ends_with('3')?7U:6U))return finish(invalid);out(output,create(std::move(value)));return finish(0);
            }
            if(method=="GetSystemFontSet" || method=="CreateFontSetBuilder" || method=="GetSystemFontCollection3" || method=="GetEudcFontCollection"){
                const unsigned output=method=="GetSystemFontCollection3"?2:1;if(!arg(output))return finish(pointer_error);out(output,0);Object value;value.kind=method=="GetSystemFontSet"?Kind::FontSet:method=="CreateFontSetBuilder"?Kind::FontSetBuilder:Kind::FontCollection;
                if(method=="GetSystemFontCollection3" && arg(3))bridge.system.reset();
                value.fonts=method=="CreateFontSetBuilder" || method=="GetEudcFontCollection"?std::make_shared<FontSet>():system();
                if(method=="GetEudcFontCollection"){
                    RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)return finish(0x80070000U|transaction.error);
                    // 未註冊 EUDC 的系統具有真正的空集合；有註冊時讀取實際檔案。
                    for(const auto& [key,values]:runtime_.registry.keys){if(!folded(key).starts_with(std::u16string(1,1)+u"\\eudc\\"))continue;for(const auto& [key_name,entry]:values){(void)key_name;if(entry.type!=1 || entry.bytes.size()<2)continue;std::u16string path;for(std::size_t i=0;i+1<entry.bytes.size();i+=2){const auto ch=static_cast<char16_t>(entry.bytes[i]|(entry.bytes[i+1]<<8));if(!ch)break;path.push_back(ch);}if(path.empty())continue;const auto file=filesystem_.open(utf16_to_utf8(path));if(file.error)continue;auto blob=std::make_shared<FontBlob>();blob->bytes=*file.bytes;blob->fonts=std::make_shared<FontSet>();blob->fonts->faces=native_load_font_faces(blob->bytes);blob->loaded=true;blob->key=bridge.next_file++;bridge.files.emplace(blob->key,blob);for(std::size_t i=0;i<blob->fonts->faces.size();++i){value.fonts->faces.push_back(blob->fonts->faces[i]);value.fonts->sources.emplace_back(blob,i);}}}complete_set(*value.fonts);
                }
                out(output,create(std::move(value)));return finish(0);
            }
            if(method=="CreateFontCollectionFromFontSet"){
                if(!arg(2))return finish(pointer_error);out(2,0);const auto set=bridge.objects.find(arg(1));if(set==bridge.objects.end() || set->second.kind!=Kind::FontSet)return finish(invalid);Object value;value.kind=Kind::FontCollection;value.fonts=set->second.fonts;out(2,create(std::move(value)));return finish(0);
            }
            if(method=="CreateFontFaceReferenceFromFile" || method=="CreateFontFaceReferenceFromPath"){
                const bool file=method.ends_with("File");const unsigned output=file?4:5;if(!arg(output))return finish(pointer_error);out(output,0);const auto index=arg(file?2:3),sim=arg(file?3:4);if(sim>3)return finish(invalid);std::shared_ptr<FontBlob> blob;
                if(file){const auto found_file=bridge.objects.find(arg(1));if(found_file==bridge.objects.end() || found_file->second.kind!=Kind::FontFile)return finish(invalid);blob=found_file->second.blob;if(!blob->loaded)return begin_file(FontOperationKind::Reference,arg(1),arg(0));}
                else {auto result=open_font(arg(1),arg(2));if(result.second)return finish(result.second);blob=std::move(result.first);}
                if(!blob->fonts || index>=blob->fonts->faces.size())return finish(0x88985000U);auto value=font_object(Kind::FontFaceReference,blob->fonts,static_cast<std::size_t>(index));value.blob=blob;value.file_index=static_cast<std::size_t>(index);value.simulations=static_cast<unsigned>(sim);out(output,create(std::move(value)));return finish(0);
            }
            if(method=="GetSystemFontFallback" || method=="CreateFontFallbackBuilder"){
                if(!arg(1))return finish(pointer_error);Object value;value.kind=method=="GetSystemFontFallback"?Kind::FontFallback:Kind::FontFallbackBuilder;if(value.kind==Kind::FontFallback){FontMapping mapping;mapping.system=true;mapping.fonts=system();value.mappings.push_back(std::move(mapping));}out(1,create(std::move(value)));return finish(0);
            }
            if(method=="GetFontDownloadQueue"){
                if(!arg(1))return finish(pointer_error);auto queue=std::find_if(o.retained.begin(),o.retained.end(),[&](auto handle){return bridge.objects.at(handle).kind==Kind::FontDownloadQueue;});std::uint64_t handle;
                if(queue==o.retained.end()){Object value;value.kind=Kind::FontDownloadQueue;handle=create(std::move(value));o.retained.insert(handle);}else handle=*queue;++bridge.objects.at(handle).references;out(1,handle);return finish(0);
            }
        }
        if(o.kind==Kind::RenderingParams){if(method=="GetGridFitMode")return finish(o.grid_fit);if(method=="GetRenderingMode1")return finish(o.rendering_mode);}
        if(o.kind==Kind::FontCollection && method=="GetFontSet"){if(!arg(1))return finish(pointer_error);Object value;value.kind=Kind::FontSet;value.fonts=o.fonts;out(1,create(std::move(value)));return finish(0);}
        if(o.kind==Kind::FontFamily){
            if(method=="GetFontLocality"){if(arg(1)>=o.indices.size())return finish(0);return finish(2);}
            if(method=="GetFontFaceReference"){if(!arg(2))return finish(pointer_error);out(2,0);if(arg(1)>=o.indices.size())return finish(invalid);out(2,create(font_object(Kind::FontFaceReference,o.fonts,o.indices[arg(1)])));return finish(0);}
        }
        if(o.kind==Kind::FontSet){
            if(method=="GetFontCount")return finish(o.fonts->faces.size());
            if(method=="GetFontFaceReference"){if(!arg(2))return finish(pointer_error);out(2,0);if(arg(1)>=o.fonts->faces.size())return finish(invalid);out(2,create(font_object(Kind::FontFaceReference,o.fonts,static_cast<std::size_t>(arg(1)))));return finish(0);}
            if(method=="FindFontFaceReference" || method=="FindFontFace"){
                if(!arg(2) || !arg(3))return finish(pointer_error);const auto other=bridge.objects.find(arg(1));if(other==bridge.objects.end() || other->second.kind!=(method=="FindFontFace"?Kind::FontFace:Kind::FontFaceReference))return finish(invalid);std::uint32_t index=UINT32_MAX;
                for(std::size_t i=0;i<o.fonts->faces.size();++i){auto candidate=font_object(Kind::FontFaceReference,o.fonts,i);if(i<o.fonts->sources.size()){candidate.blob=o.fonts->sources[i].first;candidate.file_index=o.fonts->sources[i].second;}if(same_font(candidate,other->second)){index=static_cast<std::uint32_t>(i);break;}}
                memory.store(GuestAddress{arg(2)},32,index);memory.store(GuestAddress{arg(3)},32,index!=UINT32_MAX);return finish(0);
            }
            if(method=="GetPropertyValues" || method=="GetPropertyValuesAll" || method=="GetPropertyValuesLocales"){
                const bool one=method=="GetPropertyValues";const unsigned output=one?4:method=="GetPropertyValuesLocales"?3:2;const auto id=static_cast<unsigned>(arg(one?2:1));if(!arg(output) || (one && !arg(3)))return finish(pointer_error);out(output,0);if(!id || id>13 || (one && arg(1)>=o.fonts->faces.size()))return finish(invalid);Object value;value.kind=one?Kind::LocalizedStrings:Kind::StringList;
                std::u16string preferred=method=="GetPropertyValuesLocales" && arg(2)?folded(read_utf16(memory,GuestAddress{arg(2)},32768)):u"";
                if(one){value.localized=property_values(*o.fonts,static_cast<std::size_t>(arg(1)),id);memory.store(GuestAddress{arg(3)},32,!value.localized.empty());}
                else {std::set<std::u16string> seen;for(std::size_t i=0;i<o.fonts->faces.size();++i){const auto values=property_values(*o.fonts,i,id);if(values.empty())continue;auto selected=values.begin();if(!preferred.empty())for(const auto& item:values)if(preferred.find(folded(item.locale))!=std::u16string::npos){selected=std::find_if(values.begin(),values.end(),[&](const auto& v){return v.locale==item.locale;});break;}if(seen.insert(selected->value).second)value.localized.push_back(*selected);}}
                if(!one || !value.localized.empty())out(output,create(std::move(value)));return finish(0);
            }
            if(method=="GetPropertyOccurrenceCount" || method=="GetMatchingFonts" || method=="GetMatchingFontsFamily"){
                const bool occurrence=method=="GetPropertyOccurrenceCount",family=method=="GetMatchingFontsFamily";const unsigned output=occurrence?2:family?5:3;if(!arg(output))return finish(pointer_error);if(!occurrence)out(output,0);
                const auto filters=family?std::map<unsigned,std::vector<NativeFontName>>{}:read_properties(arg(1),occurrence?1U:static_cast<unsigned>(arg(2)));const auto requested=family && arg(1)?folded(read_utf16(memory,GuestAddress{arg(1)},32768)):u"";if(family && (!arg(1) || arg(2)<1 || arg(2)>999 || arg(3)<1 || arg(3)>9 || arg(4)>2))return finish(invalid);
                auto selected=std::make_shared<FontSet>();for(std::size_t i=0;i<o.fonts->faces.size();++i){bool match=true;const auto& face=o.fonts->faces[i];if(family)match=folded(face.family)==requested && static_cast<unsigned>(face.weight)==arg(2) && font_stretch(face)==arg(3) && (face.italic?2U:0U)==arg(4);
                    for(const auto& [id,values]:filters){const auto actual=property_values(*o.fonts,i,id);bool exists=false;for(const auto& wanted:values)for(const auto& item:actual)if(folded(wanted.value)==folded(item.value) && (wanted.locale.empty() || folded(wanted.locale)==folded(item.locale)))exists=true;match&=exists;}
                    if(!match)continue;selected->faces.push_back(face);selected->sources.push_back(i<o.fonts->sources.size()?o.fonts->sources[i]:std::pair<std::shared_ptr<FontBlob>,std::size_t>{});selected->properties.push_back(i<o.fonts->properties.size()?o.fonts->properties[i]:std::map<unsigned,std::vector<NativeFontName>>{});selected->simulations.push_back(i<o.fonts->simulations.size()?o.fonts->simulations[i]:0);}
                if(occurrence){memory.store(GuestAddress{arg(output)},32,selected->faces.size());return finish(0);}complete_set(*selected);Object value;value.kind=Kind::FontSet;value.fonts=std::move(selected);out(output,create(std::move(value)));return finish(0);
            }
        }
        if(o.kind==Kind::FontSetBuilder){
            if(method=="CreateFontSet"){if(!arg(1))return finish(pointer_error);Object value;value.kind=Kind::FontSet;value.fonts=std::make_shared<FontSet>(*o.fonts);complete_set(*value.fonts);out(1,create(std::move(value)));return finish(0);}
            const auto other=bridge.objects.find(arg(1));if(other==bridge.objects.end())return finish(invalid);
            if(method=="AddFontSet"){
                if(other->second.kind!=Kind::FontSet)return finish(invalid);const auto& set=*other->second.fonts;if(o.fonts->faces.size()+set.faces.size()>65536)return finish(0x8007000eU);auto copy=*o.fonts;for(std::size_t i=0;i<set.faces.size();++i){copy.faces.push_back(set.faces[i]);copy.sources.push_back(i<set.sources.size()?set.sources[i]:std::pair<std::shared_ptr<FontBlob>,std::size_t>{});copy.properties.push_back(i<set.properties.size()?set.properties[i]:std::map<unsigned,std::vector<NativeFontName>>{});copy.simulations.push_back(i<set.simulations.size()?set.simulations[i]:0);}*o.fonts=std::move(copy);return finish(0);
            }
            if(method=="AddFontFaceReference" || method=="AddFontFaceReferenceProperties"){
                if(other->second.kind!=Kind::FontFaceReference || !other->second.fonts)return finish(invalid);const auto props=method.ends_with("Properties")?read_properties(arg(2),static_cast<unsigned>(arg(3))):std::map<unsigned,std::vector<NativeFontName>>{};
                if(o.fonts->faces.size()>=65536)return finish(0x8007000eU);auto copy=*o.fonts;copy.faces.push_back(other->second.fonts->faces.at(other->second.indices.at(0)));copy.sources.emplace_back(blob_for(other->second),other->second.file_index);copy.properties.push_back(props);copy.simulations.push_back(other->second.simulations);*o.fonts=std::move(copy);return finish(0);
            }
        }
        if(o.kind==Kind::FontFaceReference){
            if(method=="GetFontFaceIndex")return finish(o.file_index);if(method=="GetSimulations")return finish(o.simulations);if(method=="GetLocality")return finish(o.blob && !o.blob->loaded?0:2);
            if(method=="Equals"){const auto other=bridge.objects.find(arg(1));return finish(other!=bridge.objects.end() && other->second.kind==o.kind && same_font(o,other->second));}
            if(method=="CreateFontFace" || method=="CreateFontFaceWithSimulations"){
                const bool sim=method.ends_with("Simulations");const unsigned output=sim?2:1;if(!arg(output))return finish(pointer_error);out(output,0);const auto simulation=sim?arg(1):o.simulations;if(simulation>3)return finish(invalid);
                if(!o.fonts)return finish(0x88985000U);auto value=font_object(Kind::FontFace,o.fonts,o.indices.at(0));value.blob=o.blob;value.file_index=o.file_index;value.simulations=static_cast<unsigned>(simulation);out(output,create(std::move(value)));return finish(0);
            }
            if(method=="GetFontFile"){if(!arg(1))return finish(pointer_error);Object value;value.kind=Kind::FontFile;value.blob=blob_for(o);out(1,create(std::move(value)));return finish(0);}
            if(method=="GetLocalFileSize" || method=="GetFileSize"){const auto value=blob_for(o)->contents().size();if(x86_)state.reg(Gpr::Rdx)=value>>32;return finish(value);}
            if(method=="GetFileTime"){if(!arg(1))return finish(pointer_error);auto blob=blob_for(o);if(!blob->modified && blob->native_export){const auto file=publish_font_file(blob->contents());if(file.error)return finish(0x80070000U|file.error);blob->modified=file.metadata.modified;}memory.store(GuestAddress{arg(1)},64,blob->modified);return finish(blob->modified?0:0x80004001U);}
            if(method.starts_with("Enqueue")){
                const auto blob=blob_for(o);if(!blob->loaded)return finish(0x8898500dU);if(method=="EnqueueFileFragmentDownloadRequest"){const auto size=blob->contents().size();if(arg(1)>size || arg(2)>size-arg(1))return finish(invalid);}else if(method!="EnqueueFontDownloadRequest"){if(arg(2)>1048576 || (arg(2) && !arg(1)))return finish(invalid);if(arg(2))memory.check(GuestAddress{arg(1)},static_cast<std::size_t>(arg(2))*2,Access::Read);}
                // 本機完整檔案的請求不加入下載佇列；使用者字型已由真實 loader 載入。
                return finish(0);
            }
        }
        if(o.kind==Kind::FontDownloadQueue){
            if(method=="AddListener"){if(!arg(1) || !arg(2))return finish(pointer_error);memory.check(GuestAddress{arg(2)},4,Access::Write);if(o.listeners.size()>=65536 || !o.next_listener)return finish(0x8007000eU);const auto token=o.next_listener++;o.listeners.emplace(token,arg(1));memory.store(GuestAddress{arg(2)},32,token);bridge.deferred.push_back(font_callback(memory,unit,arg(1),1));return finish(0);}
            if(method=="RemoveListener"){const auto at=o.listeners.find(static_cast<std::uint32_t>(arg(1)));if(at==o.listeners.end())return finish(invalid);bridge.deferred.push_back(font_callback(memory,unit,at->second,2));o.listeners.erase(at);return finish(0);}
            if(method=="IsEmpty")return finish(1);if(method=="GetGenerationCount"){if(x86_)state.reg(Gpr::Rdx)=o.generation>>32;return finish(o.generation);}
            if(method=="BeginDownload")return finish(1);if(method=="CancelDownload")return finish(0); // 只有已完成本機檔案的 reference，沒有待下載資源。
        }
