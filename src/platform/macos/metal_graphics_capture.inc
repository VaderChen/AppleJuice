// 可選的離線繪圖診斷；未設定環境變數時不讀回 GPU，也不影響呈現路徑。
void MetalGl::graphics_capture(const GraphicsDraw& draw){
    const auto* directory=std::getenv("X64ARM_GRAPHICS_CAPTURE_DIR");if(!directory || !*directory)return;
    if(const char* filter=std::getenv("X64ARM_GRAPHICS_CAPTURE_STREAM_OUTPUT");filter && std::string_view(filter)=="1")
        if(!draw.stages[2].shader || draw.stages[2].shader->stream_output.empty())return;
    // 依實際綁定篩選，避免 UI draw 耗盡診斷張數；不依程式或 shader 名稱。
    if(const char* filter=std::getenv("X64ARM_GRAPHICS_CAPTURE_REQUIRE_CB");filter && *filter){
        char* separator=nullptr;const auto stage=std::strtoul(filter,&separator,10);
        if(separator==filter || *separator!=':')throw std::invalid_argument("擷取常數槽篩選格式應為 stage:slot");
        char* end=nullptr;const auto slot=std::strtoul(separator+1,&end,10);
        if(end==separator+1 || *end || stage>=draw.stages.size() || slot>=draw.stages[stage].constants.size())throw std::invalid_argument("擷取常數槽篩選超出範圍");
        if(!draw.stages[stage].constants[slot])return;
    }
    if(const char* filter=std::getenv("X64ARM_GRAPHICS_CAPTURE_NONINDEXED_COUNT");filter && *filter){
        char* end=nullptr;const auto count=std::strtoul(filter,&end,10);
        if(end==filter || *end || !count || count>UINT32_MAX)throw std::invalid_argument("擷取非索引 draw 頂點數不合法");
        if(draw.indexed || draw.count!=count)return;
    }
    static std::atomic<std::uint64_t> serial{};const auto id=serial.fetch_add(1,std::memory_order_relaxed);
    if(const auto* request_path=std::getenv("X64ARM_GRAPHICS_CAPTURE_REQUEST");request_path && *request_path){
        // 明確啟用的即時診斷：檔案為「請求編號 張數」，新編號才觸發，
        // 每次最多 128 張。以原子替換檔案送出，沒有請求時不讀回 GPU。
        static std::uint64_t previous_request{},remaining{};
        static auto next_poll=std::chrono::steady_clock::time_point{};
        const auto now=std::chrono::steady_clock::now();
        if(now>=next_poll){
            next_poll=now+std::chrono::milliseconds(250);
            std::ifstream request(request_path);std::uint64_t number=0,count=0;char trailing=0;
            if((request>>number>>count) && !(request>>trailing) && number && number!=previous_request){
                previous_request=number;remaining=std::min<std::uint64_t>(count,128);
            }
        }
        if(!remaining)return;
        --remaining;
    }else{
        const auto* begin_text=std::getenv("X64ARM_GRAPHICS_CAPTURE_BEGIN");const auto begin=begin_text?std::strtoull(begin_text,nullptr,10):0;
        const auto* count_text=std::getenv("X64ARM_GRAPHICS_CAPTURE_COUNT");const auto count=count_text?std::min(128ULL,std::strtoull(count_text,nullptr,10)):128ULL;
        if(id<begin || id-begin>=count)return;
    }
    const auto base=std::filesystem::path(directory)/std::to_string(::getpid());std::filesystem::create_directories(base);
    const auto prefix=base/("draw-"+std::to_string(id));std::ofstream state(prefix.string()+".txt");
    state<<"count="<<draw.count<<" instances="<<draw.instances<<" start="<<draw.start<<" base_vertex="<<draw.base_vertex<<" topology="<<draw.topology<<" indexed="<<draw.indexed<<" index_format="<<draw.index_format<<" index_offset="<<draw.index_offset<<'\n';
    for(const auto& viewport:draw.viewports){state<<"viewport";for(const auto value:viewport)state<<' '<<value;state<<'\n';}
    for(const auto& rect:draw.scissors){state<<"scissor";for(const auto value:rect)state<<' '<<value;state<<'\n';}
    const auto bytes=[&](const std::string& name,std::span<const std::uint8_t> value){std::ofstream file(prefix.string()+"-"+name,std::ios::binary);file.write(reinterpret_cast<const char*>(value.data()),static_cast<std::streamsize>(value.size()));};
    const auto resource=[&](const std::string& name,GraphicsView view){if(!view.resource)return;const auto& d=view.resource->desc;state<<name<<" dimension="<<d.dimension<<" width="<<d.width<<" height="<<d.height<<" format="<<d.format<<" mip="<<view.level<<" slice="<<view.slice<<" first="<<view.first<<" resource="<<view.resource.get()<<" view_format="<<view.format<<'\n';try{bytes(name+".bin",graphics_read(view));
        // 可選診斷：保存 SRV／RTV 涵蓋的各 mip 與 array slice，供 CPU 參考運算比對。
        if(d.dimension!=1 && std::getenv("X64ARM_GRAPHICS_CAPTURE_SUBRESOURCES")){
            const auto levels=std::min(view.levels,d.levels-view.level),slices=d.dimension==4?1U:std::min(view.slices,d.layers-view.slice);
            for(unsigned layer=0;layer<slices;++layer)for(unsigned level=0;level<levels;++level){
                auto sub=view;sub.level+=level;sub.slice+=layer;sub.levels=sub.slices=1;
                bytes(name+"-mip-"+std::to_string(sub.level)+"-slice-"+std::to_string(sub.slice)+".bin",graphics_read(sub));
            }
        }
    }catch(const std::exception& error){state<<name<<" read_error="<<error.what()<<'\n';}};
    bytes("blend.bin",draw.blend);bytes("raster.bin",draw.raster);bytes("depth.bin",draw.depth_state);
    if(draw.indices)resource("indices",GraphicsView{draw.indices});
    for(unsigned i=0;i<draw.stream_targets.size();++i)if(draw.stream_targets[i]){
        state<<"stream "<<i<<" cursor="<<draw.stream_targets[i]->stream_cursor<<" vertices="<<draw.stream_targets[i]->stream_vertices<<'\n';
        resource("stream-"+std::to_string(i),GraphicsView{draw.stream_targets[i]});
    }
    for(unsigned i=0;i<draw.vertices.size();++i)if(draw.vertices[i].resource){state<<"vertex "<<i<<" stride="<<draw.vertices[i].stride<<" offset="<<draw.vertices[i].offset<<'\n';resource("vertex-"+std::to_string(i),GraphicsView{draw.vertices[i].resource});}
    for(const auto& element:draw.layout)state<<"input "<<element.semantic<<element.index<<" format="<<element.format<<" slot="<<element.slot<<" offset="<<element.offset<<" classification="<<element.classification<<" step="<<element.step<<'\n';
    for(unsigned s=0;s<draw.stages.size();++s){const auto& stage=draw.stages[s];if(!stage.shader)continue;const auto tag="stage-"+std::to_string(s);const auto& shader=dynamic_cast<MetalShader&>(*stage.shader);std::ofstream code(prefix.string()+"-"+tag+".metal");code<<shader.source;
        for(const auto& binding:shader.bindings)state<<tag<<" binding type="<<binding.type<<" slot="<<binding.slot<<" native="<<binding.native_slot<<'\n';
        for(unsigned i=0;i<stage.constants.size();++i)resource(tag+"-constant-"+std::to_string(i),GraphicsView{stage.constants[i]});
        for(unsigned i=0;i<stage.resources.size();++i)resource(tag+"-resource-"+std::to_string(i),stage.resources[i]);
        for(unsigned i=0;i<stage.unordered.size();++i)resource(tag+"-unordered-"+std::to_string(i),stage.unordered[i]);
    }
    for(unsigned i=0;i<draw.targets.size();++i)resource("target-"+std::to_string(i),draw.targets[i]);
    resource("depth-target",draw.depth);
}

