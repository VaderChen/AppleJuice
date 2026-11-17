        bool effect_handled=symbol.starts_with("Effect.") || symbol.starts_with("EffectPool.") || symbol.starts_with("EffectCompiler.") || symbol.starts_with("D3DXCreateEffect") || symbol=="D3DXDisassembleEffect";
        const auto fx_memory=[&](fx9::Runtime& fx,std::span<const std::uint8_t> bytes){const auto address=allocate_heap(memory,std::max<std::size_t>(8,bytes.size()));require(address,out_of_memory);fx.allocations.push_back(address);if(!bytes.empty())memory.write(GuestAddress{address},bytes);return address;};
        const auto fx_string=[&](fx9::Runtime& fx,const std::string& text){auto it=fx.strings.find(text);if(it!=fx.strings.end())return it->second;std::vector<std::uint8_t> bytes(text.begin(),text.end());bytes.push_back(0);auto pointer=fx_memory(fx,bytes);fx.strings[text]=pointer;return pointer;};
        const auto fx_handles=[&](fx9::Runtime& fx){for(unsigned i=0;i<fx.nodes.size();++i){const auto pointer=fx_memory(fx,std::vector<std::uint8_t>(4));fx.handles[pointer]=i;fx.pointers.push_back(pointer);}};
        const auto fx_lookup=[&](fx9::Runtime& fx,std::uint64_t pointer){if(!pointer)return UINT32_MAX;if(auto it=fx.handles.find(pointer);it!=fx.handles.end())return it->second;return fx.find(UINT32_MAX,read_ansi(memory,pointer));};
        const auto fx_shader=[&](const std::shared_ptr<Object>& effect,unsigned id){auto& fx=*effect->effect;if(!id)return std::uint64_t{};if(auto it=fx.shaders.find(id);it!=fx.shaders.end())return it->second;auto it=fx.program.objects.find(id);require(it!=fx.program.objects.end());const auto& code=it->second;require(code.size()>=4);const auto version=word(code,0);const bool pixel=(version>>16)==0xffff;require(pixel || (version>>16)==0xfffe);auto device=get(effect->parent,Kind::Device);auto native=device->native->graphics_shader(pixel?1:0,code);auto table=std::make_shared<d3dx9::Constants>(code,true);auto shader=create(pixel?Kind::PixelShader:Kind::VertexShader,device->root);try{shader->shader=std::move(native);shader->pixels=code;shader->constant_table=std::move(table);fx.shaders[id]=shader->root;}catch(...){release(shader->root);throw;}return shader->root;};
