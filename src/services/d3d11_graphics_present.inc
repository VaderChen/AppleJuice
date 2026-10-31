            graphics_handled=true;auto swap=typed(arg(0),Kind::SwapChain);
            require(u32(1)<=4 && !(u32(2)&~0x21bU));
            // 顯示緩衝都在途時，呈現會在 nextDrawable 等到下一次垂直同步；Present 在全體靜止的服務邊界執行，等待期間
            // 所有客體執行緒一起停（DQ3 讀取畫面每幀約 12 ms，讀取執行緒近四成時間被凍結）。改讓呼叫端進入重試等待、
            // 交出 vCPU，緩衝釋出或約兩幀的保底期限到時重新執行整個 Present（見 resolve_thread_waits）。只用於常駐 AOT 多 vCPU：
            // 協調者每 0.25 ms 輪詢等待者；單 vCPU 與依指令預算切時間片的執行要到時間片結束才重新檢查，維持原本的阻塞呈現。
            // X64ARM_PRESENT_WAIT=0 關閉。
            static const bool present_wait=[] {const auto* value=std::getenv("X64ARM_PRESENT_WAIT");return !value || std::string_view(value)!="0";}();
            if(present_wait && !(u32(2)&1) && resident_execution_depth_>0 && runtime_.processor_count>1 && threads_.items.size()>1 &&
               gui_.windows.contains(swap->window) && !swap->native->graphics_present_ready()){
                std::erase_if(present_waits_,[&](const auto& entry){const auto waiter=threads_.items.find(entry.first);
                    return waiter==threads_.items.end() || waiter->second.status!=ThreadStatus::Waiting || waiter->second.wait.kind!=ThreadWaitKind::PresentRetry;});
                auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::PresentRetry;
                thread.wait.deadline=runtime_.clock_ticks+340000;present_waits_[threads_.current]=swap->native;record_diagnostic("d3d11_present_wait");
                schedule(state,memory);return std::nullopt;
            }
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
                ++swap->frames;++bridge.graphics_frames;swap->present_ticks=runtime_.clock_ticks;
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
