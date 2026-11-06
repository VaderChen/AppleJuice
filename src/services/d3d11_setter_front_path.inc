// D3D11 狀態設定的前置快路徑與共用綁定契約（X64ARM_D3D11_SETTER_FRONT_PATH=0 關閉）。
// 完整分派函式的堆疊框架很大，每次呼叫都要逐頁探測與初始化；UE 系每幀數萬次呼叫以 IA／*Set* 為大宗。這裡在進入完整函式前
// 完成不需要錯誤回報與銷毀的情況，資格與狀態更新都和完整路徑共用下列小函式。其餘情況（參數或類型不符、物件找不到、可能重疊的
// 輸出、最後一次釋放、ClassInstance、裝置鎖、D3D10）一律回傳 false 交回完整路徑，交回前不改任何可觀察狀態（只可能多出全零的
// 名稱節點或槽位向量，名稱查詢、draw_state 與 remove_conflicts 對它們的行為都與節點不存在相同，完整路徑隨後也會建立）。

// 綁定值的共用資格（bind 與 read_views）：同一裝置、資源未 offer（view 連同所屬貼圖）。texture_of 由呼叫端決定找不到貼圖時的處理。
template<class TextureOf>
bool d3d11_bindable(const Identity& context,const Identity& id,const TextureOf& texture_of){
    return id.device==context.device && !id.offered && (!id.texture || !texture_of(id.texture).offered);
}
// 共用綁定提交：先持有全部新值，再逐槽覆寫並釋放舊值，順序不可調換（新舊相同或互換時不能先歸零）。hold／release 由呼叫端決定：
// 完整路徑含 COM 持有、最後一次釋放的銷毀與延後銷毀；前置路徑已先確認不會是最後一次釋放，只改內部計數。
template<class Hold,class Release>
void d3d11_commit_bindings(std::vector<std::uint64_t>& slots,std::uint32_t start,std::span<const std::uint64_t> values,std::size_t limit,bool weak,const Hold& hold,const Release& release){
    if(!weak)for(auto value:values)if(value)hold(value);
    // 向量只長到實際用到的槽（讀取端都有界限檢查）：SRV 上限 128 槽，draw_state 每次繪製都要掃尾端的空槽。
    (void)limit;if(const auto needed=start+values.size();slots.size()<needed)slots.resize(needed);
    for(std::size_t i=0;i<values.size();++i){const auto old=slots[start+i];slots[start+i]=values[i];if(old && !weak)release(old);}
}
const GraphicsResource* d3d11_view_resource(const Identity& id) noexcept {
    return id.graphics_view.resource?id.graphics_view.resource.get():id.resource.get();
}
// 同一資源的兩個 view 是否不共用 subresource；Buffer 在 D3D11 中只有一個 subresource。
bool d3d11_views_disjoint(const GraphicsView& a,const GraphicsView& b) noexcept {
    const auto dimension=a.resource->desc.dimension;if(dimension==1)return false;
    if(a.level>=b.level+b.levels || b.level>=a.level+a.levels)return true;
    return dimension!=4 && (a.slice>=b.slice+b.slices || b.slice>=a.slice+a.slices);
}
// stage 順序與 GraphicsDraw::stages 相同（VS、PS、GS、HS、DS、CS）；SO 轉送物件由後端刻意保留 stage 0，
// 客體的六種 shader 綁定須核對 COM 類型，不以後端執行方式代替。
constexpr std::array d3d11_shader_kinds{Kind::VertexShader,Kind::PixelShader,Kind::GeometryShader,Kind::HullShader,Kind::DomainShader,Kind::ComputeShader};
bool d3d11_shader_bindable(const Identity& context,const Identity& shader,Kind kind,unsigned stage,std::uint32_t instances) noexcept {
    return bool(shader.shader) && kind==d3d11_shader_kinds[stage] && shader.device==context.device && (!shader.reflection || shader.reflection->interface_slots==instances);
}
bool d3d11_index_format_allowed(std::uint64_t buffer,std::uint32_t format) noexcept {return !buffer || format==42 || format==57;}
bool d3d11_index_buffer_bindable(const Identity& context,const Identity& buffer) noexcept {return buffer.device==context.device && (buffer.resource->desc.bind&2);}
// *SetConstantBuffers 取消這些槽位的 D3D11.1 常數範圍（數量 0 表示整個緩衝）。
void d3d11_reset_constant_ranges(Identity& context,unsigned stage,std::uint32_t start,std::uint32_t count) noexcept {
    for(std::uint32_t i=0;i<count;++i){context.constant_first[stage][start+i]=0;context.constant_count[stage][start+i]=0;}
}

