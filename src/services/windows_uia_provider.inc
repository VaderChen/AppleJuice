// 在 invoke_accessible 的共同 ABI／配置／callback 範圍內實作 UIA provider 家族。
if(service.family=="uiautomationcore.dll" || name.starts_with("UIA.")){
    constexpr std::uint32_t unavailable=0x80040201U;
    const auto object_for=[&](std::uint64_t window,std::uint32_t kind){
        if(bridge.uia_objects.size()>=65536)throw std::bad_alloc{};
        if(!bridge.uia_table){
            bridge.uia_table=runtime_allocate(memory,{},std::size(uia_methods)*unit);
            for(unsigned i=0;i<std::size(uia_methods);++i)put(bridge.uia_table+i*unit,resolve(uia_methods[i].family,uia_methods[i].symbol).value);
        }
        const auto at=allocate_heap(memory,unit);if(!at)throw std::bad_alloc{};
        put(at,bridge.uia_table);bridge.uia_objects.emplace(at,Object{window,kind,1});return at;
    };
    if(name=="UiaGetReservedMixedAttributeValue" || name=="UiaGetReservedNotSupportedValue"){
        if(!arg(0))return finish(pointer_error);put(arg(0),0);
        const unsigned index=name=="UiaGetReservedMixedAttributeValue"?0:1;
        auto& object=bridge.uia_reserved[index];if(!object)object=object_for(0,index+1);
        ++bridge.uia_objects.at(object).references;put(arg(0),object);return finish(0);
    }
    if(name=="UiaHostProviderFromHwnd"){
        if(!arg(1))return finish(pointer_error);put(arg(1),0);
        if(!gui_.windows.contains(arg(0)))return finish(0x80070578U);
        put(arg(1),object_for(arg(0),0));return finish(0);
    }
    if(name=="UiaReturnRawElementProvider"){
        std::vector<GuestCallback> calls;
        if(!arg(3) && !arg(1) && !arg(2)){
            for(auto it=bridge.uia_providers.begin();it!=bridge.uia_providers.end();){
                if(it->second.window!=arg(0)){++it;continue;}
                calls.push_back({method(it->first,2),{it->first}});it=bridge.uia_providers.erase(it);
            }
            if(calls.empty())return finish(0);
            const auto scratch=scratch_for(12);start_callbacks(state,memory,std::move(calls),Continuation::AccessibleOperation,scratch);return {};
        }
        if(!arg(3) || !gui_.windows.contains(arg(0)))return finish(0);
        if(const auto found=bridge.uia_providers.find(arg(3));found!=bridge.uia_providers.end()){
            if(found->second.window!=arg(0))return finish(0);return finish(found->second.token);
        }
        if(bridge.uia_providers.size()>=4096 || bridge.next_transfer>0x7fffffffU)return finish(0);
        const auto target=method(arg(3),1),token=bridge.next_transfer++,scratch=scratch_for(12);
        bridge.uia_providers.emplace(arg(3),AccessibleState::UiaProvider{arg(0),token});
        memory.store(GuestAddress{scratch+16},64,token);
        start_callbacks(state,memory,{{target,{arg(3)}}},Continuation::AccessibleOperation,scratch);return {};
    }
    if(name=="UiaClientsAreListening"){
        // 本後端的原生輔助使用訂閱由 macOS 管理；不虛構 Windows client 訂閱。
        return finish(0);
    }
    if(name=="UiaRaiseNotificationEvent"){
        // NotificationKind 0–4、NotificationProcessing 0–4；顯示字串為 BSTR，activityId 可為 NULL。
        if(!arg(0) || static_cast<std::uint32_t>(arg(1))>4 || static_cast<std::uint32_t>(arg(2))>4 || !arg(3))return finish(invalid);
        const auto length=memory.load(GuestAddress{arg(3)-4},32);if(length%2 || length>65536)return finish(invalid);
        std::u16string text(length/2,u'\0');for(std::size_t i=0;i<text.size();++i)text[i]=static_cast<char16_t>(memory.load(GuestAddress{arg(3)+i*2},16));
        if(arg(4))(void)read_utf16(memory,GuestAddress{arg(4)},1024);
        const auto found=bridge.uia_providers.find(arg(0));if(found==bridge.uia_providers.end())return finish(unavailable);
        if(!gui_.windows.contains(found->second.window))return finish(unavailable);
        if(!native_gui_ || !native_gui_->accessibility_announcement(found->second.window,text,static_cast<std::uint32_t>(arg(2))<=1))return finish(0x80070032U);
        return finish(0);
    }
    if(name.starts_with("UiaRaise")){
        if(!arg(0))return finish(invalid);
        std::uint32_t event=static_cast<std::uint32_t>(arg(1)),property=0;
        if(name=="UiaRaiseAutomationEvent"){
            if(event<20000 || event>29999)return finish(invalid);
        }else if(name=="UiaRaiseAutomationPropertyChangedEvent"){
            property=event;event=20004;if(property<30000 || property>39999)return finish(invalid);
            // 兩個 VARIANT 由 ABI 按值傳入；必須驗證完整寬度，而非讀宿主指標。
            for(unsigned i=2;i<4;++i){if(!arg(i))return finish(pointer_error);memory.check(GuestAddress{arg(i)},variant_size,Access::Read);}
        }else{
            if(event>5)return finish(invalid);event=20002;
            const auto count=static_cast<std::int32_t>(arg(3));if(count<0 || count>65536 || (count && !arg(2)))return finish(invalid);
            if(count)memory.check(GuestAddress{arg(2)},static_cast<std::size_t>(count)*4,Access::Read);
        }
        const auto found=bridge.uia_providers.find(arg(0));if(found==bridge.uia_providers.end())return finish(unavailable);
        if(!gui_.windows.contains(found->second.window))return finish(unavailable);
        if(!native_gui_ || !native_gui_->accessibility_event(found->second.window,event,property))return finish(0x80070032U);
        return finish(0);
    }
    const auto found=bridge.uia_objects.find(arg(0));if(found==bridge.uia_objects.end())return finish(pointer_error);
    auto& object=found->second;
    if(name=="UIA.QueryInterface"){
        if(!arg(2))return finish(pointer_error);put(arg(2),0);if(!arg(1))return finish(pointer_error);
        const auto iid=guid(arg(1));if(iid!=unknown_iid && (object.id || iid!=uia_simple_iid))return finish(no_interface);
        ++object.references;put(arg(2),arg(0));return finish(0);
    }
    if(name=="UIA.AddRef")return finish(++object.references);
    if(name=="UIA.Release"){
        const auto refs=--object.references;
        if(!refs){bridge.uia_objects.erase(found);(void)free_heap(memory,arg(0),state);}return finish(refs);
    }
    if(object.id)return finish(no_interface);
    if(!gui_.windows.contains(object.window))return finish(unavailable);
    if(name=="UIA.ProviderOptions"){
        if(!arg(1))return finish(pointer_error);memory.store(GuestAddress{arg(1)},32,2);return finish(0);
    }
    if(name=="UIA.HostRawElementProvider"){
        if(!arg(1))return finish(pointer_error);put(arg(1),0);return finish(0);
    }
    if(name=="UIA.GetPatternProvider"){
        if(!arg(2))return finish(pointer_error);put(arg(2),0);return finish(0);
    }
    if(name=="UIA.GetPropertyValue"){
        if(!arg(2))return finish(pointer_error);variant(arg(2),0,0);
        // 預設 HWND provider 的資料來自共用 Win32 視窗模型。
        const auto& window=gui_.windows.at(object.window);const auto property=static_cast<std::uint32_t>(arg(1));
        if(property==30005 || property==30012){variant(arg(2),8,0);put(arg(2)+8,bstr(property==30005?window.title:window.class_name));}
        else if(property==30002 || property==30020)variant(arg(2),3,property==30002?process_id():object.window);
        else if(property==30008 || property==30009 || property==30010 || property==30022)
            variant(arg(2),11,(property==30008?gui_.focus==object.window:property==30009?bool(window.style&0x10000U):property==30010?window.enabled:!window.visible)?0xffff:0);
        return finish(0);
    }
    return finish(not_implemented);
}
