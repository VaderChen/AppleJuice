// Shell 使用絕對 SECURITY_DESCRIPTOR。DACL 的存在旗標獨立於 ACL 指標。
const auto descriptor_size=x86_?20U:40U;
const auto write_shell_descriptor=[&](std::uint64_t address,std::uint64_t acl){
    memory.fill(GuestAddress{address},0,descriptor_size);
    memory.store(GuestAddress{address},32,0x00040001);
    memory.store(GuestAddress{address+descriptor_size-pointer_bits/8U},pointer_bits,acl);
};
const auto all_access_attributes=[&](std::uint64_t attributes,std::uint64_t descriptor){
    if(!attributes || !descriptor)return UINT64_C(0);
    write_shell_descriptor(descriptor,0);
    const auto unit=pointer_bits/8U;
    memory.store(GuestAddress{attributes},32,x86_?12:24);
    memory.store(GuestAddress{attributes+unit},pointer_bits,descriptor);
    memory.store(GuestAddress{attributes+2*unit},32,0);
    return attributes;
};
if(name=="#356")return done(all_access_attributes(arg(0),arg(1)));
if(name=="#475"){
    const auto array=arg(0);
    const auto count=u32(1);
    // ACL 的長度是 WORD；即使每筆僅一個子授權，ACE 仍至少需要 20 bytes。
    if(!array || !count || count>(UINT16_MAX-8)/20)return done(0);
    std::vector<security::Bytes> entries;
    unsigned acl_size=8;
    for(unsigned i=0;i<count;++i){
        const auto perm=memory.load(GuestAddress{array+i*(pointer_bits/8U)},pointer_bits);
        if(!perm)return done(0);
        std::array<std::uint8_t,36> row;
        memory.read(GuestAddress{perm},row);
        const auto field=[&](unsigned offset){return std::uint32_t{row[offset]}|
            (std::uint32_t{row[offset+1]}<<8)|(std::uint32_t{row[offset+2]}<<16)|(std::uint32_t{row[offset+3]}<<24);};
        const auto type=field(16);
        if(type>1)return done(0);
        security::Bytes sid;
        if(std::all_of(row.begin(),row.begin()+16,[](auto value){return value==0;})){
            initialize_tokens();
            sid=runtime_.token_objects.at(1).user.sid;
        }else{
            const unsigned subs=field(12)?2:1;
            sid.resize(8+4*subs);
            sid[0]=1;
            sid[1]=static_cast<std::uint8_t>(subs);
            std::copy_n(row.begin(),6,sid.begin()+2);
            std::copy_n(row.begin()+8,4*subs,sid.begin()+8);
        }
        const bool inherit=field(20)!=0;
        acl_size+=(8+sid.size())*(inherit?2:1);
        if(acl_size>UINT16_MAX)return done(0);
        entries.push_back(security::encode_ace({static_cast<std::uint8_t>(type),0,field(24),sid}));
        if(inherit)entries.push_back(security::encode_ace({static_cast<std::uint8_t>(type),
            static_cast<std::uint8_t>(field(28)),field(32),sid}));
    }
    const auto acl=security::encode_acl(entries);
    const auto result=handle(host("kernel32.dll","LocalAlloc",{0,descriptor_size+acl.size()}));
    if(result){
        write_shell_descriptor(result,result+descriptor_size);
        memory.write(GuestAddress{result+descriptor_size},acl);
    }
    return done(result);
}
