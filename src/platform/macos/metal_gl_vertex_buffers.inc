// VAO 與緩衝區契約集中於同一處；失效前先驗證完整範圍，未綁定 VAO 保留刪除中的物件。
static bool gl_buffer_target(unsigned target){
    return target==0x8892 || target==0x8893 || target==0x8a11 || target==0x8f36 || target==0x8f37;
}
static bool gl_shared_buffers_enabled(){
    static const bool enabled=[] {const auto* value=std::getenv("X64ARM_GL_SHARED_BUFFERS");return !value || std::string_view(value)!="0";}();return enabled;
}
// 只映射 Metal 可直接取用的格式，其他 GL 轉換仍走既有浮點展開。
static std::optional<MTLVertexFormat> gl_shared_vertex_format(const GlVertexAttribute& attribute){
    const auto components=attribute.components;if(!components || components>4)return {};
    unsigned format=0;
    if(attribute.type==0x1406){constexpr unsigned formats[]{41,16,6,2};format=formats[components-1];}
    else if(attribute.normalized && components!=3){
        const auto index=components==4?2U:components-1;
        switch(attribute.type){
        case 0x1400:{constexpr unsigned formats[]{63,51,31};format=formats[index];break;}
        case 0x1401:{constexpr unsigned formats[]{61,49,28};format=formats[index];break;}
        case 0x1402:{constexpr unsigned formats[]{58,37,13};format=formats[index];break;}
        case 0x1403:{constexpr unsigned formats[]{56,35,11};format=formats[index];break;}
        default:break;
        }
    }
    return format?std::optional<MTLVertexFormat>{metal_vertex_format(format)}:std::nullopt;
}
std::shared_ptr<GraphicsResource> MetalGl::gl_buffer_resource(const std::shared_ptr<GlBuffer>& buffer){
    if(!buffer->resource){
        GraphicsResourceDesc descriptor;descriptor.width=static_cast<std::uint32_t>(buffer->bytes.size());descriptor.bind=3;
        auto resource=graphics_resource(descriptor);graphics_write(GraphicsView{resource},buffer->bytes,0,0);
        buffer->resource=std::move(resource);++gl_.buffer_uploads;gl_.buffer_upload_bytes+=buffer->bytes.size();
    }return buffer->resource;
}
void MetalGl::gl_buffer_update(const std::shared_ptr<GlBuffer>& buffer,std::span<const std::uint8_t> data,std::uint32_t offset){
    if(!buffer->resource || data.empty())return;
    GraphicsView view{buffer->resource};view.first=offset;view.count=static_cast<std::uint32_t>(data.size());
    // DirectX 的 staging、queue 順序及資源使用追蹤保護 pending／in-flight draw。
    graphics_write(view,data,0,0);++gl_.buffer_uploads;gl_.buffer_upload_bytes+=data.size();
}
std::shared_ptr<GlBuffer> MetalGl::gl_buffer(unsigned target){
    if(target==0x8893)return gl_.vao().elements;
    const auto binding=gl_.buffer_bindings.find(target);
    if(binding==gl_.buffer_bindings.end() || !binding->second)return {};
    return gl_.buffers.at(binding->second);
}
std::optional<std::uint64_t> MetalGl::vertex_buffers_execute(GlOperation op,std::span<const std::uint64_t> args,std::span<const std::uint8_t> data,std::span<std::uint8_t> output){
    const auto u=[&](unsigned i){return static_cast<unsigned>(args[i]);};
    const auto n=[&](unsigned i){return std::bit_cast<int>(u(i));};
    const auto write=[&](unsigned value){if(output.size()>=4)std::memcpy(output.data(),&value,4);};
    const auto bound=[&](unsigned target)->std::shared_ptr<GlBuffer>{
        if(!gl_buffer_target(target)){error(0x500);return {};}
        auto buffer=gl_buffer(target);if(!buffer)error(0x502);return buffer;
    };
    switch(op){
    case GlOperation::GenVertexArrays:case GlOperation::GenBuffers:{
        if(n(0)<0 || output.size()<static_cast<std::size_t>(u(0))*4){error(0x501);return 0;}
        if(u(0)>65536 || (op==GlOperation::GenBuffers?gl_.buffers.size():gl_.vertex_arrays.size())+u(0)>65536)throw std::bad_alloc{};
        for(unsigned i=0;i<u(0);++i){unsigned name;
            if(op==GlOperation::GenBuffers){while(!gl_.next_buffer || gl_.buffers.contains(gl_.next_buffer))++gl_.next_buffer;name=gl_.next_buffer++;auto buffer=std::make_shared<GlBuffer>();buffer->name=name;gl_.buffers.emplace(name,std::move(buffer));}
            else{while(!gl_.next_vertex_array || gl_.vertex_arrays.contains(gl_.next_vertex_array))++gl_.next_vertex_array;name=gl_.next_vertex_array++;gl_.vertex_arrays.emplace(name,GlVertexArray{});}
            std::memcpy(output.data()+i*4,&name,4);
        }return 1;
    }
    case GlOperation::BindVertexArray:{
        const auto it=gl_.vertex_arrays.find(u(0));if(it==gl_.vertex_arrays.end()){error(0x502);return 0;}
        it->second.created=true;gl_.vertex_array=u(0);return 1;
    }
    case GlOperation::DeleteVertexArrays:
        if(n(0)<0 || data.size()<static_cast<std::size_t>(u(0))*4){error(0x501);return 0;}
        for(unsigned i=0;i<u(0);++i){unsigned name;std::memcpy(&name,data.data()+i*4,4);if(!name)continue;if(gl_.vertex_array==name)gl_.vertex_array=0;gl_.vertex_arrays.erase(name);}return 1;
    case GlOperation::IsVertexArray:{const auto it=gl_.vertex_arrays.find(u(0));return u(0) && it!=gl_.vertex_arrays.end() && it->second.created;}
    case GlOperation::IsBuffer:{const auto it=gl_.buffers.find(u(0));return u(0) && it!=gl_.buffers.end() && it->second->created;}
    case GlOperation::BindBuffer:{
        if(!gl_buffer_target(u(0))){error(0x500);return 0;}
        std::shared_ptr<GlBuffer> buffer;
        if(u(1)){auto it=gl_.buffers.find(u(1));if(it==gl_.buffers.end()){if(gl_.buffers.size()>=65536)throw std::bad_alloc{};buffer=std::make_shared<GlBuffer>();buffer->name=u(1);gl_.buffers.emplace(u(1),buffer);}else buffer=it->second;buffer->created=true;}
        if(u(0)==0x8893)gl_.vao().elements=buffer;else gl_.buffer_bindings[u(0)]=u(1);return 1;
    }
    case GlOperation::BufferData:case GlOperation::BufferSubData:{
        auto buffer=bound(u(0));if(!buffer)return 0;
        if(op==GlOperation::BufferData){
            if(args[1]>256U*1024U*1024U){error(0x501);return 0;}
            if(u(3)<0x88e0 || u(3)>0x88ea || (u(3)&3)==3){error(0x500);return 0;}
            if(!data.empty() && data.size()!=args[1]){error(0x501);return 0;}
            std::vector<std::uint8_t> replacement(static_cast<std::size_t>(args[1]),0);
            if(!data.empty())std::copy(data.begin(),data.end(),replacement.begin());
            buffer->bytes=std::move(replacement);buffer->usage=u(3);buffer->resource.reset();
        }else{
            if(args[1]>buffer->bytes.size() || args[2]>buffer->bytes.size()-args[1] || data.size()!=args[2]){error(0x501);return 0;}
            gl_buffer_update(buffer,data,static_cast<std::uint32_t>(args[1]));
            std::copy(data.begin(),data.end(),buffer->bytes.begin()+static_cast<std::ptrdiff_t>(args[1]));
        }return 1;
    }
    case GlOperation::GetBufferParameteriv:{
        auto buffer=bound(u(0));if(!buffer)return 0;
        switch(u(1)){case 0x8764:write(static_cast<unsigned>(buffer->bytes.size()));break;case 0x8765:write(buffer->usage);break;
        case 0x88bc:write(0);break;case 0x88bb:write(0x88ba);break;default:error(0x500);return 0;}return 1;
    }
    case GlOperation::GetBufferSubData:{
        auto buffer=bound(u(0));if(!buffer)return 0;
        if(args[1]>buffer->bytes.size() || args[2]>buffer->bytes.size()-args[1] || output.size()<args[2]){error(0x501);return 0;}
        if(args[2])std::memcpy(output.data(),buffer->bytes.data()+args[1],static_cast<std::size_t>(args[2]));return 1;
    }
    case GlOperation::CopyBufferSubData:{
        auto source=bound(u(0)),destination=bound(u(1));if(!source || !destination)return 0;
        const auto from=args[2],to=args[3],size=args[4];
        if(from>source->bytes.size() || size>source->bytes.size()-from || to>destination->bytes.size() || size>destination->bytes.size()-to){error(0x501);return 0;}
        if(source==destination && size && from<to+size && to<from+size){error(0x501);return 0;}
        if(size){gl_buffer_update(destination,std::span(source->bytes).subspan(static_cast<std::size_t>(from),static_cast<std::size_t>(size)),static_cast<std::uint32_t>(to));std::memcpy(destination->bytes.data()+to,source->bytes.data()+from,static_cast<std::size_t>(size));}return 1;
    }
    case GlOperation::DeleteBuffers:
        if(n(0)<0 || data.size()<static_cast<std::size_t>(u(0))*4){error(0x501);return 0;}
        for(unsigned i=0;i<u(0);++i){unsigned name;std::memcpy(&name,data.data()+i*4,4);if(!name)continue;auto it=gl_.buffers.find(name);if(it==gl_.buffers.end())continue;
            for(auto& [target,binding]:gl_.buffer_bindings){(void)target;if(binding==name)binding=0;}
            for(auto& [target,binding]:gl_.indexed_bindings){(void)target;if(binding==name)binding=0;}
            for(auto& attribute:gl_.vao().attributes)if(attribute.buffer==it->second)attribute.buffer.reset();
            if(gl_.vao().elements==it->second)gl_.vao().elements.reset();gl_.buffers.erase(it);
        }return 1;
    case GlOperation::BindBufferBase:{
        if(u(0)!=0x8a11){error(0x500);return 0;}if(u(1)>=16){error(0x501);return 0;}
        const std::array<std::uint64_t,2> binding{u(0),u(2)};
        vertex_buffers_execute(GlOperation::BindBuffer,binding,{},{});gl_.indexed_bindings[u(1)]=u(2);return 1;
    }
    case GlOperation::EnableVertexAttribArray:case GlOperation::DisableVertexAttribArray:case GlOperation::VertexAttribPointer:case GlOperation::VertexAttribDivisor:
    case GlOperation::VertexAttrib1f:case GlOperation::VertexAttrib2f:case GlOperation::VertexAttrib3f:case GlOperation::VertexAttrib4f:{
        if(u(0)>=16){error(0x501);return 0;}auto& attribute=gl_.vao().attributes[u(0)];
        if(op==GlOperation::EnableVertexAttribArray || op==GlOperation::DisableVertexAttribArray)attribute.enabled=op==GlOperation::EnableVertexAttribArray;
        else if(op==GlOperation::VertexAttribDivisor)attribute.divisor=u(1);
        else if(op==GlOperation::VertexAttribPointer){
            if(n(1)<1 || n(1)>4 || n(4)<0){error(0x501);return 0;}if(u(2)<0x1400 || u(2)>0x1406){error(0x500);return 0;}
            auto buffer=gl_buffer(0x8892);if(!buffer){error(0x502);return 0;}
            attribute.buffer=buffer;attribute.components=u(1);attribute.type=u(2);attribute.normalized=u(3)!=0;attribute.stride=u(4);attribute.offset=args[5];
        }else{const auto count=op==GlOperation::VertexAttrib1f?1U:op==GlOperation::VertexAttrib2f?2U:op==GlOperation::VertexAttrib3f?3U:4U;auto& value=gl_.attribute_values[u(0)];value={0,0,0,1};for(unsigned i=0;i<count;++i)value[i]=std::bit_cast<float>(u(i+1));}
        return 1;
    }
    case GlOperation::GetVertexAttribiv:case GlOperation::GetVertexAttribfv:case GlOperation::GetVertexAttribPointerv:{
        if(u(0)>=16){error(0x501);return 0;}const auto& attribute=gl_.vao().attributes[u(0)];
        if(op==GlOperation::GetVertexAttribPointerv){if(u(1)!=0x8645){error(0x500);return 0;}if(output.size()!=4 && output.size()!=8){error(0x501);return 0;}std::memcpy(output.data(),&attribute.offset,output.size());return 1;}
        std::array<double,4> values{};unsigned count=1;
        switch(u(1)){
        case 0x8622:values[0]=attribute.enabled;break;case 0x8623:values[0]=attribute.components;break;case 0x8624:values[0]=attribute.stride;break;
        case 0x8625:values[0]=attribute.type;break;case 0x886a:values[0]=attribute.normalized;break;case 0x889f:values[0]=attribute.buffer?attribute.buffer->name:0;break;
        case 0x88fe:values[0]=attribute.divisor;break;case 0x8626:count=4;std::copy(gl_.attribute_values[u(0)].begin(),gl_.attribute_values[u(0)].end(),values.begin());break;
        default:error(0x500);return 0;}
        if(output.size()<count*4){error(0x501);return 0;}for(unsigned i=0;i<count;++i){if(op==GlOperation::GetVertexAttribfv){const auto value=static_cast<float>(values[i]);std::memcpy(output.data()+i*4,&value,4);}else{const auto value=static_cast<std::int32_t>(std::clamp(std::isnan(values[i])?0.:std::round(values[i]),double(INT32_MIN),double(INT32_MAX)));std::memcpy(output.data()+i*4,&value,4);}}return 1;
    }
    default:return std::nullopt;
    }
}
