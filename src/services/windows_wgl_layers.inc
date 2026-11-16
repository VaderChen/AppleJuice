// opengl32 的 WGL 層面與 context 函式（由 windows_opengl.cpp 的 invoke_opengl 引入，位於需要目前 context 的入口之前）：
// wglDescribeLayerPlane、wglSetLayerPaletteEntries、wglGetLayerPaletteEntries、wglRealizeLayerPalette、wglCopyContext、wglGetDefaultProcAddress。
// 公開的像素格式只有雙緩衝主平面（PIXELFORMATDESCRIPTOR 的 overlay／underlay 數為零），因此 overlay／underlay 平面一律不存在：
// 依官方文件回傳失敗並設 ERROR_INVALID_PARAMETER；HDC 或像素格式無效時分別為 ERROR_INVALID_HANDLE、ERROR_INVALID_PIXEL_FORMAT。
// wglCreateLayerContext 與 wglCreateContext 共用建立流程（主平面 0）；wglCopyContext 依 glPushAttrib 位元複製狀態群組。
if(symbol=="wglDescribeLayerPlane"){
    // (hdc, iPixelFormat, iLayerPlane, nBytes, plpd)
    if(!dc_window(arg(0))){last_error_=6;return finish(0);}if(n(1)!=1){last_error_=2000;return finish(0);}
    last_error_=87;return finish(0);
}
if(symbol=="wglSetLayerPaletteEntries" || symbol=="wglGetLayerPaletteEntries" || symbol=="wglRealizeLayerPalette"){
    // 回傳設定／取得的項目數或 TRUE；沒有色彩索引層面，所以沒有可處理的項目。
    if(!dc_window(arg(0))){last_error_=6;return finish(0);}
    last_error_=87;return finish(0);
}
if(symbol=="wglCopyContext"){
    // (hglrcSrc, hglrcDst, mask)：兩者都必須是本程序的 context；GL_ALL_ATTRIB_BITS 只取低 20 位元。
    const auto source=opengl_.contexts.find(arg(0)),target=opengl_.contexts.find(arg(1));
    if(source==opengl_.contexts.end() || target==opengl_.contexts.end()){last_error_=6;return finish(0);}
    if(source==target)return finish(1);
    const auto mask=d(2)&0xfffffU;
    try{if(!target->second.native->copy_state(*source->second.native,mask)){last_error_=50;return finish(0);}}catch(const std::bad_alloc&){last_error_=8;return finish(0);}
    return finish(1);
}
if(symbol=="wglGetDefaultProcAddress"){
    // 回傳 opengl32 本身匯出的函式位址；不在匯出表的名稱為 NULL（ERROR_PROC_NOT_FOUND）。
    const auto requested=read_ansi(memory,arg(0),256);
    for(const auto& entry:opengl_service_descriptors())if(entry.family=="opengl32.dll" && entry.symbol==requested && requested.starts_with("gl"))return finish(resolve("opengl32.dll",requested).value);
    last_error_=127;return finish(0);
}