// 固定名稱的 bindings 節點：其他程式碼繼續依名稱存取同一向量。
enum : unsigned {d3d11_slot_vertices,d3d11_slot_index,d3d11_slot_layout,d3d11_slot_targets,d3d11_slot_depth,d3d11_slot_ps_uav,d3d11_slot_cs_uav,d3d11_slot_stream,d3d11_slot_stages};
enum : unsigned {d3d11_stage_shader,d3d11_stage_instances,d3d11_stage_constants,d3d11_stage_resources,d3d11_stage_samplers,d3d11_stage_slots};
constexpr unsigned d3d11_stage_slot(unsigned stage,unsigned which) noexcept {return d3d11_slot_stages+stage*d3d11_stage_slots+which;}
enum : unsigned {d3d11_slot_blend=d3d11_stage_slot(6,0),d3d11_slot_depth_state,d3d11_slot_raster,d3d11_slot_visibility,d3d11_slot_predicate,d3d11_slot_count};
static_assert(std::tuple_size_v<D3D11FixedBindings> ==d3d11_slot_count);
const std::string& d3d11_fixed_slot_name(unsigned key){
    static const auto names=[]{
        std::array<std::string,std::tuple_size_v<D3D11FixedBindings>> value;
        value[d3d11_slot_vertices]="Vertices";value[d3d11_slot_index]="Index";value[d3d11_slot_layout]="Layout";value[d3d11_slot_targets]="Targets";value[d3d11_slot_depth]="Depth";
        value[d3d11_slot_ps_uav]="PSUnorderedAccessViews";value[d3d11_slot_cs_uav]="CSUnorderedAccessViews";value[d3d11_slot_stream]="StreamOutput";
        constexpr std::array<std::string_view,6> stages{"VS","PS","GS","HS","DS","CS"};
        constexpr std::array<std::string_view,d3d11_stage_slots> suffixes{"Shader","ClassInstances","ConstantBuffers","ShaderResources","Samplers"};
        for(unsigned stage=0;stage<stages.size();++stage)for(unsigned which=0;which<suffixes.size();++which)value[d3d11_stage_slot(stage,which)]=std::string(stages[stage])+std::string(suffixes[which]);
        value[d3d11_slot_blend]="Blend";value[d3d11_slot_depth_state]="DepthState";value[d3d11_slot_raster]="Raster";value[d3d11_slot_visibility]="Visibility";value[d3d11_slot_predicate]="Predicate";
        return value;
    }();
    return names[key];
}
// 只在已確認非弱綁定的 D3D11 裝置內容上使用（快取失效見 Identity::fixed_bindings）。
std::vector<std::uint64_t>& d3d11_fixed_slots(Identity& context,unsigned key){
    if(!context.fixed_bindings)context.fixed_bindings=std::make_unique<D3D11FixedBindings>();
    auto*& slot=(*context.fixed_bindings)[key];
    if(!slot)slot=&context.bindings[d3d11_fixed_slot_name(key)];
    return *slot;
}

