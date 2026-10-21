// VC10 的 streambuf 以間接指標保存 get／put 區域；filebuf 可令它們指向 FILE 欄位。
// 因此不能直接覆寫內嵌 rpos／wpos，必須沿 prpos／pwpos 操作。
if((service.family=="msvcp100.dll" || service.family=="msvcp140.dll") && symbol.find("basic_streambuf@")!=symbol.npos && (symbol.starts_with("?_Init@") || symbol.starts_with("?_Pninc@") || symbol.starts_with("?_Gndec@") || symbol.starts_with("?pbump@") || symbol.starts_with("?gbump@") || symbol.starts_with("?setg@") || symbol.starts_with("?setbuf@") || symbol.starts_with("?sync@") || symbol.starts_with("?showmanyc@") || symbol.starts_with("?imbue@"))){
    const std::uint8_t bits=x86_?32:64;
    const auto layout=msvcp_detail::abi(x86_,service);
    const auto object=x86_?state.reg(Gpr::Rcx)&UINT32_MAX:arg(0);
    const auto parameter=[&](unsigned i){return arg(i+(x86_?0:1));};
    const auto pointer=[&](std::uint64_t at){return memory.load(GuestAddress{at},bits);};
    const auto store_pointer=[&](std::uint64_t at,std::uint64_t value){memory.store(GuestAddress{at},bits,value);};
    const auto method=symbol.substr(1,symbol.find('@')-1);
    std::uint64_t result=0;
    if(method=="_Init"){
        for(unsigned side=0;side<2;++side){
            store_pointer(object+layout.first_indirect(side),object+layout.first(side));
            store_pointer(object+layout.next_indirect(side),object+layout.next(side));
            store_pointer(object+layout.count_indirect(side),object+layout.count(side));
            store_pointer(object+layout.first(side),0);store_pointer(object+layout.next(side),0);
            memory.store(GuestAddress{object+layout.count(side)},32,0);
        }
    }else if(method=="setg"){
        const auto first=parameter(0),next=parameter(1),last=parameter(2);
        const auto count=static_cast<std::int64_t>(last-next)/layout.element;
        store_pointer(pointer(object+layout.first_indirect()),first);store_pointer(pointer(object+layout.next_indirect()),next);
        memory.store(GuestAddress{pointer(object+layout.count_indirect())},32,static_cast<std::uint32_t>(count));
    }else if(method=="pbump" || method=="gbump" || method=="_Pninc" || method=="_Gndec"){
        const bool put=method=="pbump" || method=="_Pninc";const unsigned side=put?1:0;
        const auto position=pointer(object+layout.next_indirect(side)),count=pointer(object+layout.count_indirect(side));
        const auto old=pointer(position);
        const auto delta=method=="_Pninc"?1:method=="_Gndec"?-1:std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(parameter(0)));
        const auto next=old+static_cast<std::uint64_t>(static_cast<std::int64_t>(delta)*layout.element);
        store_pointer(position,next);memory.store(GuestAddress{count},32,memory.load(GuestAddress{count},32)-static_cast<std::uint32_t>(delta));
        result=method=="_Pninc"?old:method=="_Gndec"?next:0;
    }else if(method=="setbuf"){
        // basic_streambuf 的基底實作不管理外部緩衝區；衍生 filebuf／stringbuf 自行覆寫。
        result=object;
    }else if(method=="sync" || method=="showmanyc" || method=="imbue"){
        // 基底沒有待同步輸出／可預估輸入，也不保存 locale 轉換狀態。
        result=0;
    }else throw HostCallFault("尚未實作的 VC10 streambuf 操作");
    if(x86_ && method=="showmanyc")state.reg(Gpr::Rdx)=0;
    arg.return_to(state,result);return std::nullopt;
}
