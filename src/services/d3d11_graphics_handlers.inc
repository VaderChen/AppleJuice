        static const bool discard_enabled=[] {const auto* value=std::getenv("X64ARM_GRAPHICS_DISCARD_RENAME");return !value || std::string_view(value)!="0";}();
        // 持久映射：D3D11 的 DYNAMIC 資源只有 CPU 能經 Map 寫入，GPU 唯讀，所以 CPU 端資料就是權威內容。
        // 子資源第一次映射後保留客體 heap 區塊，之後 WRITE_DISCARD／WRITE_NO_OVERWRITE 直接回傳同一區塊（內容是上次寫入的資料），
        // 不等 GPU、不讀回、不重新配置；Unmap 仍照常上傳。其他寫入路徑（UpdateSubresource、Copy*）使區塊失效。
        // Steam GPU 程序實測 Map 平均 0.35 ms，主要在 graphics_read 的 wait_resource。X64ARM_D3D11_PERSISTENT_MAP=0 關閉。
        static const bool persistent_maps_enabled=[] {const auto* value=std::getenv("X64ARM_D3D11_PERSISTENT_MAP");return !value || std::string_view(value)!="0";}();
        const auto drop_persistent=[&](auto& target){
            for(auto it=target->persistent_maps.begin();it!=target->persistent_maps.end();){
                if(target->subresource_maps.contains(it->first)){++it;continue;}
                if(!free_heap(memory,it->second.pointer,state))throw HostCallFault("D3D11 持久映射記憶體已被客體破壞");
                std::erase(target->allocations,it->second.pointer);it=target->persistent_maps.erase(it);
            }
        };
        // 共用物件操作，集中管理 COM 綁定的強參照與 guest pointer 寬度。
        const auto depth_family=[](std::uint32_t format)->std::array<std::uint32_t,4>{
            if(format>=19 && format<=22)return {19,20,21,22};
            if(format>=39 && format<=43)return {39,40,41,0};
            if(format>=44 && format<=47)return {44,45,46,47};
            if(format>=53 && format<=59)return {53,55,56,0};
            return {};
        };
        const auto copy_format_family=[](std::uint32_t format){
            // DXGI typeless 家族允許逐位元複製；不依通道值做轉換。
            constexpr std::array<std::array<unsigned,2>,20> groups{{{1,4},{5,8},{9,14},{15,18},{19,22},{23,25},{27,32},{33,38},{39,43},{44,47},{48,52},{53,59},{60,64},{70,72},{73,75},{76,78},{79,81},{82,84},{94,96},{97,99}}};
            for(const auto& group:groups)if(format>=group[0] && format<=group[1])return group[0];
            if(format==87 || format==90 || format==91)return 90U;
            if(format==88 || format==92 || format==93)return 92U;
            return format;
        };
        // 另允許位元大小相同的 BC 與未壓縮格式互拷（D3D10.1 起；未壓縮一個 texel 對應一個 4×4 區塊）：
        // 64 位元區塊（BC1、BC4）↔ R16G16B16A16／R32G32，128 位元區塊（BC2／3／5／6H／7）↔ R32G32B32A32。
        const auto copy_compatible=[&](std::uint32_t a,std::uint32_t b){
            const auto family=[&](std::uint32_t format)->unsigned{const auto f=copy_format_family(format);
                if(f==70 || f==79)return 1;if(f==73 || f==76 || f==82 || f==94 || f==97)return 2;if(f==9 || f==15)return 3;if(f==1)return 4;return 0;};
            if(copy_format_family(a)==copy_format_family(b))return true;
            const auto x=family(a),y=family(b);return (x==1 && y==3) || (x==3 && y==1) || (x==2 && y==4) || (x==4 && y==2);
        };
        const auto require=[&](bool value){if(!value)throw std::invalid_argument("D3D11 參數或狀態不合法："+std::string(symbol));};
        const auto write_pointer=[&](std::uint64_t at,std::uint64_t value){memory.store(GuestAddress{at},pointer_bits,value);};
        const auto bytes32=[&](std::uint64_t at,std::uint32_t count){std::vector<std::uint32_t> values(count);for(unsigned i=0;i<count;++i)values[i]=static_cast<std::uint32_t>(memory.load(GuestAddress{at+4*i},32));return values;};
        const auto bind=[&](const std::shared_ptr<Identity>& context,const std::string& name,std::uint32_t start,std::span<const std::uint64_t> values,std::size_t limit){
            require(start<=limit && values.size()<=limit-start);for(auto value:values)if(value){auto id=object(value).identity;require(id->device==context->device && !id->offered && (!id->texture || !object(id->texture).identity->offered));}
            if(!context->weak_bindings)for(auto value:values)if(value)hold(value);
            auto& previous=context->bindings[name];previous.resize(limit);for(std::size_t i=0;i<values.size();++i){const auto old=previous[start+i];previous[start+i]=values[i];if(old && !context->weak_bindings)release(old);}
        };
        const auto bind_one=[&](const std::shared_ptr<Identity>& context,const std::string& name,std::uint64_t value){const std::array values{value};bind(context,name,0,values,1);};
        const auto binding=[&](const std::shared_ptr<Identity>& context,const std::string& name,std::size_t index=0)->std::uint64_t{const auto found=context->bindings.find(name);return found!=context->bindings.end() && index<found->second.size()?found->second[index]:0;};
        const auto view_of=[&](std::uint64_t pointer){if(!pointer)return GraphicsView{};auto id=object(pointer).identity;require(bool(id->resource) || bool(id->graphics_view.resource));return id->graphics_view.resource?id->graphics_view:GraphicsView{id->resource,id->resource->desc.format,0,id->resource->desc.levels,0,id->resource->desc.layers};};
        const auto subresource=[&](const std::shared_ptr<Identity>& resource,std::uint32_t index){require(bool(resource->resource));const auto& d=resource->resource->desc;require(index<d.levels*d.layers);return GraphicsView{resource->resource,d.format,index%d.levels,1,index/d.levels,1};};
        const auto clear_bindings=[&](const std::shared_ptr<Identity>& context){auto previous=std::move(context->bindings);context->bindings.clear();if(!context->weak_bindings)for(const auto& [name,values]:previous){(void)name;for(auto pointer:values)if(pointer)release(pointer);}context->viewports.clear();context->scissors.clear();context->strides={};context->offsets={};context->stream_offsets={};context->topology=0;context->index_format=42;context->index_offset=0;context->blend_factor={1,1,1,1};context->sample_mask=UINT32_MAX;context->stencil_reference=0;context->predicate_value=false;context->constant_first={};context->constant_count={};};
        const auto submit=[&](const std::shared_ptr<Identity>& context,RecordedGraphicsCommand command,bool predicable=true){
            if(predicable)if(const auto pointer=binding(context,"Predicate")){
                const auto query=object(pointer).identity;const auto expected=context->predicate_value;
                auto execute=std::move(command.execute);command.execute=[query,expected,execute=std::move(execute)](auto& native){
                    if(!query->query_ready)throw std::invalid_argument("predicate 尚無已完成的查詢結果");
                    const bool value=word(query->query_data,0)!=0;
                    if(std::getenv("X64ARM_TRACE_PREDICATION"))std::fprintf(stderr,"[predication] value=%u expected=%u execute=%u\n",static_cast<unsigned>(value),static_cast<unsigned>(expected),static_cast<unsigned>(value!=expected));
                    if(value!=expected)execute(native);
                };
            }
            if(context->context_type)context->commands.push_back(std::move(command));
            else {command.execute(*context->native);context->draws+=command.draws;context->dispatches+=command.dispatches;bridge.graphics_draws+=command.draws;bridge.graphics_dispatches+=command.dispatches;}
        };
        // 與 submit 的立即路徑相同的條件與計數；X64ARM_D3D11_DIRECT_DRAW=0 回到一律經 submit（同引擎對照）。
        static const bool direct_draw_enabled=[] {const auto* value=std::getenv("X64ARM_D3D11_DIRECT_DRAW");return !value || std::string_view(value)!="0";}();
        const auto direct_submit=[&](const std::shared_ptr<Identity>& context){return direct_draw_enabled && !context->context_type && !binding(context,"Predicate");};
        const auto count_direct=[&](const std::shared_ptr<Identity>& context,std::uint64_t draws,std::uint64_t dispatches){
            context->draws+=draws;context->dispatches+=dispatches;bridge.graphics_draws+=draws;bridge.graphics_dispatches+=dispatches;};
#include "d3d11_binding_helpers.inc"
        // 遮蔽查詢的可見度緩衝（8192 個 8 位元組槽位）只清零實際用到的範圍：Begin 先清第一段，繪製用到超出
        // 已清零範圍的槽位時再依序補清下一段。原本每次 Begin 都配置並寫入 64 KB（UE4 每幀上百個查詢）。
        // 清零與繪製走同一個提交順序；已清零位元組數在提交執行時更新，延遲 context 亦同。
        const auto visibility_zero=[&](const std::shared_ptr<Identity>& context,const std::shared_ptr<Identity>& query,const GraphicsView& view,std::uint32_t offset){
            static const std::vector<std::uint8_t> zero(4096);
            const auto first=query->visibility_planned;
            if(offset<first || first>=65536)return;
            const auto count=static_cast<std::uint32_t>(std::min<std::uint64_t>(zero.size(),65536-first));
            query->visibility_planned=first+count;
            auto chunk=view;chunk.first=first;chunk.count=count;
            submit(context,{[chunk](auto& native){native.graphics_write(chunk,std::span<const std::uint8_t>(zero.data(),chunk.count),0,0);}},false);
        };
        // 繪製時為進行中的遮蔽查詢分配可見度槽位。共用池只遞增游標（池建立時已清零，不必逐次清零），用完換新池並開新區段；
        // 其他查詢沿用每查詢緩衝與逐段清零。
        const auto assign_visibility=[&](const std::shared_ptr<Identity>& context,GraphicsDraw& draw){
            const auto pointer=binding(context,"Visibility");if(!pointer)return;
            auto query=object(pointer).identity;require(query->sequence<8192);
            if(query->visibility_pooled){
                if(!context->visibility_pool || context->visibility_cursor+8>d3d11_visibility_pool_bytes){
                    GraphicsResourceDesc desc;desc.width=d3d11_visibility_pool_bytes;
                    context->visibility_pool=context->native->graphics_resource(desc);context->visibility_cursor=0;++d3d11_->resource_creations["VisibilityPool"];
                }
                auto& segments=query->visibility_segments;
                if(segments.empty() || segments.back().buffer!=context->visibility_pool)segments.push_back({context->visibility_pool,context->visibility_cursor,0});
                draw.visibility=context->visibility_pool;draw.visibility_offset=context->visibility_cursor;
                context->visibility_cursor+=8;++segments.back().count;++query->sequence;return;
            }
            const auto slot=static_cast<std::uint32_t>(query->sequence++*8);visibility_zero(context,query,view_of(pointer),slot);
            draw.visibility=query->resource;draw.visibility_offset=slot;
        };
        const auto draw_state=[&](const std::shared_ptr<Identity>& context)->GraphicsDraw& {
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
            if(snapshot_enabled && draw_cache_enabled && !context->weak_bindings && cache->destroyed==d3d11_->destroyed_objects)++d3d11_->draw_cache_incremental;
            // 就地解構再建構（命令執行緒已用完這份快取）；以暫存物件指派會讓每次呼叫的堆疊框架多出約 64 KB。
            else {std::destroy_at(cache.get());std::construct_at(cache.get());++d3d11_->draw_cache_full;}
            cache->destroyed=d3d11_->destroyed_objects;
            auto& draw=cache->draw;
            try{
            draw.topology=context->topology;draw.index_format=context->index_format;draw.index_offset=context->index_offset;draw.viewports=context->viewports;draw.scissors=context->scissors;draw.blend_factor=context->blend_factor;draw.sample_mask=context->sample_mask;draw.stencil_reference=context->stencil_reference;
            // 每次繪製的參數由呼叫端填入，先回到預設值。
            draw.count=0;draw.start=0;draw.instances=1;draw.start_instance=0;draw.base_vertex=0;draw.alpha_function=0;draw.alpha_reference=0;draw.indexed=false;
            draw.visibility.reset();draw.visibility_offset=0;draw.indirect.reset();draw.indirect_offset=0;
            const auto lookup=[&](const std::string& key)->const std::vector<std::uint64_t>* {const auto found=context->bindings.find(key);return found==context->bindings.end()?nullptr:&found->second;};
            // 只對原始指標與上次不同的槽位重新解析；raw 只保留 limit 內的槽位。
            const auto refresh=[](std::vector<std::uint64_t>& raw,const std::vector<std::uint64_t>* now,std::size_t limit,const auto& assign){
                const std::size_t current=now?std::min(limit,now->size()):0;
                if(raw.size()==current && std::equal(raw.begin(),raw.end(),now?now->begin():raw.begin()))return;
                for(std::size_t i=0;i<std::max(current,raw.size());++i){const auto value=i<current?(*now)[i]:0,old=i<raw.size()?raw[i]:0;if(value!=old)assign(i,value);}
                if(now)raw.assign(now->begin(),now->begin()+static_cast<std::ptrdiff_t>(current));else raw.clear();
            };
            for(unsigned stage=0;stage<6;++stage){const auto& name=stage_names[stage];const auto& keys=stage_keys[stage];const auto shader_pointer=binding(context,keys[0]);auto& target=draw.stages[stage];
                if(snapshot_enabled){
                    // 保留 PS 的輸出 UAV（即使 PS 為空）；其他未綁 shader 的 stage 不取快照。
                    if(!shader_pointer && stage!=1){
                        if(cache->present[stage]){std::destroy_at(&target);std::construct_at(&target);cache->present[stage]=false;cache->shaders[stage]=0;for(auto& raw:cache->stage_raw[stage])raw.clear();}
                        continue;
                    }
                    cache->present[stage]=true;
                    if(cache->shaders[stage]!=shader_pointer){target.shader=shader_pointer?object(shader_pointer).identity->shader:nullptr;cache->shaders[stage]=shader_pointer;}
                    auto& raw=cache->stage_raw[stage];
                    refresh(raw[0],lookup(keys[1]),14,[&](auto i,auto pointer){target.constants[i]=pointer?object(pointer).identity->resource:nullptr;});
                    refresh(raw[1],lookup(keys[2]),128,[&](auto i,auto pointer){target.resources[i]=view_of(pointer);});
                    refresh(raw[2],lookup(keys[3]),16,[&](auto i,auto pointer){target.samplers[i]=pointer?object(pointer).identity->sampler:nullptr;});
                    refresh(raw[3],lookup(keys[4]),8,[&](auto i,auto pointer){target.unordered[i]=view_of(pointer);});
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
                if(shader_pointer){const auto shader_id=object(shader_pointer).identity;
                    if(shader_id->reflection && shader_id->reflection->linkage){
                        require(!context->context_type);const auto& linked=*shader_id->reflection->linkage;
                        // 同一引擎的效能 A/B 可關閉快取，保留原有逐 draw 讀回／上傳路徑。
                        static const bool cache_enabled=[](){const char* option=std::getenv("X64ARM_D3D11_LINKAGE_CACHE");return !option || std::string_view(option)!="0";}();
                        ++d3d11_->linkage_draws;std::vector<std::uint8_t> bytes(4096);std::map<unsigned,std::vector<std::uint8_t>> constants;unsigned used=0;
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
                                if(!constants.contains(buffer)){auto read=context->native->graphics_read(GraphicsView{resource});d3d11_->linkage_readback_bytes+=read.size();constants.emplace(buffer,std::move(read));}
                                const auto& data=constants.at(buffer);if(offset<data.size()){const auto count=std::min<std::size_t>(type->size,data.size()-offset);std::copy_n(data.begin()+offset,count,bytes.begin()+4096+used);}
                            }
                            used+=type->size;
                        }
                        if(cache_enabled && context->linkage_constants[stage] && context->linkage_keys[stage]==bytes){
                            ++d3d11_->linkage_cache_hits;draw.stages[stage].constants[14]=context->linkage_constants[stage];
                        }else {
                            // 每個 context／stage 只快取上一組 binding；不覆寫仍在 GPU 使用的 buffer。
                            auto key=bytes;bytes.resize(65536);GraphicsResourceDesc desc;desc.width=65536;desc.bind=4;
                            auto data=context->native->graphics_resource(desc);++d3d11_->resource_creations["ClassLinkage"];context->native->graphics_write(GraphicsView{data},bytes,65536,65536);
                            context->linkage_keys[stage]=std::move(key);context->linkage_constants[stage]=data;draw.stages[stage].constants[14]=std::move(data);++d3d11_->linkage_uploads;
                        }
                    }
                }
            }
            draw.stream_statistics=context->stream_statistics;
            refresh(cache->stream_raw,lookup(stream_key),4,[&](auto i,auto pointer){draw.stream_targets[i]=pointer?object(pointer).identity->resource:nullptr;});
            if(const auto ptr=binding(context,layout_key);ptr!=cache->layout){draw.layout=ptr?object(ptr).identity->layout:std::vector<GraphicsElement>{};cache->layout=ptr;}
            if(const auto ptr=binding(context,index_key);ptr!=cache->index){draw.indices=ptr?object(ptr).identity->resource:nullptr;cache->index=ptr;}
            // stride／offset 存在 context，指標未變也要逐槽同步。
            refresh(cache->vertex_raw,lookup(vertex_key),32,[&](auto i,auto pointer){draw.vertices[i]=pointer?GraphicsVertexBuffer{object(pointer).identity->resource,0,0}:GraphicsVertexBuffer{};});
            for(unsigned i=0;i<32;++i)if(draw.vertices[i].resource){draw.vertices[i].stride=context->strides[i];draw.vertices[i].offset=context->offsets[i];}
            refresh(cache->target_raw,lookup(target_key),8,[&](auto i,auto pointer){draw.targets[i]=view_of(pointer);});
            if(const auto ptr=binding(context,depth_key);ptr!=cache->depth){draw.depth=view_of(ptr);cache->depth=ptr;}
            if(const auto ptr=binding(context,blend_key);ptr!=cache->blend){draw.blend=ptr?object(ptr).identity->desc:std::vector<std::uint8_t>{};cache->blend=ptr;}
            if(const auto ptr=binding(context,depth_state_key);ptr!=cache->depth_state){draw.depth_state=ptr?object(ptr).identity->desc:std::vector<std::uint8_t>{};cache->depth_state=ptr;}
            if(const auto ptr=binding(context,raster_key);ptr!=cache->raster){draw.raster=ptr?object(ptr).identity->desc:std::vector<std::uint8_t>{};cache->raster=ptr;}
            }catch(...){cache->destroyed=UINT64_MAX;throw;}
            // 呼叫端接著以參照排入的繪製會取得下一個序號；保守記錄，之後重用這份前等到它完成。
            cache->ticket=context->native?context->native->graphics_ticket()+1:0;
            return draw;
        };
