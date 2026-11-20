// CLIPDATA 陣列內嵌描述元，CAPROPVARIANT 則逐一沿用客體 ABI 的複製與清理。
if(name=="PropVariantClear" || name=="PropVariantCopy"){
    const bool copy=name=="PropVariantCopy";
    const auto output=call(0),source=copy?call(1):output;
    const auto type=m.load(GuestAddress{source},16);
    const bool variants=type==0x100c,vector=type!=71;
    const unsigned variant_size=x86_?16:24,stride=variants?variant_size:8+unit;
    const auto slot=8+(vector?unit:0);
    const auto first=m.load(GuestAddress{source+slot},bits);
    const auto count=vector?m.load(GuestAddress{source+8},32):static_cast<std::uint64_t>(first!=0);
    if(count>1048576 || (count && !first))co_return finish(e_invalidarg);
    m.check(GuestAddress{output},variant_size,Access::Write);
    if(count)m.check(GuestAddress{first},count*stride,Access::Read);
    auto dest=first;
    if(copy){
        m.fill(GuestAddress{output},0,variant_size);
        if(count){
            dest=allocate_heap(m,count*stride);
            if(!dest)co_return finish(e_outofmemory);
            m.fill(GuestAddress{dest},0,count*stride);
        }else dest=0;
        m.store(GuestAddress{output},16,type);
        m.store(GuestAddress{output+slot},bits,dest);
        if(vector)m.store(GuestAddress{output+8},32,count);
    }
    std::uint32_t status=0;
    std::exception_ptr failure;
    try{
        for(std::uint64_t i=0;i<count;++i){
            const auto from=first+i*stride,to=dest+i*stride;
            if(variants){
                const auto hr=static_cast<std::uint32_t>(co_await api("ole32.dll",name,
                    copy?std::vector<std::uint64_t>{to,from}:std::vector<std::uint64_t>{from}));
                if(hr && !status)status=hr;
            }else{
                const auto data=m.load(GuestAddress{from+8},bits);
                if(copy){
                    const auto size=m.load(GuestAddress{from},32);
                    if(size<4 || (size>4 && !data)){status=e_invalidarg;break;}
                    m.store(GuestAddress{to},64,m.load(GuestAddress{from},64));
                    if(size>4){
                        const auto n=size-4;
                        m.check(GuestAddress{data},n,Access::Read);
                        const auto address=allocate_heap(m,n);
                        if(!address){status=e_outofmemory;break;}
                        m.store(GuestAddress{to+8},bits,address);
                        std::vector<std::uint8_t> bytes_to_copy(n);
                        m.read(GuestAddress{data},bytes_to_copy);m.write(GuestAddress{address},bytes_to_copy);
                    }
                }else if(data && !free_heap(m,data,s))status=e_invalidarg;
            }
            if(copy && status)break;
        }
    }catch(...){failure=std::current_exception();}
    if(copy && (failure || status))
        (void)co_await api("ole32.dll","PropVariantClear",{output});
    if(!copy){
        if(first && !free_heap(m,first,s))status=e_invalidarg;
        m.fill(GuestAddress{output},0,variant_size);
    }
    if(failure)std::rethrow_exception(failure);
    co_return finish(status);
}
