// 可選的繪製文字追蹤（X64ARM_GRAPHICS_STENCIL_TRACE=<檔案>），不讀回 GPU：平時只記錄開啟 stencil 的 draw 與 Present 標記，
// 用來比對多 pass stencil 遮罩流程；另設 X64ARM_GRAPHICS_DRAW_TRACE_REQUEST=<請求檔>（內容「請求編號 Present 數」）時，
// 新請求之後的數個 Present 內記錄全部 draw 的輸入（頂點／索引緩衝、VS／PS SRV），供逐幀比對隔幀出現的繪製。
static std::ofstream* metal_stencil_trace(){
    static std::ofstream* const file=[]()->std::ofstream*{const auto* path=std::getenv("X64ARM_GRAPHICS_STENCIL_TRACE");return path && *path?new std::ofstream(path):nullptr;}();
    return file;
}
static std::atomic<std::uint64_t> metal_present_index{};
static bool metal_trace_window(){
    static const auto* const request_path=std::getenv("X64ARM_GRAPHICS_DRAW_TRACE_REQUEST");if(!request_path || !*request_path)return false;
    static std::uint64_t previous{},window_end{};static auto next_poll=std::chrono::steady_clock::time_point{};
    const auto now=std::chrono::steady_clock::now();
    if(now>=next_poll){
        next_poll=now+std::chrono::milliseconds(250);
        std::ifstream request(request_path);std::uint64_t number=0,count=0;
        if((request>>number>>count) && number && number!=previous){previous=number;window_end=metal_present_index.load()+std::min<std::uint64_t>(count,16)+1;}
    }
    return metal_present_index.load()<window_end;
}
// 常數緩衝內容：每幀每個緩衝只讀一次，全部記 FNV 雜湊；1 KB 以上（View 類）另以獨立行記完整十六進位內容。
template<class Read> static void metal_trace_constant(std::ostream& o,std::vector<std::string>& extra,const std::shared_ptr<GraphicsResource>& constant,const Read& graphics_read){
    static std::uint64_t dumped_frame=UINT64_MAX;static std::set<const void*> dumped;
    if(dumped_frame!=metal_present_index.load()){dumped_frame=metal_present_index.load();dumped.clear();}
    if(!constant || !dumped.insert(constant.get()).second)return;
    try{
        const auto bytes=graphics_read(GraphicsView{constant});std::uint64_t hash=1469598103934665603ULL;for(const auto b:bytes){hash^=b;hash*=1099511628211ULL;}
        o<<" cbh"<<constant.get()<<'='<<bytes.size()<<':'<<std::hex<<hash<<std::dec;
        if(bytes.size()>=1024){
            std::ostringstream line;line<<"cbdata frame="<<dumped_frame<<" cb="<<constant.get()<<" size="<<bytes.size()<<" hex=";
            static constexpr char digits[]="0123456789abcdef";for(const auto b:bytes)line<<digits[b>>4]<<digits[b&15];
            extra.push_back(line.str());
        }
    }catch(const std::exception&){}
}
template<class Read> static void metal_trace_stencil_draw(const GraphicsDraw& draw,const Read& graphics_read){
    auto* out=metal_stencil_trace();if(!out)return;
    const bool stencil=draw.depth_state.size()>=52 && metal_word(draw.depth_state,12);const bool window=metal_trace_window();
    if(!stencil && !window)return;
    static std::uint64_t serial{};auto& o=*out;std::vector<std::string> extra;
    const auto view=[&](const GraphicsView& v){o<<v.resource.get()<<':'<<v.format<<':'<<v.level<<':'<<v.slice;if(v.first || v.count)o<<'+'<<v.first<<'#'<<v.count;};
    o<<"draw="<<serial++<<" idx="<<draw.indexed<<" n="<<draw.count<<" inst="<<draw.instances<<" start="<<draw.start<<" base="<<draw.base_vertex<<" ref="<<draw.stencil_reference;
    if(stencil){
        o<<" dep="<<metal_word(draw.depth_state,0)<<','<<metal_word(draw.depth_state,4)<<','<<metal_word(draw.depth_state,8);
        o<<" rm="<<unsigned(draw.depth_state[16])<<" wm="<<unsigned(draw.depth_state[17]);
        for(unsigned face=0;face<2;++face){const auto off=20+face*16;o<<(face?" back=":" front=")<<metal_word(draw.depth_state,off+12)<<'/'<<metal_word(draw.depth_state,off)<<','<<metal_word(draw.depth_state,off+4)<<','<<metal_word(draw.depth_state,off+8);}
    }else o<<" front=- back=- rm=- wm=-";
    o<<" ds=";view(draw.depth);o<<":f"<<draw.depth.flags;
    for(unsigned i=0;i<draw.targets.size();++i)if(draw.targets[i].resource){o<<" rt"<<i<<'=';view(draw.targets[i]);}
    if(draw.blend.size()>=40)o<<" blend="<<metal_word(draw.blend,8)<<'/'<<metal_word(draw.blend,12)<<','<<metal_word(draw.blend,16)<<','<<metal_word(draw.blend,20)<<'/'<<metal_word(draw.blend,24)<<','<<metal_word(draw.blend,28)<<','<<metal_word(draw.blend,32)<<'/'<<unsigned(draw.blend[36]);
    for(unsigned s=0;s<2;++s)if(draw.stages[s].shader)o<<(s?" ps=":" vs=")<<dynamic_cast<const MetalShader&>(*draw.stages[s].shader).serial;
    if(draw.stages[1].shader)for(unsigned i=0;i<draw.stages[1].resources.size();++i)if(draw.stages[1].resources[i].resource){o<<" t"<<i<<'=';view(draw.stages[1].resources[i]);}
    if(window){
        for(unsigned i=0;i<draw.vertices.size();++i)if(draw.vertices[i].resource)o<<" vb"<<i<<'='<<draw.vertices[i].resource.get()<<'+'<<draw.vertices[i].offset<<'/'<<draw.vertices[i].stride;
        if(draw.indices)o<<" ib="<<draw.indices.get()<<'+'<<draw.index_offset<<'/'<<draw.index_format;
        for(unsigned i=0;i<draw.stages[0].resources.size();++i)if(draw.stages[0].resources[i].resource){o<<" vt"<<i<<'=';view(draw.stages[0].resources[i]);}
        for(unsigned i=0;i<draw.stages[0].constants.size();++i)if(draw.stages[0].constants[i])o<<" vc"<<i<<'='<<draw.stages[0].constants[i].get();
        for(unsigned i=0;i<draw.stages[1].constants.size();++i)if(draw.stages[1].constants[i])o<<" pc"<<i<<'='<<draw.stages[1].constants[i].get();
        for(unsigned st=0;st<2;++st)for(const auto& constant:draw.stages[st].constants)metal_trace_constant(o,extra,constant,graphics_read);
        if(draw.indirect){try{const auto bytes=graphics_read(GraphicsView{draw.indirect});o<<" ind="<<draw.indirect.get()<<'+'<<draw.indirect_offset<<':';for(unsigned k=0;k<5 && draw.indirect_offset+4*k+4<=bytes.size();++k){std::uint32_t value;std::memcpy(&value,bytes.data()+draw.indirect_offset+4*k,4);o<<value<<(k<4?",":"");}}catch(const std::exception&){o<<" ind=?";}
            // 間接繪製後的渲染目標 0 縮小 8 倍存成 PPM（追蹤檔旁），逐 draw 檢視哪一個畫出異常。
            if(const auto& target=draw.targets[0];target.resource && target.resource->desc.dimension==3){
                const auto format=target.format?target.format:target.resource->desc.format;
                if(format==10 || format==2 || format==28 || format==87 || format==24 || format==26){
                    try{
                        const auto pixels=graphics_read(target);const auto& d=target.resource->desc;const unsigned w=std::max(1U,d.width>>target.level),h=std::max(1U,d.height>>target.level);
                        const unsigned bpp=format==10?8:format==2?16:4,sw=w/8,sh=h/8;
                        static const std::string base=[]{const auto* path=std::getenv("X64ARM_GRAPHICS_STENCIL_TRACE");return std::string(path)+".rt";}();
                        std::filesystem::create_directories(base);
                        std::ofstream ppm(base+"/f"+std::to_string(metal_present_index.load())+"-d"+std::to_string(serial)+"-vs"+(draw.stages[0].shader?std::to_string(dynamic_cast<const MetalShader&>(*draw.stages[0].shader).serial):std::string("x"))+".ppm",std::ios::binary);
                        ppm<<"P6\n"<<sw<<' '<<sh<<"\n255\n";
                        const auto channel=[&](const std::uint8_t* p,unsigned c)->float{
                            if(format==10){std::uint16_t half;std::memcpy(&half,p+2*c,2);__fp16 value;std::memcpy(&value,&half,2);return static_cast<float>(value);}
                            if(format==2){float value;std::memcpy(&value,p+4*c,4);return value;}
                            if(format==87)return p[2-c]/255.0F;
                            return p[c]/255.0F;
                        };
                        for(unsigned y=0;y<sh;++y)for(unsigned x=0;x<sw;++x){
                            const auto* p=pixels.data()+(static_cast<std::size_t>(y*8)*w+x*8)*bpp;
                            for(unsigned c=0;c<3;++c){const float v=channel(p,c);const float mapped=std::isfinite(v)?v/(1.0F+std::max(0.0F,v)):1.0F;ppm.put(static_cast<char>(static_cast<std::uint8_t>(std::clamp(mapped*2.0F,0.0F,1.0F)*255.0F)));}
                        }
                    }catch(const std::exception&){}
                }
            }
            // 間接繪製（GPU 粒子）讀取的 VS 緩衝健康度：浮點看 NaN／Inf 數與最大絕對值，整數看最大值（排序索引是否越界）。
            for(unsigned i=0;i<draw.stages[0].resources.size();++i){
                const auto& v=draw.stages[0].resources[i];if(!v.resource || v.resource->desc.dimension!=1)continue;
                const auto format=v.format?v.format:v.resource->desc.format;if(format!=41 && format!=42 && format!=43)continue;
                try{
                    const auto bytes=graphics_read(GraphicsView{v.resource});const std::size_t begin=v.first,end=std::min<std::size_t>(bytes.size(),v.count?v.first+v.count:bytes.size());
                    std::size_t bad=0;double largest=0;std::int64_t top=INT64_MIN;
                    for(std::size_t at=begin;at+4<=end;at+=4){
                        if(format==41){float value;std::memcpy(&value,bytes.data()+at,4);if(!std::isfinite(value))++bad;else largest=std::max(largest,static_cast<double>(std::fabs(value)));}
                        else{std::int32_t value;std::memcpy(&value,bytes.data()+at,4);top=std::max<std::int64_t>(top,format==42?static_cast<std::int64_t>(static_cast<std::uint32_t>(value)):value);}
                    }
                    if(format==41)o<<" vh"<<i<<"=nan"<<bad<<"/max"<<largest;else o<<" vh"<<i<<"=imax"<<top;
                }catch(const std::exception&){o<<" vh"<<i<<"=?";}
            }
        }
        // 極小的浮點紋理（例如 1×1 曝光值）直接附上內容：讀回會等待 GPU，只在請求視窗內使用。
        for(unsigned s=0;s<2;++s)for(unsigned i=0;i<draw.stages[s].resources.size();++i){
            const auto& v=draw.stages[s].resources[i];if(!v.resource)continue;const auto& d=v.resource->desc;
            if(d.dimension!=3 || d.format!=2 || d.width*d.height>4)continue;
            try{const auto bytes=graphics_read(v);o<<(s?" pv":" vv")<<i<<'=';for(std::size_t at=0;at+4<=bytes.size() && at<32;at+=4){float value;std::memcpy(&value,bytes.data()+at,4);o<<value<<(at+8<=bytes.size() && at<28?",":"");}}
            catch(const std::exception&){o<<(s?" pv":" vv")<<i<<"=?";}
        }
    }
    for(const auto& v:draw.viewports)o<<" vp="<<v[0]<<','<<v[1]<<','<<v[2]<<','<<v[3];
    for(const auto& r:draw.scissors)o<<" sc="<<r[0]<<','<<r[1]<<','<<r[2]<<','<<r[3];
    o<<'\n';for(const auto& line:extra)o<<line<<'\n';
}
// 請求視窗內的 compute dispatch：CS、群組數或間接參數、UAV／SRV 與常數緩衝雜湊，與 draw 追蹤寫在同一檔案。
void MetalGl::graphics_trace_dispatch(const GraphicsStage& stage,std::array<std::uint32_t,3> groups,const std::shared_ptr<GraphicsResource>& arguments,std::uint32_t offset){
    auto* out=metal_stencil_trace();if(!out || !metal_trace_window() || !stage.shader)return;
    auto& o=*out;std::vector<std::string> extra;const auto read=[this](const GraphicsView& view){return graphics_read(view);};
    const auto view=[&](const GraphicsView& v){o<<v.resource.get()<<':'<<v.format<<':'<<v.level<<':'<<v.slice;if(v.first || v.count)o<<'+'<<v.first<<'#'<<v.count;};
    o<<"dispatch cs="<<dynamic_cast<const MetalShader&>(*stage.shader).serial<<" g="<<groups[0]<<','<<groups[1]<<','<<groups[2];
    if(arguments){try{const auto bytes=read(GraphicsView{arguments});o<<" ind="<<arguments.get()<<'+'<<offset<<':';for(unsigned k=0;k<3 && offset+4*k+4<=bytes.size();++k){std::uint32_t value;std::memcpy(&value,bytes.data()+offset+4*k,4);o<<value<<(k<2?",":"");}}catch(const std::exception&){o<<" ind=?";}}
    for(unsigned i=0;i<stage.unordered.size();++i)if(stage.unordered[i].resource){o<<" u"<<i<<'=';view(stage.unordered[i]);}
    for(unsigned i=0;i<stage.resources.size();++i)if(stage.resources[i].resource){o<<" t"<<i<<'=';view(stage.resources[i]);}
    for(unsigned i=0;i<stage.constants.size();++i)if(stage.constants[i])o<<" c"<<i<<'='<<stage.constants[i].get();
    for(const auto& constant:stage.constants)metal_trace_constant(o,extra,constant,read);
    // 常數緩衝實際讀到的前 32 個值（含綁定起點），與上傳紀錄比對是否為本幀內容。
    for(unsigned i=0;i<stage.constants.size();++i){
        if(!stage.constants[i])continue;
        try{const auto bytes=read(GraphicsView{stage.constants[i]});const std::size_t base=stage.constant_offsets[i];o<<" cv"<<i<<'=';for(std::size_t at=base;at+4<=bytes.size() && at<base+640;at+=4){std::uint32_t value;std::memcpy(&value,bytes.data()+at,4);o<<value<<',';}}
        catch(const std::exception&){o<<" cv"<<i<<"=?";}
    }
    // 小型緩衝 SRV（例如 GPU 粒子的任務資訊）直接附上前 32 個 32 位元值：比對各幀上傳內容是否為最新。
    for(unsigned i=0;i<stage.resources.size();++i){
        const auto& v=stage.resources[i];if(!v.resource || v.resource->desc.dimension!=1 || v.resource->desc.width>4096)continue;
        try{const auto bytes=read(GraphicsView{v.resource});o<<" tv"<<i<<'=';for(std::size_t at=v.first;at+4<=bytes.size() && at<v.first+128;at+=4){std::uint32_t value;std::memcpy(&value,bytes.data()+at,4);o<<value<<',';}}
        catch(const std::exception&){o<<" tv"<<i<<"=?";}
    }
    o<<'\n';for(const auto& line:extra)o<<line<<'\n';
}
// 請求視窗內 4 KB 以下緩衝的上傳內容（前 32 個 32 位元值），與 dispatch／draw 紀錄同序，用來比對上傳與讀取是否一致。
void MetalGl::graphics_trace_upload(const char* kind,const GraphicsView& view,std::span<const std::uint8_t> data){
    auto* out=metal_stencil_trace();if(!out || !view.resource || view.resource->desc.dimension!=1 || view.resource->desc.width>4096 || !metal_trace_window())return;
    auto& o=*out;o<<"upload kind="<<kind<<" res="<<view.resource.get()<<" first="<<view.first<<" bytes="<<data.size()<<" data=";
    for(std::size_t at=0;at+4<=data.size() && at<128;at+=4){std::uint32_t value;std::memcpy(&value,data.data()+at,4);o<<value<<',';}
    o<<'\n';
}
// 可選的離線繪圖診斷；未設定環境變數時不讀回 GPU，也不影響呈現路徑。
void MetalGl::graphics_capture(const GraphicsDraw& draw){
    metal_trace_stencil_draw(draw,[this](const GraphicsView& view){return graphics_read(view);});
    static const auto* const directory=std::getenv("X64ARM_GRAPHICS_CAPTURE_DIR");if(!directory || !*directory)return; // 每次繪製都經過，只查一次。
    // 篩選設定在程序內不變：只讀一次（擷取目錄設定後每次繪製都會經過這裡，逐次 getenv 會扭曲量測）。
    static const char* const stream_filter=std::getenv("X64ARM_GRAPHICS_CAPTURE_STREAM_OUTPUT");static const char* const cb_filter=std::getenv("X64ARM_GRAPHICS_CAPTURE_REQUIRE_CB");
    static const char* const count_filter=std::getenv("X64ARM_GRAPHICS_CAPTURE_NONINDEXED_COUNT");static const char* const request_file=std::getenv("X64ARM_GRAPHICS_CAPTURE_REQUEST");
    static const char* const begin_setting=std::getenv("X64ARM_GRAPHICS_CAPTURE_BEGIN");static const char* const count_setting=std::getenv("X64ARM_GRAPHICS_CAPTURE_COUNT");
    if(const char* filter=stream_filter;filter && std::string_view(filter)=="1")
        if(!draw.stages[2].shader || draw.stages[2].shader->stream_output.empty())return;
    // 依實際綁定篩選，避免 UI draw 耗盡診斷張數；不依程式或 shader 名稱。
    if(const char* filter=cb_filter;filter && *filter){
        char* separator=nullptr;const auto stage=std::strtoul(filter,&separator,10);
        if(separator==filter || *separator!=':')throw std::invalid_argument("擷取常數槽篩選格式應為 stage:slot");
        char* end=nullptr;const auto slot=std::strtoul(separator+1,&end,10);
        if(end==separator+1 || *end || stage>=draw.stages.size() || slot>=draw.stages[stage].constants.size())throw std::invalid_argument("擷取常數槽篩選超出範圍");
        if(!draw.stages[stage].constants[slot])return;
    }
    if(const char* filter=count_filter;filter && *filter){
        char* end=nullptr;const auto count=std::strtoul(filter,&end,10);
        if(end==filter || *end || !count || count>UINT32_MAX)throw std::invalid_argument("擷取非索引 draw 頂點數不合法");
        if(draw.indexed || draw.count!=count)return;
    }
    static std::atomic<std::uint64_t> serial{};const auto id=serial.fetch_add(1,std::memory_order_relaxed);
    if(const auto* request_path=request_file;request_path && *request_path){
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
        const auto* begin_text=begin_setting;const auto begin=begin_text?std::strtoull(begin_text,nullptr,10):0;
        const auto* count_text=count_setting;const auto count=count_text?std::min(128ULL,std::strtoull(count_text,nullptr,10)):128ULL;
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
    const auto present_number=metal_present_index.fetch_add(1);
    if(auto* out=metal_stencil_trace())*out<<"present="<<present_number<<" unix_ms="<<std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()<<'\n';
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
    state<<"present_index="<<present_number<<" resource="<<view.resource.get()<<" format="<<view.resource->desc.format<<" width="<<width_<<" height="<<height_<<" view="<<(__bridge void*)view_<<" layer="<<(__bridge void*)layer_<<'\n';
    if(view_)state<<"window="<<view_.window.windowNumber<<" active_layer="<<(__bridge void*)view_.layer<<" view_bounds="<<view_.bounds.size.width<<','<<view_.bounds.size.height<<" hidden="<<view_.hidden<<" alpha="<<view_.alphaValue<<'\n';
    if(layer_)state<<"layer_bounds="<<layer_.bounds.size.width<<','<<layer_.bounds.size.height<<" drawable_size="<<layer_.drawableSize.width<<','<<layer_.drawableSize.height<<" opacity="<<layer_.opacity<<" hidden="<<layer_.hidden<<'\n';
}