enum class D3D11Setter:std::uint8_t {None,VertexBuffers,IndexBuffer,InputLayout,ConstantBuffers,ShaderResources,Samplers,Shader};
struct D3D11SetterMethod {D3D11Setter kind{D3D11Setter::None};std::uint8_t stage{};};
// 形如 IASet*、[VPGHDC]SSet*；其他方法在前幾個字元就排除，不增加 AddRef／Release 等常見方法的成本。
// *SetConstantBuffers1、CSSetUnorderedAccessViews 與 Get* 不在此列，走完整路徑。
D3D11SetterMethod d3d11_setter_method(std::string_view symbol) noexcept {
    if(symbol.size()<11 || symbol.substr(2,3)!="Set")return {};
    const auto what=symbol.substr(5);
    if(symbol[0]=='I' && symbol[1]=='A'){
        if(what=="VertexBuffers")return {D3D11Setter::VertexBuffers,0};
        if(what=="IndexBuffer")return {D3D11Setter::IndexBuffer,0};
        if(what=="InputLayout")return {D3D11Setter::InputLayout,0};
        return {};
    }
    if(symbol[1]!='S')return {};
    std::uint8_t stage=0;
    switch(symbol[0]){case 'V':stage=0;break;case 'P':stage=1;break;case 'G':stage=2;break;case 'H':stage=3;break;case 'D':stage=4;break;case 'C':stage=5;break;default:return {};}
    if(what=="ShaderResources")return {D3D11Setter::ShaderResources,stage};
    if(what=="ConstantBuffers")return {D3D11Setter::ConstantBuffers,stage};
    if(what=="Samplers")return {D3D11Setter::Samplers,stage};
    if(what=="Shader")return {D3D11Setter::Shader,stage};
    return {};
}
// 依 bind 核對一個待綁定的值。多留一次呼叫可能持有的餘裕，內部計數溢位交回完整路徑回報。
const Object* d3d11_front_value(const D3D11State& bridge,const Identity& context,std::uint64_t value) noexcept {
    const auto found=bridge.objects.find(value);
    if(found==bridge.objects.end() || !found->second.identity)return nullptr;
    const auto& id=*found->second.identity;
    const Identity* texture=nullptr;
    if(id.texture){const auto owner=bridge.objects.find(id.texture);if(owner==bridge.objects.end() || !owner->second.identity)return nullptr;texture=owner->second.identity.get();}
    if(id.api10 || id.internal_references>UINT32_MAX-128U || !d3d11_bindable(context,id,[&](std::uint64_t)->const Identity& {return *texture;}))return nullptr;
    return &found->second;
}
// outputs_conflict 的前置判定：只放行確定不重疊的值。資源都已知且不同、或都是 view 且 subresource 不相交時不重疊；
// 可能重疊（含資源缺失與唯讀深度的例外）交回完整路徑判定並依 D3D11 語義置 null。
bool d3d11_front_no_output_conflict(D3D11State& bridge,Identity& context,std::span<const std::uint64_t> values){
    if(std::all_of(values.begin(),values.end(),[](auto value){return !value;}))return true;
    std::array<const Identity*,29> outputs; // Targets 8、Depth 1、PS UAV 8、CS UAV 8、StreamOutput 4。
    std::size_t count=0;
    for(const auto key:{d3d11_slot_targets,d3d11_slot_depth,d3d11_slot_ps_uav,d3d11_slot_cs_uav,d3d11_slot_stream})
        for(const auto pointer:d3d11_fixed_slots(context,key))if(pointer){
            const auto found=bridge.objects.find(pointer);
            if(found==bridge.objects.end() || !found->second.identity || count==outputs.size())return false;
            outputs[count++]=found->second.identity.get();
        }
    if(!count)return true;
    for(const auto value:values)if(value){
        const auto& id=*bridge.objects.find(value)->second.identity; // 呼叫端已核對存在。
        const auto* resource=d3d11_view_resource(id);if(!resource)return false;
        for(std::size_t i=0;i<count;++i){
            const auto& output=*outputs[i];const auto* other=d3d11_view_resource(output);if(!other)return false;
            if(resource!=other)continue;
            if(!id.graphics_view.resource || !output.graphics_view.resource || !d3d11_views_disjoint(id.graphics_view,output.graphics_view))return false;
        }
    }
    return true;
}
// 綁定解除後物件是否仍存活：內部計數足以撐過這次呼叫的所有釋放，或客體仍持有（計數不會在持圖形鎖時歸零：客體常式只做
// 非最後一次 Release，最後一次要經服務取鎖）。可能是最後一次釋放的銷毀與延後銷毀留給完整路徑。
bool d3d11_front_release_keeps_alive(const D3D11State& bridge,GuestMemory& memory,std::uint64_t pointer,std::size_t releases){
    const auto found=bridge.objects.find(pointer);
    if(found==bridge.objects.end() || !found->second.identity)return false;
    const auto& id=*found->second.identity;
    return id.internal_references && (id.internal_references>releases || reference_count(memory,id));
}
bool d3d11_front_bind(D3D11State& bridge,GuestMemory& memory,std::vector<std::uint64_t>& slots,std::uint32_t start,std::span<const std::uint64_t> values,std::size_t limit){
    bool changed=false;
    for(std::size_t i=0;i<values.size();++i){
        const auto index=start+i;const auto old=index<slots.size()?slots[index]:0;if(old==values[i])continue;
        changed=true;if(old && !d3d11_front_release_keeps_alive(bridge,memory,old,values.size()))return false;
    }
    if(!changed)return true; // 綁定未變：完整路徑的持有與釋放淨變化為零，draw_state 也會因原始指標相同而略過。
    const auto references=[&](std::uint64_t pointer)->std::uint32_t& {return bridge.objects.find(pointer)->second.identity->internal_references;};
    d3d11_commit_bindings(slots,start,values,limit,false,[&](std::uint64_t value){++references(value);},[&](std::uint64_t old){--references(old);});
    return true;
}
// 回傳 true 表示已完成並可直接回到客體（void 方法與完整路徑相同以 0 返回）；false 時未改可觀察狀態。
// 呼叫端持圖形鎖，且已比照前置路徑核對 compare 模式與延後銷毀清單。
[[gnu::noinline]] bool d3d11_setter_front_path(D3D11State& bridge,GuestMemory& memory,const WindowsCall& arg,const D3D11SetterMethod method,const bool x86) try {
    const auto self=bridge.objects.find(arg(0));
    if(self==bridge.objects.end() || self->second.kind!=Kind::Context || !self->second.identity)return false;
    auto& context=*self->second.identity;
    if(context.kind!=Kind::Context || context.api10 || context.weak_bindings)return false;
    const auto device=context.device?bridge.objects.find(context.device):self;
    if(device==bridge.objects.end() || !device->second.identity ||
       (device->second.identity->protected_threads && device->second.identity->lock_address))return false;
    const auto u32=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    const unsigned pointer_size=x86?4U:8U;const std::uint8_t pointer_bits=x86?32:64;
    switch(method.kind){
    case D3D11Setter::VertexBuffers:{
        const auto start=u32(1),count=u32(2);if(start>32 || count>32-start)return false;
        const auto pointers=arg(3),stride_array=arg(4),offset_array=arg(5);
        std::array<std::uint64_t,32> values;std::array<std::uint32_t,32> strides,offsets;
        for(unsigned i=0;i<count;++i){
            values[i]=pointers?memory.load(GuestAddress{pointers+i*pointer_size},pointer_bits):0;
            strides[i]=stride_array?static_cast<std::uint32_t>(memory.load(GuestAddress{stride_array+4*i},32)):0;
            offsets[i]=offset_array?static_cast<std::uint32_t>(memory.load(GuestAddress{offset_array+4*i},32)):0;
            if(values[i] && !d3d11_front_value(bridge,context,values[i]))return false;
        }
        const std::span<const std::uint64_t> view(values.data(),count);
        if(!d3d11_front_no_output_conflict(bridge,context,view) ||
           !d3d11_front_bind(bridge,memory,d3d11_fixed_slots(context,d3d11_slot_vertices),start,view,32))return false;
        for(unsigned i=0;i<count;++i){context.strides[start+i]=strides[i];context.offsets[start+i]=offsets[i];}
        return true;
    }
    case D3D11Setter::IndexBuffer:{
        const auto value=arg(1);const auto format=u32(2),offset=u32(3);
        if(!d3d11_index_format_allowed(value,format))return false;
        if(value){
            const auto* bound=d3d11_front_value(bridge,context,value);
            if(!bound || bound->kind!=Kind::BufferObject || !bound->identity->resource || !d3d11_index_buffer_bindable(context,*bound->identity))return false;
        }
        const std::array values{value};
        if(!d3d11_front_bind(bridge,memory,d3d11_fixed_slots(context,d3d11_slot_index),0,values,1))return false;
        context.index_format=format;context.index_offset=offset;
        return true;
    }
    case D3D11Setter::InputLayout:{
        const auto value=arg(1);if(value && !d3d11_front_value(bridge,context,value))return false;
        const std::array values{value};
        return d3d11_front_bind(bridge,memory,d3d11_fixed_slots(context,d3d11_slot_layout),0,values,1);
    }
    case D3D11Setter::Shader:{
        if(u32(3))return false; // ClassInstance 要核對 linkage，走完整路徑。
        const auto value=arg(1);
        if(value){
            const auto* bound=d3d11_front_value(bridge,context,value);
            if(!bound || !d3d11_shader_bindable(context,*bound->identity,bound->kind,method.stage,0))return false;
        }
        auto& instances=d3d11_fixed_slots(context,d3d11_stage_slot(method.stage,d3d11_stage_instances));
        if(std::any_of(instances.begin(),instances.end(),[](auto pointer){return pointer!=0;}))return false; // 要釋放舊 instance，走完整路徑。
        if(instances.size()!=256)instances.resize(256);
        const std::array values{value};
        return d3d11_front_bind(bridge,memory,d3d11_fixed_slots(context,d3d11_stage_slot(method.stage,d3d11_stage_shader)),0,values,1);
    }
    case D3D11Setter::ConstantBuffers:case D3D11Setter::ShaderResources:case D3D11Setter::Samplers:{
        const bool constants=method.kind==D3D11Setter::ConstantBuffers,resources=method.kind==D3D11Setter::ShaderResources;
        const std::uint32_t limit=constants?14U:resources?128U:16U;
        const auto kind=constants?Kind::BufferObject:resources?Kind::ShaderResource:Kind::Sampler;
        const auto start=u32(1),count=u32(2);if(start>limit || count>limit-start)return false;
        const auto address=arg(3);
        std::array<std::uint64_t,128> values;
        for(unsigned i=0;i<count;++i){
            values[i]=address?memory.load(GuestAddress{address+i*pointer_size},pointer_bits):0;
            if(values[i]){const auto* bound=d3d11_front_value(bridge,context,values[i]);if(!bound || bound->kind!=kind)return false;}
        }
        const std::span<const std::uint64_t> view(values.data(),count);
        if(resources && !d3d11_front_no_output_conflict(bridge,context,view))return false;
        const auto which=constants?d3d11_stage_constants:resources?d3d11_stage_resources:d3d11_stage_samplers;
        if(!d3d11_front_bind(bridge,memory,d3d11_fixed_slots(context,d3d11_stage_slot(method.stage,which)),start,view,limit))return false;
        if(constants)d3d11_reset_constant_ranges(context,method.stage,start,count);
        return true;
    }
    case D3D11Setter::None:break;
    }
    return false;
}catch(const MemoryFault&){return false;}catch(const std::bad_alloc&){return false;}