#include "d3dx9_effect_apply.inc"
        // ID3DXEffectCompiler 共用：原始碼檔案（相對目前目錄）、#include 載入，以及前置處理後原始碼的函式清單。
        const auto fx_source_file=[&](const std::string& path){const auto opened=filesystem_.open_reader(d3dx_path(path));require(!opened.error,0x80070000U|opened.error);const auto info=opened.file->information();require(!info.error && info.metadata.size<=64U*1024U*1024U);std::vector<std::uint8_t> bytes(info.metadata.size);const auto io=opened.file->read(bytes,0);require(!io.error && io.bytes==bytes.size());return bytes;};
        const auto fx_includes=[&](std::string_view child,bool local,std::string_view parent)->std::optional<HlslIncludeSource>{std::string path(child);if(local){const auto at=parent.find_last_of("/\\");if(at!=parent.npos)path=std::string(parent.substr(0,at+1))+path;}return HlslIncludeSource{path,fx_source_file(path)};};
        // 最外層（大括號與小括號之外）出現「型別 名稱 (」者是函式；常數建構（= float4(...)）與 technique 內的 compile 呼叫不算。
        const auto fx_function_names=[](std::span<const std::uint8_t> text){
            std::vector<std::string> names,tokens;std::size_t i=0;int braces=0,parens=0;
            static const std::set<std::string> keywords{"if","for","while","do","switch","return","compile","asm","technique","technique10","technique11","pass","sampler_state","stateblock_state","struct","typedef","register","packoffset"};
            const auto identifier=[](const std::string& token){return !token.empty() && (std::isalpha(static_cast<unsigned char>(token[0])) || token[0]=='_');};
            while(i<text.size()){
                const char c=static_cast<char>(text[i]);
                if(std::isspace(static_cast<unsigned char>(c))){++i;continue;}
                if(c=='/' && i+1<text.size() && text[i+1]=='/'){while(i<text.size() && text[i]!='\n')++i;continue;}
                if(c=='/' && i+1<text.size() && text[i+1]=='*'){i+=2;while(i+1<text.size() && !(text[i]=='*' && text[i+1]=='/'))++i;i+=2;continue;}
                if(c=='"'){++i;while(i<text.size() && text[i]!='"'){if(text[i]=='\\')++i;++i;}++i;tokens.emplace_back("\"");continue;}
                if(c=='#'){while(i<text.size() && text[i]!='\n')++i;continue;}
                std::string token;
                if(std::isalnum(static_cast<unsigned char>(c)) || c=='_'){while(i<text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) || text[i]=='_' || text[i]=='.'))token.push_back(static_cast<char>(text[i++]));}
                else {token.assign(1,c);++i;}
                if(token=="{")++braces;else if(token=="}")--braces;
                else if(token=="("){
                    const auto size=tokens.size();
                    if(!braces && !parens && size>=2 && identifier(tokens[size-1]) && identifier(tokens[size-2]) && !keywords.contains(tokens[size-1]) && !keywords.contains(tokens[size-2]) &&
                       std::find(names.begin(),names.end(),tokens[size-1])==names.end())names.push_back(tokens[size-1]);
                    ++parens;
                }else if(token==")")--parens;
                tokens.push_back(std::move(token));if(tokens.size()>4)tokens.erase(tokens.begin());
            }
            return names;
        };
        if(effect_handled){
            if(symbol=="D3DXCreateEffectPool"){output(0);auto pool=create(Kind::EffectPool);pool->effect_pool=std::make_shared<fx9::Pool>();store_pointer(arg(0),pool->root);}
            else if(symbol=="D3DXDisassembleEffect"){
                // (Effect, EnableColorCode, ppDisassembly)：列出參數宣告、technique／pass 與每個狀態指派；著色器以 D3DXDisassembleShader 的同一反組譯器展開，
                // 參數引用與運算式指派列出引用名稱。狀態名稱依 FX2 公開狀態序號（與 d3dx9_effect_states.inc 相同的表）。
                output(2);auto effect=get(arg(0),Kind::Effect);auto& fx=*effect->effect;const unsigned color=u(1)?1U:0U;
                static constexpr const char* state_names[]{"ZEnable","FillMode","ShadeMode","ZWriteEnable","AlphaTestEnable","LastPixel","SrcBlend","DestBlend","CullMode","ZFunc","AlphaRef","AlphaFunc","DitherEnable","AlphaBlendEnable","FogEnable","SpecularEnable","FogColor","FogTableMode","FogStart","FogEnd","FogDensity","RangeFogEnable","StencilEnable","StencilFail","StencilZFail","StencilPass","StencilFunc","StencilRef","StencilMask","StencilWriteMask","TextureFactor","Wrap0","Wrap1","Wrap2","Wrap3","Wrap4","Wrap5","Wrap6","Wrap7","Wrap8","Wrap9","Wrap10","Wrap11","Wrap12","Wrap13","Wrap14","Wrap15","Clipping","Lighting","Ambient","FogVertexMode","ColorVertex","LocalViewer","NormalizeNormals","DiffuseMaterialSource","SpecularMaterialSource","AmbientMaterialSource","EmissiveMaterialSource","VertexBlend","ClipPlaneEnable","PointSize","PointSize_Min","PointSize_Max","PointSpriteEnable","PointScaleEnable","PointScale_A","PointScale_B","PointScale_C","MultiSampleAntialias","MultiSampleMask","PatchEdgeStyle","DebugMonitorToken","IndexedVertexBlendEnable","ColorWriteEnable","TweenFactor","BlendOp","PositionDegree","NormalDegree","ScissorTestEnable","SlopeScaleDepthBias","AntialiasedLineEnable","MinTessellationLevel","MaxTessellationLevel","AdaptiveTess_X","AdaptiveTess_Y","AdaptiveTess_Z","AdaptiveTess_W","EnableAdaptiveTessellation","TwoSidedStencilMode","CCW_StencilFail","CCW_StencilZFail","CCW_StencilPass","CCW_StencilFunc","ColorWriteEnable1","ColorWriteEnable2","ColorWriteEnable3","BlendFactor","SRGBWriteEnable","DepthBias","SeparateAlphaBlendEnable","SrcBlendAlpha","DestBlendAlpha","BlendOpAlpha","ColorOp","ColorArg0","ColorArg1","ColorArg2","AlphaOp","AlphaArg0","AlphaArg1","AlphaArg2","ResultArg","BumpEnvMat00","BumpEnvMat01","BumpEnvMat10","BumpEnvMat11","TexCoordIndex","BumpEnvLScale","BumpEnvLOffset","TextureTransformFlags","Constant","NPatchMode","FVF","ProjectionTransform","ViewTransform","WorldTransform","TextureTransform","MaterialDiffuse","MaterialAmbient","MaterialSpecular","MaterialEmissive","MaterialPower","LightType","LightDiffuse","LightSpecular","LightAmbient","LightPosition","LightDirection","LightRange","LightFalloff","LightAttenuation0","LightAttenuation1","LightAttenuation2","LightTheta","LightPhi","LightEnable","VertexShader","PixelShader","VertexShaderConstantF","VertexShaderConstantB","VertexShaderConstantI","VertexShaderConstant","VertexShaderConstant1","VertexShaderConstant2","VertexShaderConstant3","VertexShaderConstant4","PixelShaderConstantF","PixelShaderConstantB","PixelShaderConstantI","PixelShaderConstant","PixelShaderConstant1","PixelShaderConstant2","PixelShaderConstant3","PixelShaderConstant4","Texture","AddressU","AddressV","AddressW","BorderColor","MagFilter","MinFilter","MipFilter","MipMapLodBias","MaxMipLevel","MaxAnisotropy","SRGBTexture","ElementIndex"};
                static constexpr const char* scalars[]{"void","bool","int","float","string","texture","texture1D","texture2D","texture3D","textureCUBE","sampler","sampler1D","sampler2D","sampler3D","samplerCUBE","pixelshader","vertexshader"};
                const auto type_name=[&](const fx9::Type& type){
                    if(type.klass==5)return std::string("struct");std::string name=type.scalar<std::size(scalars)?scalars[type.scalar]:"unknown";
                    if(type.klass==1)name+=std::to_string(type.columns);else if(type.klass==2 || type.klass==3)name+=std::to_string(type.rows)+"x"+std::to_string(type.columns);return name;};
                const auto parameter_declaration=[&](const fx9::Type& type){std::string text=type_name(type)+" "+type.name;if(type.elements)text+="["+std::to_string(type.elements)+"]";if(!type.semantic.empty())text+=" : "+type.semantic;return text+";";};
                std::string text="//\n// x64arm FX2 effect disassembly\n//\n// Parameters:\n//\n";
                for(const auto& parameter:fx.program.parameters)text+="//   "+parameter_declaration(parameter.type)+"\n";
                text+="//\n\n";
                const auto value_text=[&](const fx9::Assignment& assignment)->std::string{
                    if(assignment.kind==1)return assignment.parameter;
                    if(assignment.kind==2)return "<expression>";
                    std::size_t at=assignment.type_offset;const auto type=fx.program.type(at);const auto count=std::max(1U,type.rows*type.columns)*type.count();
                    if(type.klass==4)return "object "+std::to_string(fx9::Program::word(fx.program.data,assignment.value_offset));
                    std::string out;for(unsigned i=0;i<std::min(count,16U);++i){const auto raw=fx9::Program::word(fx.program.data,assignment.value_offset+i*4);
                        if(i)out+=", ";if(type.scalar==1)out+=raw?"true":"false";else if(type.scalar==3){char buffer[32];std::snprintf(buffer,sizeof buffer,"%g",static_cast<double>(real(raw)));out+=buffer;}else out+=std::to_string(static_cast<std::int32_t>(raw));}
                    return count>1?"{"+out+"}":out;};
                for(const auto& technique:fx.program.techniques){
                    text+="technique "+technique.name+"\n{\n";
                    for(const auto& pass:technique.passes){
                        text+="    pass "+pass.name+"\n    {\n";
                        for(const auto& assignment:pass.assignments){
                            std::string name=assignment.state<std::size(state_names)?state_names[assignment.state]:"State"+std::to_string(assignment.state);
                            if(assignment.index && assignment.state>=103)name+="["+std::to_string(assignment.index)+"]";
                            if((assignment.state==146 || assignment.state==147) && assignment.kind!=1 && assignment.kind!=2){
                                std::vector<std::uint8_t> code;
                                if(assignment.kind==0)code=assignment.code;
                                else {const auto id=fx9::Program::word(fx.program.data,assignment.value_offset);if(const auto it=fx.program.objects.find(id);it!=fx.program.objects.end())code=it->second;}
                                text+="        "+name+" =\n";
                                if(code.empty()){text+="            NULL;\n";continue;}
                                std::string listing;try{listing=graphics_disassemble_shader(code,color,{});}catch(const std::invalid_argument&){listing="// 無法反組譯的著色器\n";}
                                text+="            asm {\n";std::size_t start=0;
                                while(start<listing.size()){const auto end=listing.find('\n',start);const auto line=listing.substr(start,end==std::string::npos?std::string::npos:end-start);if(!line.empty())text+="                "+line+"\n";if(end==std::string::npos)break;start=end+1;}
                                text+="            };\n";
                            }else text+="        "+name+" = "+value_text(assignment)+";\n";
                        }
                        text+="    }\n";
                    }
                    text+="}\n\n";
                }
                text.push_back(0);store_pointer(arg(2),blob(std::span(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()))->root);
            }
            else if(symbol.starts_with("D3DXCreateEffectCompiler")){
                // (Data, Length | File | Module, Resource), Defines, Include, Flags, ppCompiler, ppParseErrors：原始碼先依 fx_2_0 編譯（驗證並取得參數與技術結構），
                // 原始碼、巨集與來源名稱保留給 CompileEffect／CompileShader 依新旗標重新編譯。ID3DXInclude 介面與 D3DXCreateEffect 一樣尚未支援。
                const bool file=symbol.find("FromFile")!=symbol.npos,resource=symbol.find("FromResource")!=symbol.npos;const unsigned shift=file?0:1;
                output(4+shift);if(arg(5+shift))output(5+shift);require(!arg(2+shift),unavailable);
                const auto name=file?(symbol.ends_with('W')?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)})):read_ansi(memory,arg(0))):std::string{resource?"resource":"memory"};
                auto bytes=file?fx_source_file(name):resource?d3dx_resource_bytes(arg(0),arg(1),10,symbol.ends_with('W')):read(arg(0),u(1));
                std::vector<std::pair<std::string,std::string>> macros;
                if(arg(1+shift))for(unsigned i=0;i<1024;++i){const auto at=arg(1+shift)+i*ps*2,key=memory.load(GuestAddress{at},pb);if(!key)break;require(i<1023);const auto value=memory.load(GuestAddress{at+ps},pb);macros.emplace_back(read_ansi(memory,key),value?read_ansi(memory,value):std::string{});}
                auto compiled=graphics_compile_hlsl_includes(bytes,name,"","fx_2_0",macros,u(3+shift),fx_includes);
                if(!compiled.messages.empty()){if(std::getenv("X64ARM_TRACE_GRAPHICS_ERRORS"))std::fprintf(stderr,"[d3dx9 effect compiler] %s\n",compiled.messages.c_str());
                    if(arg(5+shift)){compiled.messages.push_back(0);store_pointer(arg(5+shift),blob(std::span(reinterpret_cast<const std::uint8_t*>(compiled.messages.data()),compiled.messages.size()))->root);}}
                require(!compiled.bytecode.empty(),0x88760b59U);
                auto runtime=std::make_shared<fx9::Runtime>(compiled.bytecode);auto compiler=create(Kind::EffectCompiler);compiler->effect=runtime;runtime->flags=u(3+shift);
                try{
                    fx_handles(*runtime);compiler->fx_source=std::move(bytes);compiler->fx_source_name=name;compiler->fx_macros=std::move(macros);
                    const auto preprocessed=graphics_preprocess_shader(compiler->fx_source,name,compiler->fx_macros,fx_includes);
                    compiler->fx_functions=fx_function_names(preprocessed.bytecode);
                    for(unsigned i=0;i<compiler->fx_functions.size();++i)compiler->fx_function_handles[fx_memory(*runtime,std::vector<std::uint8_t>(4))]=i;
                }catch(...){release(compiler->root);throw;}
                store_pointer(arg(4+shift),compiler->root);
            }
            else if(symbol.starts_with("D3DXCreateEffect")){
                const bool file=symbol.find("FromFile")!=symbol.npos,resource=symbol.find("FromResource")!=symbol.npos;
                // Ex 版本在 pInclude 之後多一個 pSkipConstants，後續參數向後一格；略過常數清單需搭配效果集區的分享規則，尚未支援非空清單。
                const bool extended=symbol.ends_with("Ex") || symbol.ends_with("ExA") || symbol.ends_with("ExW");const unsigned shift=file?0:1,tail=shift+(extended?1U:0U);
                output(6+tail);if(arg(7+tail))output(7+tail);auto device=get(arg(0),Kind::Device);require(!arg(3+shift),unavailable);
                if(extended && arg(4+shift))require(!memory.load(GuestAddress{arg(4+shift)},8),unavailable);
                std::shared_ptr<Object> pool;if(arg(5+tail))pool=get(arg(5+tail),Kind::EffectPool);
                const auto load=[&](const std::string& path){const auto opened=filesystem_.open_reader(d3dx_path(path));require(!opened.error,0x80070000U|opened.error);const auto info=opened.file->information();require(!info.error && info.metadata.size<=64U*1024U*1024U);std::vector<std::uint8_t> bytes(info.metadata.size);const auto io=opened.file->read(bytes,0);require(!io.error && io.bytes==bytes.size());return bytes;};
                const auto name=file?(symbol.ends_with('W')?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(1)})):read_ansi(memory,arg(1))):std::string{"memory"};auto bytes=file?load(name):resource?d3dx_resource_bytes(arg(1),arg(2),10,symbol.ends_with('W')):read(arg(1),u(2));
                if(bytes.size()<4 || word(bytes,0)!=0xfeff0901){std::vector<std::pair<std::string,std::string>> macros;if(arg(2+shift))for(unsigned i=0;i<1024;++i){const auto at=arg(2+shift)+i*ps*2,key=memory.load(GuestAddress{at},pb);if(!key)break;require(i<1023);const auto value=memory.load(GuestAddress{at+ps},pb);macros.emplace_back(read_ansi(memory,key),value?read_ansi(memory,value):std::string{});}
                    const auto includes=[&](std::string_view child,bool local,std::string_view parent)->std::optional<HlslIncludeSource>{std::string path(child);if(local){const auto at=parent.find_last_of("/\\");if(at!=parent.npos)path=std::string(parent.substr(0,at+1))+path;}return HlslIncludeSource{path,load(path)};};
                    auto compiled=graphics_compile_hlsl_includes(bytes,name,"","fx_2_0",macros,u(4+tail),includes);if(!compiled.messages.empty()){if(std::getenv("X64ARM_TRACE_GRAPHICS_ERRORS"))std::fprintf(stderr,"[d3dx9 effect] %s\n",compiled.messages.c_str());if(arg(7+tail)){compiled.messages.push_back(0);store_pointer(arg(7+tail),blob(std::span(reinterpret_cast<const std::uint8_t*>(compiled.messages.data()),compiled.messages.size()))->root);}}require(!compiled.bytecode.empty(),0x88760b59U);bytes=std::move(compiled.bytecode);
                }
                auto runtime=std::make_shared<fx9::Runtime>(bytes,pool?pool->effect_pool.get():nullptr);auto effect=create(Kind::Effect,device->root);effect->effect=runtime;runtime->flags=u(4+tail);if(pool){hold(pool);runtime->pool=pool->root;}try{fx_handles(*runtime);}catch(...){release(effect->root);throw;}store_pointer(arg(6+tail),effect->root);
            }else {
                const bool pool=symbol.starts_with("EffectPool."),compiler=symbol.starts_with("EffectCompiler.");auto effect=get(arg(0),pool?Kind::EffectPool:compiler?Kind::EffectCompiler:Kind::Effect);const auto method=symbol.substr(symbol.find('.')+1);
                if(method=="QueryInterface"){output(2);Guid requested{};memory.read(GuestAddress{arg(1)},requested);constexpr Guid base{0xac,0x18,0x7c,0x01,0x3f,0x10,0x17,0x44,0x8c,0x51,0x6b,0xf6,0xef,0x1e,0x56,0xbe};if(requested==unknown || requested==iid(effect->kind) || (!pool && requested==base)){hold(effect);store_pointer(arg(2),effect->root);}else result=no_interface;}
                else if(method=="AddRef")result=hold(effect);else if(method=="Release")result=release(effect->root);
                else {require(!pool);auto& fx=*effect->effect;
                    if(compiler && (method=="SetLiteral" || method=="GetLiteral" || method=="CompileEffect" || method=="CompileShader" || method.starts_with("GetFunction"))){
                        const auto function_index=[&](std::uint64_t handle)->std::optional<unsigned>{const auto it=effect->fx_function_handles.find(handle);if(it==effect->fx_function_handles.end())return std::nullopt;return it->second;};
                        const auto function_handle=[&](unsigned index){for(const auto& [pointer,value]:effect->fx_function_handles)if(value==index)return pointer;return std::uint64_t{};};
                        const auto messages=[&](GraphicsCompilation& compiled,std::uint64_t pointer){if(compiled.messages.empty() || !pointer)return;compiled.messages.push_back(0);store_pointer(pointer,blob(std::span(reinterpret_cast<const std::uint8_t*>(compiled.messages.data()),compiled.messages.size()))->root);};
                        if(method=="GetFunction")result=u(1)<effect->fx_functions.size()?function_handle(u(1)):0;
                        else if(method=="GetFunctionByName"){result=0;if(arg(1)){const auto name=read_ansi(memory,arg(1));for(unsigned i=0;i<effect->fx_functions.size();++i)if(effect->fx_functions[i]==name){result=function_handle(i);break;}}}
                        else if(method=="GetFunctionDesc"){
                            // D3DXFUNCTION_DESC：Name（指向效果自有字串）、Annotations（函式沒有註解）。
                            const auto index=function_index(arg(1));require(index && arg(2));memory.store(GuestAddress{arg(2)},pb,fx_string(fx,effect->fx_functions[*index]));memory.store(GuestAddress{arg(2)+ps},32,0);
                        }else if(method=="SetLiteral" || method=="GetLiteral"){
                            // 只有頂層參數可設為 literal；literal 只是編譯期常數提示，輸出的效果仍以目前值作為預設值。
                            const auto index=fx_lookup(fx,arg(1));require(index!=UINT32_MAX && !fx.nodes.at(index).kind && std::find(fx.roots.begin(),fx.roots.end(),index)!=fx.roots.end());
                            if(method=="SetLiteral"){if(u(2))effect->fx_literals.insert(index);else effect->fx_literals.erase(index);}
                            else {require(arg(2));memory.store(GuestAddress{arg(2)},32,effect->fx_literals.contains(index)?1U:0U);}
                        }else if(method=="CompileEffect"){
                            // (Flags, ppEffect, ppErrorMsgs)：依新旗標重新編譯，再把目前的數值參數依名稱與型別寫回輸出的預設值。
                            output(2);if(arg(3))output(3);
                            auto compiled=graphics_compile_hlsl_includes(effect->fx_source,effect->fx_source_name,"","fx_2_0",effect->fx_macros,u(1),fx_includes);messages(compiled,arg(3));
                            require(!compiled.bytecode.empty(),0x88760b59U);auto bytes=std::move(compiled.bytecode);const fx9::Program program(bytes);
                            for(const auto& target:program.parameters){
                                if(target.type.klass==4 || !target.offset)continue;
                                for(const auto root:fx.roots){
                                    const auto& value=*fx.values.at(fx.nodes.at(root).index);
                                    if(value.type.name!=target.type.name || !fx9::Runtime::same_type(value.type,target.type) || value.bytes.size()!=target.values.size())continue;
                                    const std::size_t at=8+std::size_t{target.offset};require(at<=bytes.size() && value.bytes.size()<=bytes.size()-at);std::copy(value.bytes.begin(),value.bytes.end(),bytes.begin()+static_cast<std::ptrdiff_t>(at));break;
                                }
                            }
                            store_pointer(arg(2),blob(bytes)->root);
                        }else{
                            // CompileShader(hFunction, pTarget, Flags, ppShader, ppErrorMsgs, ppConstantTable)：hFunction 可為 GetFunction 的 handle 或函式名稱字串。
                            output(4);if(arg(5))output(5);if(arg(6))output(6);require(arg(1) && arg(2));
                            const auto index=function_index(arg(1));const auto entry=index?effect->fx_functions[*index]:read_ansi(memory,arg(1));
                            auto compiled=graphics_compile_hlsl_includes(effect->fx_source,effect->fx_source_name,entry,read_ansi(memory,arg(2)),effect->fx_macros,u(3),fx_includes);messages(compiled,arg(5));
                            if(compiled.bytecode.empty())result=0x88760b59U;
                            else{auto code=blob(compiled.bytecode);std::shared_ptr<Object> table;
                                try{if(arg(6)){table=make_constant_table(compiled.bytecode);store_pointer(arg(6),table->root);}store_pointer(arg(4),code->root);}catch(...){if(table)release(table->root);release(code->root);throw;}}
                        }
                    }else{
#include "d3dx9_effect_methods.inc"
                        // D3DXEFFECT_DESC.Functions：編譯器保有原始碼的函式清單（已編譯的效果沒有函式表）。
                        if(compiler && method=="GetDesc" && arg(1))memory.store(GuestAddress{arg(1)+ps+8},32,effect->fx_functions.size());
                    }
                }
            }
        }
