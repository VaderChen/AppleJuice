#include "x64arm/windows_services.hpp"
#include "x64arm/windows_keyboard.hpp"
#include "x64arm/windows_pointer.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <bit>

namespace x64arm {
// LockWindowUpdate、ActivateKeyboardLayout 與觸控注入：共用 GUI 狀態、輸入佇列與 WM_POINTER 管線。
namespace {
// 注入觸控裝置的固定識別；不對應任何宿主硬體。
constexpr input::Guid injected_touch_device{'x','6','4','a','r','m','-','t','o','u','c','h','-','i','n','j'};
// CreateSyntheticPointerDevice 的識別：固定前綴加上裝置控制代碼。
input::Guid synthetic_identity(std::uint64_t handle){
    input::Guid id{'x','6','4','a','r','m','-','s','y','n'};for(unsigned i=0;i<6;++i)id[10+i]=static_cast<std::uint8_t>(handle>>(i*8));return id;
}
}
// 系統預設、宿主輸入法與本程序載入的配置；GetKeyboardLayoutList、Activate／Load／Unload 共用同一份清單。
std::vector<std::uint64_t> WindowsServices::keyboard_layouts() const {
    std::vector<std::uint64_t> layouts{0x04090409};
    const auto add=[&](std::uint64_t layout){if(std::find(layouts.begin(),layouts.end(),layout)==layouts.end())layouts.push_back(layout);};
    if(native_gui_)for(const auto& method:native_input_methods())add(method.layout);
    for(const auto& layout:gui_.loaded_keyboard_layouts)add(layout.first);
    return layouts;
}
std::span<const WindowsServiceDescriptor> user_session_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"user32.dll","LockWindowUpdate",1},{"user32.dll","ActivateKeyboardLayout",2},{"user32.dll","LoadKeyboardLayoutA",2},{"user32.dll","LoadKeyboardLayoutW",2},{"user32.dll","UnloadKeyboardLayout",1},
        {"user32.dll","LoadKeyboardLayoutEx",3},{"win32u.dll","NtUserLoadKeyboardLayoutEx",8},
        {"user32.dll","InitializeTouchInjection",2},{"user32.dll","InjectTouchInput",2},
        {"user32.dll","CreateSyntheticPointerDevice",3,0,0,7},{"user32.dll","InjectSyntheticPointerInput",3,0,0,4},{"user32.dll","DestroySyntheticPointerDevice",1}
    };return table;
}
bool WindowsServices::window_update_locked(std::uint64_t handle)const{
    for(unsigned depth=0;handle && depth<256;++depth){
        if(handle==gui_.update_lock)return true;const auto found=gui_.windows.find(handle);if(found==gui_.windows.end())return false;handle=found->second.parent;
    }
    return false;
}
std::uint64_t WindowsServices::selected_keyboard_layout(std::uint64_t thread)const{
    if(const auto found=gui_.thread_keyboard_layouts.find(thread);found!=gui_.thread_keyboard_layouts.end())return found->second;
    return gui_.process_keyboard_layout;
}
std::optional<std::uint32_t> WindowsServices::invoke_user_session(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;
    const auto u32=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    std::uint64_t result=0;const auto fail=[&](std::uint32_t error){last_error_=error;result=0;};
    // InjectTouchInput 與 InjectSyntheticPointerInput 共用：驗證 POINTER_TOUCH_INFO／POINTER_PEN_INFO 後送入同一條 WM_POINTER 管線。
    // stride 是每個元素大小，header 是 POINTER_INFO 在元素內的偏移（POINTER_TYPE_INFO 的聯集在 8）。
    const auto inject_pointer=[&](const PointerDeviceState& entry,std::uint64_t base,std::uint32_t count,unsigned stride,unsigned header,std::uint32_t pointer_type)->bool{
        const bool pen=pointer_type==3;const unsigned info=header+(x86_?88U:96U),points=header+(x86_?24U:32U);
        if(!count || count>entry.device.max_contacts || !base){fail(87);return false;}
        memory.check(GuestAddress{base},std::size_t{stride}*count,Access::Read);
        if(!interactive_station()){fail(5);return false;}
        std::vector<input::PointerSample> samples;std::vector<std::uint32_t> seen;const auto& rect=entry.device.logical_rectangle;
        for(std::uint32_t i=0;i<count;++i){
            const auto at=base+std::uint64_t{i}*stride;const auto load=[&](unsigned offset){return static_cast<std::uint32_t>(memory.load(GuestAddress{at+offset},32));};
            const auto id=load(header+4),flags=load(header+12);
            const bool down=flags&0x10000U,update=flags&0x20000U,up=flags&0x40000U,range=flags&2U,contact=flags&4U;
            // DOWN 須帶 INRANGE|INCONTACT。觸控：UPDATE 帶 INRANGE、UP 單獨出現；
            // 筆：可懸停（UPDATE|INRANGE）、離開範圍（只有 UPDATE）、抬起後懸停（UP|INRANGE），接觸時必在範圍內。
            bool valid=load(header)==pointer_type && (!header || load(0)==pointer_type) && id<entry.device.max_contacts && std::find(seen.begin(),seen.end(),id)==seen.end() &&
                (int(down)+int(update)+int(up))==1 && (!down || (range && contact)) && !(flags&~0x7f6fffU) &&
                (pen?(!contact || range) && (!up || !contact):(!update || range) && (!up || !(range || contact)));
            input::PointerSample sample;sample.device=entry.device.device;sample.contact=id;
            sample.x=std::bit_cast<std::int32_t>(load(points));sample.y=std::bit_cast<std::int32_t>(load(points+4));
            if(sample.x<rect[0] || sample.y<rect[1] || sample.x>rect[2] || sample.y>rect[3])valid=false;
            sample.in_range=range;sample.in_contact=contact;const auto mask=load(info+4);sample.mask=mask;
            if(pen){
                // POINTER_PEN_INFO：penFlags（BARREL、INVERTED、ERASER）、penMask（PRESSURE、ROTATION、TILT_X、TILT_Y）。
                const auto pen_flags=load(info);sample.barrel=pen_flags&1U;sample.inverted=pen_flags&2U;sample.eraser=pen_flags&4U;
                if((pen_flags&~7U) || (mask&~15U))valid=false;
                if(mask&1U)sample.pressure=load(info+8);if(mask&2U)sample.rotation=load(info+12);
                if(mask&4U)sample.tilt_x=std::bit_cast<std::int32_t>(load(info+16));if(mask&8U)sample.tilt_y=std::bit_cast<std::int32_t>(load(info+20));
            }else {
                // POINTER_TOUCH_INFO：touchMask（CONTACTAREA、ORIENTATION、PRESSURE）；rcContact@8、orientation@40、pressure@44。
                if(mask&~7U)valid=false;
                if(mask&4U)sample.pressure=load(info+44);if(mask&2U)sample.rotation=load(info+40);
                if(mask&1U)for(unsigned j=0;j<4;++j)sample.contact_rectangle[j]=std::bit_cast<std::int32_t>(load(info+8+j*4));
            }
            if(sample.pressure>1024 || sample.rotation>359 || sample.tilt_x<-90 || sample.tilt_x>90 || sample.tilt_y<-90 || sample.tilt_y>90)valid=false;
            if(!valid){fail(87);return false;}
            seen.push_back(id);samples.push_back(sample);
        }
        for(const auto& sample:samples)deliver_pointer_sample(sample);
        return true;
    };
    // 重新初始化觸控注入與 DestroySyntheticPointerDevice 共用：移除裝置與其接觸點，佇列中的封包一併撤回，之後的查詢不再引用已刪除的裝置。
    const auto retire_pointer_device=[&](std::uint64_t handle){
        auto& bridge=*pointer_input_;const auto found=bridge.devices.find(handle);if(found==bridge.devices.end())return;
        std::erase_if(bridge.current,[&](const auto& item){return item.second.device==handle;});
        std::erase_if(bridge.packets,[&](const auto& item){return item.second.device==handle;});
        std::erase_if(bridge.delivered,[&](const auto& item){return !bridge.packets.contains(item.second);});
        std::erase_if(gui_.queue,[&](const auto& message){return message.pointer_packet && !bridge.packets.contains(message.pointer_packet);});
        bridge.identities.erase(found->second.device.device);bridge.devices.erase(found);
    };
    try{
        if(symbol=="LockWindowUpdate"){
            if(!arg(0)){
                // 解除鎖定：重新呈現整個視窗樹，並讓客體重新繪製被擋下的內容。
                const auto locked=gui_.update_lock;if(!locked){arg.return_to(state,0);return std::nullopt;}
                gui_.update_lock=0;
                if(gui_.windows.contains(locked)){
                    std::vector<std::uint64_t> tree;
                    for(const auto& [handle,window]:gui_.windows){(void)window;std::uint64_t cursor=handle;for(unsigned depth=0;cursor && depth<256;++depth){if(cursor==locked){tree.push_back(handle);break;}cursor=gui_.windows.at(cursor).parent;if(cursor && !gui_.windows.contains(cursor))break;}}
                    for(const auto handle:tree){invalidate_window(handle);if(gui_.windows.at(handle).redraw)present_native_window(handle,gui_.windows.at(handle));}
                }
                result=1;
            }else {
                const auto found=gui_.windows.find(arg(0));
                if(found==gui_.windows.end())fail(1400);
                else if(gui_.update_lock && gui_.update_lock!=arg(0))fail(5); // 同時只允許鎖定一個視窗。
                else {gui_.update_lock=arg(0);result=1;}
            }
        }else if(symbol.starts_with("LoadKeyboardLayout") || symbol=="NtUserLoadKeyboardLayoutEx"){
            const bool native=symbol=="NtUserLoadKeyboardLayoutEx",extended=symbol=="LoadKeyboardLayoutEx";
            const auto previous=native?arg(4):extended?arg(0):0;
            const auto flags=u32(native?7:extended?2:1);
            std::u16string text;
            if(native){
                // 原生入口接收計數字串，Buffer 不必以 NUL 結尾。
                const auto descriptor=arg(5);
                if(!descriptor || memory.load(GuestAddress{descriptor},16)!=16 ||
                   memory.load(GuestAddress{descriptor+2},16)<16){fail(87);arg.return_to(state,0);return std::nullopt;}
                const auto buffer=memory.load(GuestAddress{descriptor+(x86_?4U:8U)},x86_?32:64);
                for(unsigned i=0;i<8;++i)text.push_back(static_cast<char16_t>(memory.load(GuestAddress{buffer+2*i},16)));
                // 客製鍵盤檔及表格需由鍵盤轉譯後端解析。
                if(arg(0) || u32(1) || u32(2) || arg(3)){fail(50);arg.return_to(state,0);return std::nullopt;}
            }else{
                if(extended && !previous){arg.return_to(state,0);return std::nullopt;}
                const auto buffer=arg(extended?1:0);
                if(buffer)text=symbol.ends_with('A')?utf8_to_utf16(read_ansi(memory,buffer,16)):read_utf16(memory,GuestAddress{buffer},16);
            }
            std::uint32_t identifier=0;bool valid=text.size()==8;
            for(const auto c:text){const int digit=c>=u'0' && c<=u'9'?c-u'0':c>=u'a' && c<=u'f'?c-u'a'+10:c>=u'A' && c<=u'F'?c-u'A'+10:-1;if(digit<0)valid=false;identifier=identifier*16+static_cast<unsigned>(digit<0?0:digit);}
            if(!valid || !(identifier&0xffffU)){fail(87);arg.return_to(state,0);return std::nullopt;}
            const std::uint32_t layout=native && u32(6)?u32(6):
                ((identifier>>16)?(0xf000U|(identifier>>16))<<16|(identifier&0xffffU):(identifier&0xffffU)*0x10001U);
            auto& loaded=gui_.loaded_keyboard_layouts;
            const auto known=keyboard_layouts();
            if(previous && std::find(known.begin(),known.end(),previous)==known.end()){
                fail(87);arg.return_to(state,0);return std::nullopt;
            }
            if(std::find(known.begin(),known.end(),layout)==known.end())loaded.emplace_back(layout,identifier);
            if(previous && previous!=layout){
                std::erase_if(loaded,[&](const auto& item){return item.first==previous;});
                if(gui_.process_keyboard_layout==previous)gui_.process_keyboard_layout=layout;
                for(auto& item:gui_.thread_keyboard_layouts)if(item.second==previous)item.second=layout;
            }
            if(flags&1U)(void)nested_service(state,memory,"user32.dll","ActivateKeyboardLayout",{layout,flags&0x108U}); // KLF_ACTIVATE
            result=layout;
        }else if(symbol=="UnloadKeyboardLayout"){
            const auto mask=x86_?UINT64_C(0xffffffff):UINT64_MAX;const auto layout=arg(0)&mask;
            const auto known=keyboard_layouts();
            if(std::find(known.begin(),known.end(),layout)==known.end())fail(87);
            else{
                // 系統與宿主輸入法的配置一直存在；只卸載本程序載入的配置，並還原已選用它的執行緒。
                std::erase_if(gui_.loaded_keyboard_layouts,[&](const auto& item){return item.first==layout;});
                if(gui_.process_keyboard_layout==layout)gui_.process_keyboard_layout=0;
                std::erase_if(gui_.thread_keyboard_layouts,[&](const auto& item){return item.second==layout;});
                result=1;
            }
        }else if(symbol=="ActivateKeyboardLayout"){
            const auto flags=u32(1);
            if(flags&~(0x8U|0x100U|0x10000U|0x40000000U)){fail(87);arg.return_to(state,0);return std::nullopt;} // KLF_REORDER、SETFORPROCESS、SHIFTLOCK、RESET
            const auto layouts=keyboard_layouts();
            auto current=selected_keyboard_layout(threads_.current);
            if(!current){current=0x04090409;if(native_gui_)for(const auto& method:native_input_methods())if(method.current){current=method.layout;break;}}
            const auto mask=x86_?UINT64_C(0xffffffff):UINT64_MAX;auto target=arg(0)&mask;
            const auto position=std::find(layouts.begin(),layouts.end(),current);const auto index=position==layouts.end()?0:static_cast<std::size_t>(position-layouts.begin());
            if(target==1)target=layouts[(index+1)%layouts.size()];                      // HKL_NEXT
            else if(target==0)target=layouts[(index+layouts.size()-1)%layouts.size()];   // HKL_PREV
            if(std::find(layouts.begin(),layouts.end(),target)==layouts.end()){fail(87);arg.return_to(state,0);return std::nullopt;}
            if(flags&0x100U){gui_.process_keyboard_layout=target;gui_.thread_keyboard_layouts.clear();} // KLF_SETFORPROCESS
            else gui_.thread_keyboard_layouts[threads_.current]=target;
            result=current;
            if(target!=current && gui_.focus && gui_.windows.contains(gui_.focus) && gui_.windows.at(gui_.focus).thread==threads_.current){
                // 依 Windows 契約通知焦點視窗輸入語言已切換。
                GuiMessage message{gui_.focus,0,target,0x51};message.thread=threads_.current;enqueue_gui_message(std::move(message));
            }
        }else if(symbol=="InitializeTouchInjection"){
            const auto count=u32(0),mode=u32(1);
            if(!count || count>256 || mode<1 || mode>3)fail(87);
            else {
                if(!pointer_input_)pointer_input_=std::make_shared<PointerInputState>();auto& bridge=*pointer_input_;
                if(const auto existing=bridge.identities.find(injected_touch_device);existing!=bridge.identities.end())retire_pointer_device(existing->second);
                if(bridge.devices.size()>=256 || bridge.next_id+count>2562)fail(8);
                else {
                    input::PointerDevice device;device.device=injected_touch_device;device.type=3;device.name="Touch Injection";
                    device.logical_rectangle={0,0,1439,899};device.max_contacts=static_cast<std::uint16_t>(count);
                    const auto handle=runtime_.allocate_wait_handle();bridge.identities.emplace(injected_touch_device,handle);
                    PointerDeviceState entry{device,true,false,bridge.next_id,true};bridge.devices.emplace(handle,std::move(entry));bridge.next_id+=count;
                    touch_injection_feedback_=mode;result=1;
                }
            }
        }else if(symbol=="InjectTouchInput"){
            const auto device=pointer_input_?pointer_input_->identities.find(injected_touch_device):std::map<input::Guid,std::uint64_t>::iterator{};
            if(!pointer_input_ || device==pointer_input_->identities.end()){fail(21);arg.return_to(state,0);return std::nullopt;} // ERROR_NOT_READY
            if(inject_pointer(pointer_input_->devices.at(device->second),arg(1),u32(0),x86_?136U:144U,0,2))result=1;
        }else if(symbol=="CreateSyntheticPointerDevice"){
            // PT_TOUCH 最多 256 個接觸點、PT_PEN 只能 1 個；回饋模式 DEFAULT／INDIRECT／NONE。
            const auto type=u32(0),count=u32(1),mode=u32(2);
            if((type!=2 && type!=3) || !count || count>(type==2?256U:1U) || mode<1 || mode>3)fail(87);
            else {
                if(!pointer_input_)pointer_input_=std::make_shared<PointerInputState>();auto& bridge=*pointer_input_;
                if(bridge.devices.size()>=256 || bridge.next_id+count>2562)fail(8);
                else {
                    const auto handle=runtime_.allocate_wait_handle();
                    input::PointerDevice device;device.device=synthetic_identity(handle);device.type=type==2?3U:2U;device.name=type==2?"Synthetic Touch":"Synthetic Pen";
                    device.logical_rectangle={0,0,1439,899};device.max_contacts=static_cast<std::uint16_t>(count);
                    bridge.identities.emplace(device.device,handle);
                    PointerDeviceState entry{device,true,false,bridge.next_id,true};entry.synthetic_type=type;
                    bridge.devices.emplace(handle,std::move(entry));bridge.next_id+=count;result=handle;
                }
            }
        }else if(symbol=="InjectSyntheticPointerInput" || symbol=="DestroySyntheticPointerDevice"){
            const auto found=pointer_input_?pointer_input_->devices.find(arg(0)):std::map<std::uint64_t,PointerDeviceState>::iterator{};
            const bool valid=pointer_input_ && found!=pointer_input_->devices.end() && found->second.synthetic_type;
            if(symbol=="DestroySyntheticPointerDevice"){
                if(valid)retire_pointer_device(arg(0));
            }else if(!valid)fail(6);
            else if(inject_pointer(found->second,arg(1),u32(2),x86_?144U:152U,8,found->second.synthetic_type))result=1;
        }else throw HostCallFault("未知的使用者工作階段入口");
    }catch(const MemoryFault&){fail(998);}catch(const std::bad_alloc&){fail(8);}
    arg.return_to(state,result);return std::nullopt;
}
}
