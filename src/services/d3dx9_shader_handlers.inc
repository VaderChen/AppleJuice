        if(symbol=="D3DXFindShaderComment" || symbol=="D3DXGetShaderConstantTable" || symbol=="D3DXGetShaderConstantTableEx" || symbol=="D3DXGetPixelShaderProfile" ||
           symbol=="D3DXGetVertexShaderProfile" || symbol=="D3DXGetDriverLevel" || symbol=="D3DXDeclaratorFromFVF" || symbol=="D3DXFVFFromDeclarator"){
            shader_handled=true;mesh_handled=true;
            if(symbol=="D3DXFindShaderComment"){
                // (pFunction, FourCC, ppData, pSizeInBytes)：註解 token 為 0xFFFE|(長度<<16)，資料第一個 DWORD 是 FourCC；
                // SM2 以上的指令長度在位元 24～27，SM1 遇到第一個指令即停止。找到回 S_OK，否則 S_FALSE。
                const auto code=shader_code(arg(0));const auto major=(word(code,0)>>8)&0xffU;std::size_t at=4;result=1;
                while(at+4<=code.size()){
                    const auto token=word(code,at);if(token==0xffffU)break;
                    if((token&0xffffU)==0xfffeU){
                        const auto length=token>>16;
                        if(length && at+8<=code.size() && word(code,at+4)==u(1)){if(arg(2))store_pointer(arg(2),arg(0)+at+8);if(arg(3))memory.store(GuestAddress{arg(3)},32,(length-1)*4);result=0;break;}
                        at+=4+std::size_t{length}*4;continue;
                    }
                    if(major<2)break;at+=4+std::size_t{(token>>24)&0xfU}*4;
                }
            }else if(symbol.starts_with("D3DXGetShaderConstantTable")){
                // (pFunction[, Flags], ppConstantTable)：與 D3DXCompileShader 共用常數表物件；沒有 CTAB 時回 D3DXERR_INVALIDDATA。
                const unsigned out_index=symbol.ends_with("Ex")?2U:1U;output(out_index);
                try{auto table=make_constant_table(shader_code(arg(0)));store_pointer(arg(out_index),table->root);}catch(const std::invalid_argument&){result=0x88760b59U;}
            }else if(symbol=="D3DXGetPixelShaderProfile" || symbol=="D3DXGetVertexShaderProfile"){
                // 裝置能力回報 vs_3_0／ps_3_0（GetDeviceCaps 的 VertexShaderVersion／PixelShaderVersion），字串在程序內只建立一次。
                const bool pixel=symbol=="D3DXGetPixelShaderProfile";(void)get(arg(0),Kind::Device);
                const GuestAddress cell{runtime_base_.value+(pixel?216U:208U)};auto text=memory.load(cell,64);
                if(!text){const std::string_view value=pixel?"ps_3_0":"vs_3_0";std::vector<std::uint8_t> bytes(value.begin(),value.end());bytes.push_back(0);text=runtime_allocate(memory,bytes);memory.store(cell,64,text);}
                result=text;
            }else if(symbol=="D3DXGetDriverLevel"){(void)get(arg(0),Kind::Device);result=900;}
            else if(symbol=="D3DXDeclaratorFromFVF"){
                // 與 D3DXCreateMeshFVF 共用 FVF→宣告規則；無法表示的 FVF 回 D3DERR_INVALIDCALL。
                const auto decl=fvf_decl(u(0));require(arg(1));memory.write(GuestAddress{arg(1)},decl);
            }else{
                // 只有元素、順序與偏移能往返的宣告才有 FVF；空宣告為 0。
                const auto decl=declaration(arg(0));require(arg(1));const auto fvf=decl_fvf(decl);
                if(!fvf && decl.size()>8)result=0x8876086cU;else memory.store(GuestAddress{arg(1)},32,fvf);
            }
        }else if(symbol=="D3DXGetShaderInputSemantics" || symbol=="D3DXGetShaderOutputSemantics"){
            shader_handled=true;mesh_handled=true;auto semantics=graphics_legacy_semantics(shader_code(arg(0)),symbol=="D3DXGetShaderOutputSemantics");if(arg(1))memory.write(GuestAddress{arg(1)},std::span(reinterpret_cast<const std::uint8_t*>(semantics.data()),semantics.size()*8));if(arg(2))memory.store(GuestAddress{arg(2)},32,semantics.size());
        }else if(symbol=="D3DXGetShaderSize" || symbol=="D3DXGetShaderVersion"){
            shader_handled=true;mesh_handled=true;result=arg(0)?symbol=="D3DXGetShaderSize"?shader_code(arg(0)).size():memory.load(GuestAddress{arg(0)},32):0;
        }else if(symbol.starts_with("D3DXAssembleShader") || symbol.starts_with("D3DXPreprocessShader")){
            shader_handled=true;mesh_handled=true;const bool file=symbol.find("FromFile")!=symbol.npos;const unsigned shift=file?0:1;
            // D3DXPreprocessShader 與組譯同形但沒有 Flags：ppShaderText 位於 3（記憶體／資源版本再向後一格），只做前置處理並回傳以 NUL 結尾的文字。
            const bool preprocess=symbol.starts_with("D3DXPreprocessShader");const unsigned out_at=(preprocess?3:4)+shift,errors_at=out_at+1;
            output(out_at);if(arg(errors_at))output(errors_at);require(!arg(2+shift),unavailable);
            const auto load_source=[&](const std::string& path){const auto opened=filesystem_.open_reader(d3dx_path(path));require(!opened.error,0x80070000U|opened.error);const auto info=opened.file->information();require(!info.error && info.metadata.size<=16U*1024U*1024U);std::vector<std::uint8_t> data(info.metadata.size);const auto io=opened.file->read(data,0);require(!io.error && io.bytes==data.size());return data;};
            const auto name=file?(symbol.ends_with('W')?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)})):read_ansi(memory,arg(0))):std::string{"memory"};
            // FromResource 與記憶體版本同形（模組＋資源名稱取代資料指標＋長度）。
            auto source=file?load_source(name):symbol.find("FromResource")!=symbol.npos?d3dx_resource_bytes(arg(0),arg(1),10,symbol.ends_with('W')):read(arg(0),u(1));
            std::vector<std::pair<std::string,std::string>> macros;if(arg(1+shift))for(unsigned i=0;i<1024;++i){const auto at=arg(1+shift)+i*ps*2;const auto key=memory.load(GuestAddress{at},pb);if(!key)break;require(i<1023);const auto value=memory.load(GuestAddress{at+ps},pb);macros.emplace_back(read_ansi(memory,key),value?read_ansi(memory,value):std::string{});}
            const auto includes=[&](std::string_view child,bool local,std::string_view parent)->std::optional<HlslIncludeSource>{std::string path(child);if(local){const auto at=parent.find_last_of("/\\");if(at!=parent.npos)path=std::string(parent.substr(0,at+1))+path;}return HlslIncludeSource{path,load_source(path)};};
            auto assembled=preprocess?graphics_preprocess_shader(source,name,macros,includes):graphics_assemble_shader(source,name,macros,u(3+shift),includes);
            if(preprocess && !assembled.bytecode.empty())assembled.bytecode.push_back(0);
            if(!assembled.messages.empty()){if(std::getenv("X64ARM_TRACE_GRAPHICS_ERRORS"))std::fprintf(stderr,"D3DXAssembleShader: %s",assembled.messages.c_str());if(arg(errors_at)){assembled.messages.push_back(0);store_pointer(arg(errors_at),blob(std::span(reinterpret_cast<const std::uint8_t*>(assembled.messages.data()),assembled.messages.size()))->root);}}
            if(assembled.bytecode.empty())result=0x88760b59U;else store_pointer(arg(out_at),blob(assembled.bytecode)->root);
        }else if(symbol=="D3DXDisassembleShader"){
            // (pShader, EnableColorCode, pComments, ppDisassembly)：與 D3DDisassemble 共用反組譯；EnableColorCode 對應 D3D_DISASM_ENABLE_COLOR_CODE，文字以 NUL 結尾。
            shader_handled=true;mesh_handled=true;output(3);require(arg(0));const auto code=shader_code(arg(0));const auto comments=arg(2)?read_ansi(memory,arg(2)):std::string{};
            std::string text;try{text=graphics_disassemble_shader(code,u(1)?1U:0U,comments);}catch(const std::invalid_argument&){throw Failure{invalid};}
            text.push_back(0);store_pointer(arg(3),blob(std::span(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()))->root);
        }else if(symbol=="D3DXGetShaderSamplers"){
            // (pFunction, pSamplers, pCount)：CTAB 註解中 RegisterSet 為 D3DXRS_SAMPLER（3）的常數名稱，依常數表順序；名稱指標指向著色器內的字串。沒有常數表時為 0 個。
            shader_handled=true;mesh_handled=true;require(arg(0));const auto code=shader_code(arg(0));std::uint32_t count=0;
            for(std::size_t at=4;at+4<=code.size();){
                const auto token=word(code,at);if(token==0xffffU)break;
                if((token&0xffffU)==0xfffeU){
                    const auto length=token>>16;
                    if(length>=8 && at+8+std::size_t{length-1}*4<=code.size() && word(code,at+4)==0x42415443U){
                        const auto table=at+8;const auto size=std::size_t{length-1}*4;const auto constants=word(code,table+12),info=word(code,table+16);
                        require(std::uint64_t{info}+std::uint64_t{constants}*20<=size,0x88760b59U);
                        for(std::uint32_t i=0;i<constants;++i){
                            const auto entry=table+info+i*20;if((word(code,entry+4)&0xffffU)!=3)continue;
                            if(arg(1))store_pointer(arg(1)+std::uint64_t{count}*ps,arg(0)+table+word(code,entry));++count;
                        }
                        break;
                    }
                    at+=4+std::size_t{length}*4;continue;
                }
                const auto major=(word(code,0)>>8)&0xffU;if(major<2)break;at+=4+std::size_t{(token>>24)&15U}*4;
            }
            if(arg(2))memory.store(GuestAddress{arg(2)},32,count);result=0;
        }else if(symbol.starts_with("D3DXCompileShader")){
            shader_handled=true;mesh_handled=true;const bool file=symbol.find("FromFile")!=symbol.npos;const unsigned shift=file?0:1;output(6+shift);if(arg(7+shift))output(7+shift);if(arg(8+shift))output(8+shift);
            require(arg(3+shift) && arg(4+shift));require(!arg(2+shift),unavailable);
            const auto load_source=[&](const std::string& path){const auto opened=filesystem_.open_reader(d3dx_path(path));require(!opened.error,0x80070000U|opened.error);const auto info=opened.file->information();require(!info.error && info.metadata.size<=16U*1024U*1024U);std::vector<std::uint8_t> data(info.metadata.size);const auto io=opened.file->read(data,0);require(!io.error && io.bytes==data.size());return data;};
            const auto name=file?(symbol.ends_with('W')?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)})):read_ansi(memory,arg(0))):std::string{"memory"};
            // FromResource 與記憶體版本同形（模組＋資源名稱取代資料指標＋長度）。
            auto source=file?load_source(name):symbol.find("FromResource")!=symbol.npos?d3dx_resource_bytes(arg(0),arg(1),10,symbol.ends_with('W')):read(arg(0),u(1));
            std::vector<std::pair<std::string,std::string>> macros;if(arg(1+shift))for(unsigned i=0;i<1024;++i){const auto at=arg(1+shift)+i*ps*2;const auto key=memory.load(GuestAddress{at},pb);if(!key)break;require(i<1023);const auto value=memory.load(GuestAddress{at+ps},pb);macros.emplace_back(read_ansi(memory,key),value?read_ansi(memory,value):std::string{});}
            const auto includes=[&](std::string_view child,bool local,std::string_view parent)->std::optional<HlslIncludeSource>{std::string path(child);if(local){const auto at=parent.find_last_of("/\\");if(at!=parent.npos)path=std::string(parent.substr(0,at+1))+path;}return HlslIncludeSource{path,load_source(path)};};
            auto compiled=graphics_compile_hlsl_includes(source,name,read_ansi(memory,arg(3+shift)),read_ansi(memory,arg(4+shift)),macros,u(5+shift),includes);
            // File 入口的 Flags 位於 5，ppShader 位於 6；Memory 入口再向後一格。
            if(!compiled.messages.empty() && arg(7+shift)){compiled.messages.push_back(0);store_pointer(arg(7+shift),blob(std::span(reinterpret_cast<const std::uint8_t*>(compiled.messages.data()),compiled.messages.size()))->root);}
            if(compiled.bytecode.empty())result=0x88760b59U;
            else{auto code=blob(compiled.bytecode);std::shared_ptr<Object> table;
                try{if(arg(8+shift)){table=make_constant_table(compiled.bytecode);store_pointer(arg(8+shift),table->root);}store_pointer(arg(6+shift),code->root);}catch(...){if(table)release(table->root);release(code->root);throw;}
            }
        }else if(symbol.starts_with("ConstantTable.")){
            shader_handled=true;mesh_handled=true;auto table=get(arg(0),Kind::ConstantTable);auto& data=*table->constant_table;const auto name=symbol.substr(14);
            const auto handle=[&](unsigned index){return table->mapped+table->pixels.size()+std::uint64_t{index}*4;};
            const auto lookup=[&](std::uint64_t pointer)->unsigned{const auto begin=table->mapped+table->pixels.size();if(pointer>=begin && pointer<begin+data.nodes.size()*4 && (pointer-begin)%4==0)return static_cast<unsigned>((pointer-begin)/4);if(!pointer)return UINT32_MAX;const auto text=read_ansi(memory,pointer);for(auto i:data.roots)if(data.nodes[i].name==text)return i;return UINT32_MAX;};
            if(name=="AddRef")result=hold(table);else if(name=="Release")result=release(table->root);
            else if(name=="QueryInterface"){output(2);Guid requested{};memory.read(GuestAddress{arg(1)},requested);constexpr Guid iid{0x8f,0x75,0x3c,0xab,0x3e,0x09,0x56,0x43,0xb7,0x62,0x4d,0xb1,0x8f,0x1b,0x3a,0x01};if(requested==unknown || requested==iid){hold(table);store_pointer(arg(2),table->root);}else result=no_interface;}
            else if(name=="GetBufferPointer")result=table->mapped;
            else if(name=="GetBufferSize")result=table->pixels.size();
            else if(name=="GetDesc"){store_pointer(arg(1),table->mapped+data.creator);memory.store(GuestAddress{arg(1)+ps},32,data.version);memory.store(GuestAddress{arg(1)+ps+4},32,data.roots.size());}
            else if(name=="GetConstant" || name=="GetConstantElement" || name=="GetConstantByName"){
                const auto parent=lookup(arg(1));const auto& children=parent==UINT32_MAX?data.roots:name=="GetConstantElement"?data.nodes[parent].array:data.nodes[parent].members;result=0;
                if(name=="GetConstantByName"){require(arg(2));const auto found=data.find(parent,read_ansi(memory,arg(2)));if(found!=UINT32_MAX)result=handle(found);}
                else if(u(2)<children.size())result=handle(children[u(2)]);
            }else if(name=="GetSamplerIndex"){const auto i=lookup(arg(1));result=i!=UINT32_MAX && data.nodes[i].set==3?data.nodes[i].index:UINT32_MAX;}
            else if(name=="GetConstantDesc"){
                const auto i=lookup(arg(1));require(i!=UINT32_MAX && arg(3));if(arg(2)){require(memory.load(GuestAddress{arg(3)},32)>=1);const auto& n=data.nodes[i];std::vector<std::uint8_t> desc(x86_?48:56);const auto pointer=table->mapped+n.name_offset;std::memcpy(desc.data(),&pointer,ps);const std::array<unsigned,10> values{n.set,n.index,n.registers,n.klass,n.type,n.rows,n.columns,n.elements,static_cast<unsigned>(n.members.size()),n.bytes};for(unsigned j=0;j<values.size();++j)put(desc,ps+j*4,values[j]);const auto defaults=n.default_offset?table->mapped+n.default_offset:0;std::memcpy(desc.data()+ps+40,&defaults,ps);memory.write(GuestAddress{arg(2)},desc);}memory.store(GuestAddress{arg(3)},32,1);
            }else if(name=="SetDefaults"){
                auto device=get(arg(1),Kind::Device);const unsigned stage=(data.version>>16)==0xffff?1:0;for(auto i:data.roots){const auto& n=data.nodes[i];if(!n.default_offset || n.set==3)continue;const auto kind=n.set==2?0U:n.set==1?1U:2U,stride=n.set==0?4U:16U;const unsigned limit=d9_constant_limit(stage,kind);require(n.index<=limit && n.registers<=limit-n.index);const auto length=n.registers*stride;require(n.default_offset<=data.data.size() && length<=data.data.size()-n.default_offset);set_constants(*device,stage,kind,n.index,std::span(data.data).subspan(n.default_offset,length));}
            }else if(name.starts_with("Set")){
                auto device=get(arg(1),Kind::Device);const auto i=lookup(arg(2));require(i!=UINT32_MAX);const auto& n=data.nodes[i];require(n.set!=3 && n.klass!=5,unavailable);const unsigned stage=(data.version>>16)==0xffff?1:0,kind=n.set==2?0U:n.set==1?1U:2U,stride=kind==2?4U:16U,limit=kind?16U:stage?224U:256U;require(n.index<=limit && n.registers<=limit-n.index);auto values=(device->recording?device->recording->constants:device->constants).read(stage,kind);
                const bool matrix=name.find("Matrix")!=name.npos,transpose=name.find("Transpose")!=name.npos,pointers=name.find("Pointer")!=name.npos,vector=name.find("Vector")!=name.npos,array=name.find("Array")!=name.npos;
                const unsigned items=std::min(n.elements,array?u(4):1U);std::vector<std::uint8_t> source;
                unsigned components=n.rows*n.columns*items;
                if(name=="SetValue"){components=std::min(n.bytes,u(4))/4;source=read(arg(3),components*4);}
                else if(matrix || vector)source=pointers?std::vector<std::uint8_t>{}:read(arg(3),std::size_t(items)*(matrix?64:16));
                else if(array){components=std::min(n.bytes/4,u(4));source=read(arg(3),components*4);}
                else{source.resize(4);put(source,0,name=="SetFloat"?bits(scalar_arg(3)):u(3));components=1;}
                for(unsigned c=0;c<components;++c){const unsigned element=c/(n.rows*n.columns),within=c%(n.rows*n.columns),row=within/n.columns,column=within%n.columns;unsigned raw=0;
                    if(matrix){const auto offset=(transpose?column*4+row:row*4+column)*4;if(pointers){const auto pointer=memory.load(GuestAddress{arg(3)+element*ps},pb);raw=static_cast<unsigned>(memory.load(GuestAddress{pointer+offset},32));}else raw=word(source,element*64+offset);}
                    else raw=word(source,vector?element*16+within*4:c*4);
                    const bool input_float=matrix || vector || name.find("Float")!=name.npos || (name=="SetValue" && n.type==3),input_bool=name.find("Bool")!=name.npos || (name=="SetValue" && n.type==1);const double number=input_float?static_cast<double>(real(raw)):input_bool?static_cast<double>(raw!=0):static_cast<double>(static_cast<std::int32_t>(raw));
                    const unsigned registers=n.klass==2?n.rows:n.klass==3?n.columns:1;const unsigned reg=n.index+(kind==2?c:element*registers+(n.klass==3?column:row)),component=kind==2?0:n.klass==3?row:column;require(reg<n.index+n.registers);const unsigned converted=kind==0?bits(static_cast<float>(number)):kind==2?number!=0:static_cast<unsigned>(static_cast<std::int32_t>(number));put(values,reg*stride+component*4,converted);
                }
                set_constants(*device,stage,kind,n.index,std::span(values).subspan(n.index*stride,n.registers*stride));
            }else throw HostCallFault("尚未實作 D3DX9 ConstantTable 方法："+std::string(name));
        }