#include "d3d11_display_helpers.inc"
#include "d3d11_graphics_swap_helpers.inc"
#include "d3d11_dxgi_handlers.inc"
#include "d3d11_display_handlers.inc"
#include "d3d11_deferred_handlers.inc"
#include "d3d11_reflection_handlers.inc"
#include "d3d11_class_handlers.inc"
        if(graphics_handled){}
        else if(symbol=="GetPrivateData" || symbol=="SetPrivateData" || symbol=="SetPrivateDataInterface"){
            graphics_handled=true;auto id=object(arg(0)).identity;const auto guid=read_guid(memory,arg(1));
            if(symbol=="GetPrivateData"){require(arg(2)!=0);const auto capacity=memory.load(GuestAddress{arg(2)},32);std::vector<std::uint8_t> value;if(id->private_interfaces.contains(guid)){const auto pointer=id->private_interfaces.at(guid);value.resize(pointer_size);std::memcpy(value.data(),&pointer,pointer_size);}else if(id->private_data.contains(guid))value=id->private_data.at(guid);else result=not_found;
                memory.store(GuestAddress{arg(2)},32,value.size());if(!result){if(capacity<value.size())result=0x887a0003U;else if(arg(3)){memory.check(GuestAddress{arg(3)},value.size(),Access::Write);if(id->private_interfaces.contains(guid))hold(id->private_interfaces.at(guid));memory.write(GuestAddress{arg(3)},value);}}}
            else {
                // 先準備新值；錯誤參數不能刪掉既有資料，同一介面覆寫也須先持有新參照。
                std::vector<std::uint8_t> value;const auto pointer=symbol=="SetPrivateDataInterface"?arg(2):0;
                if(symbol=="SetPrivateData"){require(u32(2)<=1024*1024 && (!u32(2) || arg(3)));if(u32(2))value=read_bytes(memory,arg(3),u32(2));}
                if(pointer){hold(pointer);try{id->private_interfaces.try_emplace(guid,0);}catch(...){release(pointer);throw;}}
                if(!value.empty())id->private_data.insert_or_assign(guid,std::move(value));else id->private_data.erase(guid);
                const auto previous=id->private_interfaces.find(guid);const auto old=previous==id->private_interfaces.end()?0:previous->second;
                if(pointer)previous->second=pointer;else if(previous!=id->private_interfaces.end())id->private_interfaces.erase(previous);
                if(old)release(old);
            }
        }else if(symbol=="CreateBuffer" || symbol=="CreateTexture1D" || symbol=="CreateTexture3D" || (symbol=="CreateTexture2D" && memory.load(GuestAddress{arg(1)+16},32)!=103)){
            graphics_handled=true;auto device=typed(arg(0),Kind::Device);if(arg(3)){output(3);write_pointer(arg(3),0);}const bool buffer=symbol=="CreateBuffer",one=symbol=="CreateTexture1D",three=symbol=="CreateTexture3D";auto desc=read_bytes(memory,arg(1),buffer?24:one?32:three?36:44);GraphicsResourceDesc d;std::uint32_t usage=0,access=0;
            if(buffer){d.dimension=1;d.width=word(desc,0);usage=word(desc,4);d.bind=word(desc,8);access=word(desc,12);d.misc=word(desc,16);d.stride=(d.misc&0x40)?word(desc,20):0;require(d.width && d.width<=512*1024*1024 && (!(d.bind&4) || (!(d.width%16) && d.width<=65536)));}
            else if(one){d.dimension=2;d.width=word(desc,0);d.levels=word(desc,4);d.layers=word(desc,8);d.format=word(desc,12);usage=word(desc,16);d.bind=word(desc,20);access=word(desc,24);d.misc=word(desc,28);}
            else if(three){d.dimension=4;d.width=word(desc,0);d.height=word(desc,4);d.depth=word(desc,8);d.levels=word(desc,12);d.format=word(desc,16);usage=word(desc,20);d.bind=word(desc,24);access=word(desc,28);d.misc=word(desc,32);}
            else{d.dimension=3;d.width=word(desc,0);d.height=word(desc,4);d.levels=word(desc,8);d.layers=word(desc,12);d.format=word(desc,16);d.samples=word(desc,20);d.sample_quality=word(desc,24);require(!d.sample_quality || d.sample_quality==UINT32_MAX || d.sample_quality==UINT32_MAX-1U);usage=word(desc,28);d.bind=word(desc,32);access=word(desc,36);d.misc=word(desc,40);}
            require(usage<=3 && !(access&~0x30000U) && !(usage==0 && access) && !(usage==1 && (access || !arg(2))) && !(usage==2 && (access!=0x10000 || (d.bind&0xe0))) && !(usage==3 && (d.bind || !access)));
            if(d.misc&2)require(d.dimension==3 && d.samples==1 && d.layers==1 && usage==0 && !access && !(d.misc&0x100U));
            if(!d.levels){d.levels=1;for(auto size=std::max({d.width,d.height,d.depth});size>1;size>>=1)++d.levels;put_word(desc,buffer?0:one?4:three?12:8,d.levels);}if(d.misc&2)require(d.levels==1);require(d.levels<=15 && d.layers && d.layers<=2048 && d.samples && (d.samples==1 || d.samples==2 || d.samples==4 || d.samples==8));
            if(!buffer){if(graphics_format_bc_family(d.format))require((d.dimension==3 || d.dimension==4) && !(d.width%4) && !(d.height%4) && !(d.bind&~8U) && !(d.misc&1) && d.samples==1);if(d.misc&4)require(d.dimension==3 && d.width==d.height && !(d.layers%6) && d.samples==1);require(device->native->graphics_samples_supported(d.format,d.samples));if(d.samples>1)require(!one && !three && d.levels==1 && usage==0 && !access && !arg(2) && !(d.bind&0x80));}
            if(!buffer){
                const bool combined=(d.format>=19 && d.format<=22) || (d.format>=44 && d.format<=47);
                const auto family=depth_family(d.format);
                if((d.bind&0x40) || combined || d.format==40 || d.format==55){
                    require(d.dimension==3 && !(d.bind&0xa0) && !(d.misc&1));
                    if(d.bind&0x40)require(d.format==family[0] || d.format==family[1]);
                    if(d.format==family[1])require(!(d.bind&8));
                }
            }
            if(!arg(3))result=1;
            else{auto native=device->native->graphics_resource(d);++bridge.resource_creations[std::string(symbol)];auto id=create(buffer?Kind::BufferObject:one?Kind::Texture1D:three?Kind::Texture3D:Kind::Texture,device->root);id->resource=std::move(native);id->desc=desc;if(!buffer && !one && !three)for(unsigned i=0;i<11;++i)id->texture_desc[i]=word(desc,4*i);
                try{if(arg(2))for(unsigned i=0;i<d.levels*d.layers;++i){const auto at=arg(2)+i*(x86_?12:16);const auto data=memory.load(GuestAddress{at},pointer_bits);const auto row=static_cast<std::uint32_t>(memory.load(GuestAddress{at+pointer_size},32)),depth=static_cast<std::uint32_t>(memory.load(GuestAddress{at+pointer_size+4},32));const auto w=std::max(1U,d.width>>(i%d.levels)),h=std::max(1U,d.height>>(i%d.levels)),z=std::max(1U,d.depth>>(i%d.levels));const auto pitch=buffer?std::array{d.width,1U}:graphics_format_pitch(d.format,w,h);require(buffer || one || row>=pitch[0]);const auto size=buffer?d.width:std::uint64_t{depth}*(z-1)+std::uint64_t{row}*(pitch[1]-1)+pitch[0];require(size<=512*1024*1024);device->native->graphics_write(subresource(id,i),read_bytes(memory,data,static_cast<std::size_t>(size)),one?pitch[0]:row,three?depth:0);}}
                catch(...){release(id->root);throw;}write_pointer(arg(3),id->root);}
        }else if(symbol=="OpenSharedResource"){
            graphics_handled=true;auto device=typed(arg(0),Kind::Device);output(3);write_pointer(arg(3),0);const auto guid=read_guid(memory,arg(2));
            if(guid!=IID_ID3D11Texture2D && guid!=IID_ID3D11Resource && guid!=IID_IUnknown)result=no_interface;
            else{const auto found=arg(1)<=UINT32_MAX?bridge.shared_resources.find(u32(1)):bridge.shared_resources.end();auto resource=found==bridge.shared_resources.end()?std::shared_ptr<GraphicsResource>{}:found->second.resource.lock();
                if(!resource)result=0x887a0001U;
                else{if(auto producer=found->second.producer.lock())producer->graphics_flush(true);auto id=create(Kind::Texture,device->root);id->resource=std::move(resource);id->desc=found->second.desc;for(unsigned i=0;i<11;++i)id->texture_desc[i]=word(id->desc,4*i);write_pointer(arg(3),id->root);}}
        }else if(symbol=="BufferGetDesc" || symbol=="Texture1DGetDesc" || symbol=="Texture3DGetDesc" || symbol=="GraphicsGetDesc"){
            graphics_handled=true;auto id=object(arg(0)).identity;memory.write(GuestAddress{arg(1)},id->desc);
        }else if(symbol=="ResourceGetType" && object(arg(0)).identity->resource){graphics_handled=true;memory.store(GuestAddress{arg(1)},32,object(arg(0)).identity->resource->desc.dimension);}
        else if(symbol=="SetEvictionPriority" || symbol=="GetEvictionPriority"){
            graphics_handled=true;auto id=object(arg(0)).identity;if(symbol=="SetEvictionPriority")id->priority=u32(1);else result=id->priority;
        }else if(symbol=="CreateShaderResourceView" || symbol=="CreateRenderTargetView" || symbol=="CreateDepthStencilView" || symbol=="CreateUnorderedAccessView"){
            graphics_handled=true;auto device=typed(arg(0),Kind::Device),resource=object(arg(1)).identity;require(resource->device==device->root && bool(resource->resource));if(arg(3)){output(3);write_pointer(arg(3),0);}const bool srv=symbol=="CreateShaderResourceView",rtv=symbol=="CreateRenderTargetView",dsv=symbol=="CreateDepthStencilView";require(resource->resource->desc.bind&(srv?8U:rtv?0x20U:dsv?0x40U:0x80U));const auto& d=resource->resource->desc;GraphicsView v{resource->resource,d.format,0,d.levels,0,d.layers};std::vector<std::uint8_t> desc(srv || dsv?24:20);
            if(arg(2))desc=read_bytes(memory,arg(2),desc.size());else{put_word(desc,0,d.format);put_word(desc,4,(d.dimension==1?1:d.dimension==2?(d.layers>1?3:2):d.dimension==4?8:d.samples>1?(d.layers>1?7:6):(d.layers>1?5:4))-(dsv?1U:0U));if(srv && d.samples==1)put_word(desc,12,d.levels);if(d.layers>1){const auto at=d.samples>1?(dsv?16U:12U):srv || dsv?20U:16U;put_word(desc,at,d.layers);}}
            if(!arg(2) && srv && (d.misc&4)){put_word(desc,4,d.layers>6?10U:9U);if(d.layers>6)put_word(desc,20,d.layers/6);}
            v.format=word(desc,0)?word(desc,0):d.format;const auto dimension=word(desc,4)+(dsv?1U:0U),base=dsv?12U:8U;v.dimension=dimension;if(const auto family=graphics_format_bc_family(d.format))require(srv && v.format!=family && graphics_format_bc_family(v.format)==family && (d.format==family || d.format==v.format));
            if(dsv){v.flags=word(desc,8);require(!(v.flags&~3U));}
            if(d.dimension==1){
                require(dimension==1 || (srv && dimension==11));
                if(srv && dimension==11){v.flags=word(desc,16);require(!(v.flags&~1U) && (!(v.flags&1) || (v.format==39 && (d.misc&0x20) && !d.stride)));}
                const auto stride=d.stride?d.stride:graphics_format_bytes(v.format);require(stride);
                const auto first=std::uint64_t{word(desc,8)}*stride,count=std::uint64_t{word(desc,12)}*stride;
                if(arg(2)){require(count && first<=d.width && count<=d.width-first);v.first=static_cast<std::uint32_t>(first);v.count=static_cast<std::uint32_t>(count);}
                else{v.first=0;v.count=(d.width/stride)*stride;require(v.count);put_word(desc,8,0);put_word(desc,12,d.width/stride);}
                v.level=0;v.levels=1;
            }
            else{
                const bool array=dimension==3 || dimension==5 || dimension==7,msaa=dimension==6 || dimension==7;
                v.level=msaa?0:word(desc,base);v.levels=srv && !msaa?word(desc,12):1;
                require(v.level<d.levels);
                if(v.levels==UINT32_MAX)v.levels=d.levels-v.level;if(!v.levels)v.levels=1;
                require(v.levels<=d.levels-v.level);
                if(dimension==9 || dimension==10){require(srv && (d.misc&4) && d.dimension==3);v.slice=dimension==10?word(desc,16):0;const auto cubes=dimension==10?word(desc,20):1U;require(cubes && cubes<=d.layers/6 && !(v.slice%6));v.slices=cubes*6;}
                else if(array){const auto at=msaa?base:srv?16U:base+4;v.slice=word(desc,at);v.slices=word(desc,at+4);}
                else v.slices=d.layers;
                const auto layer_limit=d.dimension==4 && rtv?std::max(1U,d.depth>>v.level):d.layers;
                if(d.dimension==4 && rtv){require(dimension==8);v.slice=arg(2)?word(desc,12):0;v.slices=arg(2)?word(desc,16):layer_limit;if(v.slices==UINT32_MAX && v.slice<layer_limit)v.slices=layer_limit-v.slice;if(!arg(2)){put_word(desc,12,v.slice);put_word(desc,16,v.slices);}}
                require(v.slice<layer_limit && v.slices && v.slices<=layer_limit-v.slice);
                const bool depth_resource=(d.bind&0x40) || (d.format>=19 && d.format<=22) || (d.format>=44 && d.format<=47) || d.format==40 || d.format==55;
                if(depth_resource || dsv){
                    const auto family=depth_family(d.format);require(family[0] && !rtv && (srv || dsv));
                    require(d.dimension==3 && (d.samples>1?(dimension==6 || dimension==7):(dimension==4 || dimension==5)));
                    require(array || d.layers==1);
                    if(dsv)require(v.format==family[1] && (d.format==family[0] || d.format==family[1]) && (!(v.flags&2) || family[3]));
                    else require((v.format==family[2] || (family[3] && v.format==family[3])) && (d.format==family[0] || d.format==v.format));
                }
            }
            if(!srv && !rtv && !dsv && d.dimension==1){
                v.flags=word(desc,16);require(!(v.flags&~7U) && (v.flags&6)!=6 && (!(v.flags&1) || (v.flags==1 && v.format==39 && (d.misc&0x20))) && (!(v.flags&6) || (!v.format && d.stride && (d.misc&0x40))));
                if((v.flags&6) && arg(3)){GraphicsResourceDesc counter;counter.width=4;v.counter=device->native->graphics_resource(counter);++bridge.resource_creations["UavCounter"];}
            }
            if(!arg(3))result=1;else {auto id=create(srv?Kind::ShaderResource:rtv?Kind::RenderTarget:dsv?Kind::DepthView:Kind::Unordered,device->root);id->texture=arg(1);hold(arg(1));id->graphics_view=v;id->desc=desc;write_pointer(arg(3),id->root);}
        }else if(symbol=="GetResource" && graphics_view_kind(object(arg(0)).kind)){graphics_handled=true;auto id=object(arg(0)).identity;hold(id->texture);write_pointer(arg(1),id->texture);}
        else if(symbol=="CreateGeometryShaderWithStreamOutput"){
            graphics_handled=true;const bool ten=service.family=="internal-d3d10";const unsigned out=ten?6:9;auto device=typed(arg(0),Kind::Device);
            if(arg(out)){output(out);write_pointer(arg(out),0);}require(arg(2)>0 && arg(2)<=16*1024*1024 && u32(4)<=64);
            if(!ten && arg(8))require(typed(arg(8),Kind::ClassLinkage)->device==device->root);
            const auto bytes=read_bytes(memory,arg(1),static_cast<std::size_t>(arg(2)));auto reflected=std::make_shared<dxbc::Reflection>(bytes);
            const auto stage=!reflected->version?6U:(reflected->version>>16)==1?0U:2U;auto shader=device->native->graphics_shader(stage,bytes);
            for(unsigned i=0;i<u32(4);++i){
                const auto at=arg(3)+i*(ten?(x86_?12:16):(x86_?16:24));const auto name_offset=ten?0U:x86_?4U:8U;
                GraphicsStreamElement e;e.stream=ten?0:static_cast<unsigned>(memory.load(GuestAddress{at},32));
                const auto name=memory.load(GuestAddress{at+name_offset},pointer_bits);if(name)e.semantic=read_ansi(memory,name);
                const auto fields=at+name_offset+pointer_size;e.index=static_cast<unsigned>(memory.load(GuestAddress{fields},32));
                e.first=static_cast<unsigned>(memory.load(GuestAddress{fields+4},8));e.count=static_cast<unsigned>(memory.load(GuestAddress{fields+5},8));e.slot=static_cast<unsigned>(memory.load(GuestAddress{fields+6},8));shader->stream_output.push_back(std::move(e));
            }
            if(ten){if(std::all_of(shader->stream_output.begin(),shader->stream_output.end(),[](const auto& e){return !e.slot;}))shader->stream_strides[0]=u32(5);}
            else {require(u32(6)<=4);for(unsigned i=0;i<u32(6);++i)shader->stream_strides[i]=static_cast<unsigned>(memory.load(GuestAddress{arg(5)+4*i},32));shader->rasterized_stream=u32(7);}
            stream_output::validate(*shader,*reflected);
            if(!arg(out))result=1;else {auto id=create(Kind::GeometryShader,device->root);id->shader=std::move(shader);id->reflection=std::move(reflected);id->desc=bytes;write_pointer(arg(out),id->root);}
        }
        else if(symbol=="CreateVertexShader" || symbol=="CreatePixelShader" || symbol=="CreateComputeShader" || symbol=="CreateGeometryShader" || symbol=="CreateHullShader" || symbol=="CreateDomainShader"){
            graphics_handled=true;auto device=typed(arg(0),Kind::Device);if(arg(4)){output(4);write_pointer(arg(4),0);}require(arg(2)>0 && arg(2)<=16*1024*1024);if(arg(3))require(typed(arg(3),Kind::ClassLinkage)->device==device->root);const auto stage=symbol=="CreateVertexShader"?0U:symbol=="CreatePixelShader"?1U:symbol=="CreateComputeShader"?5U:symbol=="CreateGeometryShader"?2U:symbol=="CreateHullShader"?3U:4U;
            {const auto bytes=read_bytes(memory,arg(1),static_cast<std::size_t>(arg(2)));auto reflected=std::make_shared<dxbc::Reflection>(bytes);require(!reflected->interface_slots || arg(3));auto shader=device->native->graphics_shader(stage,bytes);if(!arg(4))result=1;else {auto id=create(stage==0?Kind::VertexShader:stage==1?Kind::PixelShader:stage==2?Kind::GeometryShader:stage==3?Kind::HullShader:stage==4?Kind::DomainShader:Kind::ComputeShader,device->root);id->shader=std::move(shader);id->reflection=std::move(reflected);if(arg(3)){bind_one(id,"ClassLinkage",arg(3));typed(arg(3),Kind::ClassLinkage)->linked_shaders.push_back(id->reflection);}write_pointer(arg(4),id->root);}}
        }else if(symbol=="CreateInputLayout"){
            graphics_handled=true;auto device=typed(arg(0),Kind::Device);if(arg(5)){output(5);write_pointer(arg(5),0);}require(u32(2)<=32 && arg(4)<=16*1024*1024);const dxbc::Reflection signature(read_bytes(memory,arg(3),static_cast<std::size_t>(arg(4))));std::vector<GraphicsElement> elements;std::array<std::uint32_t,32> ends{};
            for(unsigned i=0;i<u32(2);++i){const auto at=arg(1)+i*(x86_?28:32);GraphicsElement e;e.semantic=read_ansi(memory,memory.load(GuestAddress{at},pointer_bits));const auto fields=bytes32(at+pointer_size,6);e.index=fields[0];e.format=fields[1];e.slot=fields[2];e.offset=fields[3];e.classification=fields[4];e.step=fields[5];require(e.slot<32 && e.classification<2 && (!e.classification || e.step) && device->native->graphics_vertex_format_supported(e.format));if(e.offset==UINT32_MAX)e.offset=ends[e.slot];ends[e.slot]=e.offset+graphics_format_bytes(e.format);elements.push_back(e);}
            for(const auto& input:signature.signatures[0])if(input.system!=6 && input.system!=8)require(std::any_of(elements.begin(),elements.end(),[&](const auto& e){return e.semantic==input.name && e.index==input.index;}));if(!arg(5))result=1;else {auto id=create(Kind::Layout,device->root);id->layout=std::move(elements);write_pointer(arg(5),id->root);}
        }else if(symbol=="CreateBlendState" || symbol=="CreateDepthStencilState" || symbol=="CreateRasterizerState" || symbol=="CreateSamplerState"){
            graphics_handled=true;auto device=typed(arg(0),Kind::Device);if(arg(2)){output(2);write_pointer(arg(2),0);}const bool blend=symbol=="CreateBlendState",depth=symbol=="CreateDepthStencilState",sampler=symbol=="CreateSamplerState";auto desc=read_bytes(memory,arg(1),blend?264:depth?52:sampler?52:40);std::shared_ptr<GraphicsSampler> native;if(sampler)native=device->native->graphics_sampler(desc);if(!arg(2))result=1;else{auto id=create(blend?Kind::Blend:depth?Kind::DepthState:sampler?Kind::Sampler:Kind::Raster,device->root);id->desc=std::move(desc);id->sampler=std::move(native);write_pointer(arg(2),id->root);}
        }else if(symbol.size()>2 && (symbol.starts_with("VS") || symbol.starts_with("PS") || symbol.starts_with("GS") || symbol.starts_with("HS") || symbol.starts_with("DS") || symbol.starts_with("CS")) && (symbol.substr(2).starts_with("Set") || symbol.substr(2).starts_with("Get"))){
            graphics_handled=true;auto context=typed(arg(0),Kind::Context);const auto stage=stage_index(symbol);const auto what=std::string(symbol.substr(5));const auto name=stage_names[stage]+what;const bool set=symbol.substr(2).starts_with("Set");
            if(what=="Shader"){
                const auto instances_name=stage_names[stage]+"ClassInstances";
                if(set){
                    require(u32(3)<=256 && (!u32(3) || arg(2)));std::vector<std::uint64_t> instances(u32(3));
                    const auto shader=arg(1)?object(arg(1)).identity:std::shared_ptr<Identity>{};
                    if(shader)require(bool(shader->shader) && shader->shader->stage==stage && shader->device==context->device && (!shader->reflection || shader->reflection->interface_slots==u32(3)));
                    for(unsigned i=0;i<u32(3);++i){instances[i]=memory.load(GuestAddress{arg(2)+i*pointer_size},pointer_bits);const auto instance=typed(instances[i],Kind::ClassInstance);require(shader && binding(instance,"ClassLinkage")==binding(shader,"ClassLinkage"));}
                    bind(context,instances_name,0,instances,256);
                    // bind 已把槽位擴成 256；釋放 class instance 不改動 context 的 bindings 結構，可直接走訪同一向量。
                    for(auto& slots=context->bindings[instances_name];auto& slot:std::span(slots).subspan(u32(3)))if(const auto old=slot){slot=0;if(!context->weak_bindings)release(old);}
                    bind_one(context,name,arg(1));
                }else{
                    const auto pointer=binding(context,name);if(arg(1)){if(pointer)hold(pointer);write_pointer(arg(1),pointer);}
                    const auto shader=pointer?object(pointer).identity:std::shared_ptr<Identity>{};const auto count=shader && shader->reflection?shader->reflection->interface_slots:0U;
                    if(arg(3)){const auto capacity=static_cast<unsigned>(memory.load(GuestAddress{arg(3)},32));if(arg(2))for(unsigned i=0;i<std::min(capacity,count);++i){const auto instance=binding(context,instances_name,i);if(instance)hold(instance);write_pointer(arg(2)+i*pointer_size,instance);}memory.store(GuestAddress{arg(3)},32,count);}
                }
            }
            else {const auto limit=what=="ConstantBuffers"?14U:what=="Samplers"?16U:what=="UnorderedAccessViews"?8U:128U;require(u32(1)<=limit && u32(2)<=limit-u32(1));if(set){
                const auto kind=what=="ConstantBuffers"?Kind::BufferObject:what=="Samplers"?Kind::Sampler:what=="UnorderedAccessViews"?Kind::Unordered:Kind::ShaderResource;
                auto values=read_views(arg(3),u32(2),kind,context);
                if(what=="ShaderResources")for(auto& value:values)if(value && outputs_conflict(context,value))value=0;
                if(what=="UnorderedAccessViews"){
                    if(nonconflicting(values)){const auto counts=counter_values(values,arg(4));
                        for(auto pointer:values)if(pointer)hold(pointer);
                        remove_conflicts(context,values,{});bind(context,name,u32(1),values,limit);
                        for(auto pointer:values)if(pointer)release(pointer);reset_counters(context,counts);
                    }
                    else bridge.last_error="重疊的 CS UAV 綁定已拒絕";
                }else{bind(context,name,u32(1),values,limit);
                    if(what=="ConstantBuffers")for(unsigned i=0;i<u32(2);++i){context->constant_first[stage][u32(1)+i]=0;context->constant_count[stage][u32(1)+i]=0;}}
            }else get_views(context,name,u32(1),u32(2),arg(3));}
        }else if(symbol=="SetPredication" || symbol=="GetPredication"){
            graphics_handled=true;auto context=typed(arg(0),Kind::Context);
            if(symbol=="SetPredication"){
                if(arg(1)){const auto query=typed(arg(1),Kind::Predicate);require(query->device==context->device && !query->query_active && (context->context_type || query->query_ready));}
                bind_one(context,"Predicate",arg(1));context->predicate_value=u32(2)!=0;
            }else{
                if(arg(1)){const auto pointer=binding(context,"Predicate");if(pointer)hold(pointer);write_pointer(arg(1),pointer);}
                if(arg(2))memory.store(GuestAddress{arg(2)},32,context->predicate_value);
            }
        }else if(symbol=="IASetInputLayout" || symbol=="IAGetInputLayout" || symbol=="RSSetState" || symbol=="RSGetState"){
            graphics_handled=true;auto context=typed(arg(0),Kind::Context);const std::string name=symbol.starts_with("IA")?"Layout":"Raster";if(symbol.find("Set")!=symbol.npos)bind_one(context,name,arg(1));else{auto ptr=binding(context,name);if(ptr)hold(ptr);write_pointer(arg(1),ptr);}
        }else if(symbol=="IASetVertexBuffers" || symbol=="IAGetVertexBuffers"){
            graphics_handled=true;auto context=typed(arg(0),Kind::Context);require(u32(1)<=32 && u32(2)<=32-u32(1));if(symbol=="IASetVertexBuffers"){std::vector<std::uint64_t> values(u32(2));for(unsigned i=0;i<u32(2);++i){values[i]=arg(3)?memory.load(GuestAddress{arg(3)+i*pointer_size},pointer_bits):0;if(values[i] && outputs_conflict(context,values[i]))values[i]=0;context->strides[u32(1)+i]=arg(4)?static_cast<std::uint32_t>(memory.load(GuestAddress{arg(4)+4*i},32)):0;context->offsets[u32(1)+i]=arg(5)?static_cast<std::uint32_t>(memory.load(GuestAddress{arg(5)+4*i},32)):0;}bind(context,"Vertices",u32(1),values,32);}
            else for(unsigned i=0;i<u32(2);++i){if(arg(3)){const auto ptr=binding(context,"Vertices",u32(1)+i);if(ptr)hold(ptr);write_pointer(arg(3)+i*pointer_size,ptr);}if(arg(4))memory.store(GuestAddress{arg(4)+4*i},32,context->strides[u32(1)+i]);if(arg(5))memory.store(GuestAddress{arg(5)+4*i},32,context->offsets[u32(1)+i]);}
        }else if(symbol=="IASetIndexBuffer" || symbol=="IAGetIndexBuffer"){
            graphics_handled=true;auto context=typed(arg(0),Kind::Context);if(symbol=="IASetIndexBuffer"){require(!arg(1) || u32(2)==42 || u32(2)==57);if(arg(1)){const auto buffer=typed(arg(1),Kind::BufferObject);require(buffer->device==context->device && (buffer->resource->desc.bind&2));}bind_one(context,"Index",arg(1));context->index_format=u32(2);context->index_offset=u32(3);}else{if(arg(1)){auto ptr=binding(context,"Index");if(ptr)hold(ptr);write_pointer(arg(1),ptr);}if(arg(2))memory.store(GuestAddress{arg(2)},32,context->index_format);if(arg(3))memory.store(GuestAddress{arg(3)},32,context->index_offset);}
        }else if(symbol=="IASetPrimitiveTopology" || symbol=="IAGetPrimitiveTopology"){
            graphics_handled=true;auto context=typed(arg(0),Kind::Context);if(symbol=="IASetPrimitiveTopology"){require(u32(1)<=5 || (u32(1)>=10 && u32(1)<=13) || (u32(1)>=33 && u32(1)<=64));context->topology=u32(1);}else memory.store(GuestAddress{arg(1)},32,context->topology);
        }else if(symbol=="SOSetTargets" || symbol=="SOGetTargets"){
            graphics_handled=true;auto context=typed(arg(0),Kind::Context);const auto count=u32(1);require(count<=4);
            if(symbol=="SOSetTargets"){
                std::array<std::uint64_t,4> values{};std::array<std::uint32_t,4> offsets{};
                for(unsigned i=0;i<count;++i){values[i]=arg(2)?memory.load(GuestAddress{arg(2)+i*pointer_size},pointer_bits):0;offsets[i]=arg(3)?static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)+4*i},32)):0;
                    if(values[i]){const auto buffer=typed(values[i],Kind::BufferObject);require(buffer->device==context->device && (buffer->resource->desc.bind&0x10) && (offsets[i]==UINT32_MAX || (!(offsets[i]&3) && offsets[i]<=buffer->resource->desc.width)));}
                }
                require(nonconflicting(values));for(auto pointer:values)if(pointer)hold(pointer);remove_conflicts(context,values,"StreamOutput");
                bind(context,"StreamOutput",0,values,4);for(auto pointer:values)if(pointer)release(pointer);context->stream_offsets=offsets;
                for(unsigned i=0;i<count;++i)if(values[i] && offsets[i]!=UINT32_MAX){auto resource=object(values[i]).identity->resource;const auto offset=offsets[i];submit(context,{[resource,offset](auto&){resource->stream_cursor=offset;resource->stream_vertices=0;}},false);}
            }else if(arg(2))for(unsigned i=0;i<count;++i){const auto pointer=binding(context,"StreamOutput",i);if(pointer)hold(pointer);write_pointer(arg(2)+i*pointer_size,pointer);}
        }else if(symbol=="OMSetRenderTargets" || symbol=="OMGetRenderTargets" || symbol=="OMSetRenderTargetsAndUnorderedAccessViews" || symbol=="OMGetRenderTargetsAndUnorderedAccessViews"){
            graphics_handled=true;auto context=typed(arg(0),Kind::Context);const bool extended=symbol.find("Unordered")!=symbol.npos,set=symbol.starts_with("OMSet");
            const auto rt_count=u32(1),start=extended?u32(4):rt_count,uav_count=extended?u32(5):0U;
            if(!set){require(rt_count<=8 && start<=8 && uav_count<=8-start);get_views(context,"Targets",0,rt_count,arg(2));get_views(context,"Depth",0,1,arg(3));if(extended)get_views(context,"PSUnorderedAccessViews",start,uav_count,arg(6));}
            else if((rt_count==UINT32_MAX || rt_count<=8) && (uav_count==UINT32_MAX || (start<=8 && uav_count<=8-start)) &&
                    (rt_count==UINT32_MAX || uav_count==UINT32_MAX || start>=rt_count)){
                const bool keep_rt=rt_count==UINT32_MAX,keep_uav=uav_count==UINT32_MAX;
                std::array<std::uint64_t,8> targets{},unordered{};std::uint64_t depth=0;
                if(keep_rt){for(unsigned i=0;i<8;++i)targets[i]=binding(context,"Targets",i);depth=binding(context,"Depth");}
                else {const auto views=read_views(arg(2),rt_count,Kind::RenderTarget,context);std::copy(views.begin(),views.end(),targets.begin());depth=arg(3);if(depth)require(typed(depth,Kind::DepthView)->device==context->device);}
                auto requested=keep_uav?std::vector<std::uint64_t>{}:read_views(extended?arg(6):0,uav_count,Kind::Unordered,context);
                if(keep_uav){for(unsigned i=0;i<8;++i)unordered[i]=binding(context,"PSUnorderedAccessViews",i);}
                else std::copy(requested.begin(),requested.end(),unordered.begin()+start);
                std::vector<std::uint64_t> fresh;if(!keep_rt){fresh.assign(targets.begin(),targets.end());fresh.push_back(depth);}fresh.insert(fresh.end(),requested.begin(),requested.end());
                if(nonconflicting(fresh) && target_dimensions(targets,depth)){
                    const auto counts=counter_values(requested,extended && !keep_uav?arg(7):0);
                    if(keep_rt && !keep_uav)for(unsigned i=0;i<8;++i)if(i>=start || std::any_of(requested.begin(),requested.end(),[&](auto view){return overlaps(targets[i],view);}))targets[i]=0;
                    if(keep_uav && !keep_rt)for(unsigned i=0;i<8;++i)if(i<rt_count || std::any_of(fresh.begin(),fresh.end(),[&](auto view){return overlaps(unordered[i],view);}))unordered[i]=0;
                    // 保留旗標忽略對應輸入指標；先保存 COM 強參照，再解除舊的衝突。
                    for(auto pointer:targets)if(pointer)hold(pointer);for(auto pointer:unordered)if(pointer)hold(pointer);if(depth)hold(depth);
                    remove_conflicts(context,fresh,{});bind(context,"Targets",0,targets,8);bind(context,"PSUnorderedAccessViews",0,unordered,8);bind_one(context,"Depth",depth);
                    for(auto pointer:targets)if(pointer)release(pointer);for(auto pointer:unordered)if(pointer)release(pointer);if(depth)release(depth);reset_counters(context,counts);
                }else bridge.last_error="OM 輸出尺寸不符或 subresource 重疊，保留原綁定";
            }else bridge.last_error="OM 輸出槽位不合法，保留原綁定";
        }else if(symbol=="OMSetBlendState" || symbol=="OMGetBlendState" || symbol=="OMSetDepthStencilState" || symbol=="OMGetDepthStencilState"){
            graphics_handled=true;auto context=typed(arg(0),Kind::Context);const bool blend=symbol.find("Blend")!=symbol.npos,set=symbol.find("Set")!=symbol.npos;const std::string name=blend?"Blend":"DepthState";
            if(set){bind_one(context,name,arg(1));if(blend){context->blend_factor={1,1,1,1};if(arg(2)){auto bytes=read_bytes(memory,arg(2),16);std::memcpy(context->blend_factor.data(),bytes.data(),16);}context->sample_mask=u32(3);}else context->stencil_reference=u32(2);}
            else{if(arg(1)){auto ptr=binding(context,name);if(ptr)hold(ptr);write_pointer(arg(1),ptr);}if(blend){if(arg(2))memory.write(GuestAddress{arg(2)},std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(context->blend_factor.data()),16});if(arg(3))memory.store(GuestAddress{arg(3)},32,context->sample_mask);}else if(arg(2))memory.store(GuestAddress{arg(2)},32,context->stencil_reference);}
        }else if(symbol=="RSSetViewports" || symbol=="RSGetViewports" || symbol=="RSSetScissorRects" || symbol=="RSGetScissorRects"){
            graphics_handled=true;auto context=typed(arg(0),Kind::Context);const bool set=symbol.find("Set")!=symbol.npos,viewport=symbol.find("Viewports")!=symbol.npos;const auto count=set?u32(1):static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)},32));require(count<=16);
            if(set){if(viewport){context->viewports.resize(count);for(unsigned i=0;i<count;++i){const auto data=read_bytes(memory,arg(2)+24*i,24);std::memcpy(context->viewports[i].data(),data.data(),24);require(context->viewports[i][2]>=0 && context->viewports[i][3]>=0 && context->viewports[i][4]>=0 && context->viewports[i][5]<=1);}}else{context->scissors.resize(count);for(unsigned i=0;i<count;++i){const auto data=read_bytes(memory,arg(2)+16*i,16);std::memcpy(context->scissors[i].data(),data.data(),16);}}}
            else {const auto total=viewport?context->viewports.size():context->scissors.size();memory.store(GuestAddress{arg(1)},32,total);if(arg(2))for(unsigned i=0;i<std::min<std::size_t>(count,total);++i){const auto* data=viewport?reinterpret_cast<const std::uint8_t*>(context->viewports[i].data()):reinterpret_cast<const std::uint8_t*>(context->scissors[i].data());memory.write(GuestAddress{arg(2)+i*(viewport?24:16)},std::span<const std::uint8_t>{data,viewport?24U:16U});}}
        }else if(symbol=="Draw" || symbol=="DrawIndexed" || symbol=="DrawInstanced" || symbol=="DrawIndexedInstanced"){
            graphics_handled=true;auto context=typed(arg(0),Kind::Context);auto& draw=draw_state(context);draw.count=u32(1);draw.indexed=symbol.find("Indexed")!=symbol.npos;const bool instanced=symbol.find("Instanced")!=symbol.npos;draw.instances=instanced?u32(2):1;draw.start=u32(instanced?3:2);if(draw.indexed)draw.base_vertex=static_cast<std::int32_t>(u32(instanced?4:3));if(instanced)draw.start_instance=u32(draw.indexed?5:4);assign_visibility(context,draw);
            // 立即模式且未設 predicate 時直接執行：submit 對立即 context 本來就同步呼叫，包成 std::function 只多一次
            // 63 KB 狀態的搬移與堆積配置（微基準每次約 3.9 µs）。延遲 context 與 predicate 仍走 submit。
            if(direct_submit(context)){context->native->graphics_draw_retained(draw);count_direct(context,1,0);}
            else submit(context,recorded_draw(draw));
        }else if(symbol=="DrawIndexedInstancedIndirect" || symbol=="DrawInstancedIndirect" || symbol=="DispatchIndirect"){
            // 參數 buffer 須以 D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS 建立，offset 4 位元組對齊且整組參數在範圍內；
            // 不合法時與 runtime 相同略過呼叫（void 方法，debug layer 才報錯）。參數交給後端的原生間接呼叫，不讀回 CPU。
            graphics_handled=true;auto context=typed(arg(0),Kind::Context);auto arguments=typed(arg(1),Kind::BufferObject);const auto offset=u32(2);
            const bool dispatch=symbol=="DispatchIndirect",indexed=symbol=="DrawIndexedInstancedIndirect";const auto size=dispatch?12U:indexed?20U:16U;
            const auto& desc=arguments->resource->desc;
            if(arguments->device!=context->device || !(desc.misc&0x10U) || (offset&3U) || std::uint64_t{offset}+size>desc.width)bridge.last_error=std::string(symbol)+" 的參數 buffer、旗標或 offset 不合法";
            else if(dispatch){
                const auto& stage=draw_state(context).stages[5];auto resource=arguments->resource;
                if(direct_submit(context)){context->native->graphics_dispatch_indirect(stage,resource,offset);count_direct(context,0,1);}
                else submit(context,recorded_dispatch_indirect(stage,resource,offset));
            }else{
                auto& draw=draw_state(context);draw.indexed=indexed;draw.indirect=arguments->resource;draw.indirect_offset=offset;
                assign_visibility(context,draw);
                if(direct_submit(context)){context->native->graphics_draw_retained(draw);count_direct(context,1,0);}
                else submit(context,recorded_draw(draw));
            }
        }else if(symbol=="DrawAuto"){
            graphics_handled=true;auto context=typed(arg(0),Kind::Context);const auto& draw=draw_state(context);require(bool(draw.vertices[0].resource));
            submit(context,recorded_draw_auto(draw));
        }else if(symbol=="Dispatch"){
            graphics_handled=true;auto context=typed(arg(0),Kind::Context);const auto& stage=draw_state(context).stages[5];const std::array groups{u32(1),u32(2),u32(3)};
            if(direct_submit(context)){context->native->graphics_dispatch_retained(stage,groups);count_direct(context,0,1);}
            else submit(context,recorded_dispatch(stage,groups));
        }else if(symbol=="ClearRenderTargetView" || symbol=="ClearDepthStencilView"){
            graphics_handled=true;auto context=typed(arg(0),Kind::Context);auto view=view_of(arg(1));std::array<float,4> values{};const bool depth=symbol=="ClearDepthStencilView";if(depth){const auto bits=x86_?u32(3):(std::uint32_t{state.xmm[3][0]}|(std::uint32_t{state.xmm[3][1]}<<8)|(std::uint32_t{state.xmm[3][2]}<<16)|(std::uint32_t{state.xmm[3][3]}<<24));values[0]=std::bit_cast<float>(bits);values[1]=static_cast<float>(u32(4));}else {auto bytes=read_bytes(memory,arg(2),16);std::memcpy(values.data(),bytes.data(),16);}const auto flags=depth?u32(2):0;submit(context,{[view,values,flags](auto& native){native.graphics_clear(view,values,flags);}});
        }else if(symbol=="CopyStructureCount"){
            graphics_handled=true;auto context=typed(arg(0),Kind::Context),destination=typed(arg(1),Kind::BufferObject),source=typed(arg(3),Kind::Unordered);const auto offset=u32(2);auto counter=source->graphics_view.counter;
            if(counter && destination->device==context->device && source->device==context->device && !(offset&3) && std::uint64_t{offset}+4<=destination->resource->desc.width){
                const GraphicsView dst{destination->resource},src{counter};submit(context,{[dst,src,offset](auto& native){native.graphics_copy(dst,src,{offset,0,0},{0,0,0,4,1,1});}});
            }
            else bridge.last_error="CopyStructureCount 的來源或目的範圍不合法";
        }else if(symbol=="ClearUnorderedAccessViewUint" || symbol=="ClearUnorderedAccessViewFloat"){
            graphics_handled=true;auto context=typed(arg(0),Kind::Context),view=typed(arg(1),Kind::Unordered);require(view->device==context->device);const auto raw=bytes32(arg(2),4);std::array<std::uint32_t,4> values{};std::copy(raw.begin(),raw.end(),values.begin());
            const auto target=view->graphics_view;const bool floating=symbol.ends_with("Float");// 文件：Float 版只用於 FLOAT／UNORM／SNORM UAV，其他 UAV 的呼叫「不送達驅動」即略過；Uint 版適用任何 UAV。
            submit(context,{[target,values,floating](auto& native){if(!native.graphics_clear_unordered(target,values,floating) && !floating)throw std::invalid_argument("UAV 清除值與格式不相容");}});
        }else if(symbol=="ClearState"){graphics_handled=true;auto context=typed(arg(0),Kind::Context);clear_bindings(context);context->linkage_constants={};context->linkage_keys={};}
        else if(symbol=="Flush"){graphics_handled=true;auto context=typed(arg(0),Kind::Context);require(!context->context_type);context->native->graphics_flush(false);}
        else if(symbol=="GenerateMips"){graphics_handled=true;auto context=typed(arg(0),Kind::Context);const auto view=view_of(arg(1));submit(context,{[view](auto& native){native.graphics_mips(view);}});}
        else if((symbol=="CopyResource" || symbol=="CopySubresourceRegion" || symbol=="UpdateSubresource" || symbol=="Map" || symbol=="Unmap") && object(arg(1)).identity->resource){
            graphics_handled=true;auto context=typed(arg(0),Kind::Context),resource=object(arg(1)).identity;require(resource->device==context->device);
            if(symbol=="CopyResource" || symbol=="CopySubresourceRegion" || symbol=="UpdateSubresource")drop_persistent(resource);
            if(symbol=="CopyResource" || symbol=="CopySubresourceRegion"){const bool full=symbol=="CopyResource";auto source=object(arg(full?2:6)).identity;require(source->device==context->device && bool(source->resource));const auto& d=resource->resource->desc;const auto& s=source->resource->desc;if(std::getenv("X64ARM_TRACE_GRAPHICS_STATE"))std::fprintf(stderr,"[d3d11_copy] %.*s dst=%u/%u/%u/%u/%u/%u/%u/%u src=%u/%u/%u/%u/%u/%u/%u/%u\n",static_cast<int>(symbol.size()),symbol.data(),d.dimension,d.width,d.height,d.depth,d.levels,d.layers,d.format,d.samples,s.dimension,s.width,s.height,s.depth,s.levels,s.layers,s.format,s.samples);require(d.dimension==s.dimension && d.samples==s.samples && copy_compatible(d.format,s.format));if(full){require(resource->subresource_maps.empty() && source->subresource_maps.empty());require(d.width==s.width && d.height==s.height && d.depth==s.depth && d.levels==s.levels && d.layers==s.layers);for(unsigned i=0;i<d.levels*d.layers;++i){const auto dst=subresource(resource,i),src=subresource(source,i);submit(context,{[dst,src](auto& native){native.graphics_copy(dst,src,{0,0,0},{});}});}}else{require(!resource->subresource_maps.contains(u32(2)) && !source->subresource_maps.contains(u32(7)));std::array<std::uint32_t,6> box{};if(arg(8)){const auto values=bytes32(arg(8),6);std::copy(values.begin(),values.end(),box.begin());}const auto dst=subresource(resource,u32(2)),src=subresource(source,u32(7));const std::array origin{u32(3),u32(4),u32(5)};submit(context,{[dst,src,origin,box](auto& native){native.graphics_copy(dst,src,origin,box);}});}}
            else if(symbol=="UpdateSubresource"){
                require(!resource->subresource_maps.contains(u32(2)));auto view=subresource(resource,u32(2));const auto& d=resource->resource->desc;
                std::array<std::uint32_t,6> box{0,0,0,d.dimension==1?d.width:std::max(1U,d.width>>view.level),std::max(1U,d.height>>view.level),std::max(1U,d.depth>>view.level)};
                if(arg(3)){const auto values=bytes32(arg(3),6);std::copy(values.begin(),values.end(),box.begin());}
                // 空 box 是規範中的 no-op，不能讀取 pSrcData；非空區域保留其外部的既有像素。
                if(box[0]<box[3] && box[1]<box[4] && box[2]<box[5]){
                    require(box[3]<=(d.dimension==1?d.width:std::max(1U,d.width>>view.level)) && box[4]<=std::max(1U,d.height>>view.level) && box[5]<=std::max(1U,d.depth>>view.level));
                    require(!(d.dimension==1 && (d.bind&4) && arg(3)) || (partial_constant_update && !(box[0]&15U) && !(box[3]&15U)));
                    const auto w=box[3]-box[0],h=box[4]-box[1],z=box[5]-box[2];
                    const auto pitch=d.dimension==1?std::array{w,h}:graphics_format_pitch(d.format,w,h);const auto line=pitch[0];
                    const auto row=d.dimension==1?w:u32(5),image=d.dimension==1?w:u32(6);
                    require(row>=line && (z==1 || image>=std::uint64_t{row}*pitch[1]));
                    const auto size=std::uint64_t{image}*(z-1)+std::uint64_t{row}*(pitch[1]-1)+line;require(size<=512*1024*1024);
                    const auto data=read_bytes(memory,arg(4),static_cast<std::size_t>(size));
                    const bool region=arg(3)!=0;submit(context,{[view,data,row,image,box,region](auto& native){if(region)native.graphics_write_region(view,data,row,image,box);else native.graphics_write(view,data,row,image);}});
                }
            }
            else if(symbol=="Map"){const auto& d=resource->resource->desc;const auto usage=word(resource->desc,d.dimension==1?4:d.dimension==2?16:d.dimension==4?20:28),access=word(resource->desc,d.dimension==1?12:d.dimension==2?24:d.dimension==4?28:36);const auto type=u32(3);require(!resource->offered && !resource->subresource_maps.contains(u32(2)) && type>=1 && type<=5 && !(u32(4)&~0x100000U) && ((usage==3 && type<=3) || (usage==2 && type>=2)) && (!(type==1 || type==3) || (access&0x20000)) && (!(type>=2) || (access&0x10000)));auto view=subresource(resource,u32(2));
                const bool persistent=persistent_maps_enabled && usage==2 && (type==4 || type==5) && !context->context_type;
                const auto kept=persistent?resource->persistent_maps.find(u32(2)):resource->persistent_maps.end();
                std::uint64_t pointer=0;std::size_t mapped_size=0;
                if(kept!=resource->persistent_maps.end()){pointer=kept->second.pointer;mapped_size=kept->second.size;}
                else{
                    std::vector<std::uint8_t> data;if(type==4 && discard_enabled && !context->context_type){const auto pitch=d.dimension==1?std::array{d.width,1U}:graphics_format_pitch(d.format,std::max(1U,d.width>>view.level),std::max(1U,d.height>>view.level));const auto size=std::uint64_t{pitch[0]}*pitch[1]*std::max(1U,d.depth>>view.level);require(size<=512U*1024U*1024U);data.resize(static_cast<std::size_t>(size));}else{data=context->native->graphics_read(view);if(type==4)std::fill(data.begin(),data.end(),0);}
                    pointer=allocate_heap(memory,data.size());if(!pointer)throw std::bad_alloc{};resource->allocations.push_back(pointer);memory.write(GuestAddress{pointer},data);mapped_size=data.size();
                    if(persistent)resource->persistent_maps.emplace(u32(2),Identity::PersistentMap{pointer,mapped_size});
                }
                resource->subresource_maps.emplace(u32(2),Identity::SubresourceMap{pointer,mapped_size,type});write_pointer(arg(5),pointer);const auto pitch=d.dimension==1?std::array{d.width,1U}:graphics_format_pitch(d.format,std::max(1U,d.width>>view.level),std::max(1U,d.height>>view.level));memory.store(GuestAddress{arg(5)+pointer_size},32,pitch[0]);memory.store(GuestAddress{arg(5)+pointer_size+4},32,pitch[0]*pitch[1]);}
            else {const auto found=resource->subresource_maps.find(u32(2));require(found!=resource->subresource_maps.end());const auto mapping=found->second;if(mapping.type!=1){const auto view=subresource(resource,u32(2));
                    const auto kept_map=resource->persistent_maps.find(u32(2));const bool persistent=kept_map!=resource->persistent_maps.end() && kept_map->second.pointer==mapping.pointer;
                    // 持久區塊直接以短期唯讀視圖上傳，不另複製一份；視圖只在本次呼叫內使用。
                    std::vector<std::uint8_t> copied;std::span<const std::uint8_t> data;
                    if(persistent){data=memory.readable_prefix(GuestAddress{mapping.pointer},mapping.size);if(data.size()<mapping.size)data={};}
                    if(data.empty()){copied=read_bytes(memory,mapping.pointer,mapping.size);data=copied;}
                    if(mapping.type==4 && discard_enabled && !context->context_type)context->native->graphics_write_discard(view,data,0,0);
                    else if(mapping.type==5 && persistent)context->native->graphics_write_no_overwrite(view,data,0,0);
                    else context->native->graphics_write(view,data,0,0);}const auto kept=resource->persistent_maps.find(u32(2));if(kept==resource->persistent_maps.end() || kept->second.pointer!=mapping.pointer){require(free_heap(memory,mapping.pointer,state));std::erase(resource->allocations,mapping.pointer);}resource->subresource_maps.erase(found);}
        }else if(symbol=="ResolveSubresource"){
            graphics_handled=true;auto context=typed(arg(0),Kind::Context),destination=object(arg(1)).identity,source=object(arg(3)).identity;
            require(destination->device==context->device && source->device==context->device && bool(destination->resource) && bool(source->resource));
            require(destination->resource->desc.dimension==3 && word(destination->desc,28)==0 && !destination->subresource_maps.contains(u32(2)) && !source->subresource_maps.contains(u32(4)) && !destination->offered && !source->offered);
            const auto dst=subresource(destination,u32(2)),src=subresource(source,u32(4));const auto format=u32(5);submit(context,{[dst,src,format](auto& native){native.graphics_resolve(dst,src,format);}});
        }else if(symbol=="CheckFormatSupport" && u32(1)!=103){
            graphics_handled=true;auto device=typed(arg(0),Kind::Device);require(bool(arg(2)));const auto f=u32(1);const auto support=format_support(device,f);
            if(!support)result=not_supported;
            memory.store(GuestAddress{arg(2)},32,support.value_or(0));
            if(std::getenv("X64ARM_TRACE_GRAPHICS_STATE"))std::fprintf(stderr,"[d3d11_caps] format=%u support=0x%x\n",f,support.value_or(0));
        }else if(symbol=="CheckMultisampleQualityLevels"){
            graphics_handled=true;auto device=typed(arg(0),Kind::Device);require(bool(arg(3)));const auto quality=device->native->graphics_samples_supported(u32(1),u32(2))?1U:0U;
            memory.store(GuestAddress{arg(3)},32,quality);if(!u32(2) || u32(2)>32)result=invalid;
            if(std::getenv("X64ARM_TRACE_GRAPHICS_STATE"))std::fprintf(stderr,"[d3d11_caps] format=%u samples=%u quality=%u\n",u32(1),u32(2),quality);
        }
        else if(symbol=="BlendGetDesc1"){
            // D3D11_BLEND_DESC1：每個 RT 補 LogicOpEnable=FALSE、LogicOp=NOOP（不支援邏輯運算混合）。
            graphics_handled=true;const auto& desc=typed(arg(0),Kind::Blend)->desc;require(desc.size()>=264);std::vector<std::uint8_t> out(328);put_word(out,0,word(desc,0));put_word(out,4,word(desc,4));
            for(unsigned i=0;i<8;++i){const auto in=8+i*32,at=8+i*40;put_word(out,at,word(desc,in));for(unsigned j=0;j<6;++j)put_word(out,at+8+j*4,word(desc,in+4+j*4));put_word(out,at+32,4);out[at+36]=desc[in+28];}
            memory.write(GuestAddress{arg(1)},out);
        }else if(symbol=="RasterGetDesc1" || symbol=="RasterGetDesc2"){
            // ForcedSampleCount 與 ConservativeRaster 建立時只接受 0。
            graphics_handled=true;auto desc=typed(arg(0),Kind::Raster)->desc;desc.resize(symbol=="RasterGetDesc1"?44:48);memory.write(GuestAddress{arg(1)},desc);
        }else if(symbol=="Texture3DGetDesc1"){
            graphics_handled=true;auto desc=typed(arg(0),Kind::Texture3D)->desc;desc.resize(40);memory.write(GuestAddress{arg(1)},desc);
        }else if(symbol=="ViewGetDesc1"){
            // *_DESC1 只在 TEXTURE2D／TEXTURE2DARRAY 多 PlaneSlice（恆為 0），其餘維度原樣並補長度。
            graphics_handled=true;const auto& value=object(arg(0));require(value.kind==Kind::ShaderResource || value.kind==Kind::RenderTarget || value.kind==Kind::Unordered);
            const bool srv=value.kind==Kind::ShaderResource;auto desc=value.identity->desc;desc.resize(srv?24:20);std::vector<std::uint8_t> out(srv?28:24);
            const auto dimension=word(desc,4);const unsigned keep=dimension==4?(srv?16U:12U):(srv?24U:20U);
            std::copy(desc.begin(),desc.begin()+keep,out.begin());memory.write(GuestAddress{arg(1)},out);
        }else if(symbol=="QueryGetDesc1"){
            graphics_handled=true;auto id=typed(arg(0),Kind::Query);auto desc=id->desc;desc.resize(12);put_word(desc,8,id->query_context);memory.write(GuestAddress{arg(1)},desc);
        }else if(symbol=="CheckMultisampleQualityLevels1"){
            // TILED_RESOURCE 查詢：不支援 tiled resources，品質等級 0（一般查詢已轉接到 11.0）。
            graphics_handled=true;(void)typed(arg(0),Kind::Device);memory.check(GuestAddress{arg(4)},4,Access::Write);memory.store(GuestAddress{arg(4)},32,0);
        }else if(symbol=="GetResourceTiling"){
            // 非 tiled 資源：磚數、packed mip 與磚形狀皆為 0，子資源磚資訊數量回 0。
            graphics_handled=true;auto device=typed(arg(0),Kind::Device);const auto resource=object(arg(1)).identity;require(resource_kind(object(arg(1)).kind) && resource->device==device->root);
            if(arg(2))memory.store(GuestAddress{arg(2)},32,0);
            if(arg(3))memory.write(GuestAddress{arg(3)},std::vector<std::uint8_t>(12));
            if(arg(4))memory.write(GuestAddress{arg(4)},std::vector<std::uint8_t>(12));
            if(arg(5))memory.store(GuestAddress{arg(5)},32,0);
        }else if(symbol=="RegisterDeviceRemovedEvent" || symbol=="UnregisterDeviceRemoved"){
            // 共用 Metal 裝置在程序存活期間不會被移除，登記的事件不會觸發；仍驗證事件並配發 cookie。
            graphics_handled=true;(void)typed(arg(0),Kind::Device);
            if(symbol=="RegisterDeviceRemovedEvent"){
                static std::uint32_t next_removed_cookie=1;const auto event=runtime_.wait_objects.find(arg(1));
                if(!arg(2) || event==runtime_.wait_objects.end() || event->second.kind!=WaitObject::Kind::Event)result=invalid;
                else{memory.check(GuestAddress{arg(2)},4,Access::Write);memory.store(GuestAddress{arg(2)},32,next_removed_cookie++);}
            }
        }else if(symbol=="CreateFence" || symbol=="OpenSharedFence"){
            // 不建立跨程序共享 fence（SHARED／SHARED_CROSS_ADAPTER 回 DXGI_ERROR_UNSUPPORTED），因此也沒有可開啟的共享 handle。
            graphics_handled=true;auto device=typed(arg(0),Kind::Device);const bool creating=symbol=="CreateFence";const unsigned out=creating?4U:3U;
            if(arg(out)){output(out);write_pointer(arg(out),0);}
            const auto guid=read_guid(memory,arg(creating?3U:2U));
            if(!creating || !arg(out))result=invalid;
            else if(u32(2)&~8U)result=(u32(2)&6U) && !(u32(2)&~14U)?not_supported:invalid;
            else if(guid!=IID_ID3D11Fence && guid!=IID_IUnknown && guid!=IID_ID3D11DeviceChild)result=no_interface;
            else{auto id=create(Kind::Fence,device->root);id->fence_value=arg(1);write_pointer(arg(out),id->root);}
        }else if(symbol=="FenceGetCompletedValue"){
            graphics_handled=true;const auto value=typed(arg(0),Kind::Fence)->fence_value;result=value;if(x86_)state.reg(Gpr::Rdx)=value>>32;
        }else if(symbol=="FenceSetEventOnCompletion"){
            graphics_handled=true;auto fence=typed(arg(0),Kind::Fence);const auto value=arg(1),handle=arg(2);const auto event=runtime_.wait_objects.find(handle);
            if(event==runtime_.wait_objects.end() || event->second.kind!=WaitObject::Kind::Event)result=invalid;
            else if(value<=fence->fence_value)event->second.set_signal(true);
            else{require(fence->fence_events.size()<4096);fence->fence_events.emplace_back(value,handle);}
        }else if(symbol=="FenceCreateSharedHandle"){
            graphics_handled=true;(void)typed(arg(0),Kind::Fence);if(arg(4)){memory.check(GuestAddress{arg(4)},pointer_size,Access::Write);write_pointer(arg(4),0);}result=invalid;
        }else if(symbol=="Signal" || symbol=="Wait"){
            // 單一 Metal 佇列：Signal 等先前提交的命令完成後才更新 fence 並觸發已到達的事件；Wait 依佇列順序不需額外等待。
            graphics_handled=true;auto context=typed(arg(0),Kind::Context);auto fence=typed(arg(1),Kind::Fence);require(!context->context_type && fence->device==context->device);
            if(symbol=="Signal"){
                context->native->graphics_flush(true);fence->fence_value=arg(2);
                std::erase_if(fence->fence_events,[&](const auto& item){if(item.first>fence->fence_value)return false;
                    if(const auto event=runtime_.wait_objects.find(item.second);event!=runtime_.wait_objects.end())event->second.set_signal(true);return true;});
            }
        }else if(symbol=="DiscardResource" || symbol=="DiscardView" || symbol=="DiscardView1"){
            // 捨棄只允許內容變為未定義；保留原內容同樣符合語義（D3D11_OPTIONS 回報 DiscardAPIsSeenByDriver=FALSE）。
            graphics_handled=true;auto context=typed(arg(0),Kind::Context);const auto& value=object(arg(1));
            require(symbol=="DiscardResource"?resource_kind(value.kind):graphics_view_kind(value.kind));require(value.identity->device==context->device);
            if(symbol=="DiscardView1")require(!u32(3) || arg(2));
        }else if(symbol=="ClearView"){
            // RTV 以清除色、UAV 以浮點值轉格式。有矩形時與 D3D12 的矩形 UAV 清除相同：清暫存資源後逐矩形複製，其餘像素保持原值。
            graphics_handled=true;auto context=typed(arg(0),Kind::Context);const auto& value=object(arg(1));
            require((value.kind==Kind::RenderTarget || value.kind==Kind::Unordered) && value.identity->device==context->device);
            const bool render_target=value.kind==Kind::RenderTarget;const auto view=view_of(arg(1));std::array<float,4> color{};
            {const auto bytes=read_bytes(memory,arg(2),16);std::memcpy(color.data(),bytes.data(),16);}
            require(!u32(4) || arg(3));require(u32(4)<=4096);std::vector<std::array<std::int32_t,4>> rects(u32(4));
            for(unsigned i=0;i<rects.size();++i){const auto bytes=read_bytes(memory,arg(3)+i*16,16);std::memcpy(rects[i].data(),bytes.data(),16);}
            submit(context,{[view,color,rects,render_target](auto& native){
                std::array<std::uint32_t,4> bits{};std::memcpy(bits.data(),color.data(),16);
                const auto clear=[&](const GraphicsView& target){if(render_target)native.graphics_clear(target,color,0);else (void)native.graphics_clear_unordered(target,bits,true);};
                if(rects.empty()){clear(view);return;}
                const auto& desc=view.resource->desc;
                if(desc.dimension==1){
                    const auto element=std::max(1U,graphics_format_bytes(view.format?view.format:desc.format));const auto width=view.count/element;
                    for(const auto& r:rects){const auto x=static_cast<std::uint32_t>(std::clamp<std::int64_t>(r[0],0,width)),right=static_cast<std::uint32_t>(std::clamp<std::int64_t>(r[2],0,width));
                        if(x<right){auto region=view;region.first+=x*element;region.count=(right-x)*element;clear(region);}}
                    return;
                }
                GraphicsView cleared=view;cleared.resource=native.graphics_resource(desc);clear(cleared);
                const auto width=std::max(1U,desc.width>>view.level),height=std::max(1U,desc.height>>view.level);
                for(const auto& r:rects){
                    const auto x=static_cast<std::uint32_t>(std::clamp<std::int64_t>(r[0],0,width)),y=static_cast<std::uint32_t>(std::clamp<std::int64_t>(r[1],0,height));
                    const auto right=static_cast<std::uint32_t>(std::clamp<std::int64_t>(r[2],0,width)),bottom=static_cast<std::uint32_t>(std::clamp<std::int64_t>(r[3],0,height));
                    if(x<right && y<bottom)native.graphics_copy(view,cleared,{x,y,0},{x,y,0,right,bottom,1});
                }
            }});
        }else if(symbol=="UpdateTileMappings" || symbol=="CopyTileMappings" || symbol=="ResizeTilePool"){
            // 沒有 tiled resources 與 tile pool（D3D11_OPTIONS1 TiledResourcesTier=0），參數必然無效。
            graphics_handled=true;(void)typed(arg(0),Kind::Context);result=invalid;
        }else if(symbol=="CopyTiles" || symbol=="UpdateTiles" || symbol=="TiledResourceBarrier"){
            // 非 tiled 資源的磚操作由 runtime 丟棄；屏障沒有 tiled 存取需要排序。
            graphics_handled=true;(void)typed(arg(0),Kind::Context);
        }else if(symbol=="IsAnnotationEnabled"){graphics_handled=true;(void)typed(arg(0),Kind::Context);result=0;}
        else if(symbol=="SetMarkerInt" || symbol=="BeginEventInt" || symbol=="EndEvent"){graphics_handled=true;(void)typed(arg(0),Kind::Context);}
        else if(symbol=="AnnotationBeginEvent" || symbol=="AnnotationEndEvent" || symbol=="AnnotationSetMarker" || symbol=="AnnotationGetStatus"){
            // 沒有附加 PIX 等分析工具：BeginEvent／EndEvent 回 -1、GetStatus 回 FALSE，標記不記錄。
            graphics_handled=true;(void)typed(arg(0),Kind::Annotation);result=symbol=="AnnotationBeginEvent" || symbol=="AnnotationEndEvent"?UINT32_MAX:0;
        }else if(symbol=="Flush1"){
            graphics_handled=true;auto context=typed(arg(0),Kind::Context);require(u32(1)<=4);
            if(!context->context_type){
                if(arg(2)){const auto event=runtime_.wait_objects.find(arg(2));require(event!=runtime_.wait_objects.end() && event->second.kind==WaitObject::Kind::Event);
                    context->native->graphics_flush(true);event->second.set_signal(true);}
                else context->native->graphics_flush(false);
            }
        }else if(symbol=="SetHardwareProtectionState"){graphics_handled=true;typed(arg(0),Kind::Context)->hardware_protection=u32(1)!=0;}
        else if(symbol=="GetHardwareProtectionState"){
            graphics_handled=true;auto context=typed(arg(0),Kind::Context);memory.check(GuestAddress{arg(1)},4,Access::Write);memory.store(GuestAddress{arg(1)},32,context->hardware_protection?1:0);
        }else if(symbol=="OpenSharedResourceByName"){
            // 不建立具名共享資源；與 Windows 名稱不存在時相同。
            graphics_handled=true;(void)typed(arg(0),Kind::Device);if(arg(4)){output(4);write_pointer(arg(4),0);}result=0x80070002U;
        }
        else if(symbol=="CheckFeatureSupport"){
            graphics_handled=true;auto device=typed(arg(0),Kind::Device);const auto feature=u32(1),size=u32(3);require(size<=256);std::vector<std::uint8_t> data(size);
            const auto words=[&](std::initializer_list<std::uint32_t> values){if(size!=values.size()*4){result=invalid;return;}unsigned at=0;for(const auto value:values){put_word(data,at,value);at+=4;}};
            switch(feature){
            case 0:words({1,1});break; // THREADING：並行建立與 command list
            case 1:words({0});break; // DOUBLES：Metal 著色器沒有雙精度
            case 2:case 3:{
                if(size!=8){result=invalid;break;}const auto format=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)},32));put_word(data,0,format);
                if(feature==2){const auto support=format_support(device,format);put_word(data,4,support.value_or(0));if(!support)result=not_supported;}
                else put_word(data,4,format_support2(device,format));
                break;}
            case 4:words({1});break; // D3D10_X_HARDWARE_OPTIONS：cs_4_x
            // D3D11_OPTIONS：ClearView、常數緩衝部分更新與位移、動態常數緩衝／SRV 緩衝 NO_OVERWRITE；邏輯運算、捨棄與複製旗標不交給驅動。
            case 5:if(legacy_format_caps)words({0,0,0,0,0,0,0,0,0,0,0,0,0,0});else words({0,0,0,0,1,0,1,1,1,1,0,0,0,0});break;
            case 6:words({0});break; // ARCHITECTURE_INFO
            case 7:words({1});break; // D3D9_OPTIONS：完整非 2 冪紋理
            case 8:words({0,0});break; // 最小精度：一律以 32 位元執行
            case 9:words({1});break; // D3D9_SHADOW_SUPPORT：深度比較取樣
            case 10:words({0,0,0,0});break; // OPTIONS1：無 tiled resources、min/max 濾波、深度 ClearView、預設緩衝映射
            case 11:words({1});break; // D3D9_SIMPLE_INSTANCING_SUPPORT
            case 12:words({0});break; // MARKER_SUPPORT
            case 13:words({1,1,1,1});break; // D3D9_OPTIONS1
            case 14:words({0,0,0,0,0,0,0,0});break; // OPTIONS2：無 ROV、保守光柵化、tiled、預設紋理映射與標準 swizzle
            case 15:case 17:case 18:case 19:words({0});break; // OPTIONS3、OPTIONS4、SHADER_CACHE、OPTIONS5
            case 16:words({32,40});break; // GPU 虛擬位址：每資源 32 位元、每程序 40 位元
            case 20:words({0,0});break; // DISPLAYABLE
            default:result=invalid;break;
            }
            if(!result || result==not_supported)memory.write(GuestAddress{arg(2)},data);
        }
        else if(symbol=="CheckCounterInfo"){
            // 不提供裝置相依計數器：LastDeviceDependentCounter=0、可同時啟用 0 個、可偵測平行單元 1。
            graphics_handled=true;(void)typed(arg(0),Kind::Device);if(arg(1)){std::vector<std::uint8_t> info(12);put_word(info,8,1);memory.write(GuestAddress{arg(1)},info);}
        }else if(symbol=="CreateCounter" || symbol=="CheckCounter"){
            // D3D11 只剩裝置相依計數器（>=D3D11_COUNTER_DEVICE_DEPENDENT_0），CheckCounterInfo 回報 0 個：
            // 合法描述回 DXGI_ERROR_UNSUPPORTED，其餘 E_INVALIDARG；輸出介面清為 NULL。
            graphics_handled=true;(void)typed(arg(0),Kind::Device);if(symbol=="CreateCounter" && arg(2)){output(2);write_pointer(arg(2),0);}
            const auto counter=arg(1)?static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)},32)):0U;
            const auto flags=arg(1) && symbol=="CreateCounter"?static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)+4},32)):0U;
            result=arg(1) && counter>=0x40000000U && !flags?not_supported:invalid;
        }else if(symbol=="SetExceptionMode"){graphics_handled=true;auto device=typed(arg(0),Kind::Device);if(u32(1)&~1U)result=invalid;else device->exception_mode=u32(1);}
        else if(symbol=="GetExceptionMode"){graphics_handled=true;result=typed(arg(0),Kind::Device)->exception_mode;}
        else if(symbol=="CreateQuery" || symbol=="CreatePredicate"){
            graphics_handled=true;auto device=typed(arg(0),Kind::Device);if(arg(2)){output(2);write_pointer(arg(2),0);}auto desc=read_bytes(memory,arg(1),8);const auto type=word(desc,0);require(symbol!="CreatePredicate" || type==5 || type==7);require(!(word(desc,4)&~1U) && (!word(desc,4) || type==5));if(((type==2 || type==3) && !device->native->graphics_timestamp_supported()) || (type!=0 && type!=1 && type!=2 && type!=3 && type!=5 && type!=6 && type!=7)){result=not_supported;}else if(!arg(2))result=1;else{auto id=create(symbol=="CreatePredicate"?Kind::Predicate:Kind::Query,device->root);id->desc=desc;id->query_size=type==0 || type==5 || type==7?4:type==3?16:type==4?88:type>=6?16:8;id->query_data.resize(id->query_size);if(type==5 || (type==1 && !d3d11_visibility_pool_enabled())){GraphicsResourceDesc resource;resource.width=65536;id->resource=device->native->graphics_resource(resource);++bridge.resource_creations[std::string(symbol)];}write_pointer(arg(2),id->root);}
        }else if(symbol=="GetDataSize"){graphics_handled=true;result=object(arg(0)).identity->query_size;}
        else if(symbol=="Begin" || symbol=="End" || symbol=="GetData"){
            graphics_handled=true;auto context=typed(arg(0),Kind::Context),query=object(arg(1)).identity;require(query->device==context->device && query->query_size);const auto type=word(query->desc,0);
            if(symbol=="Begin"){
                require(binding(context,"Predicate")!=arg(1));require(type!=0 && type!=2 && !query->query_active);query->query_active=true;query->query_ready=false;query->sequence=0;
                if(type==6 || type==7){auto stats=context->stream_statistics;submit(context,{[query,stats](auto&){query->stream_begin=*stats;}},false);}
                if(type==1 || type==5){require(!binding(context,"Visibility"));submit(context,{[query](auto&){query->query_ready=false;}},false);
                    query->visibility_pooled=type==1 && !context->context_type && d3d11_visibility_pool_enabled();query->visibility_segments.clear();
                    // 立即內容 OCCLUSION 改用共用池，建立時不再配置每查詢緩衝；延遲內容等其他用途在第一次 Begin 時補配置。
                    if(!query->visibility_pooled && !query->resource){GraphicsResourceDesc resource;resource.width=65536;query->resource=context->native->graphics_resource(resource);}
                    if(!query->visibility_pooled){const auto view=view_of(arg(1));query->visibility_planned=0;visibility_zero(context,query,view,0);}
                    bind_one(context,"Visibility",arg(1));}
            }else if(symbol=="End"){
                require(binding(context,"Predicate")!=arg(1));require(type==0 || type==2 || query->query_active);query->query_active=false;query->query_ready=false;query->query_fence.reset();query->query_timestamp.reset();query->query_polls=0;const auto sequence=query->sequence;
                // 事件查詢：提交後取完成旗標，GetData 輪詢旗標。Chromium／ANGLE 以此當 fence，每幀數十次；
                // 同步等待 GPU 閒置會把 GPU 程序卡在每次 End（Steam 實測每次約 0.4 ms）。
                static const bool trace_state=std::getenv("X64ARM_TRACE_GRAPHICS_STATE")!=nullptr;
                if(trace_state){static std::array<std::uint32_t,8> ends{};if(type<ends.size() && ++ends[type]%500==1)std::fprintf(stderr,"[d3d11_query_end] type=%u count=%u\n",type,ends[type]);}
                if(type==0){submit(context,{[query](auto& native){std::fill(query->query_data.begin(),query->query_data.end(),0);put_word(query->query_data,0,1);query->query_fence=native.graphics_fence_pending();}},false);}
                // 時間戳與 disjoint：在提交點記錄 GPU 時間戳，GetData 輪詢完成旗標後填入。
                else if(type==2 || type==3){submit(context,{[query](auto& native){std::fill(query->query_data.begin(),query->query_data.end(),0);query->query_timestamp=native.graphics_timestamp_future();}},false);}
                // 遮蔽／遮蔽判定：原本在 End 同步 flush 並讀回（UE4 每秒上百次、每次等 GPU 約 0.4 ms），
                // 改為提交後取完成旗標，GetData 看到旗標完成才讀回累加；型別 6／7 仍走同步統計。
                else if(type==1 || type==5){submit(context,{[query,sequence](auto& native){std::fill(query->query_data.begin(),query->query_data.end(),0);query->query_pending_sequence=sequence;query->query_fence=native.graphics_fence_pending();}},false);}
                else submit(context,{[query,type,sequence,stats=context->stream_statistics](auto& native){
                    native.graphics_flush(true);std::fill(query->query_data.begin(),query->query_data.end(),0);
                    if(type==2){const auto stamp=native.graphics_timestamp();std::memcpy(query->query_data.data(),&stamp.ticks,8);}
                    else if(type==3){const auto stamp=native.graphics_timestamp();std::memcpy(query->query_data.data(),&stamp.frequency,8);put_word(query->query_data,8,stamp.disjoint?1U:0U);}
                    else if(type==1 || type==5){const auto samples=native.graphics_read(GraphicsView{query->resource});std::uint64_t total=0;for(unsigned i=0;i<sequence;++i){std::uint64_t value=0;std::memcpy(&value,samples.data()+i*8,8);total+=value;}if(type==5)put_word(query->query_data,0,total?1:0);else std::memcpy(query->query_data.data(),&total,8);}
                    if(type==6 || type==7){const auto written=stats->written-query->stream_begin.written,needed=stats->needed-query->stream_begin.needed;if(type==7)put_word(query->query_data,0,written<needed);else {std::memcpy(query->query_data.data(),&written,8);std::memcpy(query->query_data.data()+8,&needed,8);}}
                    query->query_ready=true;
                }},false);
                if(type==1 || type==5)bind_one(context,"Visibility",0);
            }else {require(!context->context_type);
                if(query->query_fence && query->query_fence->load(std::memory_order_acquire)){
                    if(type==1 || type==5){
                        // fence 已完成：只讀本查詢的取樣範圍，不等待同一緩衝上較新的批次，也不排空命令執行緒。
                        std::uint64_t total=0;
                        const auto add=[&](const std::shared_ptr<GraphicsResource>& buffer,std::uint32_t base,std::uint64_t count){
                            if(!count)return;GraphicsView view{buffer};view.first=base;view.count=static_cast<std::uint32_t>(count*8);
                            const auto samples=context->native->graphics_read_completed(view);
                            for(std::size_t i=0;i<count && (i+1)*8<=samples.size();++i){std::uint64_t value=0;std::memcpy(&value,samples.data()+i*8,8);total+=value;}
                        };
                        if(query->visibility_pooled)for(const auto& segment:query->visibility_segments)add(segment.buffer,segment.base,segment.count);
                        else add(query->resource,0,query->query_pending_sequence);
                        std::fill(query->query_data.begin(),query->query_data.end(),0);
                        if(type==5)put_word(query->query_data,0,total?1U:0U);else std::memcpy(query->query_data.data(),&total,8);
                    }
                    query->query_ready=true;query->query_fence.reset();
                }
                if(query->query_timestamp && query->query_timestamp->done.load(std::memory_order_acquire)){
                    const auto& stamp=query->query_timestamp->value;
                    if(type==2)std::memcpy(query->query_data.data(),&stamp.ticks,8);
                    else {std::memcpy(query->query_data.data(),&stamp.frequency,8);put_word(query->query_data,8,stamp.disjoint?1U:0U);}
                    query->query_ready=true;query->query_timestamp.reset();
                }
                // D3D11 契約：GetData 沒帶 D3D11_ASYNC_GETDATA_DONOTFLUSH 就隱含提交；帶了也在連續輪詢 32 次後提交，避免永不完成。
                if(!query->query_ready){if(!(u32(4)&1U) || ++query->query_polls>=32U){query->query_polls=0;context->native->graphics_flush(false);}result=1;}
                else{require(!arg(2) || u32(3)==query->query_size);if(arg(2))memory.write(GuestAddress{arg(2)},query->query_data);}}
        }
        else if(symbol=="DXGIGetDebugInterface1"){
            // 未安裝圖形工具（DXGI 除錯層）的 Windows 回 E_NOINTERFACE；Flags 保留須為 0。
            graphics_handled=true;output(2);write_pointer(arg(2),0);result=u32(0)?invalid:no_interface;
        }
        else if(symbol=="CreateDXGIFactory2"){
            graphics_handled=true;output(2);write_pointer(arg(2),0);const auto guid=read_guid(memory,arg(1));if(u32(0)&~1U)result=invalid;else if(!dxgi_factory_guid(guid) && guid!=IID_IUnknown)result=no_interface;else write_pointer(arg(2),create(Kind::Factory)->root);
        }else if(symbol=="GetAdapter"){
            graphics_handled=true;auto device=typed(arg(0),Kind::DxgiDevice);output(1);const auto adapter=device_adapter(device);hold(adapter);write_pointer(arg(1),adapter);
        }else if(symbol=="DxgiSubGetDevice"){
            graphics_handled=true;auto id=object(arg(0)).identity;output(2);const auto guid=read_guid(memory,arg(1));write_pointer(arg(2),0);
            std::uint64_t pointer=0;if(guid==IID_IUnknown || (object(id->device).identity->api10?(guid==d3d10_guid::IID_ID3D10Device || guid==d3d10_guid::IID_ID3D10Device1):guid==IID_ID3D11Device))pointer=id->device;
            else if(guid==IID_IDXGIDevice || guid==IID_IDXGIDevice1 || guid==IID_IDXGIDevice2 || guid==IID_IDXGIObject)pointer=interface(object(id->device).identity,Kind::DxgiDevice);
            if(!pointer)result=no_interface;else {hold(pointer);write_pointer(arg(2),pointer);}
        }else if(symbol=="CreateSwapChain" || symbol=="CreateSwapChainForHwnd"){
            graphics_handled=true;const bool modern=symbol=="CreateSwapChainForHwnd";
            // D3D12 以命令佇列作為交換鏈的裝置；由 D3D12 模組建立翻轉模型交換鏈。
            if(d3d12_queue(arg(1)))return invoke_d3d12(state,memory,WindowsServiceDescriptor{"internal-d3d12",modern?"DXGI.CreateSwapChainForHwnd":"DXGI.CreateSwapChain",modern?7U:4U});
            auto factory=typed(arg(0),Kind::Factory),device=object(arg(1)).identity;require(device->kind==Kind::Device);const auto out=modern?6U:3U;output(out);write_pointer(arg(out),0);auto swap=make_swap(device,factory->root,arg(modern?3:2),modern?arg(2):0,modern,modern?arg(4):0,modern?arg(5):0);write_pointer(arg(out),swap->root);
        }else if(symbol=="MakeWindowAssociation" || symbol=="GetWindowAssociation"){
            graphics_handled=true;auto factory=typed(arg(0),Kind::Factory);if(symbol=="MakeWindowAssociation"){require(!arg(1) || gui_.windows.contains(arg(1)) || shared_gui_window(arg(1)).has_value());require(!(u32(2)&~7U));factory->window=arg(1);}else write_pointer(arg(1),factory->window);
        }else if(symbol=="GetBuffer"){
            graphics_handled=true;auto swap=typed(arg(0),Kind::SwapChain);output(3);write_pointer(arg(3),0);const auto guid=read_guid(memory,arg(2));if(u32(1)>=swap->bindings.at("Backbuffers").size())result=invalid;else if(guid!=IID_IUnknown && (object(swap->device).identity->api10?(guid!=d3d10_guid::IID_ID3D10Texture2D && guid!=d3d10_guid::IID_ID3D10Resource):(guid!=IID_ID3D11Texture2D && guid!=IID_ID3D11Resource)))result=no_interface;else {const auto ptr=swap->bindings.at("Backbuffers")[u32(1)];hold(ptr);write_pointer(arg(3),ptr);}
        }else if(symbol=="Present" || symbol=="Present1"){
#include "d3d11_graphics_present.inc"
        }else if(symbol=="SwapGetDesc"){graphics_handled=true;memory.write(GuestAddress{arg(1)},typed(arg(0),Kind::SwapChain)->desc);}
        else if(symbol=="SwapGetDesc1"){
            graphics_handled=true;auto swap=typed(arg(0),Kind::SwapChain);std::vector<std::uint8_t> desc(48);put_word(desc,0,word(swap->desc,0));put_word(desc,4,word(swap->desc,4));put_word(desc,8,word(swap->desc,16));put_word(desc,16,word(swap->desc,28));put_word(desc,20,word(swap->desc,32));put_word(desc,24,word(swap->desc,36));put_word(desc,28,word(swap->desc,40));put_word(desc,36,word(swap->desc,x86_?52:60));put_word(desc,40,3);put_word(desc,44,word(swap->desc,x86_?56:64));memory.write(GuestAddress{arg(1)},desc);
        }else if(symbol=="ResizeBuffers"){
            graphics_handled=true;auto swap=typed(arg(0),Kind::SwapChain);for(auto ptr:swap->bindings.at("Backbuffers"))require(object(ptr).identity->references==1);const auto remote=gui_.windows.contains(swap->window)?std::nullopt:shared_gui_window(swap->window);require(gui_.windows.contains(swap->window) || remote.has_value());const auto count=u32(1)?u32(1):word(swap->desc,40),width=u32(2)?u32(2):(remote?remote->width:gui_.windows.at(swap->window).width),height=u32(3)?u32(3):(remote?remote->height:gui_.windows.at(swap->window).height),format=u32(4)?u32(4):word(swap->desc,16);swap_buffers(swap,width,height,format,count,word(swap->desc,36),word(swap->desc,28));put_word(swap->desc,0,width);put_word(swap->desc,4,height);put_word(swap->desc,16,format);put_word(swap->desc,40,count);put_word(swap->desc,x86_?56:64,u32(5));
        }else if(symbol=="GetHwnd"){graphics_handled=true;write_pointer(arg(1),typed(arg(0),Kind::SwapChain)->window);}
        else if(symbol=="GetLastPresentCount"){graphics_handled=true;memory.store(GuestAddress{arg(1)},32,typed(arg(0),Kind::SwapChain)->frames);}
        else if(symbol=="GetFrameStatistics"){
            // 依目前顯示模式的更新率，把最後一次 Present 對齊到其後的第一個 vblank；尚未呈現時統計不連續。
            graphics_handled=true;auto swap=typed(arg(0),Kind::SwapChain);
            if(!arg(1))result=invalid;
            else if(!swap->frames)result=0x887a000bU;
            else{
                std::uint64_t numerator=60,denominator=1;
                if(native_gui_)if(const auto mode=native_gui_->current_display_mode();mode && mode->refresh_numerator && mode->refresh_denominator){numerator=mode->refresh_numerator;denominator=mode->refresh_denominator;}
                const auto period=static_cast<unsigned __int128>(denominator)*10000000U;
                const auto refresh=static_cast<std::uint64_t>((static_cast<unsigned __int128>(swap->present_ticks)*numerator+period-1)/period);
                const auto sync=static_cast<std::uint64_t>(static_cast<unsigned __int128>(refresh)*period/numerator);
                std::vector<std::uint8_t> stats(32);put_word(stats,0,static_cast<std::uint32_t>(swap->frames));put_word(stats,4,static_cast<std::uint32_t>(refresh));put_word(stats,8,static_cast<std::uint32_t>(refresh));
                for(unsigned i=0;i<8;++i)stats[16+i]=static_cast<std::uint8_t>(sync>>(8*i));
                memory.write(GuestAddress{arg(1)},stats);
            }
        }else if(symbol=="GetCoreWindow"){
            // HWND 交換鏈沒有 CoreWindow。
            graphics_handled=true;(void)typed(arg(0),Kind::SwapChain);output(2);write_pointer(arg(2),0);result=0x887a0001U;
        }else if(symbol=="IsTemporaryMonoSupported"){graphics_handled=true;(void)typed(arg(0),Kind::SwapChain);result=0;}
        else if(symbol=="SetBackgroundColor" || symbol=="GetBackgroundColor"){
            graphics_handled=true;auto swap=typed(arg(0),Kind::SwapChain);
            if(!arg(1))result=invalid;
            else if(symbol=="SetBackgroundColor")memory.read(GuestAddress{arg(1)},swap->background_color);
            else memory.write(GuestAddress{arg(1)},swap->background_color);
        }else if(symbol=="SetRotation"){
            // 呈現路徑不旋轉輸出；只接受 IDENTITY，其他方向依不支援的交換鏈回 DXGI_ERROR_INVALID_CALL。
            graphics_handled=true;auto swap=typed(arg(0),Kind::SwapChain);
            if(!u32(1) || u32(1)>4)result=invalid;else if(u32(1)!=1)result=0x887a0001U;else swap->rotation=1;
        }else if(symbol=="GetRotation"){
            graphics_handled=true;auto swap=typed(arg(0),Kind::SwapChain);if(!arg(1))result=invalid;else memory.store(GuestAddress{arg(1)},32,swap->rotation);
        }
        else if(symbol=="SetGPUThreadPriority" || symbol=="GetGPUThreadPriority" || symbol=="SetMaximumFrameLatency" || symbol=="GetMaximumFrameLatency"){
            graphics_handled=true;auto device=typed(arg(0),Kind::DxgiDevice);if(symbol=="SetGPUThreadPriority"){require(static_cast<std::int32_t>(u32(1))>=-7 && static_cast<std::int32_t>(u32(1))<=7);device->priority=u32(1);}else if(symbol=="GetGPUThreadPriority")memory.store(GuestAddress{arg(1)},32,device->priority);else if(symbol=="SetMaximumFrameLatency"){require(u32(1)<=16);device->slice=u32(1)?u32(1):3;}else memory.store(GuestAddress{arg(1)},32,device->slice?device->slice:3);
        }

        else if(symbol.starts_with("D3DX11CompileFrom") || symbol=="D3DCompile" || symbol=="D3DCompile2" || symbol=="D3DDisassemble" || symbol=="D3DCreateBlob" || symbol=="BlobPointer" || symbol=="BlobSize" || symbol=="D3DGetInputSignatureBlob" || symbol=="D3DGetOutputSignatureBlob" || symbol=="D3DGetInputAndOutputSignatureBlob"){
            graphics_handled=true;
            const auto blob=[&](std::span<const std::uint8_t> data){auto id=create(Kind::Blob);try{const auto ptr=allocate_heap(memory,std::max<std::size_t>(1,data.size()));if(!ptr)throw std::bad_alloc{};id->allocations.push_back(ptr);id->mapped=ptr;id->sequence=data.size();memory.write(GuestAddress{ptr},data);}catch(...){release(id->root);throw;}return id->root;};
            if(symbol=="BlobPointer")result=typed(arg(0),Kind::Blob)->mapped;
            else if(symbol=="BlobSize")result=typed(arg(0),Kind::Blob)->sequence;
            else if(symbol=="D3DGetInputSignatureBlob" || symbol=="D3DGetOutputSignatureBlob" || symbol=="D3DGetInputAndOutputSignatureBlob"){
                output(2);write_pointer(arg(2),0);require(arg(1)>0 && arg(1)<=16U*1024U*1024U);
                const auto mask=symbol=="D3DGetInputSignatureBlob"?1U:symbol=="D3DGetOutputSignatureBlob"?2U:3U;
                const auto data=graphics_signature_blob(read_bytes(memory,arg(0),static_cast<std::size_t>(arg(1))),mask);write_pointer(arg(2),blob(data));
            }
            else if(symbol=="D3DDisassemble"){
                output(4);write_pointer(arg(4),0);require(arg(0) && arg(1)>=4 && arg(1)<=16U*1024U*1024U);
                auto text=graphics_disassemble_shader(read_bytes(memory,arg(0),static_cast<std::size_t>(arg(1))),u32(2),arg(3)?read_ansi(memory,arg(3),65536):std::string{});
                text.push_back(0);write_pointer(arg(4),blob({reinterpret_cast<const std::uint8_t*>(text.data()),text.size()}));
            }
            else if(symbol=="D3DCreateBlob"){require(arg(0)<=64*1024*1024);output(1);std::vector<std::uint8_t> data(static_cast<std::size_t>(arg(0)));write_pointer(arg(1),blob(data));}
            else {
                const bool d3dx=symbol.starts_with("D3DX11CompileFrom"),file=symbol.starts_with("D3DX11CompileFromFile"),resource=symbol.starts_with("D3DX11CompileFromResource");
                const unsigned macros_arg=file?1:3,include_arg=file?2:4,entry_arg=file?3:5,profile_arg=file?4:6,flags_arg=file?5:7;
                const unsigned code=d3dx?(file?8:10):(symbol=="D3DCompile2"?12:9),errors=code+1;
                output(code);write_pointer(arg(code),0);if(arg(errors)){output(errors);write_pointer(arg(errors),0);}
                // 同步編譯共用 HLSL 後端；自訂 include 經原執行緒的真正客體 callback。
                if(d3dx)require(!arg(file?7:9) && !arg(code+2));
                else require(!(u32(8)&~3U) && (!u32(8) || read_ansi(memory,arg(profile_arg)).starts_with("fx_")));
                if(symbol=="D3DCompile2")require(!u32(9) && !arg(10) && !arg(11));
                std::string source_name="shader";
                std::vector<std::uint8_t> source;
                if(file){
                    source_name=symbol.ends_with('W')?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)})):read_ansi(memory,arg(0));
                    const auto opened=filesystem_.open_reader(source_name);
                    if(opened.error)throw texture_image::Error(0x80070000U|opened.error,"無法讀取 shader 檔案");
                    const auto info=opened.file->information();
                    if(info.error)throw texture_image::Error(0x80070000U|info.error,"無法取得 shader 檔案大小");
                    require(info.metadata.size>0 && info.metadata.size<=16*1024*1024);
                    source.resize(static_cast<std::size_t>(info.metadata.size));const auto io=opened.file->read(source,0);
                    if(io.error || io.bytes!=source.size())throw texture_image::Error(0x80070000U|(io.error?io.error:38U),"shader 檔案讀取未完成");
                }else if(resource){
                    const auto found=gui_resource(memory,arg(0),arg(1),10,symbol.ends_with('W'));
                    if(!found)throw texture_image::Error(0x80070716U,"找不到 shader RCDATA 資源");
                    const auto size=memory.load(GuestAddress{found+4},32);require(size>0 && size<=16*1024*1024);
                    source=read_bytes(memory,resource_image(found,true)->base+memory.load(GuestAddress{found},32),size);
                    if(arg(2))source_name=symbol.ends_with('W')?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(2)})):read_ansi(memory,arg(2));
                }else{
                    require(arg(1)>0 && arg(1)<=16*1024*1024);source=read_bytes(memory,arg(0),static_cast<std::size_t>(arg(1)));
                    if(arg(2))source_name=read_ansi(memory,arg(2));
                }
                std::vector<std::pair<std::string,std::string>> macros;
                if(arg(macros_arg))for(unsigned i=0;i<256;++i){const auto at=arg(macros_arg)+i*2*pointer_size,name=memory.load(GuestAddress{at},pointer_bits);if(!name)break;const auto value=memory.load(GuestAddress{at+pointer_size},pointer_bits);macros.emplace_back(read_ansi(memory,name),value?read_ansi(memory,value):"");require(i<255);}
                D3DCompileOperation* operation=nullptr;
                if(arg(include_arg)>1){
                    for(auto& [key,job]:bridge.compilations){(void)key;if(job.thread==threads_.current && job.stack==state.reg(Gpr::Rsp) && job.service.symbol==service.symbol){operation=&job;break;}}
                    if(!operation){
                        require(bridge.compilations.size()<128);
                        D3DCompileOperation job;job.id=++bridge.next_compilation;job.thread=threads_.current;job.stack=state.reg(Gpr::Rsp);
                        job.handler=arg(include_arg);job.service=service;job.source=source;
                        operation=&bridge.compilations.emplace(job.id,std::move(job)).first->second;
                    }else source=operation->source;
                }
                HlslIncludeLoader include_loader;
                std::size_t include_cursor=0;
                if(operation)include_loader=[&](std::string_view filename,bool local,std::string_view parent)->std::optional<HlslIncludeSource>{
                    if(operation->pending)return std::nullopt;
                    if(include_cursor<operation->includes.size()){
                        const auto& entry=operation->includes[include_cursor++];
                        require(entry.filename==filename && entry.local==local && entry.parent==parent);
                        if(entry.success)return HlslIncludeSource{entry.name,entry.bytes};return std::nullopt;
                    }
                    if(operation->includes.size()>=256)return std::nullopt;
                    D3DCompileInclude entry;entry.filename=filename;entry.local=local;entry.parent=parent;
                    entry.name="include#"+std::to_string(include_cursor)+":"+std::string(filename);
                    operation->pending=std::move(entry);return std::nullopt;
                };

                // D3DX 的檔案入口提供預設 include；D3DCompile 記憶體入口須明確傳入 STANDARD_FILE_INCLUDE。
                if(!operation && (file || arg(include_arg)==1))include_loader=[&](std::string_view filename,bool local,std::string_view parent)->std::optional<HlslIncludeSource>{
                    const auto directory=[](std::string_view path){const auto at=path.find_last_of("/\\");return at==path.npos?std::string{}:std::string(path.substr(0,at+1));};
                    const bool absolute=filename.starts_with('/') || filename.starts_with('\\') || (filename.size()>1 && filename[1]==':');
                    std::vector<std::string> paths;if(absolute)paths.emplace_back(filename);else{
                        if(local)paths.push_back(directory(parent)+std::string(filename));
                        paths.push_back(directory(source_name)+std::string(filename));paths.emplace_back(filename);
                    }
                    std::set<std::string> seen;
                    for(const auto& path:paths){
                        if(!seen.insert(path).second)continue;const auto opened=filesystem_.open_reader(path);if(opened.error)continue;
                        const auto info=opened.file->information();if(info.error || info.metadata.size>16U*1024U*1024U)continue;
                        HlslIncludeSource included{path,std::vector<std::uint8_t>(static_cast<std::size_t>(info.metadata.size))};const auto io=opened.file->read(included.bytes,0);
                        if(!io.error && io.bytes==included.bytes.size())return included;
                    }
                    return std::nullopt;
                };
                auto compilation=operation && operation->result?*operation->result:graphics_compile_hlsl_includes(source,source_name,arg(entry_arg)?read_ansi(memory,arg(entry_arg)):"main",read_ansi(memory,arg(profile_arg)),macros,u32(flags_arg),include_loader,d3dx?u32(file?6:8):u32(8));
                if(operation){
                    if(operation->pending){
                        const auto& entry=*operation->pending;
                        operation->scratch=allocate_heap(memory,pointer_size+4+entry.filename.size()+1);if(!operation->scratch)throw std::bad_alloc{};
                        memory.fill(GuestAddress{operation->scratch},0,pointer_size+4+entry.filename.size()+1);
                        const auto filename=operation->scratch+pointer_size+4;
                        memory.write(GuestAddress{filename},std::span(reinterpret_cast<const std::uint8_t*>(entry.filename.data()),entry.filename.size()));
                        auto parent=operation->root_data;
                        for(const auto& opened:operation->includes)if(opened.name==entry.parent){parent=opened.data;break;}
                        const auto table=memory.load(GuestAddress{operation->handler},pointer_bits);
                        const auto callback=memory.load(GuestAddress{table},pointer_bits);
                        start_callbacks(state,memory,{{callback,{operation->handler,entry.local?0U:1U,filename,parent,operation->scratch,operation->scratch+pointer_size}}},Continuation::D3DCompileOperation,operation->id);
                        return std::nullopt;
                    }
                    operation->result=compilation;
                    if(!operation->closed){
                        std::vector<GuestCallback> callbacks;
                        const auto table=memory.load(GuestAddress{operation->handler},pointer_bits);
                        const auto callback=memory.load(GuestAddress{table+pointer_size},pointer_bits);
                        for(auto it=operation->includes.rbegin();it!=operation->includes.rend();++it)if(it->success)callbacks.push_back({callback,{operation->handler,it->data}});
                        operation->closing=true;
                        start_callbacks(state,memory,std::move(callbacks),Continuation::D3DCompileOperation,operation->id);
                        return std::nullopt;
                    }
                    bridge.compilations.erase(operation->id);operation=nullptr;
                }
                if(compilation.bytecode.empty() && std::getenv("X64ARM_TRACE_GRAPHICS_ERRORS"))std::fprintf(stderr,"[hlsl] %s: %s\n",source_name.c_str(),compilation.messages.c_str());
                if(!compilation.messages.empty() && arg(errors)){std::vector<std::uint8_t> message(compilation.messages.begin(),compilation.messages.end());message.push_back(0);write_pointer(arg(errors),blob(message));}if(compilation.bytecode.empty())result=failure;else write_pointer(arg(code),blob(compilation.bytecode));
            }
        }
