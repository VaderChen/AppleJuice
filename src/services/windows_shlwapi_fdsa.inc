// 固定大小元素的 DSA。控制塊與元素都保存在客體記憶體，首次成長才取得儲存區所有權。
if(service.family=="internal-shell-fdsa"){
    const bool initialize=name=="FDSA_Initialize";
    const auto info=arg(initialize?2:0);
    const auto data_field=info+pointer_bits/8, capacity_field=data_field+pointer_bits/8;
    const auto increment_field=capacity_field+4, width_field=increment_field+1, flags_field=width_field+1;
    if(initialize){
        const auto data=arg(3);
        if(data)memory.fill(GuestAddress{data},0,u32(0)*u32(4));
        memory.store(GuestAddress{info},32,0);
        memory.store(GuestAddress{data_field},pointer_bits,data);
        memory.store(GuestAddress{capacity_field},32,u32(4));
        memory.store(GuestAddress{increment_field},8,u32(1)?u32(1):1);
        memory.store(GuestAddress{width_field},8,u32(0));
        memory.store(GuestAddress{flags_field},8,0);
        return done(1);
    }
    if(name=="FDSA_Destroy"){
        if(!(memory.load(GuestAddress{flags_field},8)&1))return done(1);
        (void)free_heap(memory,memory.load(GuestAddress{data_field},pointer_bits),state);
        return done(0);
    }
    const auto count=memory.load(GuestAddress{info},32);
    const bool erase=name=="FDSA_DeleteItem";
    if(erase && u32(1)>=count)return done(0);
    const auto index=std::min<std::uint64_t>(u32(1),count);
    const auto width=memory.load(GuestAddress{width_field},8);
    auto data=memory.load(GuestAddress{data_field},pointer_bits);
    if(erase){
        const auto dest=data+index*width, tail=(count-index-1)*width;
        if(tail)memory.move(GuestAddress{dest},GuestAddress{dest+width},tail);
        if(width)memory.fill(GuestAddress{dest+tail},0,width);
        memory.store(GuestAddress{info},32,count-1);
        return done(1);
    }
    auto source=arg(2);
    if(width)memory.check(GuestAddress{source},width,Access::Read);
    const auto capacity=memory.load(GuestAddress{capacity_field},32);
    if(count>=capacity){
        const auto next=capacity+memory.load(GuestAddress{increment_field},8);
        if(next<=count || next>UINT32_MAX || next*width>UINT32_MAX)return done(UINT32_MAX);
        const auto flags=memory.load(GuestAddress{flags_field},8);
        const auto old_bytes=capacity*width, bytes=next*width;
        const auto grown=(flags&1)?reallocate_heap(memory,data,bytes,state):allocate_heap(memory,bytes);
        if(!grown)return done(UINT32_MAX);
        if(!(flags&1) && old_bytes)memory.move(GuestAddress{grown},GuestAddress{data},old_bytes);
        if(bytes>old_bytes)memory.fill(GuestAddress{grown+old_bytes},0,bytes-old_bytes);
        if((flags&1) && source>=data && source-data<old_bytes)source=grown+(source-data);
        data=grown;
        memory.store(GuestAddress{data_field},pointer_bits,data);
        memory.store(GuestAddress{capacity_field},32,next);
        memory.store(GuestAddress{flags_field},8,flags|1);
    }
    const auto dest=data+index*width, tail=(count-index)*width;
    if(tail)memory.move(GuestAddress{dest+width},GuestAddress{dest},tail);
    if(width)memory.move(GuestAddress{dest},GuestAddress{source},width);
    memory.store(GuestAddress{info},32,count+1);
    return done(index);
}
