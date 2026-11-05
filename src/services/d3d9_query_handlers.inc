// D3D9 查詢物件沿用裝置的 GPU 命令順序、完成旗標與硬體時間戳。
const unsigned size=object.query_type==10 || object.query_type==12?8U:4U;
if(symbol=="GetDevice"){output(1);hold(get(object.parent,Kind::Device));store_pointer(arg(1),object.parent);}
else if(symbol=="GetType")result=object.query_type;
else if(symbol=="GetDataSize")result=size;
else if(symbol=="Issue"){
    auto device=get(object.parent,Kind::Device);const auto flags=u(1);
    require(flags==1 || flags==2);require(flags!=2 || object.query_type==9 || object.query_type==11);
    if(flags==2){
        object.query_active=true;object.query_issued=false;object.query_slots.clear();object.query_fence.reset();object.query_timestamp.reset();
        if(object.query_type==9){
            std::erase_if(device->active_queries,[&](const auto& weak){const auto query=weak.lock();return !query || query==value || !query->query_active;});
            device->active_queries.push_back(value);
        }else object.query_begin=device->native->graphics_timestamp_future();
    }else{
        require((object.query_type!=9 && object.query_type!=11) || object.query_active);
        object.query_active=false;object.query_issued=true;
        if(object.query_type>=10)object.query_timestamp=device->native->graphics_timestamp_future();
        object.query_fence=device->native->graphics_fence_pending();
    }
}else if(symbol=="GetData"){
    require(!(u(3)&~1U) && (arg(1)?u(2)==size:u(2)==0));
    if(arg(1))memory.check(GuestAddress{arg(1)},size,Access::Write);
    auto device=get(object.parent,Kind::Device);
    if(object.query_active)result=1;
    else{
        const auto ready=[&](){return (!object.query_fence || object.query_fence->load(std::memory_order_acquire)) &&
            (!object.query_timestamp || object.query_timestamp->done.load(std::memory_order_acquire)) &&
            (!object.query_begin || object.query_begin->done.load(std::memory_order_acquire));};
        if(!ready() && (u(3)&1U))device->native->graphics_flush(false);
        if(!ready())result=1;
        else if(arg(1)){
            std::uint64_t answer=object.query_type==8?1U:0U;
            if(object.query_type==9){
                std::shared_ptr<GraphicsResource> previous;std::vector<std::uint8_t> bytes;
                for(const auto& slot:object.query_slots){
                    if(slot.buffer!=previous){bytes=device->native->graphics_read_completed(GraphicsView{slot.buffer});previous=slot.buffer;}
                    require(std::uint64_t{slot.offset}+8<=bytes.size());std::uint64_t samples{};std::memcpy(&samples,bytes.data()+slot.offset,8);answer+=samples;
                }
                answer=std::min<std::uint64_t>(answer,UINT32_MAX);
            }else if(object.query_timestamp){
                const auto& stamp=object.query_timestamp->value;
                if(object.query_type==10)answer=stamp.ticks;
                else if(object.query_type==12)answer=stamp.frequency;
                else if(object.query_type==11)answer=stamp.disjoint || !object.query_begin || object.query_begin->value.disjoint ||
                    object.query_begin->value.frequency!=stamp.frequency || object.query_begin->value.ticks>stamp.ticks;
            }
            memory.store(GuestAddress{arg(1)},static_cast<std::uint8_t>(size*8),answer);
        }
    }
}else require(false);