void MetalGl::graphics_capture_present(const GraphicsView& view){
    const auto* request_path=std::getenv("X64ARM_GRAPHICS_PRESENT_CAPTURE_REQUEST");
    const auto* directory=std::getenv("X64ARM_GRAPHICS_CAPTURE_DIR");
    if(!request_path || !*request_path || !directory || !*directory)return;
    // 可略過啟動畫面，依 Present 次序擷取動畫中後段；不修改客體時鐘或亂數。
    static std::uint64_t present_index{};const auto current=present_index++;
    if(const char* begin=std::getenv("X64ARM_GRAPHICS_PRESENT_CAPTURE_BEGIN");begin && current<std::strtoull(begin,nullptr,10))return;
    static std::uint64_t request_id{},remaining{},sequence{};
    static auto next_poll=std::chrono::steady_clock::time_point{};
    const auto now=std::chrono::steady_clock::now();
    if(now>=next_poll){
        next_poll=now+std::chrono::milliseconds(250);
        std::ifstream request(request_path);std::uint64_t number=0,count=0;char trailing=0;
        if((request>>number>>count) && !(request>>trailing) && number && number!=request_id){request_id=number;remaining=std::min<std::uint64_t>(count,16);}
    }
    if(!remaining)return;
    --remaining;
    const auto prefix=std::filesystem::path(directory)/std::to_string(::getpid())/("present-"+std::to_string(request_id)+"-"+std::to_string(++sequence));
    std::filesystem::create_directories(prefix.parent_path());
    const auto pixels=graphics_read(view);std::ofstream bytes(prefix.string()+"-source.bin",std::ios::binary);
    bytes.write(reinterpret_cast<const char*>(pixels.data()),static_cast<std::streamsize>(pixels.size()));
    capture(prefix.string()+"-surface.png");
    std::ofstream state(prefix.string()+".txt");
    state<<"resource="<<view.resource.get()<<" format="<<view.resource->desc.format<<" width="<<width_<<" height="<<height_<<" view="<<(__bridge void*)view_<<" layer="<<(__bridge void*)layer_<<'\n';
    if(view_)state<<"window="<<view_.window.windowNumber<<" active_layer="<<(__bridge void*)view_.layer<<" view_bounds="<<view_.bounds.size.width<<','<<view_.bounds.size.height<<" hidden="<<view_.hidden<<" alpha="<<view_.alphaValue<<'\n';
    if(layer_)state<<"layer_bounds="<<layer_.bounds.size.width<<','<<layer_.bounds.size.height<<" drawable_size="<<layer_.drawableSize.width<<','<<layer_.drawableSize.height<<" opacity="<<layer_.opacity<<" hidden="<<layer_.hidden<<'\n';
}
