// opengl32 1D 貼圖（由 windows_opengl.cpp 的 invoke_opengl 引入）：glTexImage1D、glTexSubImage1D、glCopyTexImage1D、glCopyTexSubImage1D。
// 參數換成高度 1、yoffset 0 的 2D 形式交給後端，像素依 GL_UNPACK_* 版面讀取；後端依目標 GL_TEXTURE_1D（0x0DE0）／GL_PROXY_TEXTURE_1D（0x8063）
// 使用獨立的綁定與 proxy 狀態（Metal 以高度 1 的 2D 貼圖保存並固定 t 方向 clamp，宿主 OpenGL 直接呼叫系統的 1D 函式）。
if(symbol=="glTexImage1D" || symbol=="glTexSubImage1D"){
    // glTexImage1D(target,level,internalformat,width,border,format,type,pixels)；glTexSubImage1D(target,level,xoffset,width,format,type,pixels)。
    const bool sub=symbol=="glTexSubImage1D",proxy=!sub && d(0)==0x8063;
    if(n(3)<0 || (!proxy && n(3)>16384)){error(c,invalid_value);return finish(0);}
    const auto format=d(sub?4:5),type=d(sub?5:6);const auto pixels=arg(sub?6:7);
    auto& input=c.input_scratch;input.clear();
    if(!proxy && pixels){const auto bytes=pixel_bytes(c.client,n(3),1,format,type,false);if(!bytes){error(c,invalid_enum);return finish(0);}read_reusable(memory,pixels,*bytes,input);}
    const std::array<std::uint64_t,9> values=sub?std::array<std::uint64_t,9>{d(0),d(1),d(2),0,d(3),1,format,type,pixels}:std::array<std::uint64_t,9>{d(0),d(1),d(2),d(3),1,d(4),format,type,pixels};
    try{c.native->execute(sub?GlOperation::TexSubImage2D:GlOperation::TexImage2D,values,input,{});}catch(const std::bad_alloc&){error(c,out_of_memory);}
    return finish(0);
}
if(symbol=="glCopyTexImage1D" || symbol=="glCopyTexSubImage1D"){
    // glCopyTexImage1D(target,level,internalformat,x,y,width,border)；glCopyTexSubImage1D(target,level,xoffset,x,y,width)。
    const bool sub=symbol=="glCopyTexSubImage1D";if(n(5)<0){error(c,invalid_value);return finish(0);}
    const std::array<std::uint64_t,8> values=sub?std::array<std::uint64_t,8>{d(0),d(1),d(2),0,d(3),d(4),d(5),1}:std::array<std::uint64_t,8>{d(0),d(1),d(2),d(3),d(4),d(5),1,d(6)};
    try{c.native->execute(sub?GlOperation::CopyTexSubImage2D:GlOperation::CopyTexImage2D,values,{},{});}catch(const std::bad_alloc&){error(c,out_of_memory);}
    return finish(0);
}
