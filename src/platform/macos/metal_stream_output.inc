// GPU 完成 VS／GS 後，依 SO 宣告搬移原始分量。溢位以完整圖元為單位，
// 多個 buffer 同步停止；$SKIP 與 stride 尾端保留原資料。
struct MetalStreamLayout {
    struct Copy {unsigned source{},destination{},bytes{},slot{};};
    std::vector<Copy> copies;std::array<bool,4> used{};
};
MetalStreamLayout metal_stream_layout(const MetalShader& shader){
    MetalStreamLayout result;std::array<unsigned,4> offsets{};
    const auto equal=[](std::string_view a,std::string_view b){if(a.size()!=b.size())return false;for(std::size_t i=0;i<a.size();++i){auto upper=[](char c){return c>='a' && c<='z'?c-'a'+'A':int(c);};if(upper(a[i])!=upper(b[i]))return false;}return true;};
    for(const auto& e:shader.stream_output){
        if(e.slot>=4 || e.stream)throw std::invalid_argument("Metal SO 宣告範圍不合法");result.used[e.slot]=true;
        if(!e.semantic.empty()){
            const auto v=std::find_if(shader.output_varyings.begin(),shader.output_varyings.end(),[&](const auto& item){return equal(item.semantic,e.semantic) && item.index==e.index;});
            if(v==shader.output_varyings.end() || e.first+e.count>v->components)throw std::invalid_argument("Metal SO 輸出分量不存在");
            result.copies.push_back({static_cast<unsigned>(v-shader.output_varyings.begin())*16+e.first*4,offsets[e.slot],e.count*4,e.slot});
        }
        offsets[e.slot]+=e.count*4;
    }
    return result;
}
void metal_stream_output(const GraphicsDraw& d,const MetalShader& shader,const std::uint8_t* vertices,
                         const std::uint32_t* counts,unsigned invocations,unsigned capacity,unsigned width){
    const auto layout=metal_stream_layout(shader);
    std::array<std::uint8_t*,4> destination{};bool bound=false,overflow=false;
    for(unsigned i=0;i<4;++i)if(layout.used[i] && d.stream_targets[i]){destination[i]=static_cast<std::uint8_t*>(metal_resource(d.stream_targets[i]).buffer.contents);bound=true;}
    std::uint64_t written=0,needed=0;
    for(unsigned invocation=0;invocation<invocations;++invocation){
        const auto count=counts?counts[invocation]:capacity;
        if(count>capacity || count%width)throw std::invalid_argument("Metal SO GPU 頂點計數不合法");
        needed+=count/width;
        for(unsigned at=0;at<count;at+=width){
            if(!bound || overflow)continue;
            for(unsigned slot=0;slot<4;++slot)if(destination[slot]){const auto& target=*d.stream_targets[slot];const auto bytes=std::uint64_t{width}*shader.stream_strides[slot];if(target.stream_cursor>target.desc.width || bytes>target.desc.width-target.stream_cursor)overflow=true;}
            if(overflow)continue;
            for(unsigned vertex=0;vertex<width;++vertex){const auto source=vertices+(std::uint64_t{invocation}*capacity+at+vertex)*shader.output_varyings.size()*16;
                for(const auto& copy:layout.copies)if(destination[copy.slot])std::memcpy(destination[copy.slot]+d.stream_targets[copy.slot]->stream_cursor+std::uint64_t{vertex}*shader.stream_strides[copy.slot]+copy.destination,source+copy.source,copy.bytes);
            }
            for(unsigned slot=0;slot<4;++slot)if(destination[slot]){auto& target=*d.stream_targets[slot];target.stream_cursor+=std::uint64_t{width}*shader.stream_strides[slot];target.stream_vertices+=width;}
            ++written;
        }
    }
    if(d.stream_statistics){d.stream_statistics->written+=written;d.stream_statistics->needed+=needed;}
}
