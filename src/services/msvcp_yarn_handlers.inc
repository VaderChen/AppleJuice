// 此容器沒有 vtable；欄位為字串指標與一個空值字元。
if(service.family=="msvcp140.dll" && symbol.find("_Yarn@")!=symbol.npos){
    const bool win32_member=symbol.find("@@QAE")!=symbol.npos || symbol.find("@@QBE")!=symbol.npos || symbol.find("@@AAE")!=symbol.npos;
    const auto object=x86_ && win32_member?state.reg(Gpr::Rcx)&UINT32_MAX:arg(0);
    const unsigned unit=x86_?4:8,element=symbol.find("_Yarn@D@")!=symbol.npos?1:2;
    const std::uint8_t bits=x86_?32:64;
    const bool constructor=symbol.starts_with("??0"),assignment=symbol.starts_with("??4"),destroy=symbol.starts_with("??1") || symbol.starts_with("?_Tidy@");
    memory.check(GuestAddress{object},unit+element,constructor || assignment || destroy?Access::Write:Access::Read);
    if(constructor){memory.store(GuestAddress{object},bits,0);memory.store(GuestAddress{object+unit},static_cast<std::uint8_t>(element*8),0);}
    auto old=memory.load(GuestAddress{object},bits);std::uint64_t result=object;
    if(symbol.starts_with("?c_str@") || symbol.starts_with("?_C_str@"))result=old?old:object+unit;
    else if(symbol.starts_with("?empty@") || symbol.starts_with("?_Empty@"))result=old?0:1;
    else if(destroy || assignment || (constructor && !symbol.ends_with("@XZ"))){
        std::uint64_t source=0;
        if(!destroy){source=arg(x86_ && win32_member?0:1);if(symbol.find("BV01@")!=symbol.npos)source=memory.load(GuestAddress{source},bits);}
        if(destroy || old!=source){
            if(old && !free_heap(memory,old,state))throw HostCallFault("MSVCP Yarn 不是有效 CRT 字串配置");
            memory.store(GuestAddress{object},bits,0);
            if(source)memory.store(GuestAddress{object},bits,crt_duplicate_string(memory,source,element==2));
        }
        if(destroy)result=0;
    }
    arg.return_to(state,result);return std::nullopt;
}
