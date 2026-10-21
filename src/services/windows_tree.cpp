#include "x64arm/windows_services.hpp"
#include "x64arm/windows_gui_abi.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <bit>
#include <map>

namespace x64arm {
namespace {
std::int32_t tree_integer(std::uint64_t value) {return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));}
std::uint32_t tree_callback_mask(const GuiItem& item,std::uint32_t requested) {
    const auto available=(item.callback_subitems.contains(0)?1U:0U)|(item.image==-1?2U:0U)|
                         (item.selected_image==-1?32U:0U)|(item.tree_children==-1?64U:0U);
    return available&requested;
}
std::vector<std::uint64_t> tree_visible_items(const GuiWindow& window) {
    std::map<std::uint64_t,std::vector<const GuiItem*>> children;
    for(const auto& item:window.items)children[item.parent].push_back(&item);
    std::vector<const GuiItem*> pending;
    for(auto it=children[0].rbegin();it!=children[0].rend();++it)pending.push_back(*it);
    std::vector<std::uint64_t> result;
    while(!pending.empty()){
        const auto* item=pending.back();pending.pop_back();result.push_back(item->handle);
        if(item->state&0x20U)if(const auto found=children.find(item->handle);found!=children.end())
            for(auto it=found->second.rbegin();it!=found->second.rend();++it)pending.push_back(*it);
    }
    return result;
}
}

bool WindowsServices::begin_gui_tree(GuestState& state,GuestMemory& memory,const GuiMessage& message,bool wide) {
    const auto m=message.message;
    const bool expand=m==0x1102 || m==0x1001a,get=m==0x110c || m==0x113e,paint=m==15,display=m==0x1001b;
    if(!expand && !get && !paint && !display)return false;
    if(get && message.wparam){complete_call(state,memory,0);return true;}
    auto& window=gui_.windows.at(message.window);const GuiControlAbi abi(x86_);
    const auto identity=get?(message.lparam?memory.load(GuestAddress{message.lparam+abi.tree_handle},abi.bits):0):expand?message.lparam:display?message.wparam:0;
    // 父視窗在 GETDISPINFO 中重入同一查詢時讀取目前資料，不遞迴呼叫同一 provider。
    if(std::any_of(gui_.control_operations.begin(),gui_.control_operations.end(),[&](const auto& entry){
        return entry.second.message.window==message.window && entry.second.tree &&
               (paint ||
                ((get || display) && entry.second.tree->pending && entry.second.item==identity));
    }))return false;
    if(paint){std::erase_if(gui_.queue,[&](const auto& queued){return queued.window==message.window && queued.message==15;});window.invalid=false;if(!window.redraw){complete_call(state,memory,0);return true;}}
    GuiControlOperation operation;operation.message=message;operation.thread=threads_.current;operation.wide=wide;operation.tree.emplace();
    auto& request=*operation.tree;request.kind=expand?GuiControlOperation::Tree::Kind::Expand:GuiControlOperation::Tree::Kind::Display;request.native=m==0x1001a;
    if(expand){
        if((message.wparam&~UINT64_C(0xc003)) || !(message.wparam&3U) ||
           ((message.wparam&0x8000U) && (message.wparam&3U)!=1U) || ((message.wparam&0x4000U) && (message.wparam&3U)!=2U) ||
           (request.native && !window.enabled)){complete_call(state,memory,0);return true;}
        operation.item=identity;request.action=static_cast<std::uint32_t>(message.wparam);request.mask=64;
    }else if(get){
        if(!message.lparam){complete_call(state,memory,0);return true;}
        memory.check(GuestAddress{message.lparam},abi.tree_size,Access::Read);
        operation.item=identity;request.mask=static_cast<std::uint32_t>(memory.load(GuestAddress{message.lparam},32));operation.identities.push_back(identity);
    }else if(display)operation.identities.push_back(identity);
    else operation.identities=tree_visible_items(window);
    if(gui_.control_operations.size()>=128)throw HostCallFault("TreeView 動態資料回呼超過上限");
    const auto token=gui_.next_object++;gui_.control_operations.emplace(token,std::move(operation));
    try{(void)resume_gui_tree(state,memory,token,0);}catch(...){discard_gui_control(token);throw;}return true;
}

