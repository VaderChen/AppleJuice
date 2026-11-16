// opengl32 像素傳輸家族（由 windows_opengl.cpp 的 invoke_opengl 引入）：glPixelTransferf／i 換成雙精度；glPixelMapfv／uiv／usv 讀入 mapsize 個值，
// glGetPixelMap* 先查該表大小（GL_PIXEL_MAP_*_SIZE）再寫回；glDrawPixels 依 GL_UNPACK_* 計算來源大小。格式、型別、表大小上限與狀態檢查由後端依規格處理。
// glDebugEntry：Windows opengl32 的內部除錯入口，沒有公開語義；發行版不提供除錯功能，固定回傳 FALSE。
if(symbol=="glDebugEntry")return finish(0);
if(symbol=="glPixelTransferf" || symbol=="glPixelTransferi" || symbol.starts_with("glPixelMap") || symbol.starts_with("glGetPixelMap") || symbol=="glDrawPixels"){
    const auto run=[&](GlOperation operation,std::span<const std::uint64_t> values,std::span<const std::uint8_t> data,std::span<std::uint8_t> out)->std::uint64_t{
        try{return c.native->execute(operation,values,data,out);}catch(const std::bad_alloc&){error(c,out_of_memory);return 0;}};
    if(symbol.starts_with("glPixelTransfer")){
        const double value=symbol.back()=='i'?static_cast<double>(n(1)):static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(floating_argument(1,false))));
        const std::array<std::uint64_t,2> values{d(0),std::bit_cast<std::uint64_t>(value)};run(GlOperation::PixelTransfer,values,{},{});return finish(0);
    }
    if(symbol=="glDrawPixels"){
        if(n(0)<0 || n(1)<0){error(c,invalid_value);return finish(0);}
        const auto bytes=pixel_bytes(c.client,n(0),n(1),d(2),d(3),false);if(!bytes){error(c,invalid_enum);return finish(0);}
        auto& input=c.input_scratch;if(arg(4) && *bytes)read_reusable(memory,arg(4),*bytes,input);else input.clear();
        const std::array<std::uint64_t,4> values{d(0),d(1),d(2),d(3)};run(GlOperation::DrawPixels,values,input,{});return finish(0);
    }
    // 元素型別：fv 為 GL_FLOAT、uiv 為 GL_UNSIGNED_INT、usv 為 GL_UNSIGNED_SHORT；表名稱為 GL_PIXEL_MAP_I_TO_I（0x0C70）～GL_PIXEL_MAP_A_TO_A（0x0C79）。
    const std::uint64_t element=symbol.ends_with("usv")?0x1403U:symbol.ends_with("uiv")?0x1405U:0x1406U;const std::size_t width=element==0x1403?2U:4U;
    if(d(0)<0xc70 || d(0)>0xc79){error(c,invalid_enum);return finish(0);}
    if(symbol.starts_with("glGetPixelMap")){
        std::array<std::uint8_t,4> size_bytes{};const std::array<std::uint64_t,1> query{d(0)-0xc70+0xcb0};run(GlOperation::GetIntegerv,query,{},size_bytes);
        std::int32_t size=0;std::memcpy(&size,size_bytes.data(),4);if(size<=0 || size>65536)return finish(0);
        auto& out=c.output_scratch;out.assign(static_cast<std::size_t>(size)*width,0);const std::array<std::uint64_t,2> values{d(0),element};
        const auto written=run(GlOperation::GetPixelMap,values,{},out);
        if(written && arg(1))memory.write(GuestAddress{arg(1)},std::span<const std::uint8_t>(out).first(static_cast<std::size_t>(std::min<std::uint64_t>(written,out.size()))));
        return finish(0);
    }
    const auto size=n(1);if(size<1 || size>65536){error(c,invalid_value);return finish(0);}
    auto& input=c.input_scratch;read_reusable(memory,arg(2),static_cast<std::size_t>(size)*width,input);
    const std::array<std::uint64_t,3> values{d(0),static_cast<std::uint64_t>(size),element};run(GlOperation::PixelMap,values,input,{});return finish(0);
}
