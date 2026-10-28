// 原生繪圖與 ANGLE 使用相同 counter 實作；GL 工作的 queue 邊界由各自的後端維持。
bool MetalGl::graphics_timestamp_supported()const{return metal_timestamp_supported(device_);}
GraphicsTimestamp MetalGl::graphics_timestamp(){@autoreleasepool{
    begin_commands();auto commands=commands_;return metal_timestamp_sample(device_,commands,[&]{flush(true);});
}}
// 非同步：取樣編在目前命令緩衝（沒有待提交工作時建立一個只含取樣的緩衝），提交後由完成回呼解析。
// 不等待 GPU；Steam 的 Chromium 每幀約兩對 TIMESTAMP／DISJOINT 查詢，同步版每次 End 要等 GPU 閒置約 0.4 ms。
std::shared_ptr<const GraphicsTimestampFuture> MetalGl::graphics_timestamp_future(){
    auto future=std::make_shared<GraphicsTimestampFuture>();timestamp_into(future);return future;
}
// 把取樣編進目前命令緩衝，完成時寫入呼叫端提供的 future（命令執行緒代理先回傳 future、再依序排入此步驟）。
void MetalGl::timestamp_into(const std::shared_ptr<GraphicsTimestampFuture>& future){@autoreleasepool{
    begin_commands();
    // block 擷取 C++ 參照時不複製物件：先複製 shared_ptr，回呼持有自己的所有權。
    auto target=future;
    auto sample=metal_timestamp_encode(device_,commands_,&timestamp_pool_);
    [commands_ addCompletedHandler:^(id<MTLCommandBuffer> completed){
        GraphicsTimestamp value{};
        try{if(completed.status==MTLCommandBufferStatusCompleted)value=metal_timestamp_resolve(sample.samples,sample.index);else value.disjoint=true;}
        catch(const std::exception&){value.disjoint=true;}
        (void)sample.reservation;
        target->value=value;target->done.store(true,std::memory_order_release);
    }];
    // 不在此提交：時間戳隨下一次 Present／GetData（無 DONOTFLUSH）或 flight 額度一起提交，避免每個 End 一個 command buffer。
}}
