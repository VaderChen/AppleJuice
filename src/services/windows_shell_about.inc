// shell32 的 ShellAboutA／W、OpenAs_RunDLL／A／W、SHGetSetSettings 與 ReadCabinetState（由 windows_shell.cpp 的 invoke_shell 引入）。
// - ShellAbout：與 PrintDlg 相同由主機產生對話範本（文字直接寫入範本），圖示以 lParam 交給內部對話程序再以 STM_SETICON 設定；版本取自 RtlGetVersion，
//   授權對象取自 HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion 的 RegisteredOwner／RegisteredOrganization。
// - OpenAs_RunDLL：rundll32 入口，命令列即檔案路徑，轉呼叫既有的 SHOpenWithDialog（OAIF_ALLOW_REGISTRATION｜OAIF_REGISTER_EXT｜OAIF_EXEC）。
// - SHGetSetSettings：與 SHGetSettings 相同的來源（Explorer\Advanced 的 DWORD 與 Explorer\ShellState 二進位），設定時寫回同一處。
// - ReadCabinetState：HKCU\...\Explorer\CabinetState 的 Settings 二進位；沒有值時填入預設。
if(service.symbol.starts_with("ShellAbout") || service.symbol.starts_with("OpenAs_RunDLL") || service.symbol=="SHGetSetSettings" || service.symbol=="ReadCabinetState" ||
   service.family=="internal-shell-about"){
    const std::uint64_t current_user=x86_?0x80000001U:0xffffffff80000001ULL,local_machine=x86_?0x80000002U:0xffffffff80000002ULL;
    if(service.family=="internal-shell-about")
        return nested_continuation(state,memory,"關於對話程序",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);const auto window=call(0);const auto message=static_cast<std::uint32_t>(call(1));
            const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
            const auto user=[&](std::string_view name){return resolve("user32.dll",name).value;};
            if(message==0x110){
                // 沒有指定圖示時使用系統的 Windows 標誌圖示（IDI_WINLOGO）。
                auto icon=call(3);if(!icon){icon=co_await service_callback::call_value(user("LoadIconW"),{0,32517});if(x86_)icon&=UINT32_MAX;}
                (void)co_await service_callback::call(user("SendDlgItemMessageW"),{window,0x3000,0x170,icon,0});co_return finish(1);
            }
            if(message==0x10 || (message==0x111 && ((call(2)&65535U)==1 || (call(2)&65535U)==2))){
                if(gui_.windows.contains(window)){auto& w=gui_.windows.at(window);w.ended=true;w.dialog_result=1;}co_return finish(1);
            }
            co_return finish(0);
        });
    if(service.symbol.starts_with("ShellAbout"))
        return nested_continuation(state,memory,"ShellAbout",[this,service,local_machine](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);const bool wide=service.symbol.ends_with('W');const unsigned unit=x86_?4U:8U;
            const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
            const auto text=[&](std::uint64_t at){return at?(wide?read_utf16(m,GuestAddress{at},4096):utf8_to_utf16_lossy(read_ansi(m,at,4096))):std::u16string{};};
            // szApp：「標題#第一行」時 # 前為標題、後為第一行；沒有 # 時兩者相同。
            const auto app=text(call(1)),other=text(call(2));const auto mark=app.find(u'#');
            const auto title=mark==app.npos?app:app.substr(0,mark),first=mark==app.npos?app:app.substr(mark+1);
            GuestScratch scratch(*this,m,s);
            std::u16string version=u"Windows";
            if(const auto info=scratch.allocate(284)){m.fill(GuestAddress{info},0,284);m.store(GuestAddress{info},32,284);
                if(!nested_service(s,m,"ntdll.dll","RtlGetVersion",{info}))
                    version+=u" 版本 "+utf8_to_utf16_lossy(std::to_string(m.load(GuestAddress{info+4},32)))+u"."+utf8_to_utf16_lossy(std::to_string(m.load(GuestAddress{info+8},32)))+
                        u"（組建 "+utf8_to_utf16_lossy(std::to_string(m.load(GuestAddress{info+12},32)))+u"）";}
            const auto registered=[&](std::u16string_view name)->std::u16string{
                const auto key=scratch.text(u"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion"),value=scratch.text(name),buffer=scratch.allocate(512),size=scratch.allocate(4);
                if(!key || !value || !buffer || !size)return {};m.store(GuestAddress{size},32,512);
                if(nested_service(s,m,"advapi32.dll","RegGetValueW",{local_machine,key,value,2,0,buffer,size}))return {};return read_utf16(m,GuestAddress{buffer},255);};
            const auto owner_name=registered(u"RegisteredOwner"),organization=registered(u"RegisteredOrganization");
            // 對話範本：WS_POPUP｜WS_VISIBLE｜WS_CAPTION｜WS_SYSMENU｜DS_MODALFRAME｜DS_SETFONT，9 點 Segoe UI；圖示 static（SS_ICON）ID 0x3000。
            std::vector<std::uint8_t> items;unsigned count=0;const auto put=[](std::vector<std::uint8_t>& out,std::uint32_t value,unsigned size){for(unsigned i=0;i<size;++i)out.push_back(static_cast<std::uint8_t>(value>>(8*i)));};
            const auto string=[&](std::vector<std::uint8_t>& out,std::u16string_view value){for(const auto c:value)put(out,c,2);put(out,0,2);};
            const auto item=[&](unsigned id,unsigned cls,std::u16string_view label,unsigned x,unsigned y,unsigned w,unsigned h,std::uint32_t style=0){
                while(items.size()%4)items.push_back(0);put(items,0x50000000U|style,4);put(items,0,4);put(items,x,2);put(items,y,2);put(items,w,2);put(items,h,2);put(items,id,2);put(items,0xffff,2);put(items,cls,2);string(items,label);put(items,0,2);++count;};
            item(0x3000,0x82,u"",10,10,24,24,3);
            unsigned y=10;const auto line=[&](std::u16string_view value){item(0xffff,0x82,value,44,y,250,10);y+=12;};
            line(first);line(version);
            if(!owner_name.empty() || !organization.empty()){y+=6;line(u"本產品授權給：");if(!owner_name.empty())line(owner_name);if(!organization.empty())line(organization);}
            if(!other.empty()){y+=6;item(0xffff,0x82,other.substr(0,1024),44,y,250,24);y+=28;}
            y+=8;item(1,0x80,u"確定",244,y,50,14,0x10001);
            std::vector<std::uint8_t> bytes;put(bytes,0x90c80040U|0x80U,4);put(bytes,1,4);put(bytes,count,2);put(bytes,30,2);put(bytes,30,2);put(bytes,304,2);put(bytes,y+22,2);put(bytes,0,2);put(bytes,0,2);
            string(bytes,u"關於 "+title.substr(0,200));put(bytes,9,2);string(bytes,u"Segoe UI");
            // 標題長度不定：第一個控制項必須從 DWORD 邊界開始（控制項之間的對齊相對於這個起點）。
            while(bytes.size()%4)bytes.push_back(0);bytes.insert(bytes.end(),items.begin(),items.end());
            const auto source=scratch.allocate(bytes.size());if(!source)co_return finish(0);m.write(GuestAddress{source},bytes);
            if(call(0) && !gui_.windows.contains(call(0)))co_return finish(0);(void)unit;
            const auto result=co_await service_callback::call(resolve("user32.dll","DialogBoxIndirectParamW").value,{0,source,call(0),resolve("internal-shell-about","DialogProc").value,call(3)});
            co_return finish(result==UINT32_MAX?0:1);
        });
    if(service.symbol.starts_with("OpenAs_RunDLL"))
        return nested_continuation(state,memory,"OpenAs_RunDLL",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            // (hwnd, hinst, lpszCmdLine, nCmdShow)：沒有回傳值；命令列去掉前後空白與外層引號即為檔案路徑。
            const WindowsCall call(s,m,service);const bool wide=service.symbol.ends_with('W');const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
            const auto finish=[&]{call.return_to(s,0);return std::optional<std::uint32_t>{};};
            if(!call(2))co_return finish();
            auto file=wide?read_utf16(m,GuestAddress{call(2)},32767):utf8_to_utf16_lossy(read_ansi(m,call(2),32767));
            while(!file.empty() && (file.front()==u' ' || file.front()==u'\t'))file.erase(0,1);while(!file.empty() && (file.back()==u' ' || file.back()==u'\t'))file.pop_back();
            if(file.size()>=2 && file.front()==u'"' && file.back()==u'"')file=file.substr(1,file.size()-2);
            if(file.empty())co_return finish();
            GuestScratch scratch(*this,m,s);const auto path=scratch.text(file),info=scratch.allocate(unit*3);if(!path || !info)co_return finish();
            m.fill(GuestAddress{info},0,unit*3);m.store(GuestAddress{info},bits,path);m.store(GuestAddress{info+unit*2},32,7);
            const auto owner=call(0) && gui_.windows.contains(call(0))?call(0):0;
            (void)co_await service_callback::call(resolve("shell32.dll","SHOpenWithDialog").value,{owner,info});co_return finish();
        });
    const WindowsCall call(state,memory,service);GuestScratch scratch(*this,memory,state);
    const auto done=[&](std::uint64_t value){call.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto key_of=[&](std::u16string_view path){const auto at=scratch.text(path);if(!at)throw std::bad_alloc{};return at;};
    // 以既有的登錄服務讀寫（與 SHGetSettings 相同的值）；RRF_RT_REG_DWORD＝0x10、RRF_RT_REG_BINARY＝0x08。
    const auto read_value=[&](std::u16string_view path,std::u16string_view name,std::uint32_t type,std::size_t capacity)->std::optional<std::vector<std::uint8_t>>{
        const auto buffer=scratch.allocate(capacity),size=scratch.allocate(4);if(!buffer || !size)return std::nullopt;memory.store(GuestAddress{size},32,capacity);
        if(nested_service(state,memory,"advapi32.dll","RegGetValueW",{current_user,key_of(path),key_of(name),type,0,buffer,size}))return std::nullopt;
        std::vector<std::uint8_t> bytes(memory.load(GuestAddress{size},32));memory.read(GuestAddress{buffer},bytes);return bytes;};
    const auto write_value=[&](std::u16string_view path,std::u16string_view name,std::uint32_t type,const std::vector<std::uint8_t>& bytes){
        const auto data=scratch.allocate(std::max<std::size_t>(bytes.size(),4));if(!data)throw std::bad_alloc{};memory.write(GuestAddress{data},bytes);
        (void)nested_service(state,memory,"advapi32.dll","RegSetKeyValueW",{current_user,key_of(path),key_of(name),type,data,bytes.size()});};
    constexpr std::u16string_view explorer=u"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer",advanced=u"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
    const auto le32=[](const std::vector<std::uint8_t>& b,std::size_t at){return at+4<=b.size()?static_cast<std::uint32_t>(b[at])|static_cast<std::uint32_t>(b[at+1])<<8|static_cast<std::uint32_t>(b[at+2])<<16|static_cast<std::uint32_t>(b[at+3])<<24:0U;};
    const auto set32=[](std::vector<std::uint8_t>& b,std::size_t at,std::uint32_t v){for(unsigned i=0;i<4;++i)b[at+i]=static_cast<std::uint8_t>(v>>(8*i));};
    try{
    if(service.symbol=="ReadCabinetState"){
        // (CABINETSTATE* pcs, int cLength)：CABINETSTATE 為 {WORD cLength, WORD nVersion, 旗標位元欄位, UINT fMenuEnumFilter}（12 位元組）。
        // 預設：版本 2、fSaveLocalView｜fSimpleDefault｜fAdminsCreateCommonGroups、fMenuEnumFilter＝SHCONTF_FOLDERS｜SHCONTF_NONFOLDERS。
        const auto length=static_cast<std::int32_t>(call(1));if(!call(0) || length<=0)return done(0);
        const auto copy=std::min<std::size_t>(static_cast<std::size_t>(length),12);memory.check(GuestAddress{call(0)},copy,Access::Write);
        std::vector<std::uint8_t> cabinet{12,0,2,0,0x0a,0x01,0,0,0x60,0,0,0};
        if(const auto stored=read_value(std::u16string(explorer)+u"\\CabinetState",u"Settings",0x08,64);stored && stored->size()>=12 && (*stored)[0]==12)cabinet.assign(stored->begin(),stored->begin()+12);
        memory.write(GuestAddress{call(0)},std::span(cabinet).first(copy));return done(1);
    }
    // SHGetSetSettings(LPSHELLSTATE, DWORD dwMask, BOOL bSet)：SHELLSTATE 32 位元組；第一組旗標在 0、lParamSort 在 12、iSortDirection 在 16、第二組旗標在 28。
    // ShellState 二進位為 {cbSize＝0x24, SHELLSTATE}；沒有值時 fDoubleClickInWebView 開啟、fStartPanelOn 開啟。
    const auto lpss=call(0);const auto mask=static_cast<std::uint32_t>(call(1));const bool set=call(2)!=0;if(!lpss)return done(0);
    memory.check(GuestAddress{lpss},32,set?Access::Read:Access::Write);
    struct Flag{std::uint32_t mask;unsigned word,bit;std::u16string_view name;std::uint32_t fallback,on;};
    // name 為空表示存在 ShellState 二進位；on 為「開啟」對應的 DWORD 值（Hidden 1／2、HideFileExt 反向）。
    static constexpr Flag flags[]{
        {0x1,0,0,u"Hidden",2,1},{0x2,0,1,u"HideFileExt",1,0},{0x8000,0,2,{},0,1},{0x20,0,3,u"ShowSuperHidden",0,1},{0x8,0,4,u"ShowCompColor",1,1},
        {0x80,0,5,{},0,1},{0x200,0,6,{},0,1},{0x400,0,7,{},0,1},{0x800,0,8,u"DontPrettyPath",0,1},{0x100,0,9,u"ShowAttribCol",0,1},{0x1000,0,10,u"MapNetDrvBtn",0,1},
        {0x2000,0,11,u"ShowInfoTip",1,1},{0x4000,0,12,u"HideIcons",0,1},{0x20000,0,13,u"WebView",1,1},{0x10000,0,14,u"Filter",0,1},{0x40000,0,15,u"ShowSuperHidden",0,1},
        {0x100000,0,16,u"NoNetCrawling",0,1},{0x80000,1,0,u"SeparateProcess",0,1},{0x200000,1,1,{},0,1},{0x400000,1,2,{},0,1},{0x800000,1,3,u"AutoCheckSelect",0,1},
        {0x1000000,1,4,u"IconsOnly",0,1},{0x2000000,1,5,u"ShowTypeOverlay",1,1},{0x4000000,1,6,u"ShowStatusBar",1,1}};
    std::vector<std::uint8_t> blob(36,0);blob[0]=0x24;blob[4]=0x20;blob[32]=0x02;
    if(const auto stored=read_value(explorer,u"ShellState",0x08,256);stored && stored->size()>=36)blob.assign(stored->begin(),stored->begin()+36);
    std::vector<std::uint8_t> shell(32);memory.read(GuestAddress{lpss},shell);bool blob_changed=false;
    for(const auto& flag:flags){
        if(!(mask&flag.mask))continue;const auto offset=flag.word?28U:0U;const auto bit=1U<<flag.bit;
        if(set){
            const bool on=(le32(shell,offset)&bit)!=0;
            if(flag.name.empty()){const auto value=le32(blob,4+offset);set32(blob,4+offset,on?value|bit:value&~bit);blob_changed=true;}
            else{const std::uint32_t value=flag.name==u"Hidden"?(on?1U:2U):flag.name==u"HideFileExt"?(on?0U:1U):(on?1U:0U);
                std::vector<std::uint8_t> bytes(4);set32(bytes,0,value);write_value(advanced,flag.name,4,bytes);}
        }else{
            bool on=false;
            if(flag.name.empty())on=(le32(blob,4+offset)&bit)!=0;
            else{const auto stored=read_value(advanced,flag.name,0x10,4);const auto value=stored && stored->size()>=4?le32(*stored,0):flag.fallback;
                on=flag.name==u"Hidden"?value==1:flag.name==u"HideFileExt"?value==0:value!=0;}
            set32(shell,offset,on?le32(shell,offset)|bit:le32(shell,offset)&~bit);
        }
    }
    if(mask&0x10U){ // SSF_SORTCOLUMNS：lParamSort 與 iSortDirection
        if(set){set32(blob,4+12,le32(shell,12));set32(blob,4+16,le32(shell,16));blob_changed=true;}
        else{set32(shell,12,le32(blob,4+12));set32(shell,16,le32(blob,4+16));}
    }
    if(set){if(blob_changed)write_value(explorer,u"ShellState",3,blob);}else memory.write(GuestAddress{lpss},shell);
    return done(0);
    }catch(const std::bad_alloc&){return done(0);}catch(const MemoryFault&){return done(0);}
}
