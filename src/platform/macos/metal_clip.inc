// Metal 後端的使用者剪裁面（opengl_metal.mm 引入，位於 metal_texgen.inc 之後以共用反矩陣）：glClipPlane 在設定當下把方程式乘上模型視圖反矩陣
// 存成眼座標平面（p' = p M⁻¹），glGetClipPlane 回傳眼座標平面。固定管線的 vertex_main 以 dot(平面, 眼座標) 寫入 [[clip_distance]]，
// 選取／回饋模式的 CPU 裁切也一併套用已啟用的平面。GL_CLIP_PLANE0～7（0x3000～0x3007）的啟用狀態由可程式路徑共用的 state_.enabled 保存。
std::uint64_t MetalGl::clip_plane(GlOperation op,std::span<const std::uint64_t> a,std::span<const std::uint8_t> data,std::span<std::uint8_t> output){
    const auto plane=static_cast<unsigned>(a[0]);if(plane<0x3000 || plane>=0x3000+max_clip_planes){error(0x500);return 0;}
    auto& stored=state_.clip_planes[plane-0x3000];
    if(op==GlOperation::GetClipPlane){if(output.size()>=32)std::memcpy(output.data(),stored.data(),32);return 4;}
    if(data.size()<32){error(0x501);return 0;}
    std::array<double,4> equation{};std::memcpy(equation.data(),data.data(),32);
    const auto inverse=texgen_inverse(matrices_[0].back());
    if(!inverse){stored=equation;return 0;}
    std::array<double,4> eye{};for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)eye[i]+=equation[j]*(*inverse)[i*4+j];
    stored=eye;return 0;
}
// 頂點著色器的 buffer(7)：8 個眼座標平面，第 9 個 float4 的 x 為啟用位元；清除畫面時不啟用任何平面。
std::array<Vec,9> MetalGl::clip_uniform(bool clearing)const{
    std::array<Vec,9> result{};unsigned mask=0;
    for(unsigned i=0;i<max_clip_planes;++i){
        for(unsigned c=0;c<4;++c)result[i][c]=static_cast<float>(state_.clip_planes[i][c]);
        if(!clearing && state_.enabled.contains(0x3000+i))mask|=1U<<i;
    }
    result[8][0]=static_cast<float>(mask);return result;
}
