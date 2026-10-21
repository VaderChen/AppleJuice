// 原生繪圖與 ANGLE 使用相同 counter 實作；GL 工作的 queue 邊界由各自的後端維持。
bool MetalGl::graphics_timestamp_supported()const{return metal_timestamp_supported(device_);}
GraphicsTimestamp MetalGl::graphics_timestamp(){@autoreleasepool{
    begin_commands();auto commands=commands_;return metal_timestamp_sample(device_,commands,[&]{flush(true);});
}}
