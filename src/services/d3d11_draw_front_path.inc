// 繪製狀態快照與繪製方法的前置快路徑（X64ARM_D3D11_DRAW_FRONT_PATH=0 只關閉前置路徑）。
// 快照由完整路徑與前置路徑共用。立即內容的 Draw*／Dispatch* 每幀數千次，原本每次都以字串鍵查 bindings 約 30 次，又要走完完整分派的方法名稱比對鏈；
// 這裡改用固定名稱的節點指標（弱綁定的 D3D10 內容仍依名稱查詢），並在只需要 compute 或繪圖階段時略過另一邊。
// 其餘情況（predicate、延遲內容、裝置鎖、非共用池的查詢、參數不合法、D3D10）在改動可觀察狀態前交回完整路徑；途中拋出的例外（含取回命令執行緒延後的例外）
// 交回完整路徑重新拋出，與在處理程式內拋出時走相同的錯誤處理。
bool d3d11_direct_draw_enabled() noexcept {
    // 與完整路徑的立即提交共用；X64ARM_D3D11_DIRECT_DRAW=0 回到一律經 submit（同引擎對照）。
    static const bool value=[] {const auto* text=std::getenv("X64ARM_D3D11_DIRECT_DRAW");return !text || std::string_view(text)!="0";}();
    return value;
}
// 共用池的可見度槽位（池建立時已清零，只遞增游標，用完換新池並開新區段）。呼叫端已核對 query 用共用池且 sequence 未滿。
// 共用可見度池分配一個槽位給這次繪製；查詢以區段（池、起點、槽數）記錄自己用到的槽位，不連續時另開區段。
// 遮蔽查詢與管線統計查詢（像素著色器呼叫數）可以共用同一次繪製的槽位。
void d3d11_pooled_slot(D3D11State& bridge,Identity& context,GraphicsDraw& draw){
    if(!context.visibility_pool || context.visibility_cursor+8>d3d11_visibility_pool_bytes){
        GraphicsResourceDesc desc;desc.width=d3d11_visibility_pool_bytes;
        context.visibility_pool=context.native->graphics_resource(desc);context.visibility_cursor=0;++bridge.resource_creations["VisibilityPool"];
    }
    draw.visibility=context.visibility_pool;draw.visibility_offset=context.visibility_cursor;
    context.visibility_cursor+=8;
}
void d3d11_visibility_append(Identity& query,const GraphicsDraw& draw){
    auto& segments=query.visibility_segments;
    if(segments.empty() || segments.back().buffer!=draw.visibility || segments.back().base+segments.back().count*8U!=draw.visibility_offset)
        segments.push_back({draw.visibility,draw.visibility_offset,0});
    ++segments.back().count;
}
void d3d11_pooled_visibility(D3D11State& bridge,Identity& context,Identity& query,GraphicsDraw& draw){
    d3d11_pooled_slot(bridge,context,draw);d3d11_visibility_append(query,draw);++query.sequence;
}
// Graphics 不更新 CS 階段，Compute 只更新 CS 階段；未更新的部分在這份快取裡維持上次內容（原始指標與解析值仍一致），只有對應的提交才會讀它。
enum class D3D11DrawScope:std::uint8_t {All,Graphics,Compute};
GraphicsDraw& d3d11_draw_state(D3D11State& bridge,const std::shared_ptr<Identity>& context,const D3D11DrawScope scope,const std::string_view symbol){
    static const std::array<std::string,6> stage_names{"VS","PS","GS","HS","DS","CS"};
    const auto require=[&](bool value){if(!value)throw std::invalid_argument("D3D11 參數或狀態不合法："+std::string(symbol));};
    const auto object=[&](std::uint64_t address)->const Object& {const auto found=bridge.objects.find_fast(address);if(found==bridge.objects.end())throw HostCallFault("D3D11 COM 物件已釋放或位址無效");return found->second;};
    const auto typed=[&](std::uint64_t address,Kind kind)->const std::shared_ptr<Identity>& {const auto& value=object(address);if(value.kind!=kind)throw HostCallFault("D3D11 COM 介面類型不符");return value.identity;};
    const auto view_of=[&](std::uint64_t pointer){if(!pointer)return GraphicsView{};const auto& id=*object(pointer).identity;require(bool(id.resource) || bool(id.graphics_view.resource));return id.graphics_view.resource?id.graphics_view:GraphicsView{id.resource,id.resource->desc.format,0,id.resource->desc.levels,0,id.resource->desc.layers};};
    const auto binding=[&](const std::shared_ptr<Identity>& owner,const std::string& name,std::size_t index=0)->std::uint64_t{const auto found=owner->bindings.find(name);return found!=owner->bindings.end() && index<found->second.size()?found->second[index]:0;};
    // 固定名稱走節點指標；弱綁定內容的 bindings 會被整份換掉（見 Identity::fixed_bindings），維持依名稱查詢。
    const bool fixed=!context->weak_bindings;
    const auto named=[&](const std::string& name)->const std::vector<std::uint64_t>* {const auto found=context->bindings.find(name);return found==context->bindings.end()?nullptr:&found->second;};
    const auto values=[&](unsigned key,const std::string& name)->const std::vector<std::uint64_t>* {return fixed?&d3d11_fixed_slots(*context,key):named(name);};
    const auto single=[&](unsigned key,const std::string& name)->std::uint64_t {const auto* slots=values(key,name);return slots && !slots->empty()?slots->front():0;};
    // D3D11 只有 PS（OMSetRenderTargetsAndUnorderedAccessViews）與 CS 有 UAV 綁定。
    const auto stage_uavs=[&](unsigned stage,const std::string& name)->const std::vector<std::uint64_t>* {
        if(stage==1)return values(d3d11_slot_ps_uav,name);
        if(stage==5)return values(d3d11_slot_cs_uav,name);
        return fixed?nullptr:named(name);
    };
    static const bool snapshot_enabled=[](){const char* option=std::getenv("X64ARM_D3D11_BINDING_SNAPSHOT");return !option || std::string_view(option)!="0";}();
    // X64ARM_D3D11_DRAW_CACHE=0 每次整份重新解析（快取前的行為）。
    static const bool draw_cache_enabled=[](){const char* option=std::getenv("X64ARM_D3D11_DRAW_CACHE");return !option || std::string_view(option)!="0";}();
    static const auto stage_keys=[]{
        std::array<std::array<std::string,5>,6> keys;const std::array<std::string_view,5> suffixes{"Shader","ConstantBuffers","ShaderResources","Samplers","UnorderedAccessViews"};
        for(unsigned stage=0;stage<6;++stage)for(unsigned kind=0;kind<5;++kind)keys[stage][kind]=stage_names[stage]+std::string(suffixes[kind]);
        return keys;
    }();
    static const std::string stream_key{"StreamOutput"},layout_key{"Layout"},index_key{"Index"},vertex_key{"Vertices"},target_key{"Targets"},depth_key{"Depth"},blend_key{"Blend"},depth_state_key{"DepthState"},raster_key{"Raster"};
    // 多份快取輪流使用：先前以參照排入命令執行緒的繪製可能仍在讀前幾份，只等使用同一份的那次繪製
    // （連續繪製間客體工作很少時，少量份數仍會等待；DQ3 四份時同步等待約 6%，改八份吸收更深的突發）。
    context->draw_slot=(context->draw_slot+1)%d3d11_draw_cache_count();
    auto& cache=context->draw_caches[context->draw_slot];if(!cache)cache=std::make_unique<D3D11DrawCache>();
    if(context->native)context->native->graphics_sync_until(cache->ticket);
    if(snapshot_enabled && draw_cache_enabled && !context->weak_bindings && cache->destroyed==bridge.destroyed_objects)++bridge.draw_cache_incremental;
    // 就地解構再建構（命令執行緒已用完這份快取）；以暫存物件指派會讓每次呼叫的堆疊框架多出約 64 KB。
    else {std::destroy_at(cache.get());std::construct_at(cache.get());++bridge.draw_cache_full;}
    cache->destroyed=bridge.destroyed_objects;
    auto& draw=cache->draw;
    try{
    if(scope!=D3D11DrawScope::Compute){
    draw.topology=context->topology;draw.index_format=context->index_format;draw.index_offset=context->index_offset;draw.viewports=context->viewports;draw.scissors=context->scissors;draw.blend_factor=context->blend_factor;draw.sample_mask=context->sample_mask;draw.stencil_reference=context->stencil_reference;
    }
    // 每次繪製的參數由呼叫端填入，先回到預設值。
    draw.count=0;draw.start=0;draw.instances=1;draw.start_instance=0;draw.base_vertex=0;draw.alpha_function=0;draw.alpha_reference=0;draw.indexed=false;
    draw.visibility.reset();draw.visibility_offset=0;draw.indirect.reset();draw.indirect_offset=0;
        // 只對原始指標與上次不同的槽位重新解析；raw 只保留 limit 內的槽位。
    const auto refresh=[](std::vector<std::uint64_t>& raw,const std::vector<std::uint64_t>* now,std::size_t limit,const auto& assign){
        // 向量尾端的空槽（曾綁定後解除）與未綁定相同，不必逐槽比對，raw 也只存到最後一個非空槽。
        std::size_t current=now?std::min(limit,now->size()):0;
        if(now)while(current && !(*now)[current-1])--current;
        if(raw.size()==current && std::equal(raw.begin(),raw.end(),now?now->begin():raw.begin()))return;
        for(std::size_t i=0;i<std::max(current,raw.size());++i){const auto value=i<current?(*now)[i]:0,old=i<raw.size()?raw[i]:0;if(value!=old)assign(i,value);}
        if(now)raw.assign(now->begin(),now->begin()+static_cast<std::ptrdiff_t>(current));else raw.clear();
    };
    const unsigned first_stage=scope==D3D11DrawScope::Compute?5U:0U,last_stage=scope==D3D11DrawScope::Graphics?4U:5U;
    for(unsigned stage=first_stage;stage<=last_stage;++stage){const auto& name=stage_names[stage];const auto& keys=stage_keys[stage];const auto shader_pointer=single(d3d11_stage_slot(stage,d3d11_stage_shader),keys[0]);auto& target=draw.stages[stage];
        if(snapshot_enabled){
            // 保留 PS 的輸出 UAV（即使 PS 為空）；其他未綁 shader 的 stage 不取快照。
            if(!shader_pointer && stage!=1){
                if(cache->present[stage]){std::destroy_at(&target);std::construct_at(&target);cache->present[stage]=false;cache->shaders[stage]=0;for(auto& raw:cache->stage_raw[stage])raw.clear();}
                continue;
            }
            cache->present[stage]=true;
            if(cache->shaders[stage]!=shader_pointer){target.shader=shader_pointer?object(shader_pointer).identity->shader:nullptr;cache->shaders[stage]=shader_pointer;}
            auto& raw=cache->stage_raw[stage];
            refresh(raw[0],values(d3d11_stage_slot(stage,d3d11_stage_constants),keys[1]),14,[&](auto i,auto pointer){target.constants[i]=pointer?object(pointer).identity->resource:nullptr;});
            refresh(raw[1],values(d3d11_stage_slot(stage,d3d11_stage_resources),keys[2]),128,[&](auto i,auto pointer){target.resources[i]=view_of(pointer);});
            refresh(raw[2],values(d3d11_stage_slot(stage,d3d11_stage_samplers),keys[3]),16,[&](auto i,auto pointer){target.samplers[i]=pointer?object(pointer).identity->sampler:nullptr;});
            refresh(raw[3],stage_uavs(stage,keys[4]),8,[&](auto i,auto pointer){target.unordered[i]=view_of(pointer);});
            target.constants[14].reset();
        }else {
            if(shader_pointer)target.shader=object(shader_pointer).identity->shader;
            for(unsigned i=0;i<14;++i)if(auto ptr=binding(context,keys[1],i))target.constants[i]=object(ptr).identity->resource;
            for(unsigned i=0;i<128;++i)if(auto ptr=binding(context,keys[2],i))target.resources[i]=view_of(ptr);
            for(unsigned i=0;i<16;++i)if(auto ptr=binding(context,keys[3],i))target.samplers[i]=object(ptr).identity->sampler;
            for(unsigned i=0;i<8;++i)if(auto ptr=binding(context,keys[4],i))target.unordered[i]=view_of(ptr);
        }
        // D3D11.1 常數緩衝範圍：位移以 16 位元組常數計，256 位元組對齊，符合 Metal 的緩衝位移要求。
        for(unsigned i=0;i<14;++i)target.constant_offsets[i]=context->constant_first[stage][i]*16U;
        if(shader_pointer){const auto& shader_id=object(shader_pointer).identity;
            if(shader_id->reflection && shader_id->reflection->linkage){
                require(!context->context_type);const auto& linked=*shader_id->reflection->linkage;
                // 同一引擎的效能 A/B 可關閉快取，保留原有逐 draw 讀回／上傳路徑。
                static const bool cache_enabled=[](){const char* option=std::getenv("X64ARM_D3D11_LINKAGE_CACHE");return !option || std::string_view(option)!="0";}();
                ++bridge.linkage_draws;std::vector<std::uint8_t> bytes(4096);std::map<unsigned,std::vector<std::uint8_t>> constants;unsigned used=0;
                for(const auto& entry:linked.interfaces)for(unsigned element=0;element<entry.count;++element){
                    const auto slot=entry.slot+element;const auto instance=typed(binding(context,name+"ClassInstances",slot),Kind::ClassInstance);
                    std::string type_name=instance->class_type;unsigned buffer=instance->class_desc[3],offset=instance->class_desc[4]*16;
                    if(!instance->class_desc[7]){
                        const auto found=std::find_if(linked.instances.begin(),linked.instances.end(),[&](const auto& item){return item.name==instance->class_name;});
                        require(found!=linked.instances.end() && instance->class_desc[1]<found->count);type_name=found->type;buffer=found->buffer;offset=found->offset;
                        put_word(bytes,slot*16+8,static_cast<std::uint32_t>(found-linked.instances.begin())+1);put_word(bytes,slot*16+12,instance->class_desc[1]);
                    }
                    const auto type=std::find_if(linked.types.begin(),linked.types.end(),[&](const auto& item){return item.name==type_name;});
                    require(type!=linked.types.end() && std::find(entry.types.begin(),entry.types.end(),type->id)!=entry.types.end());
                    if(!instance->class_desc[7])offset+=type->size*instance->class_desc[1];
                    require(used<=61440 && type->size<=61440-used);put_word(bytes,slot*16,type->id);put_word(bytes,slot*16+4,used/16);
                    // 具名 class 已由 shader 讀取原 CB，無須先同步讀回 GPU。
                    // 舊版中繼資料與 CreateClassInstance 自訂 offset 保留 raw payload。
                    if(!instance->class_desc[7] && linked.direct_named && cache_enabled)continue;
                    bytes.resize(65536);
                    if(const auto resource=draw.stages[stage].constants[buffer]){
                        if(!constants.contains(buffer)){auto read=context->native->graphics_read(GraphicsView{resource});bridge.linkage_readback_bytes+=read.size();constants.emplace(buffer,std::move(read));}
                        const auto& data=constants.at(buffer);if(offset<data.size()){const auto count=std::min<std::size_t>(type->size,data.size()-offset);std::copy_n(data.begin()+offset,count,bytes.begin()+4096+used);}
                    }
                    used+=type->size;
                }
                if(cache_enabled && context->linkage_constants[stage] && context->linkage_keys[stage]==bytes){
                    ++bridge.linkage_cache_hits;draw.stages[stage].constants[14]=context->linkage_constants[stage];
                }else {
                    // 每個 context／stage 只快取上一組 binding；不覆寫仍在 GPU 使用的 buffer。
                    auto key=bytes;bytes.resize(65536);GraphicsResourceDesc desc;desc.width=65536;desc.bind=4;
                    auto data=context->native->graphics_resource(desc);++bridge.resource_creations["ClassLinkage"];context->native->graphics_write(GraphicsView{data},bytes,65536,65536);
                    context->linkage_keys[stage]=std::move(key);context->linkage_constants[stage]=data;draw.stages[stage].constants[14]=std::move(data);++bridge.linkage_uploads;
                }
            }
        }
    }
    draw.stages[5].statistics=context->pipeline_statistics;
    if(scope!=D3D11DrawScope::Compute){
    draw.stream_statistics=context->stream_statistics;draw.pipeline_statistics=context->pipeline_statistics;
    refresh(cache->stream_raw,values(d3d11_slot_stream,stream_key),4,[&](auto i,auto pointer){draw.stream_targets[i]=pointer?object(pointer).identity->resource:nullptr;});
    if(const auto ptr=single(d3d11_slot_layout,layout_key);ptr!=cache->layout){
        const auto* layout=ptr?object(ptr).identity.get():nullptr;
        draw.layout=layout?layout->layout:std::vector<GraphicsElement>{};
        draw.pipeline_layout_serial=layout?layout->pipeline_state_serial:0;
        cache->layout=ptr;
    }
    // 診斷：X64ARM_TRACE_D3D11_LAYOUT=1 列出沒有輸入佈局的繪製（前 400 次）與佈局物件的元素數。
    {static const bool trace_layout=std::getenv("X64ARM_TRACE_D3D11_LAYOUT")!=nullptr;static std::atomic<unsigned> traced{};
        if(trace_layout && draw.layout.empty() && traced.fetch_add(1,std::memory_order_relaxed)<400){const auto ptr=single(d3d11_slot_layout,layout_key);std::fprintf(stderr,"[d3d11_layout] draw context=%p type=%u fixed=%d slot_pointer=%llx cached=%llx object_elements=%zu slot=%u\n",static_cast<void*>(context.get()),static_cast<unsigned>(context->context_type),fixed?1:0,static_cast<unsigned long long>(ptr),static_cast<unsigned long long>(cache->layout),ptr?object(ptr).identity->layout.size():0U,static_cast<unsigned>(context->draw_slot));}}
    if(const auto ptr=single(d3d11_slot_index,index_key);ptr!=cache->index){draw.indices=ptr?object(ptr).identity->resource:nullptr;cache->index=ptr;}
    // stride／offset 存在 context，指標未變也要逐槽同步。
    refresh(cache->vertex_raw,values(d3d11_slot_vertices,vertex_key),32,[&](auto i,auto pointer){draw.vertices[i]=pointer?GraphicsVertexBuffer{object(pointer).identity->resource,0,0}:GraphicsVertexBuffer{};});
    for(std::size_t i=0;i<cache->vertex_raw.size();++i)if(draw.vertices[i].resource){draw.vertices[i].stride=context->strides[i];draw.vertices[i].offset=context->offsets[i];}
    refresh(cache->target_raw,values(d3d11_slot_targets,target_key),8,[&](auto i,auto pointer){draw.targets[i]=view_of(pointer);});
    if(const auto ptr=single(d3d11_slot_depth,depth_key);ptr!=cache->depth){draw.depth=view_of(ptr);cache->depth=ptr;}
    if(const auto ptr=single(d3d11_slot_blend,blend_key);ptr!=cache->blend){
        const auto* blend=ptr?object(ptr).identity.get():nullptr;
        draw.blend=blend?blend->desc:std::vector<std::uint8_t>{};
        draw.pipeline_blend_serial=blend?blend->pipeline_state_serial:0;
        cache->blend=ptr;
    }
    if(const auto ptr=single(d3d11_slot_depth_state,depth_state_key);ptr!=cache->depth_state){draw.depth_state=ptr?object(ptr).identity->desc:std::vector<std::uint8_t>{};cache->depth_state=ptr;}
    if(const auto ptr=single(d3d11_slot_raster,raster_key);ptr!=cache->raster){draw.raster=ptr?object(ptr).identity->desc:std::vector<std::uint8_t>{};cache->raster=ptr;}
    }
    }catch(...){cache->destroyed=UINT64_MAX;throw;}
    // 呼叫端接著以參照排入的繪製會取得下一個序號；保守記錄，之後重用這份前等到它完成。
    cache->ticket=context->native?context->native->graphics_ticket()+1:0;
    return draw;
}
enum class D3D11DrawMethod:std::uint8_t {None,Draw,DrawIndexed,DrawInstanced,DrawIndexedInstanced,DrawInstancedIndirect,DrawIndexedInstancedIndirect,Dispatch,DispatchIndirect};
// 其他方法在第一個字元就排除，不增加常見方法的成本。DrawAuto 走完整路徑。
D3D11DrawMethod d3d11_draw_method(std::string_view symbol) noexcept {
    if(symbol.size()<4 || symbol[0]!='D')return D3D11DrawMethod::None;
    if(symbol=="DrawIndexed")return D3D11DrawMethod::DrawIndexed;
    if(symbol=="DrawIndexedInstanced")return D3D11DrawMethod::DrawIndexedInstanced;
    if(symbol=="Draw")return D3D11DrawMethod::Draw;
    if(symbol=="DrawInstanced")return D3D11DrawMethod::DrawInstanced;
    if(symbol=="Dispatch")return D3D11DrawMethod::Dispatch;
    if(symbol=="DrawIndexedInstancedIndirect")return D3D11DrawMethod::DrawIndexedInstancedIndirect;
    if(symbol=="DrawInstancedIndirect")return D3D11DrawMethod::DrawInstancedIndirect;
    if(symbol=="DispatchIndirect")return D3D11DrawMethod::DispatchIndirect;
    return D3D11DrawMethod::None;
}
D3D11DrawMethod d3d11_draw_method(const WindowsServiceDescriptor& service) noexcept {
    static const std::vector<D3D11DrawMethod> table=[] {
        const auto descriptors=runtime_service_descriptors();std::vector<D3D11DrawMethod> value(descriptors.size());
        for(std::size_t i=0;i<descriptors.size();++i)value[i]=d3d11_draw_method(descriptors[i].symbol);
        return value;
    }();
    return d3d11_method_by_descriptor(service,table,[](std::string_view symbol){return d3d11_draw_method(symbol);});
}
// 前置路徑途中的例外：同一執行緒接著走完整路徑時，在它的 try 內重新拋出。
thread_local std::exception_ptr d3d11_front_exception;
// 回傳 true 表示已完成並可直接回到客體（void 方法與完整路徑相同以 0 返回）。呼叫端持圖形鎖，且已比照前置路徑核對 compare 模式與延後銷毀清單。
// arguments 已由呼叫端在映射鎖內讀好；release_mapping 在通過全部資格之後、開始做事之前呼叫一次，之後不再碰客體記憶體（見呼叫端）。
template<class ReleaseMapping>
[[gnu::noinline]] bool d3d11_draw_front_path(D3D11State& bridge,const std::array<std::uint64_t,6>& arg,const D3D11DrawMethod method,const std::string_view symbol,const ReleaseMapping& release_mapping) try {
    if(!d3d11_direct_draw_enabled())return false;
    const auto self=bridge.objects.find_fast(arg[0]);
    if(self==bridge.objects.end() || self->second.kind!=Kind::Context || !self->second.identity)return false;
    const auto& context=self->second.identity;
    if(context->kind!=Kind::Context || context->api10 || context->weak_bindings || context->context_type || !context->native)return false;
    const auto device=context->device?bridge.objects.find_fast(context->device):self;
    if(device==bridge.objects.end() || !device->second.identity ||
       (device->second.identity->protected_threads && device->second.identity->lock_address))return false;
    if(const auto& predicate=d3d11_fixed_slots(*context,d3d11_slot_predicate);!predicate.empty() && predicate.front())return false;
    // 進行中的管線統計查詢要為每次繪製分配可見度槽位（像素著色器呼叫數），走完整路徑。
    if(!context->pipeline_queries.empty())return false;
    const auto u32=[&](unsigned i){return static_cast<std::uint32_t>(arg[i]);};
    // 進行中的遮蔽查詢：只處理共用池；其他查詢要在同一提交順序補清緩衝，走完整路徑。
    Identity* query=nullptr;
    const auto visibility_ready=[&]{
        const auto& slots=d3d11_fixed_slots(*context,d3d11_slot_visibility);
        if(slots.empty() || !slots.front())return true;
        const auto found=bridge.objects.find_fast(slots.front());
        if(found==bridge.objects.end() || !found->second.identity)return false;
        query=found->second.identity.get();
        return query->visibility_pooled && query->sequence<8192;
    };
    const auto arguments=[&](std::uint32_t size)->const GraphicsResource* {
        const auto found=bridge.objects.find_fast(arg[1]);
        if(found==bridge.objects.end() || found->second.kind!=Kind::BufferObject || !found->second.identity || !found->second.identity->resource)return nullptr;
        const auto& id=*found->second.identity;const auto& desc=id.resource->desc;const auto offset=u32(2);
        if(id.device!=context->device || !(desc.misc&0x10U) || (offset&3U) || std::uint64_t{offset}+size>desc.width)return nullptr;
        return id.resource.get();
    };
    const auto argument_resource=[&]()->const std::shared_ptr<GraphicsResource>& {return bridge.objects.find_fast(arg[1])->second.identity->resource;};
    const auto submit_draw=[&](GraphicsDraw& draw){
        if(query)d3d11_pooled_visibility(bridge,*context,*query,draw);
        context->native->graphics_draw_retained(draw);
        ++context->draws;++bridge.graphics_draws;
    };
    switch(method){
    case D3D11DrawMethod::Draw:case D3D11DrawMethod::DrawIndexed:case D3D11DrawMethod::DrawInstanced:case D3D11DrawMethod::DrawIndexedInstanced:{
        if(!visibility_ready())return false;
        release_mapping();
        const bool indexed=method==D3D11DrawMethod::DrawIndexed || method==D3D11DrawMethod::DrawIndexedInstanced;
        const bool instanced=method==D3D11DrawMethod::DrawInstanced || method==D3D11DrawMethod::DrawIndexedInstanced;
        auto& draw=d3d11_draw_state(bridge,context,D3D11DrawScope::Graphics,symbol);
        draw.count=u32(1);draw.indexed=indexed;draw.instances=instanced?u32(2):1;draw.start=u32(instanced?3:2);
        if(indexed)draw.base_vertex=static_cast<std::int32_t>(u32(instanced?4:3));
        if(instanced)draw.start_instance=u32(indexed?5:4);
        submit_draw(draw);
        return true;
    }
    case D3D11DrawMethod::DrawInstancedIndirect:case D3D11DrawMethod::DrawIndexedInstancedIndirect:{
        const bool indexed=method==D3D11DrawMethod::DrawIndexedInstancedIndirect;
        if(!arguments(indexed?20U:16U) || !visibility_ready())return false;
        release_mapping();
        auto& draw=d3d11_draw_state(bridge,context,D3D11DrawScope::Graphics,symbol);
        draw.indexed=indexed;draw.indirect=argument_resource();draw.indirect_offset=u32(2);
        submit_draw(draw);
        return true;
    }
    case D3D11DrawMethod::Dispatch:{
        release_mapping();
        const auto& stage=d3d11_draw_state(bridge,context,D3D11DrawScope::Compute,symbol).stages[5];
        context->native->graphics_dispatch_retained(stage,std::array{u32(1),u32(2),u32(3)});
        ++context->dispatches;++bridge.graphics_dispatches;
        return true;
    }
    case D3D11DrawMethod::DispatchIndirect:{
        if(!arguments(12U))return false;
        release_mapping();
        const auto& stage=d3d11_draw_state(bridge,context,D3D11DrawScope::Compute,symbol).stages[5];
        context->native->graphics_dispatch_indirect(stage,argument_resource(),u32(2));
        ++context->dispatches;++bridge.graphics_dispatches;
        return true;
    }
    case D3D11DrawMethod::None:break;
    }
    return false;
}catch(...){d3d11_front_exception=std::current_exception();return false;}
