// 包含於 windows_modules.cpp；檔案虛擬化沿用可攔截的 Native 入口與既有 CPU 回呼。
FileSnapshot WindowsServices::read_module_file(GuestState* state,GuestMemory& memory,const std::u16string& requested) {
    const auto host_read=[&]{return filesystem_.open_library(utf16_to_utf8(requested),runtime_.application_source,utf16_to_utf8(runtime_.application_directory));};
    if(!state)return host_read(); // 初始映像繫結尚未執行客體，沒有程序內攔截。
    constexpr std::array names{"NtOpenFile","NtCreateFile","NtQueryInformationFile","NtReadFile","NtClose"};
    std::array<std::uint64_t,names.size()> targets{};std::array<bool,names.size()> changed{};
    for(std::size_t i=0;i<names.size();++i){const auto entry=native_file_entry(memory,names[i]);targets[i]=entry.first;changed[i]=entry.second;}
    if(std::none_of(changed.begin(),changed.end(),[](bool value){return value;}))return host_read();
    // DLL 載入仍持有整次 loader lock；一般屬性查詢不借用載入鎖。
    if(runtime_.loader_owner!=threads_.current || !runtime_.loader_depth)throw HostCallFault("DLL 檔案回呼缺少載入鎖");
    if(!has_nested_runner())throw ModuleError(50,"目前執行範圍無法完成客體檔案回呼");

    // 這是有界的本次載入資料，不是保留快取。沿用最後引用歸帳，連巢狀讀取也共用額度。
    constexpr std::size_t maximum=256U*1024U*1024U,chunk=64U*1024U;
    static const auto budget=std::make_shared<RetainedCapacity>(retained_capacity_setting("X64ARM_LOADER_SOURCE_BYTES",maximum));
    auto path=full_windows_environment_path(requested,runtime_.current_directory,runtime_.environment);
    const auto key=module_key(path);
    if(path.starts_with(u"\\\\?\\") || path.starts_with(u"\\\\.\\"))path=u"\\??\\"+path.substr(4);
    else if(path.starts_with(u"\\\\"))path=u"\\??\\UNC\\"+path.substr(2);
    else path=u"\\??\\"+path;
    if(path.size()>32766)return {key,{},206};
    const auto scratch_size=256+path.size()*2+2+chunk;
    auto scratch_lease=RetainedCapacityLease::acquire(budget,(scratch_size+4095)&~std::size_t{4095});if(!scratch_lease)return {key,{},8};
    const auto scratch=allocate_heap(memory,scratch_size);if(!scratch)return {key,{},8};
    struct Release {WindowsServices& self;GuestMemory& memory;GuestState& state;std::uint64_t address;
        ~Release(){(void)self.free_heap(memory,address,state);}} release{*this,memory,*state,scratch};
    const auto bits=static_cast<std::uint8_t>(x86_?32:64);const auto unit=x86_?4U:8U;
    const auto output=scratch,ios=scratch+16,attributes=scratch+48,name=scratch+112,info=scratch+144,offset=scratch+176,text=scratch+256,data=text+path.size()*2+2;
    memory.write(GuestAddress{scratch},std::array<std::uint8_t,256>{});memory.write(GuestAddress{text},utf16_bytes(path));
    memory.store(GuestAddress{name},16,path.size()*2);memory.store(GuestAddress{name+2},16,path.size()*2+2);memory.store(GuestAddress{name+(x86_?4:8)},bits,text);
    memory.store(GuestAddress{attributes},32,x86_?24:48);memory.store(GuestAddress{attributes+(x86_?8:16)},bits,name);memory.store(GuestAddress{attributes+(x86_?12:24)},32,0x40);
    const auto invoke=[&](unsigned index,std::vector<std::uint64_t> args){return call_guest_native_file(*state,memory,targets[index],std::move(args));};
    const auto dos=[&](std::uint32_t status){return static_cast<std::uint32_t>(nested_service(*state,memory,"ntdll.dll","RtlNtStatusToDosError",{status}));};
    // NtOpenFile 為既有檔案的正常路徑；僅 CreateFile 入口受攔截時保留相同 OPEN 語義。
    auto status=changed[0] || !changed[1]?invoke(0,{output,0x80100000U,attributes,ios,5,0x60}):invoke(1,{output,0x80100000U,attributes,ios,0,0,5,1,0x60,0,0});
    if(status)return {key,{},dos(status),status==0xc0000022U};
    const auto file=memory.load(GuestAddress{output},bits);
    FileSnapshot result{key,{},0};
    try {
        status=invoke(2,{file,ios,info,24,5});
        if(status)result.error=dos(status);
        else {
            const auto size=memory.load(GuestAddress{info+8},64);
            if(size>maximum || memory.load(GuestAddress{info+21},8))result.error=193;
            else if(auto lease=RetainedCapacityLease::acquire(budget,size+sizeof(std::vector<std::uint8_t>)+64)){
                struct Bytes {RetainedCapacityLease lease;std::vector<std::uint8_t> value;
                    Bytes(RetainedCapacityLease held,std::size_t size):lease(std::move(held)),value(size){}};
                auto bytes=std::make_shared<Bytes>(std::move(*lease),static_cast<std::size_t>(size));
                for(std::size_t at=0;at<size;){
                    const auto count=std::min<std::uint64_t>(chunk,size-at);memory.store(GuestAddress{offset},64,at);
                    memory.store(GuestAddress{ios},bits,0x103);memory.store(GuestAddress{ios+unit},bits,0);
                    status=invoke(3,{file,0,0,0,ios,data,count,offset,0});
                    if(status){result.error=dos(status);break;}
                    const auto copied=memory.load(GuestAddress{ios+unit},bits);
                    if(!copied || copied>count){result.error=38;break;}
                    memory.read(GuestAddress{data},std::span(bytes->value).subspan(at,copied));at+=copied;
                }
                if(!result.error)result.bytes=std::shared_ptr<const std::vector<std::uint8_t>>(bytes,&bytes->value);
            }else result.error=8;
        }
    }catch(const std::bad_alloc&){result.error=8;}
    // 失敗亦經相同可攔截 Close；CPU 停止／程序退出由既有回呼與程序物件生命週期收尾。
    status=invoke(4,{file});if(status && !result.error){result.error=dos(status);result.bytes.reset();}
    result.access_denied=result.error==5;return result;
}
