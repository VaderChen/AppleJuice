// 原生繪圖與 ANGLE 使用相同 counter 實作；GL 工作的 queue 邊界由各自的後端維持。
bool MetalGl::graphics_timestamp_supported()const{return metal_timestamp_supported(device_);}
GraphicsTimestamp MetalGl::graphics_timestamp(){@autoreleasepool{
    begin_commands();auto commands=commands_;return metal_timestamp_sample(device_,commands,[&]{flush(true);});
}}
// 非同步：取樣編在目前命令緩衝（沒有待提交工作時建立一個只含取樣的緩衝），提交後由完成回呼解析。
// 不等待 GPU；Steam 的 Chromium 每幀約兩對 TIMESTAMP／DISJOINT 查詢，同步版每次 End 要等 GPU 閒置約 0.4 ms。
std::shared_ptr<const GraphicsTimestampFuture> MetalGl::graphics_timestamp_future(){@autoreleasepool{
    begin_commands();auto future=std::make_shared<GraphicsTimestampFuture>();
    auto sample=metal_timestamp_encode(device_,commands_,&timestamp_pool_);
    [commands_ addCompletedHandler:^(id<MTLCommandBuffer> completed){
        GraphicsTimestamp value{};
        try{if(completed.status==MTLCommandBufferStatusCompleted)value=metal_timestamp_resolve(sample.samples,sample.index);else value.disjoint=true;}
        catch(const std::exception&){value.disjoint=true;}
        (void)sample.reservation;
        future->value=value;future->done.store(true,std::memory_order_release);
    }];
    // 不在此提交：時間戳隨下一次 Present／GetData（無 DONOTFLUSH）或 flight 額度一起提交，避免每個 End 一個 command buffer。
    return future;
}}
