// MCI 驅動程式端 API（由 windows_mci.cpp 的 invoke_mci 引入；裝置為 mciSendCommand 開啟的同一組 MciState 裝置）。
// - mciGetDriverData／mciSetDriverData：每個裝置一個 DWORD_PTR。
// - mciGetCreatorTask：開啟裝置的執行緒（Win32 的 HTASK 即執行緒 ID）。
// - mciDriverNotify：張貼 MM_MCINOTIFY（wParam 為狀態、lParam 為裝置 ID），回傳 PostMessage 的結果。
// - mciSetYieldProc／mciGetYieldProc／mciDriverYield：yield 程序與資料；MCI_WAIT 期間每 100 ms 呼叫（見 resume_mci_wait），mciDriverYield 直接呼叫並回傳其值。
// - mciGetDeviceIDFromElementID：只有以 MCI_OPEN_ELEMENT_ID 開啟的裝置有元素 ID；開啟命令不接受該旗標，因此不會有相符的裝置。
// - mciLoadCommandResource／mciFreeCommandResource：從模組的 RCDATA 資源載入命令表（UTF-16 名稱＋DWORD 識別碼＋WORD 型別，至 MCI_END_COMMAND_LIST），
//   驗證格式後配發表格編號；格式錯誤或找不到資源回傳 MCI_NO_COMMAND_TABLE。
if(name=="mciGetDriverData" || name=="mciSetDriverData" || name=="mciGetCreatorTask" || name=="mciDriverNotify" || name=="mciDriverYield" ||
   name=="mciSetYieldProc" || name=="mciGetYieldProc" || name.starts_with("mciGetDeviceIDFromElementID") || name=="mciLoadCommandResource" || name=="mciFreeCommandResource"){
    const auto device_at=[&](unsigned index){return mci_control(bridge,static_cast<std::uint32_t>(arg(index)));};
    if(name=="mciGetDriverData"){const auto device=device_at(0);return finish(device?device->driver_data:0);}
    if(name=="mciSetDriverData"){const auto device=device_at(0);if(!device)return finish(0);device->driver_data=arg(1);return finish(1);}
    if(name=="mciGetCreatorTask"){const auto device=device_at(0);return finish(device?thread_identifier(device->creator):0);}
    if(name=="mciDriverNotify"){
        if(!arg(0))return finish(0);
        return finish(nested_service(state,memory,"user32.dll","PostMessageW",{arg(0),0x3b9,static_cast<std::uint32_t>(arg(2)),static_cast<std::uint32_t>(arg(1))})?1:0);
    }
    if(name=="mciSetYieldProc"){const auto device=device_at(0);if(!device)return finish(0);if(arg(1))memory.check(GuestAddress{arg(1)},1,Access::Execute);device->yield_proc=arg(1);device->yield_data=static_cast<std::uint32_t>(arg(2));return finish(1);}
    if(name=="mciGetYieldProc"){
        const auto device=device_at(0);if(!device)return finish(0);
        if(arg(1))memory.store(GuestAddress{arg(1)},32,device->yield_data);return finish(device->yield_proc);
    }
    if(name=="mciDriverYield"){
        const auto device=device_at(0);if(!device || !device->yield_proc)return finish(0);
        start_callbacks(state,memory,{{device->yield_proc,{static_cast<std::uint32_t>(arg(0)),device->yield_data}}},Continuation::ReturnCallback);return {};
    }
    if(name.starts_with("mciGetDeviceIDFromElementID")){(void)string(arg(1));return finish(0);}
    if(name=="mciFreeCommandResource")return finish(bridge.command_tables.erase(static_cast<std::uint32_t>(arg(0)))?1:0);
    // mciLoadCommandResource(hInstance, lpResName, wType)
    constexpr std::uint64_t no_table=0xffffffffU;
    if(!arg(1))return finish(no_table);
    const auto resource=nested_service(state,memory,"kernel32.dll","FindResourceW",{arg(0),arg(1),10});if(!resource)return finish(no_table);
    const auto size=static_cast<std::uint32_t>(nested_service(state,memory,"kernel32.dll","SizeofResource",{arg(0),resource}));
    const auto loaded=nested_service(state,memory,"kernel32.dll","LoadResource",{arg(0),resource});
    const auto data=loaded?nested_service(state,memory,"kernel32.dll","LockResource",{loaded}):0;
    if(!data || !size || size>1024*1024)return finish(no_table);
    std::vector<std::uint8_t> table(size);memory.read(GuestAddress{data},table);
    std::size_t at=0;unsigned commands=0;bool ended=false;
    while(at+2<=table.size()){
        while(at+2<=table.size() && (table[at] || table[at+1]))at+=2;
        if(at+2>table.size())break;at+=2;if(at+6>table.size())break;
        const auto type=static_cast<unsigned>(table[at+4]|(table[at+5]<<8));at+=6;
        if(type>13)break;if(type==6){ended=true;break;}if(type==0)++commands;
    }
    if(!ended || !commands || bridge.command_tables.size()>=256)return finish(no_table);
    const auto id=bridge.next_table++;bridge.command_tables.emplace(id,std::move(table));return finish(id);
}