std::optional<std::uint32_t> WindowsServices::resume_gui_tree(GuestState& state,GuestMemory& memory,std::uint64_t token,std::uint64_t result) {
    auto& operation=gui_.control_operations.at(token);auto& request=*operation.tree;const auto handle=operation.message.window;
    const auto finish=[&](std::uint64_t value)->std::optional<std::uint32_t>{discard_gui_control(token);if(gui_.windows.contains(handle))present_gui(handle);complete_call(state,memory,value);return {};};
    const auto live=gui_.windows.find(handle);if(live==gui_.windows.end())return finish(0);
    auto& window=live->second;const GuiControlAbi abi(x86_);
    const auto lookup=[&](std::uint64_t identity){return std::find_if(window.items.begin(),window.items.end(),[&](const auto& item){return item.handle==identity;});};
    const auto buffer=[&]{
        if(!operation.buffer){if(gui_.control_notification_buffers.empty())operation.buffer=runtime_allocate(memory,{},gui_control_notification_bytes);else {operation.buffer=gui_.control_notification_buffers.back();gui_.control_notification_buffers.pop_back();}}
        memory.write(GuestAddress{operation.buffer},std::vector<std::uint8_t>(gui_control_notification_bytes));
    };
    const auto notify_header=[&](std::int32_t code){
        memory.store(GuestAddress{operation.buffer},abi.bits,handle);memory.store(GuestAddress{operation.buffer+abi.unit},abi.bits,window.id);
        memory.store(GuestAddress{operation.buffer+abi.notify_code},32,static_cast<std::uint32_t>(code));
    };
    const auto send=[&]{start_callbacks(state,memory,{{resolve("user32.dll",window.list_unicode?"SendMessageW":"SendMessageA").value,{window.parent,0x4e,window.id,operation.buffer}}},Continuation::GuiControlOperation,token);};
    const auto item_data=[&](std::uint64_t at,const GuiItem& item,std::uint32_t mask){
        memory.store(GuestAddress{at},32,mask);memory.store(GuestAddress{at+abi.tree_handle},abi.bits,item.handle);
        memory.store(GuestAddress{at+abi.tree_state},32,item.state);memory.store(GuestAddress{at+abi.tree_mask},32,UINT32_MAX);
        memory.store(GuestAddress{at+abi.tree_data},abi.bits,item.data);
    };
    if(request.pending){
        request.pending=false;const auto item=lookup(operation.item);
        if(item!=window.items.end() && item->revision==operation.parameter){
            const auto at=operation.buffer+abi.notify_size;const auto returned=static_cast<std::uint32_t>(memory.load(GuestAddress{at},32));const bool retain=(returned&0x1000U)!=0;
            if((request.mask&1U) && item->callback_subitems.contains(0)){
                const auto pointer=memory.load(GuestAddress{at+abi.tree_text},abi.bits);
                item->text=pointer?(window.list_unicode?read_utf16(memory,GuestAddress{pointer},65535):utf8_to_utf16_lossy(read_ansi(memory,pointer,65535))):std::u16string{};
                item->resolved_subitems.insert(0);if(retain)item->callback_subitems.erase(0);
            }
            for(const auto [bit,offset,original,slot]:std::array<std::tuple<unsigned,unsigned,std::int32_t*,unsigned>,3>{{
                {2,abi.tree_image,&item->image,0},{32,abi.tree_selected,&item->selected_image,1},{64,abi.tree_children,&item->tree_children,2}}}){
                if((request.mask&bit) && *original==-1){auto value=tree_integer(memory.load(GuestAddress{at+offset},32));if(bit==64)value=std::max(0,value);item->tree_display[slot]=value;item->resolved_subitems.insert(slot+1);if(retain)*original=value;}
            }
            ++item->revision;
        }
    }
    const auto display_item=[&](GuiItem& item,std::uint32_t mask){
        mask=tree_callback_mask(item,mask);if(!mask || !gui_.windows.contains(window.parent))return false;
        buffer();notify_header(window.list_unicode?-452:-403);const auto at=operation.buffer+abi.notify_size;item_data(at,item,mask);
        memory.store(GuestAddress{at+abi.tree_text},abi.bits,operation.buffer+256);memory.store(GuestAddress{at+abi.tree_capacity},32,window.list_unicode?2048U:4096U);
        memory.store(GuestAddress{at+abi.tree_image},32,static_cast<std::uint32_t>(item.image));memory.store(GuestAddress{at+abi.tree_selected},32,static_cast<std::uint32_t>(item.selected_image));memory.store(GuestAddress{at+abi.tree_children},32,static_cast<std::uint32_t>(item.tree_children));
        operation.item=item.handle;operation.parameter=item.revision;request.mask=mask;request.pending=true;send();return true;
    };
    if(request.kind==GuiControlOperation::Tree::Kind::Display){
        const bool get=operation.message.message==0x110c || operation.message.message==0x113e;
        while(operation.start<operation.identities.size()){
            const auto item=lookup(operation.identities[operation.start++]);if(item==window.items.end())continue;
            const auto mask=get?static_cast<std::uint32_t>(memory.load(GuestAddress{operation.message.lparam},32)):1U|64U|((item->state&2U)?32U:2U);
            if(display_item(*item,mask))return {};
        }
        if(get){if(lookup(operation.identities.front())==window.items.end())return finish(0);return finish(gui_control_message(memory,operation.message,operation.wide).value_or(0));}
        return finish(0);
    }
    auto item=lookup(operation.item);if(item==window.items.end())return finish(0);
    if(!request.stage){request.stage=1;if(display_item(*item,64))return {};}
    if(request.stage==1){
        const bool expanded=(item->state&0x20U)!=0;const auto mode=request.action&3U;
        if(mode==3)request.action=(request.action&~3U)|(expanded?1U:2U);
        const bool opening=(request.action&3U)==2U;
        const bool children=(item->tree_children==-1?item->tree_display[2]:item->tree_children)>0 || std::any_of(window.items.begin(),window.items.end(),[&](const auto& entry){return entry.parent==item->handle;});
        if(!children || (!opening && !expanded && !(request.action&0x8000U)))return finish(0);
        if(opening && expanded && ((item->state&0x80U)!=0)==((request.action&0x4000U)!=0))return finish(1);
        request.previous_state=item->state&0xe0U;request.notify=request.native || !(item->state&0x40U);request.stage=2;
        if(request.notify && gui_.windows.contains(window.parent)){
            buffer();notify_header(window.list_unicode?-454:-405);memory.store(GuestAddress{operation.buffer+abi.notify_size},32,request.action);
            item_data(operation.buffer+abi.notify_size+abi.unit+abi.tree_size,*item,0x1cU);send();return {};
        }
    }else if(request.stage==2 && result)return finish(0);
    if(request.stage==2){
        if((item->state&0xe0U)!=request.previous_state)return finish(0);
        // cChildren 可以要求先顯示 disclosure，但展開通知後仍須有真正的子節點。
        if((request.action&3U)==2U && std::none_of(window.items.begin(),window.items.end(),[&](const auto& entry){return entry.parent==operation.item;}))return finish(0);
        request.return_value=gui_control_message(memory,{handle,request.action,operation.item,0x1102},operation.wide).value_or(0);if(!request.return_value)return finish(0);
        request.stage=3;
        // 折疊隱藏 caret 時改選祖先；此隱含變更不額外送選取通知。
        if((request.action&3U)==1U && window.selection>=0 && static_cast<std::size_t>(window.selection)<window.items.size()){
            auto ancestor=window.items[static_cast<std::size_t>(window.selection)].parent;bool descendant=false;
            for(std::size_t depth=0;ancestor && depth<window.items.size();++depth){if(ancestor==operation.item){descendant=true;break;}const auto parent=lookup(ancestor);if(parent==window.items.end())break;ancestor=parent->parent;}
            if(descendant){for(auto& entry:window.items)entry.state&=~2U;item->state|=2U;window.selection=static_cast<std::int32_t>(item-window.items.begin());++window.tree_revision;}
        }
    }
    if(request.stage==3){
        request.stage=4;if(request.action&0x8000U)for(const auto& child:window.items)if(child.parent==operation.item)operation.identities.push_back(child.handle);
    }
    if(request.stage==4){
        while(operation.start<operation.identities.size()){
            const auto child=operation.identities[operation.start++];if(lookup(child)==window.items.end())continue;
            start_callbacks(state,memory,{{resolve("user32.dll","SendMessageW").value,{handle,0x1101,0,child}}},Continuation::GuiControlOperation,token);return {};
        }
        request.stage=5;item=lookup(operation.item);if(item==window.items.end())return finish(0);
        if(request.notify && gui_.windows.contains(window.parent)){
            buffer();notify_header(window.list_unicode?-455:-406);memory.store(GuestAddress{operation.buffer+abi.notify_size},32,request.action);
            item_data(operation.buffer+abi.notify_size+abi.unit+abi.tree_size,*item,0x1cU);send();return {};
        }
    }
    item=lookup(operation.item);if(item!=window.items.end() && (request.action&3U)==2U && (item->state&0x20U))item->state|=0x40U;
    return finish(request.return_value);
}
}
