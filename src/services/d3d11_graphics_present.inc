            graphics_handled=true;auto swap=typed(arg(0),Kind::SwapChain);
            require(u32(1)<=4 && !(u32(2)&~0x21bU));
            if(!(u32(2)&1)){
                auto view=view_of(swap->bindings.at("Backbuffers")[0]);const auto shared_view=view;
                const auto width=view.resource->desc.width,height=view.resource->desc.height;
                using Rect=std::array<std::int64_t,4>;
                std::vector<Rect> dirty;std::optional<Rect> scroll;std::array<std::int64_t,2> offset{};
                if(symbol=="Present1" && arg(3)){
                    const auto data=read_bytes(memory,arg(3),x86_?16:32);const auto count=word(data,0);
                    const auto pointer=[&](unsigned index){return memory.load(GuestAddress{arg(3)+pointer_size*(index+1)},pointer_bits);};
                    const auto rect=[&](std::uint64_t address){
                        const auto bytes=read_bytes(memory,address,16);Rect r{};
                        for(unsigned i=0;i<4;++i)r[i]=static_cast<std::int32_t>(word(bytes,4*i));
                        require(r[0]>=0 && r[1]>=0 && r[0]<r[2] && r[1]<r[3] && r[2]<=width && r[3]<=height);return r;
                    };
                    // 先驗證全部客體輸入，再建立 GPU 命令；無效參數不改變前幀與計數。
                    require(count<=1048576 && (!count || pointer(0)));
                    if(count){memory.check(GuestAddress{pointer(0)},std::uint64_t{count}*16,Access::Read);dirty.reserve(count);
                        for(unsigned i=0;i<count;++i)dirty.push_back(rect(pointer(0)+std::uint64_t{i}*16));}
                    const auto scroll_pointer=pointer(1),offset_pointer=pointer(2);
                    require(bool(scroll_pointer)==bool(offset_pointer));
                    if(scroll_pointer){
                        require(word(swap->desc,x86_?52:60)==3);scroll=rect(scroll_pointer);
                        const auto bytes=read_bytes(memory,offset_pointer,8);
                        for(unsigned i=0;i<2;++i)offset[i]=static_cast<std::int32_t>(word(bytes,4*i));
                    }
                    require(view.resource->desc.samples==1 || (dirty.empty() && !scroll));
                }
                auto producer=object(swap->device).identity->native;
                static const bool gpu_handoff=[] {const auto* value=std::getenv("X64ARM_METAL_PRESENT_HANDOFF");return !value || std::string_view(value)=="1";}();
                if(gpu_handoff)producer->graphics_handoff(*swap->native,shared_view);else producer->graphics_flush(true);
                swap->native->swap_interval(u32(1)?1:0);
                if(view.resource->desc.samples>1){auto resolved=view_of(swap->bindings.at("PresentResolve")[0]);swap->native->graphics_resolve(resolved,view,view.resource->desc.format);view=resolved;}
                // 每條交換鏈獨立保存完整畫面，使用兩個 GPU 紋理避免捲動來源與目的重疊。
                // 只有呈現成功才交換前／後幀，失敗或 DXGI_PRESENT_TEST 不提交新歷史。
                if(!swap->present_spare){auto desc=view.resource->desc;desc.samples=1;desc.levels=desc.layers=1;desc.bind=0x28;swap->present_spare=swap->native->graphics_resource(desc);}
                GraphicsView composed{swap->present_spare,view.format,0,1,0,1};
                if(dirty.empty() || !swap->present_frame)swap->native->graphics_copy(composed,view,{},{});
                else {
                    const GraphicsView previous{swap->present_frame,view.format,0,1,0,1};
                    swap->native->graphics_copy(composed,previous,{},{});
                    if(scroll){
                        // pScrollRect 描述目前影格的目的區域；來源座標為目的減去位移。
                        // 以 64 位元運算裁切超出來源邊界的區域，避免 LONG 位移溢位。
                        const auto left=std::max((*scroll)[0],offset[0]),top=std::max((*scroll)[1],offset[1]);
                        const auto right=std::min((*scroll)[2],std::int64_t{width}+offset[0]),bottom=std::min((*scroll)[3],std::int64_t{height}+offset[1]);
                        if(left<right && top<bottom)swap->native->graphics_copy(composed,previous,
                            {static_cast<std::uint32_t>(left),static_cast<std::uint32_t>(top),0},
                            {static_cast<std::uint32_t>(left-offset[0]),static_cast<std::uint32_t>(top-offset[1]),0,
                             static_cast<std::uint32_t>(right-offset[0]),static_cast<std::uint32_t>(bottom-offset[1]),1});
                    }
                    // 更新區域優先於捲動；未更新的像素維持前幀內容。
                    for(const auto& r:dirty)swap->native->graphics_copy(composed,view,
                        {static_cast<std::uint32_t>(r[0]),static_cast<std::uint32_t>(r[1]),0},
                        {static_cast<std::uint32_t>(r[0]),static_cast<std::uint32_t>(r[1]),0,static_cast<std::uint32_t>(r[2]),static_cast<std::uint32_t>(r[3]),1});
                }
                if(gui_.windows.contains(swap->window)){swap->native->graphics_present(composed);record_diagnostic("d3d11_present_local");}
                else {publish_remote_frame(swap->window,*swap->native,composed);record_diagnostic("d3d11_present_remote");}
                std::swap(swap->present_frame,swap->present_spare);
                if(gpu_handoff)swap->native->graphics_handoff(*producer,shared_view);else swap->native->graphics_flush(true);
                ++swap->frames;++bridge.graphics_frames;
                // 診斷：X64ARM_TRACE_GRAPHICS_STATS=1 每 600 幀輸出一次裝置的 Metal 後端統計（GPU 忙碌、CPU 等待、encoder 數），
                // 遊戲被強制結束時也有資料；統計字串由命令執行緒代理同步後讀取。
                static const bool trace_stats=[] {const auto* value=std::getenv("X64ARM_TRACE_GRAPHICS_STATS");return value && std::string_view(value)=="1";}();
                if(trace_stats && swap->frames%600==0){
                    std::string creations;for(const auto& [name,count]:bridge.resource_creations)creations+=name+":"+std::to_string(count)+",";
                    std::fprintf(stderr,"[d3d11_metal] frames=%llu draws=%llu d3d11_resource_creations=%s %s\n",static_cast<unsigned long long>(bridge.graphics_frames),
                        static_cast<unsigned long long>(bridge.graphics_draws),creations.empty()?"none":creations.c_str(),producer->statistics().c_str());
                }
                graphics_close_after_present(swap->window,swap->frames,[&](std::uint64_t target){if(!gui_.windows.contains(target))return false;enqueue_gui_message({target,0,0,0x10});return true;});
            }
