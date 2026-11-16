// opengl32 用戶端陣列的其餘入口（由 windows_opengl.cpp 的 invoke_opengl 引入，緊接在 glVertexPointer 等之後）：
// glEdgeFlagPointer、glIndexPointer，以及依 GL 1.1 §2.8 表 2.5 設定四個陣列的 glInterleavedArrays。
// 指標與 GL_ARRAY_BUFFER 綁定的處理和 glVertexPointer 相同：有綁定緩衝時 pointer 是緩衝內位移，使用 VAO 時沒有緩衝不可給客體指標。
const auto set_array=[&](unsigned index,std::int32_t size,std::uint32_t type,std::int32_t stride,std::uint64_t pointer)->bool{
    auto buffer=c.native->bound_buffer(0x8892);
    if(c.vertex_array && !buffer && pointer){error(c,invalid_operation);return false;}
    auto& a=c.client.arrays[index];a.pointer=pointer;a.size=size;a.stride=stride;a.type=type;a.buffer=std::move(buffer);return true;
};
if(symbol=="glEdgeFlagPointer"){
    // (stride, pointer)：元素為 GLboolean。
    if(n(0)<0){error(c,invalid_value);return finish(0);}set_array(4,1,0x1401,n(0),arg(1));return finish(0);
}
if(symbol=="glIndexPointer"){
    // (type, stride, pointer)：GL_UNSIGNED_BYTE、GL_SHORT、GL_INT、GL_FLOAT、GL_DOUBLE。
    const auto type=d(0);if(type!=0x1401 && type!=0x1402 && type!=0x1404 && type!=0x1406 && type!=0x140a){error(c,invalid_enum);return finish(0);}
    if(n(1)<0){error(c,invalid_value);return finish(0);}set_array(5,1,type,n(1),arg(2));return finish(0);
}
if(symbol=="glInterleavedArrays"){
    // 格式 GL_V2F（0x2A20）～GL_T4F_C4F_N3F_V4F（0x2A2D）：貼圖座標分量、顏色分量與型別、是否有法線、頂點分量，以及顏色／法線／頂點的位移與整體大小。
    struct Layout {std::int32_t texture,color;std::uint32_t color_type;bool normal;std::int32_t vertex;std::uint32_t color_offset,normal_offset,vertex_offset,size;};
    static constexpr Layout layouts[]{
        {0,0,0,false,2,0,0,0,8},{0,0,0,false,3,0,0,0,12},{0,4,0x1401,false,2,0,0,4,12},{0,4,0x1401,false,3,0,0,4,16},
        {0,3,0x1406,false,3,0,0,12,24},{0,0,0,true,3,0,0,12,24},{0,4,0x1406,true,3,0,16,28,40},{2,0,0,false,3,0,0,8,20},
        {4,0,0,false,4,0,0,16,32},{2,4,0x1401,false,3,8,0,12,24},{2,3,0x1406,false,3,8,0,20,32},{2,0,0,true,3,0,8,20,32},
        {2,4,0x1406,true,3,8,24,36,48},{4,4,0x1406,true,4,16,32,44,60}};
    const auto format=d(0);if(format<0x2a20 || format>0x2a2d){error(c,invalid_enum);return finish(0);}if(n(1)<0){error(c,invalid_value);return finish(0);}
    const auto& layout=layouts[format-0x2a20];const auto stride=n(1)?n(1):static_cast<std::int32_t>(layout.size);const auto pointer=arg(2);
    if(c.vertex_array && !c.native->bound_buffer(0x8892) && pointer){error(c,invalid_operation);return finish(0);}
    c.client.arrays[4].enabled=false;c.client.arrays[5].enabled=false;
    c.client.arrays[3].enabled=layout.texture!=0;if(layout.texture)set_array(3,layout.texture,0x1406,stride,pointer);
    c.client.arrays[2].enabled=layout.color!=0;if(layout.color)set_array(2,layout.color,layout.color_type,stride,pointer+layout.color_offset);
    c.client.arrays[1].enabled=layout.normal;if(layout.normal)set_array(1,3,0x1406,stride,pointer+layout.normal_offset);
    c.client.arrays[0].enabled=true;set_array(0,layout.vertex,0x1406,stride,pointer+layout.vertex_offset);
    return finish(0);
}
