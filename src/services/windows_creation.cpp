#include "x64arm/windows_services.hpp"
#include "x64arm/windows_geometry.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <bit>

namespace x64arm {
std::uint64_t WindowsServices::allocate_gui_scratch(GuestMemory& memory,std::vector<std::uint64_t>& scratch,std::span<const std::uint8_t> bytes,std::size_t size){
    if(scratch.size()==scratch.capacity())scratch.reserve(scratch.capacity()?scratch.capacity()*2:8);const auto address=allocate_heap(memory,std::max(size,bytes.size()));
    if(!address)throw std::bad_alloc{};
    scratch.push_back(address);if(!bytes.empty())memory.write(GuestAddress{address},bytes);return address;
}
void WindowsServices::release_gui_scratch(GuestMemory& memory,const GuestState& state,const std::vector<std::uint64_t>& scratch){
    for(auto it=scratch.rbegin();it!=scratch.rend();++it)(void)free_heap(memory,*it,state);
}
std::uint64_t WindowsServices::prepare_gui_create_data(GuestMemory& memory,GuiWindow& window,std::vector<std::uint64_t>& scratch,bool wide,std::uint64_t parameter,std::optional<std::uint64_t> class_pointer,std::optional<std::uint64_t> name_pointer){
    const auto string=[&](std::u16string_view text){
        if(wide)return allocate_gui_scratch(memory,scratch,utf16_bytes(text));
        const auto encoded=utf16_to_utf8(text);std::vector<std::uint8_t> bytes(encoded.begin(),encoded.end());bytes.push_back(0);return allocate_gui_scratch(memory,scratch,bytes);
    };
    const auto cls=class_pointer?*class_pointer:string(window.class_name),name=name_pointer?*name_pointer:string(window.title);
    const std::uint8_t bits=x86_?32:64;const unsigned stride=x86_?4:8,ints=x86_?16:32;
    const auto data=allocate_gui_scratch(memory,scratch,{},x86_?48:80);const auto size=gui_outer_size(window);
    for(const auto& [offset,value]:std::array{std::pair{0U,parameter},std::pair{stride,window.instance},std::pair{2*stride,window.parent?window.id:window.menu},std::pair{3*stride,window.parent?window.parent:window.owner},std::pair{x86_?36U:56U,name},std::pair{x86_?40U:64U,cls}})memory.store(GuestAddress{data+offset},bits,value);
    memory.store(GuestAddress{data+ints},32,size[1]);memory.store(GuestAddress{data+ints+4},32,size[0]);
    memory.store(GuestAddress{data+ints+8},32,static_cast<std::uint32_t>(window.y));memory.store(GuestAddress{data+ints+12},32,static_cast<std::uint32_t>(window.x));
    memory.store(GuestAddress{data+ints+16},32,window.style);memory.store(GuestAddress{data+(x86_?44:72)},32,window.ex_style);
    window.create_data=data;return data;
}
std::optional<std::uint32_t> WindowsServices::start_gui_creation(GuestState& state,GuestMemory& memory,std::vector<GuiWindowCreation> pending,std::uint64_t parameter){
    if(pending.empty())throw HostCallFault("建立視窗缺少計畫");
    const auto failure=pending.front().window.modal?UINT64_MAX:0;
    const auto release_menus=[&]{for(const auto& plan:pending){if(plan.owned_menu)destroy_gui_menu(plan.window.menu);if(plan.window.owned_font)(gui_.font_natives.erase(plan.window.font),gui_.fonts.erase(plan.window.font));if(const auto sheet=gui_.property_sheets.find(plan.handle);sheet!=gui_.property_sheets.end()){release_gui_scratch(memory,state,sheet->second.scratch);gui_.property_sheets.erase(sheet);}}};
    if(gui_.windows.size()>4096 || pending.size()>4096-gui_.windows.size()){release_menus();last_error_=8;complete_call(state,memory,failure);return std::nullopt;}
    initialize_user_objects();const auto desktop=thread_desktop_object(threads_.current);
    const auto thread=threads_.items.find(threads_.current);
    const auto desktop_handle=thread!=threads_.items.end() && thread->second.desktop_handle?thread->second.desktop_handle:gui_.initial_desktop_handle;
    if(!(gui_.user_handles.at(desktop_handle).access&2)){release_menus();last_error_=5;complete_call(state,memory,failure);return std::nullopt;}
    for(auto& plan:pending){plan.window.desktop_object=desktop;
        const auto parent=plan.window.parent?plan.window.parent:plan.window.owner;
        if(const auto found=gui_.windows.find(parent);found!=gui_.windows.end() && found->second.desktop_object!=desktop){release_menus();last_error_=5;complete_call(state,memory,failure);return std::nullopt;}
    }
    const auto id=pending.front().handle;
    try{
        // 先建立 map node，再移交 plan；配置失敗時 pending 仍保有內部 menu ownership。
        const auto [it,inserted]=gui_.creation_operations.try_emplace(id);
        if(!inserted)throw HostCallFault("重複的視窗建立作業");
        it->second.pending=std::move(pending);it->second.init_parameter=parameter;it->second.thread=threads_.current;
    }catch(const std::bad_alloc&){release_menus();last_error_=8;complete_call(state,memory,failure);return std::nullopt;}catch(...){release_menus();throw;}
    try{return advance_gui_creation(state,memory,id);}catch(const std::bad_alloc&){cancel_gui_creation(id,memory,state);last_error_=8;complete_call(state,memory,failure);return std::nullopt;}catch(...){cancel_gui_creation(id,memory,state);throw;}
}
void WindowsServices::cancel_gui_creation(std::uint64_t id,GuestMemory& memory,const GuestState& state){
    const auto found=gui_.creation_operations.find(id);if(found==gui_.creation_operations.end())return;
    auto operation=std::move(found->second);gui_.creation_operations.erase(found);
    for(auto it=operation.pending.rbegin();it!=operation.pending.rend();++it){
        if(gui_.windows.contains(it->handle))erase_window(it->handle,false);else unpublish_gui_window(it->handle);
        // 失敗的 CreateWindowEx 不取得呼叫端 HMENU；模板內部載入的選單則須回收。
        if(it->owned_menu)destroy_gui_menu(it->window.menu);
        if(it->window.owned_font)(gui_.font_natives.erase(it->window.font),gui_.fonts.erase(it->window.font));
        if(const auto sheet=gui_.property_sheets.find(it->handle);sheet!=gui_.property_sheets.end()){release_gui_scratch(memory,state,sheet->second.scratch);gui_.property_sheets.erase(sheet);}
    }
    release_gui_scratch(memory,state,operation.scratch);
}
std::optional<std::uint32_t> WindowsServices::abort_gui_creation(GuestState& state,GuestMemory& memory,std::uint64_t id){
    auto& operation=gui_.creation_operations.at(id);std::vector<GuestCallback> callbacks;
    const auto current=operation.pending.at(operation.index).handle;
    operation.skip_failed_child=operation.index && operation.pending.front().window.dialog && (operation.pending.front().window.style&0x10U);
    operation.abort_end=operation.index+1;
    const auto descendant=[&](std::uint64_t handle){
        for(std::size_t n=0;n<operation.pending.size();++n){
            if(handle==current)return true;
            const auto item=std::find_if(operation.pending.begin(),operation.pending.end(),[&](const auto& v){return v.handle==handle;});
            if(item==operation.pending.end())return false;
            handle=item->window.parent?item->window.parent:item->window.owner;
            if(!handle)return false;
        }return false;
    };
    while(operation.abort_end<operation.pending.size() && descendant(operation.pending[operation.abort_end].handle))++operation.abort_end;
    // 已成功 NCCREATE 才有 DESTROY；失敗的 NCCREATE 仍須 NCDESTROY。
    for(auto it=operation.pending.rbegin();it!=operation.pending.rend();++it){
        if(operation.skip_failed_child && !descendant(it->handle))continue;
        const auto found=gui_.windows.find(it->handle);if(found==gui_.windows.end() || found->second.destroying)continue;
        found->second.destroying=true;const auto proc=found->second.procedure;
        const bool completed=std::find(operation.completed.begin(),operation.completed.end(),it->handle)!=operation.completed.end();
        if(proc){if(completed)callbacks.push_back({proc,{it->handle,2,0,0}});callbacks.push_back({proc,{it->handle,0x82,0,0}});}
        auto release=revoke_gui_drop_targets(memory,it->handle);callbacks.insert(callbacks.end(),release.begin(),release.end());
    }
    start_callbacks(state,memory,std::move(callbacks),Continuation::GuiCreationAbort,id);return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::advance_gui_creation(GuestState& state,GuestMemory& memory,std::uint64_t id){
    auto& operation=gui_.creation_operations.at(id);
    if(operation.index==operation.pending.size()){
        const auto parameter=operation.init_parameter;
        for(const auto& plan:operation.pending)if(gui_.windows.contains(plan.handle))gui_.windows.at(plan.handle).create_data=0;
        release_gui_scratch(memory,state,operation.scratch);gui_.creation_operations.erase(id);
        const auto found=gui_.windows.find(id);if(found==gui_.windows.end()){complete_call(state,memory,0);return std::nullopt;}
        const auto& root=found->second;
        if(root.dialog){start_callbacks(state,memory,root.procedure?std::vector<GuestCallback>{{root.procedure,{id,0x110,0,parameter}}}:std::vector<GuestCallback>{},root.modal?Continuation::GuiModalInit:Continuation::GuiDialogInit,id);return std::nullopt;}
        complete_call(state,memory,id);return std::nullopt;
    }
    auto& item=operation.pending.at(operation.index);
    if(!item.window.create_data){
        const auto parameter=item.parameter_bytes.empty()?item.parameter:allocate_gui_scratch(memory,operation.scratch,item.parameter_bytes);
        prepare_gui_create_data(memory,item.window,operation.scratch,item.wide,parameter,item.class_pointer,item.name_pointer);
    }
    auto window=item.window;window.desktop_object=thread_desktop_object(window.thread);window.visible=false;
    const auto cls=std::find_if(gui_.classes.begin(),gui_.classes.end(),[&](const auto& entry){return window.class_atom && entry.second.atom==window.class_atom;});
    const auto stock_class=stock_gui_class(window.procedure);window.unicode=!stock_class.empty()?window.procedure==stock_gui_proc(stock_class,true):window.dialog?item.wide:cls==gui_.classes.end()?item.wide:cls->second.unicode;
    if(!gui_.windows.emplace(item.handle,std::move(window)).second)throw HostCallFault("建立計畫包含重複 HWND");
    register_mdi_child(item.handle);
    publish_gui_window(item.handle);
    const bool bottom=item.window.parent && !(item.window.style&0x01000000U);
    gui_.z_order.insert(bottom?gui_.z_order.end():gui_.z_order.begin(),item.handle);
    operation.cbt_data=allocate_gui_scratch(memory,operation.scratch,{},16);memory.store(GuestAddress{operation.cbt_data},x86_?32:64,item.window.create_data);
    memory.store(GuestAddress{operation.cbt_data+(x86_?4:8)},x86_?32:64,bottom?1:0);
    if(start_gui_hooks(state,memory,5,3,item.handle,operation.cbt_data,Continuation::GuiCbtCreate,id,item.wide))return std::nullopt;
    start_callbacks(state,memory,{},Continuation::GuiCbtCreate,id);return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::resume_gui_creation(GuestState& state,GuestMemory& memory,const CallbackFrame& frame,std::uint64_t result)try{
    const auto id=frame.value;const auto found=gui_.creation_operations.find(id);
    if(found==gui_.creation_operations.end()){complete_call(state,memory,0);return std::nullopt;}
    auto& operation=found->second;
    if(frame.continuation==Continuation::GuiCreationAbort){
        if(operation.skip_failed_child){
            for(auto i=operation.abort_end;i>operation.index;){const auto& plan=operation.pending[--i];const auto failed=plan.handle;if(gui_.windows.contains(failed))erase_window(failed,false);if(plan.owned_menu)destroy_gui_menu(plan.window.menu);std::erase(operation.completed,failed);}
            operation.pending.erase(operation.pending.begin()+static_cast<std::ptrdiff_t>(operation.index),operation.pending.begin()+static_cast<std::ptrdiff_t>(operation.abort_end));
            operation.skip_failed_child=false;return advance_gui_creation(state,memory,id);
        }
        const auto failure=operation.pending.front().window.modal?UINT64_MAX:0;cancel_gui_creation(id,memory,state);complete_call(state,memory,failure);return std::nullopt;
    }
    const auto handle=operation.pending.at(operation.index).handle;const auto window=gui_.windows.find(handle);
    if(window==gui_.windows.end())return abort_gui_creation(state,memory,id);
    auto& current=window->second;
    if(frame.continuation==Continuation::GuiCbtCreate){
        if(result){
            erase_window(handle,false);
            return abort_gui_creation(state,memory,id);
        }
        const auto data=current.create_data;
        const auto width=static_cast<std::uint32_t>(memory.load(GuestAddress{data+(x86_?20:36)},32)),height=static_cast<std::uint32_t>(memory.load(GuestAddress{data+(x86_?16:32)},32));
        if(width>8192 || height>8192){last_error_=87;erase_window(handle,false);return abort_gui_creation(state,memory,id);}
        current.x=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{data+(x86_?28:44)},32)));
        current.y=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{data+(x86_?24:40)},32)));
        const auto client=gui_client_size(width,height,gui_frame_insets(current));current.width=client[0];current.height=client[1];
        const auto after=memory.load(GuestAddress{operation.cbt_data+(x86_?4:8)},x86_?32:64);
        if(after>1 && (!gui_.windows.contains(after) || gui_.windows.at(after).parent!=current.parent)){last_error_=1400;erase_window(handle,false);return abort_gui_creation(state,memory,id);}
        if(after!=handle){std::erase(gui_.z_order,handle);auto position=after==1?gui_.z_order.end():after==0?gui_.z_order.begin():std::find(gui_.z_order.begin(),gui_.z_order.end(),after);if(after>1 && position!=gui_.z_order.end())++position;gui_.z_order.insert(position,handle);}
        if(!current.parent && !gui_.application_icon_loaded){
            gui_.application_icon_loaded=true;const auto saved_error=last_error_;
            // 自動取得裝飾圖示不可讓沒有 PE 資源或資源受損的程式無法建立視窗。
            try{const auto icon=load_gui_icon(memory,runtime_.image.base,0,true,128,128,false,true);
                if(icon){gui_.default_application_icon=gui_.icons.at(icon);gui_.icons.erase(icon);}
            }catch(const MemoryFault&){}catch(const HostCallFault&){}
            last_error_=saved_error;
        }
        if(native_gui_){native_gui_->create(handle,current);native_gui_->reorder(handle,after);}
        if(!current.parent)present_gui(handle);
        if(current.menu)present_gui_menu(memory,handle);
        auto children=plan_gui_control_children(handle,operation.pending.at(operation.index).wide);
        operation.pending.insert(operation.pending.begin()+static_cast<std::ptrdiff_t>(operation.index+1),std::make_move_iterator(children.begin()),std::make_move_iterator(children.end()));
        if(begin_gui_text_thunk(state,memory,{handle,0,data,0x81},operation.pending.at(operation.index).wide,Continuation::GuiCreationNcCreate,id))return std::nullopt;
        start_callbacks(state,memory,{{current.procedure,{handle,0x81,0,data}}},Continuation::GuiCreationNcCreate,id);return std::nullopt;
    }
    if(frame.continuation==Continuation::GuiCreationNcCreate){
        if(!result)return abort_gui_creation(state,memory,id);
        operation.completed.push_back(handle);
        operation.nc_data=allocate_gui_scratch(memory,operation.scratch,{},32);
        const auto origin=gui_client_origin(gui_,handle);const auto f=gui_frame_insets(current);const auto outer=gui_outer_size(current);
        const std::array<std::int64_t,4> rect{origin[0]-f.left,origin[1]-f.top,origin[0]-f.left+outer[0],origin[1]-f.top+outer[1]};
        for(unsigned j=0;j<4;++j){memory.store(GuestAddress{operation.nc_data+4*j},32,static_cast<std::uint32_t>(rect[j]));memory.store(GuestAddress{operation.nc_data+16+4*j},32,static_cast<std::uint32_t>(rect[j]));}
        start_callbacks(state,memory,{{current.procedure,{handle,0x83,0,operation.nc_data}}},Continuation::GuiCreationNcCalc,id);return std::nullopt;
    }
    if(frame.continuation==Continuation::GuiCreationNcCalc){
        std::array<std::int32_t,4> outer{},client_rect{};
        for(unsigned j=0;j<4;++j){outer[j]=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{operation.nc_data+16+4*j},32)));client_rect[j]=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{operation.nc_data+4*j},32)));}
        gui_calculated_frame(current,outer,client_rect);
        const auto size=gui_client_size(static_cast<std::uint32_t>(std::int64_t{outer[2]}-outer[0]),static_cast<std::uint32_t>(std::int64_t{outer[3]}-outer[1]),gui_frame_insets(current));current.width=size[0];current.height=size[1];present_gui(handle);
        if(begin_gui_text_thunk(state,memory,{handle,0,current.create_data,1},operation.pending.at(operation.index).wide,Continuation::GuiCreationCreate,id))return std::nullopt;
        start_callbacks(state,memory,{{current.procedure,{handle,1,0,current.create_data}}},Continuation::GuiCreationCreate,id);return std::nullopt;
    }
    if(frame.continuation==Continuation::GuiCreationCreate){
        if(static_cast<std::uint32_t>(result)==UINT32_MAX)return abort_gui_creation(state,memory,id);
        if(!current.dialog && (current.style&0x10000000U)){current.visible=true;if(native_gui_)show_native_window(handle,true);present_gui(handle);invalidate_window(handle);}
        std::vector<GuestCallback> callbacks;
        if(!current.dialog && !(current.initial_size_pending && !current.visible)){
            current.initial_size_pending=false;callbacks={{current.procedure,{handle,5,current.mdi_child?(current.show_command==2?1U:current.show_command==3?2U:0U):0U,(current.width&65535U)|((current.height&65535U)<<16)}},{current.procedure,{handle,3,0,gui_move_lparam(current)}}};
        }
        auto mdi=finish_mdi_creation(handle,memory);callbacks.insert(callbacks.end(),mdi.begin(),mdi.end());
        start_callbacks(state,memory,std::move(callbacks),Continuation::GuiCreationFinish,id);return std::nullopt;
    }
    ++operation.index;return advance_gui_creation(state,memory,id);
}catch(const std::bad_alloc&){const auto operation=gui_.creation_operations.find(frame.value);const auto failure=operation!=gui_.creation_operations.end() && operation->second.pending.front().window.modal?UINT64_MAX:0;cancel_gui_creation(frame.value,memory,state);last_error_=8;complete_call(state,memory,failure);return std::nullopt;}catch(...){cancel_gui_creation(frame.value,memory,state);throw;}
}
