// 由 windows_dbghelp.cpp 的 invoke_dbghelp 引入（位於 MapAndLoad 之後、PE 標頭工具之前）：imagehlp 的映像檔工具與 dbghelp 的檔案搜尋。
// 開檔、對映與檔案時間都經既有 kernel32 服務；ImageLoad／ImageUnload 沿用 MapAndLoad／UnMapAndLoad；搜尋沿用 SearchTreeForFile 的走訪。
if(name=="CheckSumMappedFile" || name.starts_with("MapFileAndCheckSum")){
    // PE 校驗和：16 位元字組累加並折回進位（奇數長度補零），PE 映像再扣除 OptionalHeader.CheckSum（偏移 64）的兩個字組，最後加上檔案長度。
    // 不是 PE 時 HeaderSum 為 0、CheckSum 仍是累加值加長度，CheckSumMappedFile 回 NULL。
    // 回傳 NT 標頭的檔案偏移（不是 PE 時為空），並寫入 HeaderSum 與 CheckSum。
    const auto finish_sum=[&](std::span<const std::uint8_t> bytes,std::uint64_t header_out,std::uint64_t sum_out)->std::optional<std::uint32_t>{
        std::uint32_t partial=0;
        for(std::size_t i=0;i<bytes.size();i+=2){partial+=bytes[i]|(i+1<bytes.size()?std::uint32_t{bytes[i+1]}<<8:0U);partial=(partial>>16)+(partial&0xffffU);}
        auto sum=static_cast<std::uint16_t>((partial>>16)+partial);
        const auto read32=[&](std::size_t at){return std::uint32_t{bytes[at]}|std::uint32_t{bytes[at+1]}<<8|std::uint32_t{bytes[at+2]}<<16|std::uint32_t{bytes[at+3]}<<24;};
        std::optional<std::uint32_t> nt;std::uint32_t header=0;
        if(bytes.size()>=0x40 && bytes[0]=='M' && bytes[1]=='Z'){
            const auto at=read32(0x3c);
            if(at>0 && std::uint64_t{at}+24+68<=bytes.size() && read32(at)==0x4550U){
                nt=at;header=read32(at+24+64);
                for(const auto half:{static_cast<std::uint16_t>(header),static_cast<std::uint16_t>(header>>16)}){sum=static_cast<std::uint16_t>(sum-(sum<half));sum=static_cast<std::uint16_t>(sum-half);}
            }
        }
        memory.store(GuestAddress{header_out},32,header);memory.store(GuestAddress{sum_out},32,std::uint32_t{sum}+static_cast<std::uint32_t>(bytes.size()));
        return nt;
    };
    if(name=="CheckSumMappedFile"){
        // (BaseAddress, FileLength, HeaderSum*, CheckSum*)：回傳 IMAGE_NT_HEADERS 位址；讀取映像失敗回 NULL。
        if(!arg(2) || !arg(3)){fail(87);arg.return_to(state,0);return std::nullopt;}
        try{
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(arg(1)&UINT32_MAX));memory.read(GuestAddress{arg(0)},bytes);
            const auto nt=finish_sum(bytes,arg(2),arg(3));arg.return_to(state,nt?arg(0)+*nt:0);
        }catch(const MemoryFault&){arg.return_to(state,0);}
        return std::nullopt;
    }
    // MapFileAndCheckSumA／W(Filename, HeaderSum*, CheckSum*)：CHECKSUM_SUCCESS 0、OPEN_FAILURE 1、MAP_FAILURE 2、MAPVIEW_FAILURE 3、UNICODE_FAILURE 4。
    if(!arg(0) || !arg(1) || !arg(2)){arg.return_to(state,1);return std::nullopt;}
    std::u16string path;try{path=text(arg(0));}catch(const std::exception&){arg.return_to(state,4);return std::nullopt;}
    GuestScratch scratch(*this,memory,state);const auto path_at=scratch.text(path);if(!path_at){arg.return_to(state,1);return std::nullopt;}
    const auto invalid_handle=x86_?UINT64_C(0xffffffff):UINT64_MAX;
    const auto close=[&](std::uint64_t handle){const auto error=last_error_;(void)nested_service(state,memory,"kernel32.dll","CloseHandle",{handle});last_error_=error;};
    const auto file=nested_service(state,memory,"kernel32.dll","CreateFileW",{path_at,0x80000000U,1,0,3,0x80,0});
    if(file==invalid_handle || !file){arg.return_to(state,1);return std::nullopt;}
    const auto size=nested_service(state,memory,"kernel32.dll","GetFileSize",{file,0})&UINT32_MAX;
    const auto mapping=nested_service(state,memory,"kernel32.dll","CreateFileMappingW",{file,0,2,0,0,0});
    if(!mapping){close(file);arg.return_to(state,2);return std::nullopt;}
    const auto view=nested_service(state,memory,"kernel32.dll","MapViewOfFile",{mapping,4,0,0,0});
    if(!view){close(mapping);close(file);arg.return_to(state,3);return std::nullopt;}
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));memory.read(GuestAddress{view},bytes);
    (void)finish_sum(bytes,arg(1),arg(2));
    (void)nested_service(state,memory,"kernel32.dll","UnmapViewOfFile",{view});close(mapping);close(file);
    arg.return_to(state,0);return std::nullopt;
}
if(name=="GetImageUnusedHeaderBytes"){
    // (LOADED_IMAGE*, SizeUnusedHeaderBytes*)：區段表之後（若有標頭內的繫結匯入表則在其後）到 SizeOfHeaders 之間的空間；回傳其檔案偏移。
    if(!arg(0)){fail(87);arg.return_to(state,0);return std::nullopt;}
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto mapped=memory.load(GuestAddress{arg(0)+unit*2},bits),nt=memory.load(GuestAddress{arg(0)+unit*3},bits);
    const auto count=memory.load(GuestAddress{arg(0)+unit*5},32),sections=memory.load(GuestAddress{arg(0)+unit*6},bits);
    const auto optional=nt+24;const bool plus=memory.load(GuestAddress{optional},16)==0x20bU;
    auto offset=sections+count*40-mapped;
    const auto directories=optional+(plus?112U:96U);
    if(memory.load(GuestAddress{optional+(plus?108U:92U)},32)>11){
        const auto bound=memory.load(GuestAddress{directories+11*8},32),bound_size=memory.load(GuestAddress{directories+11*8+4},32);
        if(bound && offset<bound+bound_size)offset=bound+bound_size;
    }
    const auto headers=memory.load(GuestAddress{optional+60},32);
    if(arg(1))memory.store(GuestAddress{arg(1)},32,(headers-offset)&UINT32_MAX);
    arg.return_to(state,offset&UINT32_MAX);return std::nullopt;
}
if(name=="ImageLoad" || name=="ImageUnload"){
    // ImageLoad(DllName, DllPath)：以程序 heap 配置 LOADED_IMAGE，經 MapAndLoad（預設 .dll 副檔名、唯讀）載入；ImageUnload 只接受 ImageLoad 的結果。
    const unsigned struct_size=x86_?48U:88U;
    if(name=="ImageUnload"){
        if(!arg(0) || !runtime_.imagehlp_images.erase(arg(0))){fail(87);arg.return_to(state,0);return std::nullopt;}
        (void)nested_service(state,memory,"imagehlp.dll","UnMapAndLoad",{arg(0)});(void)free_heap(memory,arg(0),state);
        arg.return_to(state,1);return std::nullopt;
    }
    if(!arg(0)){fail(87);arg.return_to(state,0);return std::nullopt;}
    const auto image=allocate_heap(memory,struct_size);if(!image){fail(8);arg.return_to(state,0);return std::nullopt;}
    if(!(nested_service(state,memory,"imagehlp.dll","MapAndLoad",{arg(0),arg(1),image,1,1})&UINT32_MAX)){
        const auto error=last_error_;(void)free_heap(memory,image,state);last_error_=error;arg.return_to(state,0);return std::nullopt;}
    runtime_.imagehlp_images.insert(image);arg.return_to(state,image);return std::nullopt;
}
if(name=="TouchFileTimes"){
    // (FileHandle, SYSTEMTIME*)：以指定時間（NULL 為目前 UTC 時間）設定最後寫入時間，建立與存取時間不變。
    GuestScratch scratch(*this,memory,state);auto when=arg(1);const auto file_time=scratch.allocate(8);
    if(!file_time){fail(8);arg.return_to(state,0);return std::nullopt;}
    if(!when){when=scratch.allocate(16);if(!when){fail(8);arg.return_to(state,0);return std::nullopt;}(void)nested_service(state,memory,"kernel32.dll","GetSystemTime",{when});}
    if(!(nested_service(state,memory,"kernel32.dll","SystemTimeToFileTime",{when,file_time})&UINT32_MAX)){arg.return_to(state,0);return std::nullopt;}
    arg.return_to(state,nested_service(state,memory,"kernel32.dll","SetFileTime",{arg(0),0,0,file_time})&UINT32_MAX);return std::nullopt;
}
if(name.starts_with("FindExecutableImage") || name.starts_with("FindDebugInfoFile")){
    // (FileName, SymbolPath, FoundPath[MAX_PATH], [Callback(Handle, FoundPath, CallerData), CallerData])：先試 FileName 本身（相對目前目錄），
    // 再依 SymbolPath 各目錄以 SearchTreeForFile 的走訪尋找；找到即以唯讀共用開啟並寫入完整路徑。回呼回 FALSE 時關閉後繼續找下一個候選。
    if(!arg(0) || !arg(2)){fail(87);arg.return_to(state,0);return std::nullopt;}
    const auto file=text(arg(0));const auto search=arg(1)?text(arg(1)):std::u16string{};
    std::vector<std::u16string> candidates;
    if(!file.empty()){
        const auto direct=normalized_windows_path(file,runtime_.current_directory);const auto info=filesystem_.information(utf16_to_utf8(direct));
        if(!info.error && !(info.metadata.attributes&0x10))candidates.push_back(direct);
        for(std::size_t at=0;at<=search.size();){
            const auto end=search.find(u';',at);auto entry=search.substr(at,end==search.npos?search.npos:end-at);
            while(!entry.empty() && entry.front()==u' ')entry.erase(entry.begin());while(!entry.empty() && entry.back()==u' ')entry.pop_back();
            if(!entry.empty())if(auto found=search_tree_for_file(normalized_windows_path(entry,runtime_.current_directory),file);found && std::find(candidates.begin(),candidates.end(),*found)==candidates.end())candidates.push_back(std::move(*found));
            if(end==search.npos)break;at=end+1;
        }
    }
    // 「Executable」本身含 Ex，Ex 版只看名稱結尾。
    const bool extended=name.ends_with("Ex") || name.ends_with("ExW");const auto output=arg(2),callback=extended?arg(3):0,context=extended?arg(4):0;
    const auto invalid_handle=x86_?UINT64_C(0xffffffff):UINT64_MAX;
    // 寫入找到的路徑後以同一個緩衝開啟（A 版用 CreateFileA，W 版用 CreateFileW）；路徑超過 MAX_PATH 的候選略過。
    const auto open=[this,wide,output,invalid_handle](GuestState& s,GuestMemory& m,const std::u16string& path)->std::uint64_t{
        auto bytes=wide?utf16_bytes(path):[&]{auto narrow=utf16_to_utf8(path);std::vector<std::uint8_t> value(narrow.begin(),narrow.end());value.push_back(0);return value;}();
        if(bytes.size()>(wide?520U:260U))return 0;
        m.write(GuestAddress{output},bytes);
        const auto handle=nested_service(s,m,"kernel32.dll",wide?"CreateFileW":"CreateFileA",{output,0x80000000U,3,0,3,0x80,0});
        return handle==invalid_handle?0:handle;
    };
    if(!callback){
        for(const auto& path:candidates)if(const auto handle=open(state,memory,path)){arg.return_to(state,handle);return std::nullopt;}
        fail(2);arg.return_to(state,0);return std::nullopt;
    }
    return nested_continuation(state,memory,"FindImageFile",
        [this,service,candidates,open,callback,context,output](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall inner(s,m,service);
            for(const auto& path:candidates){
                const auto handle=open(s,m,path);if(!handle)continue;
                if(co_await service_callback::call(callback,{handle,output,context})){inner.return_to(s,handle);co_return std::optional<std::uint32_t>{};}
                const auto error=last_error_;(void)nested_service(s,m,"kernel32.dll","CloseHandle",{handle});last_error_=error;
            }
            last_error_=2;inner.return_to(s,0);co_return std::optional<std::uint32_t>{};
        });
}
