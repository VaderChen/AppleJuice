        // D3DX10／D3DX11 非同步資料載入器（ID3DX10DataLoader／ID3DX11DataLoader，供執行緒幫浦或客體自行呼叫）：
        // - 檔案載入器建立時只保存路徑，Load 才讀入整個檔案（客體 heap，再次 Load 重新讀取，Destroy 時歸還）；開不到檔案回傳 D3D10／D3D11_ERROR_FILE_NOT_FOUND。
        // - 記憶體載入器直接回傳原指標與長度；資源載入器建立時找 RT_RCDATA 資源（找不到回傳 D3DX_ERR_INVALID_DATA），回傳模組映像內的位址。
        // - Decompress 寫回資料指標與 SIZE_T 長度；檔案載入器尚未 Load 時回傳 E_FAIL。D3DX10 入口經 d3d10_adapter 改名為 D3DX11 版本，錯誤碼依原始家族選擇。
        if(!graphics_handled && ((symbol.starts_with("D3DX11CreateAsync") && symbol.find("Loader")!=std::string_view::npos) || symbol.starts_with("DataLoader"))){
            graphics_handled=true;result=0;
            constexpr std::uint32_t loader_failure=0x80004005U,invalid_data=0x88760b59U;
            if(symbol.starts_with("D3DX11CreateAsync")){
                const bool file=symbol.starts_with("D3DX11CreateAsyncFileLoader"),memory_source=symbol=="D3DX11CreateAsyncMemoryLoader",wide=symbol.ends_with('W');
                const unsigned out=file?1:2;
                if(!arg(out) || (!memory_source && !arg(file?0:1)) || (memory_source && !arg(0)))result=loader_failure;
                else{
                    output(out);write_pointer(arg(out),0);
                    Identity::DataLoader loader;loader.source=file?1:memory_source?2:3;loader.d3d10=service.family=="d3dx10_43.dll";
                    if(file)loader.path=wide?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)})):ansi_to_utf8(read_ansi(memory,arg(0)));
                    else if(memory_source){loader.data=arg(0);loader.size=arg(1);loader.loaded=true;}
                    else{
                        const auto found=gui_resource(memory,arg(0),arg(1),10,wide);const auto* owner=found?resource_image(found,true):nullptr;
                        if(!owner)result=invalid_data;
                        else{loader.data=owner->base+memory.load(GuestAddress{found},32);loader.size=memory.load(GuestAddress{found+4},32);loader.loaded=true;}
                    }
                    if(!result){auto id=create(Kind::DataLoader);id->data_loader=std::move(loader);write_pointer(arg(out),id->root);}
                }
            }else{
                const auto id=typed(arg(0),Kind::DataLoader);auto& loader=id->data_loader;
                if(symbol=="DataLoaderLoad" && loader.source==1){
                    const auto opened=filesystem_.open_reader(loader.path);
                    if(opened.error)result=loader.d3d10?0x88790002U:0x887c0002U;
                    else{
                        const auto metadata=opened.file->information();if(metadata.error)throw texture_image::Error(0x80070000U|metadata.error,"無法取得資料檔案大小");
                        require(metadata.metadata.size<=512U*1024U*1024U);
                        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(metadata.metadata.size));const auto io=opened.file->read(bytes,0);
                        if(io.error || io.bytes!=bytes.size())throw texture_image::Error(0x80070000U|(io.error?io.error:38U),"資料檔案讀取未完成");
                        const auto pointer=allocate_heap(memory,std::max<std::size_t>(1,bytes.size()));if(!pointer)throw std::bad_alloc{};
                        try{memory.write(GuestAddress{pointer},bytes);}catch(...){(void)free_heap(memory,pointer,state);throw;}
                        // 再次 Load：先歸還前一次讀入的資料。
                        if(loader.loaded && loader.data){std::erase(id->allocations,loader.data);(void)free_heap(memory,loader.data,state);}
                        id->allocations.push_back(pointer);loader.data=pointer;loader.size=bytes.size();loader.loaded=true;
                    }
                }else if(symbol=="DataLoaderDecompress"){
                    if(!loader.loaded || !arg(1) || !arg(2))result=loader_failure;
                    else{write_pointer(arg(1),loader.data);memory.store(GuestAddress{arg(2)},pointer_bits,loader.size);}
                }else if(symbol=="DataLoaderDestroy")release(id->root);
            }
        }
