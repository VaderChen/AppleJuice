// Windows.Gaming.Input.Gamepad：與 DirectInput／XInput 共用裝置、輸入及震動後端。
// ABI 依 Windows SDK 的 windows.gaming.input.h；本檔在 DirectInputState 定義後引入。
std::uint64_t WindowsServices::winrt_gamepad_view(GuestState&,GuestMemory& memory,const std::shared_ptr<WinrtGamepadObject>& object,unsigned kind){
    auto& bridge=dinput_->winrt_gamepads;
    if(const auto it=object->interfaces.find(kind);it!=object->interfaces.end())return it->second;
    const auto bits=static_cast<std::uint8_t>(x86_?32:64);const unsigned unit=bits/8;
    if(!bridge.tables.contains(kind)){
        const auto methods=winrt_gamepad::slots(kind,x86_);const auto table=allocate_heap(memory,methods.size()*unit);if(!table)throw std::bad_alloc{};
        for(unsigned i=0;i<methods.size();++i)memory.store(GuestAddress{table+i*unit},bits,resolve("internal-dinput",methods[i]).value);
        bridge.tables.emplace(kind,table);
    }
    const auto address=allocate_heap(memory,unit);if(!address)throw std::bad_alloc{};
    memory.store(GuestAddress{address},bits,bridge.tables.at(kind));bridge.views.emplace(address,WinrtGamepadBridge::Interface{object,kind});
    object->interfaces.emplace(kind,address);if(!object->canonical)object->canonical=address;return address;
}
std::uint32_t WindowsServices::release_winrt_gamepad(GuestState& state,GuestMemory& memory,const std::shared_ptr<WinrtGamepadObject>& object){
    if(!object->references)throw HostCallFault("WinRT Gamepad 重複釋放");
    if(--object->references)return object->references;
    auto items=std::move(object->items);
    for(const auto& [kind,address]:object->interfaces){(void)kind;dinput_->winrt_gamepads.views.erase(address);(void)free_heap(memory,address,state);}
    object->interfaces.clear();object->canonical=0;
    for(const auto& item:items)(void)release_winrt_gamepad(state,memory,item);
    return 0;
}
std::optional<std::uint32_t> WindowsServices::activate_winrt_gamepad(GuestState& state,GuestMemory& memory,std::u16string_view name,bool activate,std::uint64_t iid_pointer,std::uint64_t output){
    if(name!=u"Windows.Gaming.Input.Gamepad")return {};
    using namespace winrt_gamepad;
    if(!runtime_.network.external_io)return 0x80070032U;
    if(activate)return 0x8000000eU; // E_ILLEGAL_METHOD_CALL：Gamepad 由裝置列舉取得，沒有公開建構子。
    prepare_input_backend();if(!input_backend())return 0x80070032U;
    input::Guid iid{};memory.read(GuestAddress{iid_pointer},iid);
    unsigned kind=Statics;
    if(iid==iid_statics2)kind=Statics2;else if(iid==iid_activation)kind=Activation;
    else if(iid!=iid_statics && iid!=iid_inspectable && iid!=iid_agile && iid!=dinput::unknown)return 0x80004002U;
    auto& bridge=dinput_->winrt_gamepads;
    if(!bridge.factory)bridge.factory=std::make_shared<WinrtGamepadObject>(); // 系統工廠持有一個程序內引用。
    poll_winrt_gamepads(state,memory,true);
    if(bridge.factory->references==UINT32_MAX)return 0x8007000eU;
    const auto address=winrt_gamepad_view(state,memory,bridge.factory,kind);++bridge.factory->references;
    memory.store(GuestAddress{output},x86_?32:64,address);
    return 0;
}
void WindowsServices::poll_winrt_gamepads(const GuestState& origin,GuestMemory& memory,bool force){
    if(!dinput_ || !dinput_->backend || !dinput_->winrt_gamepads.factory)return;
    using namespace winrt_gamepad;
    auto& bridge=dinput_->winrt_gamepads;
    const auto now=detail::monotonic_ticks();
    if(!force && bridge.polled && now-bridge.polled<20000)return;
    bridge.polled=now;
    auto state=origin;
    const auto pads=dinput_->backend->gamepads();
    const auto notify=[&](const std::shared_ptr<WinrtGamepadObject>& pad,unsigned kind){
        for(const auto& [token,subscription]:bridge.subscriptions){
            (void)token;if(subscription->kind!=kind || subscription->removed)continue;
            const auto event=bridge.next_event++;++subscription->pending;++pad->references;
            bridge.pending.emplace(event,WinrtGamepadBridge::Event{subscription,pad});
        }
    };
    for(const auto& pad:pads)if(!bridge.pads.contains(pad.device)){
        auto object=std::make_shared<WinrtGamepadObject>();object->kind=WinrtGamepadObject::Kind::Pad;object->device=pad.device;
        (void)winrt_gamepad_view(state,memory,object,Gamepad);bridge.pads.emplace(pad.device,object);
        if(bridge.initialized)notify(object,0);
    }
    for(auto it=bridge.pads.begin();it!=bridge.pads.end();){
        if(std::any_of(pads.begin(),pads.end(),[&](const auto& item){return item.device==it->first;})){++it;continue;}
        auto object=it->second;notify(object,1);dinput_->gamepad_vibrations.erase(it->first);it=bridge.pads.erase(it);(void)release_winrt_gamepad(state,memory,object);
    }
    bridge.initialized=true;
    for(auto& [event,pending]:bridge.pending){
        if(pending.dispatched)continue;
        GuestCallback callback{resolve("internal-dinput","Winrt.Dispatch").value,{event}};callback.wide_arguments=1;
        if(queue_pool_callback(state,memory,std::move(callback)))break; // 執行緒池暫滿時保留，下一次輪詢重試。
        pending.dispatched=true;
    }
}
std::optional<std::uint32_t> WindowsServices::invoke_winrt_gamepad(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    return service_callback::invoke<std::optional<std::uint32_t>>(
        this,threads_.current,state,memory,service.symbol,
        [this](GuestState& s,GuestMemory& m,std::uint64_t target,std::vector<std::uint64_t> args,std::uint64_t id){start_callbacks(s,m,{{target,std::move(args)}},Continuation::NestedResult,id);},
        [this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            using namespace winrt_gamepad;
            const WindowsCall call(s,m,service);const auto name=service.symbol;const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
            const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
            constexpr std::uint32_t pointer_error=0x80004003U,invalid_argument=0x80070057U,bounds=0x8000000bU;
            const auto output=[&](std::uint64_t address,std::size_t size){if(!address)throw MemoryFault(GuestAddress{},Access::Write,size,"WinRT 輸出指標為空");m.check(GuestAddress{address},size,Access::Write);};
            const auto method=[&](std::uint64_t address,unsigned slot){const auto table=m.load(GuestAddress{address},bits);return m.load(GuestAddress{table+slot*unit},bits);};
            if(!dinput_)co_return finish(pointer_error);
            auto& bridge=dinput_->winrt_gamepads;
            try{
                if(name=="Winrt.Dispatch"){
                    const auto event=bridge.pending.find(call(0));if(event==bridge.pending.end())co_return finish(0);
                    const auto id=event->first;const auto subscription=event->second.subscription;const auto pad=event->second.pad;
                    const auto address=winrt_gamepad_view(s,m,pad,Gamepad);
                    // IEventHandler<Gamepad>::Invoke(this, sender, args)，系統靜態事件的 sender 為 NULL。
                    (void)co_await service_callback::call(method(subscription->handler,3),{subscription->handler,0,address});
                    bridge.pending.erase(id);(void)release_winrt_gamepad(s,m,pad);
                    if(!subscription->pending)throw HostCallFault("WinRT Gamepad 事件引用不平衡");
                    --subscription->pending;
                    if(subscription->removed && !subscription->pending){
                        (void)release_winrt_gamepad(s,m,subscription->owner);
                        (void)co_await service_callback::call(method(subscription->handler,2),{subscription->handler});
                    }
                    co_return finish(0);
                }
                const auto found=bridge.views.find(call(0));if(found==bridge.views.end())co_return finish(pointer_error);
                const auto object=found->second.object;const auto view=found->second.kind;
                const auto retain=[&](const std::shared_ptr<WinrtGamepadObject>& item,unsigned kind){
                    if(item->references==UINT32_MAX)throw std::bad_alloc{};
                    const auto address=winrt_gamepad_view(s,m,item,kind);++item->references;return address;
                };
                const auto interfaces=[&]{
                    switch(object->kind){
                    case WinrtGamepadObject::Kind::Factory:return std::vector<std::pair<Guid,unsigned>>{{iid_statics,Statics},{iid_statics2,Statics2},{iid_activation,Activation}};
                    case WinrtGamepadObject::Kind::Pad:return std::vector<std::pair<Guid,unsigned>>{{iid_gamepad,Gamepad},{iid_gamepad2,Gamepad2},{iid_controller,Controller}};
                    case WinrtGamepadObject::Kind::Vector:return std::vector<std::pair<Guid,unsigned>>{{iid_vector,Vector},{iid_iterable,Iterable}};
                    case WinrtGamepadObject::Kind::Iterator:return std::vector<std::pair<Guid,unsigned>>{{iid_iterator,Iterator}};
                    }throw HostCallFault("WinRT Gamepad 物件種類無效");
                };
                if(name=="Winrt.QueryInterface"){
                    output(call(2),unit);m.store(GuestAddress{call(2)},bits,0);if(!call(1))co_return finish(pointer_error);
                    Guid iid{};m.read(GuestAddress{call(1)},iid);unsigned kind=UINT32_MAX;
                    if(iid==dinput::unknown || iid==iid_inspectable || iid==iid_agile){
                        for(const auto& [key,address]:object->interfaces)if(address==object->canonical){kind=key;break;}
                    }else for(const auto& [id,key]:interfaces())if(iid==id){kind=key;break;}
                    if(kind==UINT32_MAX)co_return finish(no_interface);
                    m.store(GuestAddress{call(2)},bits,retain(object,kind));co_return finish(0);
                }
                if(name=="Winrt.AddRef"){if(object->references==UINT32_MAX)co_return finish(UINT32_MAX);co_return finish(++object->references);}
                if(name=="Winrt.Release")co_return finish(release_winrt_gamepad(s,m,object));
                if(name=="Winrt.GetIids"){
                    output(call(1),4);output(call(2),unit);m.store(GuestAddress{call(1)},32,0);m.store(GuestAddress{call(2)},bits,0);
                    const auto ids=interfaces();const auto data=allocate_heap(m,ids.size()*16);if(!data)throw std::bad_alloc{};
                    for(unsigned i=0;i<ids.size();++i)m.write(GuestAddress{data+i*16},ids[i].first);
                    m.store(GuestAddress{call(1)},32,ids.size());m.store(GuestAddress{call(2)},bits,data);co_return finish(0);
                }
                if(name=="Winrt.GetRuntimeClassName"){
                    output(call(1),unit);m.store(GuestAddress{call(1)},bits,0);
                    const std::u16string text=object->kind==WinrtGamepadObject::Kind::Vector?u"AppleJuice.Collections.GamepadVectorView":object->kind==WinrtGamepadObject::Kind::Iterator?u"AppleJuice.Collections.GamepadIterator":u"Windows.Gaming.Input.Gamepad";
                    const auto bytes=utf16_bytes(text);const auto handle=allocate_heap(m,24+bytes.size());if(!handle)throw std::bad_alloc{};
                    m.write(GuestAddress{handle+24},bytes);runtime_.winrt_strings.emplace(handle,WinrtString{handle+24,static_cast<std::uint32_t>(text.size()),1});
                    m.store(GuestAddress{call(1)},bits,handle);co_return finish(0);
                }
                if(name=="Winrt.GetTrustLevel"){output(call(1),4);m.store(GuestAddress{call(1)},32,0);co_return finish(0);}
                if(name=="Winrt.ActivateInstance"){output(call(1),unit);m.store(GuestAddress{call(1)},bits,0);co_return finish(0x8000000eU);}
                const bool add=name=="Winrt.Added" || name=="Winrt.Removed" || name=="Winrt.HeadsetConnected" || name=="Winrt.HeadsetDisconnected" || name=="Winrt.UserChanged";
                const bool remove=name=="Winrt.RemoveAdded" || name=="Winrt.RemoveRemoved" || name=="Winrt.RemoveHeadsetConnected" || name=="Winrt.RemoveHeadsetDisconnected" || name=="Winrt.RemoveUserChanged";
                const auto event_kind=[&]()->unsigned{
                    if(name=="Winrt.Added" || name=="Winrt.RemoveAdded")return 0;
                    if(name=="Winrt.Removed" || name=="Winrt.RemoveRemoved")return 1;
                    if(name=="Winrt.HeadsetConnected" || name=="Winrt.RemoveHeadsetConnected")return 2;
                    if(name=="Winrt.HeadsetDisconnected" || name=="Winrt.RemoveHeadsetDisconnected")return 3;
                    return 4;
                };
                if(add){
                    output(call(2),8);m.store(GuestAddress{call(2)},64,0);if(!call(1))co_return finish(pointer_error);
                    // 註冊前先取得清單，避免把既有手把誤報為新插入。
                    poll_winrt_gamepads(s,m,true);
                    auto subscription=std::make_shared<WinrtGamepadSubscription>();subscription->handler=call(1);subscription->kind=event_kind();subscription->owner=object;
                    const auto target=call(2);
                    (void)co_await service_callback::call(method(subscription->handler,1),{subscription->handler});
                    subscription->token=bridge.next_token++;++object->references;bridge.subscriptions.emplace(subscription->token,subscription);
                    m.store(GuestAddress{target},64,subscription->token);co_return finish(0);
                }
                if(remove){
                    const auto it=bridge.subscriptions.find(call(1));
                    if(it==bridge.subscriptions.end() || it->second->kind!=event_kind() || it->second->owner!=object)co_return finish(0);
                    const auto subscription=it->second;bridge.subscriptions.erase(it);subscription->removed=true;
                    if(!subscription->pending){(void)release_winrt_gamepad(s,m,object);(void)co_await service_callback::call(method(subscription->handler,2),{subscription->handler});}
                    co_return finish(0);
                }
                if(name=="Winrt.Gamepads"){
                    output(call(1),unit);m.store(GuestAddress{call(1)},bits,0);poll_winrt_gamepads(s,m,true);
                    auto vector=std::make_shared<WinrtGamepadObject>();vector->kind=WinrtGamepadObject::Kind::Vector;
                    vector->items.reserve(bridge.pads.size());
                    for(const auto& [id,pad]:bridge.pads){(void)id;++pad->references;vector->items.push_back(pad);}
                    try{m.store(GuestAddress{call(1)},bits,winrt_gamepad_view(s,m,vector,Vector));}
                    catch(...){(void)release_winrt_gamepad(s,m,vector);throw;}co_return finish(0);
                }
                if(name=="Winrt.FromController"){
                    output(call(2),unit);m.store(GuestAddress{call(2)},bits,0);if(!call(1))co_return finish(invalid_argument);
                    if(const auto it=bridge.views.find(call(1));it!=bridge.views.end() && it->second.object->kind==WinrtGamepadObject::Kind::Pad)
                        m.store(GuestAddress{call(2)},bits,retain(it->second.object,Gamepad));
                    else {
                        const auto scratch=allocate_heap(m,16+unit);if(!scratch)throw std::bad_alloc{};m.write(GuestAddress{scratch},iid_gamepad);m.store(GuestAddress{scratch+16},bits,0);
                        const auto destination=call(2),controller=call(1);
                        const auto result=static_cast<std::uint32_t>(co_await service_callback::call(method(controller,0),{controller,scratch,scratch+16}));
                        if(!(result&0x80000000U))m.store(GuestAddress{destination},bits,m.load(GuestAddress{scratch+16},bits));
                        (void)free_heap(m,scratch,s);co_return finish(result==no_interface?0:result);
                    }
                    co_return finish(0);
                }
                if(name=="Winrt.GetAt" || name=="Winrt.Current"){
                    const auto index=name=="Winrt.Current"?object->cursor:static_cast<std::size_t>(static_cast<std::uint32_t>(call(1)));const auto target=call(name=="Winrt.Current"?1:2);
                    output(target,unit);m.store(GuestAddress{target},bits,0);if(index>=object->items.size())co_return finish(bounds);
                    m.store(GuestAddress{target},bits,retain(object->items[index],Gamepad));co_return finish(0);
                }
                if(name=="Winrt.Size"){output(call(1),4);m.store(GuestAddress{call(1)},32,object->items.size());co_return finish(0);}
                if(name=="Winrt.IndexOf"){
                    output(call(2),4);output(call(3),1);m.store(GuestAddress{call(2)},32,0);m.store(GuestAddress{call(3)},8,0);
                    const auto candidate=bridge.views.find(call(1));
                    if(candidate!=bridge.views.end())for(std::size_t i=0;i<object->items.size();++i)if(object->items[i]==candidate->second.object){m.store(GuestAddress{call(2)},32,i);m.store(GuestAddress{call(3)},8,1);break;}
                    co_return finish(0);
                }
                if(name=="Winrt.First"){
                    output(call(1),unit);m.store(GuestAddress{call(1)},bits,0);auto iterator=std::make_shared<WinrtGamepadObject>();iterator->kind=WinrtGamepadObject::Kind::Iterator;iterator->items=object->items;
                    for(const auto& item:iterator->items)++item->references;
                    try{m.store(GuestAddress{call(1)},bits,winrt_gamepad_view(s,m,iterator,Iterator));}
                    catch(...){(void)release_winrt_gamepad(s,m,iterator);throw;}co_return finish(0);
                }
                if(name=="Winrt.HasCurrent" || name=="Winrt.MoveNext"){
                    output(call(1),1);if(name=="Winrt.MoveNext" && object->cursor<object->items.size())++object->cursor;
                    m.store(GuestAddress{call(1)},8,object->cursor<object->items.size());co_return finish(0);
                }
                if(name=="Winrt.GetMany" || name=="Winrt.IteratorGetMany"){
                    const bool iterator=name=="Winrt.IteratorGetMany";const auto start=iterator?object->cursor:static_cast<std::size_t>(static_cast<std::uint32_t>(call(1)));
                    const auto capacity=static_cast<std::uint32_t>(call(iterator?1:2));const auto data=call(iterator?2:3),actual=call(iterator?3:4);
                    output(actual,4);m.store(GuestAddress{actual},32,0);if(start>object->items.size())co_return finish(bounds);
                    const auto count=std::min<std::size_t>(capacity,object->items.size()-start);if(capacity)output(data,static_cast<std::size_t>(capacity)*unit);
                    for(std::size_t i=0;i<count;++i)m.store(GuestAddress{data+i*unit},bits,retain(object->items[start+i],Gamepad));
                    if(iterator)object->cursor+=count;m.store(GuestAddress{actual},32,count);co_return finish(0);
                }
                if(object->kind!=WinrtGamepadObject::Kind::Pad)co_return finish(no_interface);
                if(name=="Winrt.Headset" || name=="Winrt.User"){
                    // 後端未提供與 Windows User／Headset 的關聯，這兩個可空屬性因此為 NULL。
                    output(call(1),unit);m.store(GuestAddress{call(1)},bits,0);co_return finish(0);
                }
                const auto pads=dinput_->backend->gamepads();const auto pad=std::find_if(pads.begin(),pads.end(),[&](const auto& item){return item.device==object->device;});
                if(pad==pads.end())co_return finish(0x8007048fU);
                if(name=="Winrt.IsWireless"){
                    output(call(1),1);if(!pad->wireless)co_return finish(0x80070032U);
                    m.store(GuestAddress{call(1)},8,*pad->wireless);co_return finish(0);
                }
                if(name=="Winrt.GetVibration"){
                    output(call(1),32);const auto vibration=dinput_->gamepad_vibrations.find(object->device);
                    for(unsigned i=0;i<4;++i)m.store(GuestAddress{call(1)+i*8},64,std::bit_cast<std::uint64_t>(vibration==dinput_->gamepad_vibrations.end()?0.0:vibration->second[i]));co_return finish(0);
                }
                if(name=="Winrt.PutVibration" || name=="Winrt.PutVibration32"){
                    const auto data=x86_?s.reg(Gpr::Rsp)+8:call(1);m.check(GuestAddress{data},32,Access::Read);std::array<double,4> values;std::array<std::uint16_t,4> motors;
                    for(unsigned i=0;i<4;++i){values[i]=std::bit_cast<double>(m.load(GuestAddress{data+i*8},64));if(!std::isfinite(values[i]) || values[i]<0 || values[i]>1)co_return finish(invalid_argument);motors[i]=static_cast<std::uint16_t>(std::lround(values[i]*65535.0));}
                    const auto error=dinput_->backend->vibrate(object->device,motors);if(error)co_return finish(0x80070000U|(error&65535U));
                    dinput_->gamepad_vibrations[object->device]=values;co_return finish(0);
                }
                if(name=="Winrt.Reading"){
                    output(call(1),64);const auto data=call(1);m.write(GuestAddress{data},std::array<std::uint8_t,64>{});
                    m.store(GuestAddress{data},64,detail::monotonic_ticks()/10);std::uint32_t buttons=0;
                    static constexpr std::array<std::pair<std::uint16_t,std::uint32_t>,14> mapping{{{0x10,1},{0x20,2},{0x1000,4},{0x2000,8},{0x4000,16},{0x8000,32},{1,64},{2,128},{4,256},{8,512},{0x100,1024},{0x200,2048},{0x40,4096},{0x80,8192}}};
                    for(const auto& [source,target]:mapping)if(pad->buttons&pad->supported_buttons&source)buttons|=target;
                    m.store(GuestAddress{data+8},32,buttons);
                    for(unsigned i=0;i<6;++i){const bool trigger=i<2;const bool supported=trigger?pad->supported_triggers[i]:pad->supported_sticks[i-2];const double source=trigger?pad->triggers[i]:pad->sticks[i-2];const auto value=supported && std::isfinite(source)?std::clamp(source,trigger?0.0:-1.0,1.0):0.0;m.store(GuestAddress{data+16+i*8},64,std::bit_cast<std::uint64_t>(value));}
                    co_return finish(0);
                }
                if(name=="Winrt.ButtonLabel"){
                    output(call(2),4);const auto button=static_cast<std::uint32_t>(call(1));static constexpr std::array<std::uint32_t,14> labels{3,4,9,10,11,12,5,6,7,8,13,16,15,18};
                    const auto index=button?static_cast<unsigned>(std::countr_zero(button)):32U;const auto label=button && std::has_single_bit(button) && index<labels.size()?labels[index]:0U;
                    m.store(GuestAddress{call(2)},32,label);co_return finish(0);
                }
                (void)view;co_return finish(no_interface);
            }catch(const MemoryFault&){co_return finish(pointer_error);}catch(const std::bad_alloc&){co_return finish(0x8007000eU);}
        });
}

