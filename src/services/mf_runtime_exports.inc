// mfplat 的雜項執行階段物件（由 windows_media_foundation.cpp 的匯出鏈引入）：
// - CreatePropertyStore：沿用 propsys 的 PSCreateMemoryPropertyStore（IPropertyStore）。
// - MFCreateTransformActivate：空的 MFT 啟用物件；呼叫端設定 MFT_TRANSFORM_CLSID_Attribute 後，ActivateObject 以 CoCreateInstance 建立（與登錄 MFT 同一路徑）。
// - MFCreateMFByteStreamWrapper：本引擎的位元組串流產生共用同一狀態（位置、能力、關閉）的新物件並持有原串流；客體實作的串流以原物件回傳（呼叫全部轉給它）。
// - MFGetPluginControl：程序內唯一的 IMFPluginControl／IMFPluginControl2，偏好 CLSID（依類型與選擇字串，不分大小寫）與停用清單只在本程序有效；
//   MFTEnumEx 帶 MFT_ENUM_FLAG_SORTANDFILTER 時排除 MF_Plugin_Type_MFT 停用的 CLSID。
// - MFCreateSystemTimeSource：IMFPresentationTimeSource（同時是 IMFClock 與 IMFClockStateSink），時間以 MFGetSystemTime 為準、10 MHz；
//   OnClockStart 帶 PRESENTATION_CURRENT_POSITION 時由目前時間續行，暫停時凍結，停止歸零，速率改變時重設基準。
// - MFCreateTrackedSample：可查出 IMFTrackedSample 的樣本；SetAllocator 後參照歸零時改經工作佇列呼叫回呼（非同步結果的物件即該樣本）。
// - MFCreateVideoSampleAllocatorEx：IMFVideoSampleAllocatorEx／IMFVideoSampleAllocatorCallback，樣本參照歸零時回到閒置清單。
if(name=="CreatePropertyStore" || name=="MFCreateTransformActivate" || name=="MFCreateMFByteStreamWrapper" || name=="MFGetPluginControl" || name=="MFCreateSystemTimeSource" ||
   name=="MFCreateTrackedSample" || name=="MFCreateVideoSampleAllocatorEx" || name=="MFCreateD3D12SynchronizationObject"){
    if(name=="CreatePropertyStore"){
        require(arg(0)!=0,pointer_error);GuestScratch scratch(*this,memory,state);const auto iid=scratch.allocate(16);require(iid!=0,0x8007000eU);
        memory.write(GuestAddress{iid},mf::guid(0x886d8eeb,0x8cf2,0x4446,{0x8d,0x02,0xcd,0xba,0x1d,0xbd,0xcf,0x99}));
        const auto status=static_cast<std::uint32_t>(nested_service(state,memory,"propsys.dll","PSCreateMemoryPropertyStore",{iid,arg(0)}));require(!status,status);
    }else if(name=="MFCreateTransformActivate"){
        out(arg(0));const auto p=make(mf::Activate);object(p,mf::Activate)->identifier=6;put(arg(0),p,bits);
    }else if(name=="MFCreateMFByteStreamWrapper"){
        out(arg(1));require(arg(0)!=0,pointer_error);
        if(const auto found=bridge.objects.find(arg(0));found!=bridge.objects.end()){
            auto inner=object(arg(0),mf::ByteStream);require(inner->byte_stream!=nullptr,no_interface);
            const auto p=make(mf::ByteStream);auto wrapper=object(p,mf::ByteStream);wrapper->byte_stream=inner->byte_stream;wrapper->parent=arg(0);hold(arg(0));put(arg(1),p,bits);
        }else {(void)foreign_method(arg(0),3);hold(arg(0));put(arg(1),arg(0),bits);}
    }else if(name=="MFGetPluginControl"){
        out(arg(0));if(!bridge.plugin_control || !bridge.objects.contains(bridge.plugin_control))bridge.plugin_control=make(mf::PluginControl);
        hold(bridge.plugin_control);put(arg(0),bridge.plugin_control,bits);
    }else if(name=="MFCreateSystemTimeSource"){
        out(arg(0));const auto p=make(mf::TimeSource);put(arg(0),p,bits);
    }else if(name=="MFCreateTrackedSample"){
        out(arg(0));const auto p=make(mf::Sample);auto sample=object(p,mf::Sample);sample->tracked=true;put(arg(0),create(sample,mf::TrackedSample),bits);
    }else if(name=="MFCreateD3D12SynchronizationObject"){
        // (pDevice, riid, ppvSyncObject)：以裝置的 CreateFence 建立就緒與釋放兩個 fence（初值 0），持有裝置；建立在續行中，重播時直接取回結果。
        if(service_callback::completed(this,threads_.current,service.symbol,state))return nested_continuation(state,memory,service.symbol,[](GuestState&,GuestMemory&)->service_callback::Task<std::optional<std::uint32_t>>{throw HostCallFault("服務續行重播不應重新建立");});
        out(arg(2));require(arg(0)!=0,pointer_error);(void)guid(arg(1));(void)foreign_method(arg(0),36);
        const auto p=make(mf::D3D12Sync);object(p,mf::D3D12Sync)->d3d12=std::make_shared<Object::D3D12Sync>();const auto device=arg(0),iid=arg(1),output=arg(2);
        return nested_continuation(state,memory,service.symbol,[this,service,p,device,iid,output](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);const std::uint8_t width=x86_?32:64;const unsigned size_of_pointer=width/8U;
            const auto method=[&](std::uint64_t target,unsigned slot){const auto table=m.load(GuestAddress{target},width);return m.load(GuestAddress{table+slot*size_of_pointer},width);};
            const auto block=allocate_heap(m,32);std::uint32_t hr=0x8007000eU;std::array<std::uint64_t,2> fences{};
            if(block){
                // IID_ID3D12Fence {0a753dcf-c4d8-4b91-adf6-be5a60d95a76}
                m.write(GuestAddress{block},mf::guid(0x0a753dcf,0xc4d8,0x4b91,{0xad,0xf6,0xbe,0x5a,0x60,0xd9,0x5a,0x76}));hr=0;
                for(auto& fence:fences){
                    if(hr)break;m.store(GuestAddress{block+16},width,0);
                    std::vector<std::uint64_t> args{device};if(x86_){args.push_back(0);args.push_back(0);}else args.push_back(0);args.insert(args.end(),{0,block,block+16});
                    hr=static_cast<std::uint32_t>(co_await service_callback::call_value(method(device,36),args));if(!hr)fence=m.load(GuestAddress{block+16},width);
                }
                (void)free_heap(m,block,s);
            }
            if(!hr){
                (void)co_await service_callback::call_value(method(device,1),{device});
                if(const auto found=media_foundation_->objects.find(p);found!=media_foundation_->objects.end() && found->second.object->d3d12){
                    auto& sync=*found->second.object->d3d12;sync.device=device;sync.ready=fences[0];sync.release=fences[1];fences={};}
                hr=static_cast<std::uint32_t>(co_await service_callback::call_value(method(p,0),{p,iid,output}));
            }
            for(const auto fence:fences)if(fence)(void)co_await service_callback::call_value(method(fence,2),{fence});
            (void)co_await service_callback::call_value(resolve("internal-mf","Release").value,{p});
            call.return_to(s,hr);co_return std::nullopt;
        });
    }else if(name=="MFCreateVideoSampleAllocatorEx"){
        // (riid, ppSampleAllocator)
        out(arg(1));const auto p=make(mf::SampleAllocator);object(p,mf::SampleAllocator)->pool=std::make_shared<SamplePool>();
        try{query(p,guid(arg(0)),arg(1));}catch(...){release(p);throw;}release(p);
    }
}
