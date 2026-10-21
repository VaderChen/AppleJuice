#include "x64arm/windows_services.hpp"
#include "x64arm/windows_geometry.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_cpu_time.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <set>

namespace x64arm {
namespace {
std::int32_t signed_word(std::uint64_t value) {
    return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));
}
std::u16string lower_name(std::u16string text) {
    for (auto& c : text) if (c >= u'A' && c <= u'Z') c = static_cast<char16_t>(c + 32);
    return text;
}
}

std::uint64_t WindowsServices::allocate_gui_global(GuestMemory& memory,std::uint64_t size,std::uint32_t flags){
    if(size>output_limit || gui_.global_memory.size()>=65536)return 0;
    const auto address=size || !(flags&2)?allocate_heap(memory,size):0;
    if(!address && (size || !(flags&2)))return 0;
    const auto handle=flags&2?gui_.next_object++:address;
    try{gui_.global_memory.emplace(handle,GuiGlobalMemory{address,size,flags,0,false});}
    catch(...){if(address)(void)free_heap(memory,address,GuestState{});throw;}
    return handle;
}
void WindowsServices::release_gui_clipboard(GuestState& state,GuestMemory& memory){
    std::map<std::uint64_t,std::uint32_t> handles;
    for(const auto& [format,handle]:gui_.clipboard)if(handle && (format<0x200 || format>0x2ff))handles.try_emplace(handle,gui_.clipboard_types.contains(format)?gui_.clipboard_types.at(format):1U);
    for(const auto& [handle,type]:handles){
        if(type==16){gui_.bitmaps.erase(handle);rasters_.erase(handle);}
        else if(type==64)gui_.metafiles.erase(handle);
        else if(const auto block=gui_.global_memory.find(handle);block!=gui_.global_memory.end()){
            if(block->second.address)(void)free_heap(memory,block->second.address,state);
            gui_.global_memory.erase(block);
        }
    }
    gui_.clipboard.clear();gui_.clipboard_order.clear();gui_.clipboard_types.clear();gui_.clipboard_ole_entries.clear();
    gui_.clipboard_stale=false;++gui_.clipboard_ole_generation;
}

std::optional<std::uint32_t> WindowsServices::invoke_gui_extra(
    GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service) {
    const WindowsCall arg(state, memory, service);
    const auto symbol = service.symbol;
    const auto dword = [&](unsigned index) { return static_cast<std::uint32_t>(arg(index)); };
    std::uint64_t result = 0;
    const auto fail = [&](std::uint32_t error) { last_error_ = error; result = 0; };
    const auto ui_limits=job_ui_restrictions();
    if((symbol=="GetClipboardData" && (ui_limits&2U)) || ((symbol=="SetClipboardData" || symbol=="EmptyClipboard") && (ui_limits&4U))){
        fail(5);arg.return_to(state,0);return std::nullopt;
    }
    const auto release_clipboard=[&]{release_gui_clipboard(state,memory);};
    const auto allocate_global=[&](std::uint64_t size,std::uint32_t flags){return allocate_gui_global(memory,size,flags);};
    const auto global_text = [&](std::uint64_t handle, std::uint32_t format) -> std::optional<std::u16string> {
        const auto found = gui_.global_memory.find(handle);
        if (found == gui_.global_memory.end() || !found->second.address) return std::nullopt;
        const auto& block = found->second;
        if (format == 13) {
            std::u16string text;
            for (std::uint64_t i = 0; i + 1 < block.size; i += 2) {
                const auto c = static_cast<char16_t>(memory.load(GuestAddress{block.address + i}, 16));
                if (!c) return text;
                text.push_back(c);
            }
        } else if (format == 1) {
            std::string text;
            for (std::uint64_t i = 0; i < block.size; ++i) {
                const auto c = static_cast<char>(memory.load(GuestAddress{block.address + i}, 8));
                if (!c) return utf8_to_utf16_lossy(text); // 共用虛擬 ACP 為 UTF-8。
                text.push_back(c);
            }
        }
        return std::nullopt;
    };
    const auto cache_text = [&](std::uint32_t format, const std::u16string& text) -> std::uint64_t {
        std::vector<std::uint8_t> bytes;
        if (format == 13) bytes = utf16_bytes(text);
        else { const auto ansi = utf16_to_utf8(text); bytes.assign(ansi.begin(), ansi.end()); bytes.push_back(0); }
        const auto handle = allocate_global(bytes.size(), 2);
        if (handle) {
            auto& block = gui_.global_memory.at(handle);
            memory.write(GuestAddress{block.address}, bytes);
            block.clipboard_owned = true;
            gui_.clipboard[format] = handle;
        }
        return handle;
    };

    if(symbol.find("Clipboard")!=symbol.npos){refresh_clipboard();if(gui_.clipboard_stale){
        if(gui_.clipboard_ole_object){const auto retirement=release_com_proxy_owned(state,memory,{gui_.clipboard_ole_thread,gui_.clipboard_ole_object});if(retirement)queue_com_proxy_retirement(memory,retirement);gui_.clipboard_ole_object=gui_.clipboard_ole_thread=0;}
        release_clipboard();}}
    if(symbol=="EmptyClipboard" && gui_.clipboard_ole_object)return invoke_ole_clipboard(state,memory,{"internal-ole-clipboard","Empty",0});
    if(symbol=="GetClipboardData" && (gui_.clipboard_ole_object || gui_.clipboard_owner) && gui_.clipboard_open_thread==threads_.current &&
       (!gui_.clipboard.contains(dword(0)) || !gui_.clipboard.at(dword(0)))){
        const auto other=dword(0)==13?1U:dword(0)==1?13U:0;
        const bool offered=gui_.clipboard.contains(dword(0));
        if(offered || (other && gui_.clipboard.contains(other) && !gui_.clipboard.at(other)))return invoke_ole_clipboard(state,memory,{"internal-ole-clipboard","Render",1});
    }
    const auto clipboard_formats=[&]{auto formats=gui_.clipboard_order;for(const auto& [id,handle]:gui_.clipboard){(void)handle;if(std::find(formats.begin(),formats.end(),id)==formats.end())formats.push_back(id);}
        const bool text=gui_.clipboard_native_text || gui_.clipboard.contains(1) || gui_.clipboard.contains(13);
        if(text){if(std::find(formats.begin(),formats.end(),13)==formats.end())formats.push_back(13);if(std::find(formats.begin(),formats.end(),1)==formats.end())formats.push_back(1);}return formats;};
    if(symbol=="OleDuplicateData"){
        const auto format=static_cast<std::uint16_t>(arg(1));
        if(format==2){
            const auto bitmap=gui_.bitmaps.find(arg(0));
            if(bitmap==gui_.bitmaps.end())fail(6);
            else {sync_raster_bitmap(memory,arg(0));GuiBitmap copy{bitmap->second.width,bitmap->second.height,bitmap->second.pixels};copy.bits_per_pixel=bitmap->second.bits_per_pixel;copy.raw_alpha=bitmap->second.raw_alpha;result=gui_.next_object++;gui_.bitmaps.emplace(result,std::move(copy));}
        }else if(format==14){
            start_callbacks(state,memory,{{resolve("gdi32.dll","CopyEnhMetaFileW").value,{arg(0),0}}},Continuation::ReturnCallback);return std::nullopt;
        }else if(format==3 || format==9)fail(50);
        else {
            const auto block=gui_.global_memory.find(arg(0));const auto flags=dword(2)?dword(2):2U;
            if(flags&~0x7f72U)fail(87);
            else if(block==gui_.global_memory.end() || !block->second.address)fail(6);
            else {
                std::vector<std::uint8_t> bytes(static_cast<std::size_t>(block->second.size));memory.read(GuestAddress{block->second.address},bytes);
                result=allocate_global(bytes.size(),flags);if(!result)last_error_=8;else if(!bytes.empty())memory.write(GuestAddress{gui_.global_memory.at(result).address},bytes);
            }
        }
    }else if(symbol=="DragAcceptFiles"){
        const auto found=gui_.windows.find(arg(0));if(found==gui_.windows.end())fail(1400);else {if(arg(1))found->second.ex_style|=0x10U;else found->second.ex_style&=~0x10U;present_gui(arg(0));}
    }else if(symbol=="AttachThreadInput"){
        const auto first=thread_from_identifier(static_cast<std::uint32_t>(arg(0))),second=thread_from_identifier(static_cast<std::uint32_t>(arg(1)));
        if(first==second || !threads_.items.contains(first) || !threads_.items.contains(second))fail(87);
        else if(thread_desktop_object(first)!=thread_desktop_object(second))fail(5);
        else if(!gui_.message_threads.contains(first) || !gui_.message_threads.contains(second))fail(87);
        else {const auto key=std::make_pair(std::min(first,second),std::max(first,second));if(arg(2))gui_.input_attachments.emplace(key.first,key.second);else gui_.input_attachments.erase({key.first,key.second});gui_.keyboard.fill(0);result=1;}
    }else if(symbol=="ClipCursor" || symbol=="GetClipCursor"){
        if(symbol=="GetClipCursor"){
            if(!arg(0))fail(87);else {const auto rect=gui_.cursor_clip.value_or(std::array<std::int32_t,4>{0,0,1440,900});for(unsigned i=0;i<4;++i)memory.store(GuestAddress{arg(0)+i*4},32,static_cast<std::uint32_t>(rect[i]));result=1;}
        }else if(!arg(0)){gui_.cursor_clip.reset();result=1;}
        else {std::array<std::int32_t,4> rect{};for(unsigned i=0;i<4;++i)rect[i]=static_cast<std::int32_t>(memory.load(GuestAddress{arg(0)+i*4},32));
            if(rect[0]>=rect[2] || rect[1]>=rect[3])fail(87);else {gui_.cursor_clip=rect;auto point=gui_.pointer_position;if(native_gui_)if(auto current=native_gui_->cursor_position())point=*current;point[0]=std::clamp(point[0],rect[0],rect[2]-1);point[1]=std::clamp(point[1],rect[1],rect[3]-1);if(native_gui_)native_gui_->set_cursor_position(point[0],point[1]);gui_.pointer_position=point;result=1;}}
    }else if(symbol=="DragFinish"){
        start_callbacks(state,memory,{{resolve("kernel32.dll","GlobalFree").value,{arg(0)}}},Continuation::Return,0);return std::nullopt;
    }else if(symbol.starts_with("DragQuery")){
        const auto block=gui_.global_memory.find(arg(0));
        if(block==gui_.global_memory.end() || !block->second.address)fail(6);
        else if(block->second.size<20 || block->second.size>output_limit)fail(13);
        else {
            const auto base=block->second.address,size=block->second.size;
            if(symbol=="DragQueryPoint"){
                if(!arg(1))fail(87);else {const auto point=memory.load(GuestAddress{base+4},64);const bool client=memory.load(GuestAddress{base+12},32)==0;memory.store(GuestAddress{arg(1)},64,point);result=client?1:0;}
            }else {
                const auto offset=memory.load(GuestAddress{base},32);const bool wide=memory.load(GuestAddress{base+16},32)!=0;const unsigned unit=wide?2:1;
                if(offset<20 || offset>size || size-offset<unit*2)fail(13);
                else {
                    std::vector<std::u16string> paths;std::uint64_t at=offset;bool terminated=false;
                    while(at+unit<=size){
                        if(!memory.load(GuestAddress{base+at},wide?16:8)){terminated=!paths.empty() || (at+unit*2<=size && !memory.load(GuestAddress{base+at+unit},wide?16:8));break;}
                        std::u16string text;std::string ansi;bool ended=false;
                        while(at+unit<=size){const auto c=memory.load(GuestAddress{base+at},wide?16:8);at+=unit;if(!c){ended=true;break;}if(wide)text+=static_cast<char16_t>(c);else ansi+=static_cast<char>(c);}
                        if(!ended)break;paths.push_back(wide?std::move(text):utf8_to_utf16_lossy(ansi));
                    }
                    if(!terminated)fail(13);
                    else if(dword(1)==UINT32_MAX)result=paths.size();
                    else if(dword(1)<paths.size()){
                        const auto& text=paths[dword(1)];const bool output_wide=symbol.ends_with('W');const auto ansi=output_wide?std::string{}:utf16_to_utf8(text);const auto length=output_wide?text.size():ansi.size();
                        if(!arg(2))result=length;
                        else if(dword(3)){
                            result=std::min<std::size_t>(length,dword(3)-1);
                            if(output_wide)memory.write(GuestAddress{arg(2)},utf16_bytes(text.substr(0,result)));
                            else {std::vector<std::uint8_t> bytes(ansi.begin(),ansi.begin()+static_cast<std::ptrdiff_t>(result));bytes.push_back(0);memory.write(GuestAddress{arg(2)},bytes);}
                        }
                    }
                }
            }
        }
    }else if (symbol == "GlobalAlloc") {
        // 已淘汰的 DDESHARE／DISCARDABLE 等旗標可接受；配置一律留在客體 heap。
        if (dword(0) & ~0x7f72U) fail(87);
        else { result = allocate_global(arg(1), dword(0)); if (!result) last_error_ = 8; }
    } else if (symbol == "GlobalReAlloc" || symbol == "LocalReAlloc") {
        try {
        const auto handle=arg(0),requested=arg(1);const auto flags=dword(2);
        auto found=gui_.global_memory.find(handle);
        if(found==gui_.global_memory.end() && runtime_.local_allocations.contains(handle)) {
            const auto allocation=allocations_.find(handle);
            if(allocation!=allocations_.end())found=gui_.global_memory.emplace(handle,GuiGlobalMemory{handle,allocation->second,0,0,false}).first;
        }
        if(flags&~0x7ff2U)fail(87);
        else if(found==gui_.global_memory.end())fail(6);
        else if(found->second.clipboard_owned)fail(5);
        else {
            auto& block=found->second;const bool movable=(block.flags&2)!=0;
            if(flags&0x80U) {
                // MODIFY 忽略大小。Global 可將 fixed 改為 handle；Local 保留原型態。
                if(!movable && symbol=="GlobalReAlloc" && (flags&2)) {
                    if(gui_.global_memory.size()>=65536)fail(8);
                    else {
                        auto replacement=block;replacement.flags|=flags&0x2102U;
                        const auto next=gui_.next_object;
                        gui_.global_memory.emplace(next,replacement);++gui_.next_object;
                        gui_.global_memory.erase(found);runtime_.local_allocations.erase(handle);result=next;
                    }
                } else {if(movable)block.flags|=flags&0x2100U;result=handle;}
            } else if(flags&(symbol=="LocalReAlloc"?0xf00U:0x100U))fail(87);
            else if(movable && !requested) {
                if(!(flags&2) || block.locks)fail(87);
                else {
                    if(block.address)(void)free_heap(memory,block.address,state);
                    block.address=0;block.size=0;result=handle;
                }
            } else if(requested>memory.limit())fail(8);
            else {
                const auto allocation=allocations_.find(block.address);
                const auto capacity=allocation==allocations_.end()?0:allocation->second;
                if(block.address && requested<=capacity) {
                    if((flags&0x40U) && requested>block.size) {
                        std::vector<std::uint8_t> zeros(static_cast<std::size_t>(requested-block.size));
                        memory.write(GuestAddress{block.address+block.size},zeros);
                    }
                    block.size=requested;result=handle;
                } else if((!movable || block.locks) && !(flags&2))fail(8);
                else {
                    const auto keep=static_cast<std::size_t>(std::min(block.size,requested));
                    std::vector<std::uint8_t> bytes(keep);
                    if(keep)memory.read(GuestAddress{block.address},bytes);
                    const auto address=allocate_heap(memory,requested);
                    if(!address)fail(8);
                    else {
                        try {
                            if(keep)memory.write(GuestAddress{address},bytes);
                            auto replacement=block;replacement.address=address;replacement.size=requested;
                            if(!movable)gui_.global_memory.emplace(address,replacement);
                            const bool local=runtime_.local_allocations.contains(block.address);
                            if(local && !movable)runtime_.local_allocations.insert(address);
                            if(block.address)(void)free_heap(memory,block.address,state);
                            if(local)runtime_.local_allocations.erase(block.address);
                            if(movable){block.address=address;block.size=requested;result=handle;}
                            else {gui_.global_memory.erase(found);result=address;}
                        } catch(...) {
                            if(!movable)gui_.global_memory.erase(address);
                            runtime_.local_allocations.erase(address);
                            (void)free_heap(memory,address,state);throw;
                        }
                    }
                }
            }
        }
        } catch(const std::bad_alloc&) { fail(8); }
    } else if (symbol == "GlobalHandle") {
        for (const auto& [handle, block] : gui_.global_memory)
            if (block.address == arg(0) && block.address) { result = handle; break; }
        if (!result) last_error_ = 6;
    } else if (symbol == "GlobalFree" || symbol == "GlobalLock" || symbol == "GlobalUnlock" ||
               symbol == "GlobalSize" || symbol == "GlobalFlags") {
        const auto found = gui_.global_memory.find(arg(0));
        if (symbol == "GlobalFree" && !arg(0)) result = 0;
        else if (found == gui_.global_memory.end()) {
            fail(6);
            if (symbol == "GlobalFree") result = arg(0);
            else if (symbol == "GlobalFlags") result = 0x8000;
        } else {
            auto& block = found->second;
            if (symbol == "GlobalSize") result = block.size;
            else if (symbol == "GlobalFlags") result = std::min(block.locks, 255U) | ((block.flags&2)?block.flags&0x2100U:0U) | ((block.flags & 2) && !block.address ? 0x4000U : 0U);
            else if (symbol == "GlobalLock") {
                if (!block.address) fail(157);
                else if ((block.flags & 2) && block.locks == UINT32_MAX) fail(8);
                else { if (block.flags & 2) ++block.locks; result = block.address; }
            } else if (symbol == "GlobalUnlock") {
                if (!(block.flags & 2)) result = 1;
                else if (!block.locks) fail(158);
                else { --block.locks; result = block.locks ? 1 : 0; if (!block.locks) last_error_ = 0; }
            } else {
                if (block.clipboard_owned) { last_error_ = 5; result = arg(0); }
                else if (block.address && !free_heap(memory, block.address, state)) { last_error_ = 6; result = arg(0); }
                else { runtime_.local_allocations.erase(block.address);gui_.global_memory.erase(found); result = 0; }
            }
        }
    } else if(symbol=="GetClipboardSequenceNumber")result=gui_.clipboard_sequence;
    else if(symbol=="GetOpenClipboardWindow")result=gui_.clipboard_open_window;
    else if(symbol=="AddClipboardFormatListener" || symbol=="RemoveClipboardFormatListener"){
        if(!gui_.windows.contains(arg(0)))fail(1400);
        else if(symbol=="AddClipboardFormatListener") {if(!gui_.clipboard_listeners.insert(arg(0)).second)fail(87);else result=1;}
        else if(!gui_.clipboard_listeners.erase(arg(0)))fail(87);else result=1;
    }else if(symbol=="CountClipboardFormats" || symbol=="EnumClipboardFormats" || symbol=="GetUpdatedClipboardFormats" || symbol=="GetPriorityClipboardFormat"){
        const auto formats=clipboard_formats();
        if(symbol=="CountClipboardFormats")result=formats.size();
        else if(symbol=="EnumClipboardFormats"){
            if(gui_.clipboard_open_thread!=threads_.current)fail(1418);
            else if(!dword(0)){result=formats.empty()?0:formats.front();if(!result)last_error_=0;}
            else {const auto found=std::find(formats.begin(),formats.end(),dword(0));if(found==formats.end())fail(87);else {result=std::next(found)==formats.end()?0:*std::next(found);if(!result)last_error_=0;}}
        }else if(symbol=="GetUpdatedClipboardFormats"){
            if(!arg(2) || (dword(1) && !arg(0)))fail(87);
            else {memory.store(GuestAddress{arg(2)},32,formats.size());if(dword(1)<formats.size())fail(122);else {memory.check(GuestAddress{arg(0)},formats.size()*4,Access::Write);for(std::size_t i=0;i<formats.size();++i)memory.store(GuestAddress{arg(0)+4*i},32,formats[i]);result=1;}}
        }else if(dword(1)>65536 || (dword(1) && !arg(0))){last_error_=87;result=UINT32_MAX;}
        else if(!formats.empty()){memory.check(GuestAddress{arg(0)},static_cast<std::size_t>(dword(1))*4,Access::Read);result=UINT32_MAX;for(unsigned i=0;i<dword(1);++i){const auto format=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)+4ULL*i},32));if(std::find(formats.begin(),formats.end(),format)!=formats.end()){result=format;break;}}}
    }else if(symbol.starts_with("GetClipboardFormatName")){
        const auto found=gui_.clipboard_format_names.find(dword(0));const bool wide=symbol.ends_with('W');
        if(found==gui_.clipboard_format_names.end() || !arg(1) || signed_word(arg(2))<=0)fail(87);
        else {const auto narrow=wide?std::string{}:utf16_to_utf8(found->second);const auto length=wide?found->second.size():narrow.size();result=std::min<std::size_t>(length,dword(2)-1);memory.check(GuestAddress{arg(1)},(result+1)*(wide?2:1),Access::Write);for(std::size_t i=0;i<result;++i)memory.store(GuestAddress{arg(1)+i*(wide?2:1)},wide?16:8,wide?found->second[i]:static_cast<unsigned char>(narrow[i]));memory.store(GuestAddress{arg(1)+result*(wide?2:1)},wide?16:8,0);}
    } else if (symbol == "OpenClipboard") {
        if (arg(0) && !gui_.windows.contains(arg(0))) fail(1400);
        else if (gui_.clipboard_open_thread) fail(5);
        else {
            if (!(ui_limits&2U) && native_gui_ && interactive_station()) {
                const auto text = native_gui_->clipboard_text();
                if (text != gui_.clipboard_native_text) {
                    release_clipboard(); gui_.clipboard_owner = 0; gui_.clipboard_native_text = text;
                }
            }
            gui_.clipboard_open_thread = threads_.current;
            gui_.clipboard_open_window = arg(0); result = 1;
        }
    } else if (symbol == "CloseClipboard") {
        if (gui_.clipboard_open_thread != threads_.current) fail(1418);
        else { gui_.clipboard_open_thread = gui_.clipboard_open_window = 0;if(gui_.clipboard_dirty)notify_clipboard(); result = 1; }
    } else if (symbol == "EmptyClipboard") {
        if (gui_.clipboard_open_thread != threads_.current) fail(1418);
        else if (native_gui_ && interactive_station() && !native_gui_->clear_clipboard()) fail(5);
        else {
            const auto previous_owner=gui_.clipboard_owner;
            release_clipboard(); gui_.clipboard_owner = gui_.clipboard_open_window;
            gui_.clipboard_native_text.reset();refresh_clipboard(true); result = 1;
            if(previous_owner && gui_.windows.contains(previous_owner)){
                start_callbacks(state,memory,{{resolve("user32.dll","SendMessageW").value,{previous_owner,0x307,0,0}}},Continuation::Return,1);return std::nullopt;
            }
        }
    } else if (symbol == "GetClipboardOwner") result = gui_.clipboard_owner;
    else if (symbol.starts_with("RegisterClipboardFormat")) {
        if (!arg(0)) fail(87);
        else {
            const auto original=symbol.ends_with('W') ? read_utf16(memory, GuestAddress{arg(0)}) : utf8_to_utf16_lossy(read_ansi(memory, arg(0)));auto name=lower_name(original);
            if (name.empty() || name.size() > 255) fail(87);
            else if (const auto found = gui_.clipboard_formats.find(name); found != gui_.clipboard_formats.end()) result = found->second;
            else if (gui_.clipboard_next_format > 0xffff) fail(8);
            else { result = gui_.clipboard_next_format++; gui_.clipboard_formats.emplace(std::move(name), static_cast<std::uint32_t>(result));gui_.clipboard_format_names.emplace(static_cast<std::uint32_t>(result),original); }
        }
    } else if (symbol == "SetClipboardData") {
        const auto format=dword(0),type=format==2?16U:format==14?64U:1U;
        const bool private_format=format>=0x200 && format<=0x2ff;
        const auto found = gui_.global_memory.find(arg(1));
        const bool rendering=std::any_of(gui_.clipboard_operations.begin(),gui_.clipboard_operations.end(),[&](const auto& p){const auto& op=p.second;
            return op.kind==GuiClipboardOperation::Kind::Render && !op.source && op.source_thread==threads_.current && op.window==gui_.clipboard_owner && op.type==format;});
        if (gui_.clipboard_open_thread != threads_.current && !rendering) fail(1418);
        else if (!gui_.clipboard_owner) fail(5);
        else if (!format || format>0xffff) fail(87);
        else if (arg(1) && !private_format && ((type==16 && !gui_.bitmaps.contains(arg(1))) || (type==64 && !gui_.metafiles.contains(arg(1))) || (type==1 && (found==gui_.global_memory.end() || !(found->second.flags&2))))) fail(6);
        else {
            std::optional<std::u16string> text;
            if (arg(1) && (format==1 || format==13)) text=global_text(arg(1),format);
            if (arg(1) && (format==1 || format==13) && !text) fail(87);
            else if (text && native_gui_ && interactive_station() && !native_gui_->set_clipboard_text(*text)) fail(5);
            else {
                const auto old = gui_.clipboard.find(format);
                if (old != gui_.clipboard.end() && old->second != arg(1)) {
                    const auto previous = old->second;
                    const auto old_type=gui_.clipboard_types.contains(format)?gui_.clipboard_types.at(format):1U;
                    gui_.clipboard.erase(old);
                    const bool retained = std::any_of(gui_.clipboard.begin(), gui_.clipboard.end(), [&](const auto& item) { return item.second == previous; });
                    const auto block = gui_.global_memory.find(previous);
                    if (!retained && !private_format) {
                        if(old_type==16){gui_.bitmaps.erase(previous);rasters_.erase(previous);}
                        else if(old_type==64)gui_.metafiles.erase(previous);
                        else if(block!=gui_.global_memory.end()){if(block->second.address)(void)free_heap(memory,block->second.address,state);gui_.global_memory.erase(block);}
                    }
                }
                if(found!=gui_.global_memory.end() && !private_format)found->second.clipboard_owned=true;
                gui_.clipboard[format]=arg(1);gui_.clipboard_types[format]=type;if(std::find(gui_.clipboard_order.begin(),gui_.clipboard_order.end(),format)==gui_.clipboard_order.end())gui_.clipboard_order.push_back(format);
                std::erase_if(gui_.clipboard_ole_entries,[&](const auto& entry){return entry.format.format==format;});
                if (text) gui_.clipboard_native_text = text;
                refresh_clipboard(true);result = arg(1);
            }
        }
    } else if (symbol == "GetClipboardData" || symbol == "IsClipboardFormatAvailable") {
        const bool get = symbol == "GetClipboardData";
        if (get && gui_.clipboard_open_thread != threads_.current) fail(1418);
        else if (const auto found = gui_.clipboard.find(dword(0)); found != gui_.clipboard.end()) result = get ? found->second : 1;
        else if (dword(0) == 1 || dword(0) == 13) {
            std::optional<std::u16string> text;
            const auto other = gui_.clipboard.find(dword(0) == 13 ? 1 : 13);
            if (other != gui_.clipboard.end()) text = global_text(other->second, other->first);
            else text = native_gui_ && interactive_station() ? native_gui_->clipboard_text() : gui_.clipboard_native_text;
            if (text) { result = get ? cache_text(dword(0), *text) : 1; if (!result) last_error_ = 8; }
        }
    } else if(symbol=="TrackMouseEvent" || symbol=="_TrackMouseEvent"){
        const unsigned size=x86_?16U:24U,hover_offset=x86_?12U:16U;const std::uint8_t bits=x86_?32:64;
        if(!arg(0))fail(87);
        else try{
            if(memory.load(GuestAddress{arg(0)},32)!=size)fail(87);
            else {memory.check(GuestAddress{arg(0)},size,Access::Read);const auto flags=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)+4},32));const auto handle=memory.load(GuestAddress{arg(0)+8},bits);
                if(flags&0x40000000U){memory.check(GuestAddress{arg(0)},size,Access::Write);const auto found=gui_.mouse_tracking.find(threads_.current);const auto track=found==gui_.mouse_tracking.end()?GuiMouseTracking{}:found->second;memory.store(GuestAddress{arg(0)+4},32,track.flags);memory.store(GuestAddress{arg(0)+8},bits,track.window);memory.store(GuestAddress{arg(0)+hover_offset},32,track.hover_time);result=1;}
                else if(flags&~0x80000013U)fail(87);
                else if(!gui_.windows.contains(handle))fail(1400);
                else if(gui_.windows.at(handle).thread!=threads_.current)fail(5);
                else if(flags&0x80000000U){const auto found=gui_.mouse_tracking.find(threads_.current);if(found!=gui_.mouse_tracking.end() && found->second.window==handle && ((found->second.flags^flags)&16)==0){found->second.flags&=~(flags&3U);if(!(found->second.flags&3))gui_.mouse_tracking.erase(found);}result=1;}
                else if(!(flags&3))result=1;
                else {
                    if(native_gui_)if(const auto point=native_gui_->cursor_position())gui_.pointer_position=*point;
                    auto& track=gui_.mouse_tracking[threads_.current];if(track.window!=handle || ((track.flags^flags)&16)){track={};track.window=handle;track.flags=flags&16;}
                    track.flags|=flags&3;
                    if(flags&1){const auto requested=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)+hover_offset},32));track.hover_time=requested==UINT32_MAX?gui_.mouse_hover_time:requested;track.deadline=runtime_.clock_ticks+std::uint64_t{track.hover_time}*10000;track.anchor=gui_.pointer_position;}
                    pump_mouse_tracking();result=1;
                }
            }
        }catch(const MemoryFault&){fail(998);}
    } else if(symbol=="GetAsyncKeyState"){
        const auto key=dword(0);
        if(key<256 && interactive_station() && gui_.input_desktop==thread_desktop_object(threads_.current)){
            pump_gui(memory,false);auto& value=gui_.async_keyboard[key];
            if(native_gui_ && !gui_.injected_keys[key])if(const auto down=native_gui_->physical_key_state(key)){if(*down && !(value&0x80))value|=1;value=static_cast<std::uint8_t>((value&1U)|(*down?0x80U:0));}
            result=((value&0x80)?0x8000U:0U)|(value&1U);value&=0x80;
        }
    } else if(symbol=="GetLastInputInfo"){
        if(!arg(0))fail(87);
        else try{
            if(memory.load(GuestAddress{arg(0)},32)!=8)fail(87);
            else {memory.check(GuestAddress{arg(0)},8,Access::Write);pump_gui(memory,false);auto tick=gui_.last_input_ticks/10000;
                if(native_gui_ && interactive_station())if(const auto idle=native_gui_->input_idle_milliseconds()){const auto now=runtime_.clock_ticks/10000;tick=std::max(tick,now-std::min(now,*idle));}
                memory.store(GuestAddress{arg(0)+4},32,static_cast<std::uint32_t>(tick));result=1;}
        }catch(const MemoryFault&){fail(998);}
    } else if(symbol=="SetCursorPos"){
        if(!interactive_station() || gui_.input_desktop!=thread_desktop_object(threads_.current) || (gui_.station_handle && !(gui_.user_handles.at(gui_.station_handle).access&0x10)))fail(5);
        else {auto x=signed_word(arg(0)),y=signed_word(arg(1));if(gui_.cursor_clip){const auto& rect=*gui_.cursor_clip;x=std::clamp(x,rect[0],rect[2]-1);y=std::clamp(y,rect[1],rect[3]-1);}
            if(native_gui_ && !native_gui_->set_cursor_position(x,y))fail(5);else{gui_.pointer_position={x,y};if(native_gui_)dinput_cursor_warped();result=1;}}
    } else if(symbol=="GetCursorInfo"){
        const unsigned size=x86_?20:24,point=x86_?12:16;
        if(!arg(0) || memory.load(GuestAddress{arg(0)},32)!=size)fail(87);
        else {memory.check(GuestAddress{arg(0)},size,Access::Write);if(native_gui_)if(const auto position=native_gui_->cursor_position())gui_.pointer_position=*position;
            memory.store(GuestAddress{arg(0)+4},32,gui_.cursor && gui_.cursor_count>=0?1:0);memory.store(GuestAddress{arg(0)+8},x86_?32:64,gui_.cursor);
            memory.store(GuestAddress{arg(0)+point},32,static_cast<std::uint32_t>(gui_.pointer_position[0]));memory.store(GuestAddress{arg(0)+point+4},32,static_cast<std::uint32_t>(gui_.pointer_position[1]));result=1;}
    } else if (symbol == "GetCursorPos") {
        if (!arg(0)) fail(87);
        else {
            memory.check(GuestAddress{arg(0)}, 8, Access::Write);
            if (native_gui_) if (const auto position = native_gui_->cursor_position()) gui_.pointer_position = *position;
            memory.store(GuestAddress{arg(0)}, 32, static_cast<std::uint32_t>(gui_.pointer_position[0]));
            memory.store(GuestAddress{arg(0) + 4}, 32, static_cast<std::uint32_t>(gui_.pointer_position[1])); result = 1;
        }
    } else if (symbol == "GlobalMemoryStatus" || symbol == "GlobalMemoryStatusEx") {
        const bool extended = symbol.ends_with("Ex");
        if (!arg(0) || (extended && memory.load(GuestAddress{arg(0)}, 32) != 64)) fail(87);
        else {
            try{
                std::array<std::uint64_t,6> values{};const auto error=memory_status_fields(memory,values);
                if(error)fail(error);else{
                    const unsigned unit=extended || !x86_?8:4,size=extended?64:8+6*unit;
                    memory.check(GuestAddress{arg(0)},size,Access::Write);memory.write(GuestAddress{arg(0)},std::vector<std::uint8_t>(size));
                    memory.store(GuestAddress{arg(0)},32,size);memory.store(GuestAddress{arg(0)+4},32,(values[0]-values[1])*100/values[0]);
                    for(unsigned i=0;i<values.size();++i){auto value=values[i];
                        if(unit==4){value=std::min<std::uint64_t>(value,UINT32_MAX);
                            if(i<2 && !runtime_.image.large_address_aware && value>UINT64_C(0x80000000) && value<UINT32_MAX)value=UINT64_C(0x80000000);}
                        memory.store(GuestAddress{arg(0)+8+i*unit},static_cast<std::uint8_t>(unit*8),value);
                    }
                    result=extended?1:0;
                }
            }catch(const MemoryFault&){fail(998);}catch(const std::bad_alloc&){fail(8);}
        }
    } else if (symbol == "GetThreadTimes" || symbol == "GetProcessTimes" || symbol == "QueryProcessCycleTime") {
        const bool cycles = symbol == "QueryProcessCycleTime",process = symbol != "GetThreadTimes";
        const auto tid = process ? 0 : thread_id(arg(0));
        const bool known=process?process_object_type(arg(0))==u"Process":tid!=0;
        const bool allowed=process?(process_handle_pid(arg(0),0x400)!=0 || process_handle_pid(arg(0),0x1000)!=0):(thread_access(arg(0))&0x840U)!=0;
        if(!known)fail(6);
        else if(!allowed)fail(5);

        else if (!arg(1) || (!cycles && (!arg(2) || !arg(3) || !arg(4)))) fail(87);
        else if(process){
            std::array<std::uint64_t,5> values{};const auto error=query_process_accounting(memory,arg(0),values,cycles);
            if(error)fail(error);else if(cycles){memory.store(GuestAddress{arg(1)},64,values[4]);result=1;}
            else {for(unsigned i=1;i<=4;++i)memory.check(GuestAddress{arg(i)},8,Access::Write);for(unsigned i=0;i<4;++i)memory.store(GuestAddress{arg(i+1)},64,values[i]);result=1;}
        }
        else if(cycles){
            std::uint64_t total=0;for(const auto& [id,thread]:threads_.items){(void)id;total+=thread.cycles;}
            memory.store(GuestAddress{arg(1)},64,total);result=1;
        }
        else if(runtime_.host_clock && !process)fail(50); // 尚未按客體執行緒分攤宿主 CPU 時間。
        else {
            const auto cpu=runtime_.host_clock?host_process_cpu_time():std::optional<ProcessCpuTime>{ProcessCpuTime{runtime_.clock_ticks,0}};
            if(!cpu){fail(50);arg.return_to(state,result);return std::nullopt;}
            for (unsigned i = 1; i <= 4; ++i) memory.check(GuestAddress{arg(i)}, 8, Access::Write);
            const bool exited = !process && threads_.items.contains(tid) && threads_.items.at(tid).status == ThreadStatus::Exited;
            memory.store(GuestAddress{arg(1)}, 64, runtime_.process_creation_time);
            memory.store(GuestAddress{arg(2)}, 64, exited ? runtime_.system_filetime() : 0);
            memory.store(GuestAddress{arg(3)}, 64, cpu->kernel-(runtime_.host_clock?runtime_.process_start_kernel:0));
            memory.store(GuestAddress{arg(4)}, 64, cpu->user-(runtime_.host_clock?runtime_.process_start_user:0));result=1;
        }
    } else if (symbol == "MessageBeep") {
        if (native_gui_) native_gui_->beep(); result = 1;
    } else if (symbol == "MakeDragList") {
        const auto found = gui_.windows.find(arg(0));
        if (found == gui_.windows.end()) fail(1400);
        else if (found->second.class_name != u"listbox" || (found->second.style & (2U | 8U | 0x800U))) fail(87);
        else { gui_.drag_lists.insert(arg(0)); result = 1; }
    } else if (symbol == "LBItemFromPt") {
        result = UINT64_MAX;
        const auto found = gui_.windows.find(arg(0));
        if (found != gui_.windows.end() && found->second.class_name == u"listbox") {
            const auto& window = found->second;
            auto x = static_cast<std::int64_t>(signed_word(arg(1))), y = static_cast<std::int64_t>(signed_word(arg(1) >> 32));
            const auto origin=gui_client_origin(gui_,arg(0));x-=origin[0];y-=origin[1];
            const auto row = y / 16;
            if (x >= 0 && y >= 0 && static_cast<std::uint64_t>(x) < window.width && static_cast<std::uint64_t>(y) < window.height && static_cast<std::uint64_t>(row) < window.items.size()) result = static_cast<std::uint64_t>(row);
        } else last_error_ = 1400;
    } else if (symbol == "DrawInsert") {
        const auto found = gui_.windows.find(arg(1));
        if (found == gui_.windows.end() || found->second.parent != arg(0) || !gui_.drag_lists.contains(arg(1))) last_error_ = 1400;
        else {
            const auto position = signed_word(arg(2));
            if (position < 0) gui_.drag_insertions.erase(arg(1));
            else if (static_cast<std::size_t>(position) <= found->second.items.size()) gui_.drag_insertions[arg(1)] = position;
            else last_error_ = 87;
        }
    } else throw HostCallFault("尚未實作 GUI 延伸服務：" + std::string(symbol));
    arg.return_to(state, result);
    return std::nullopt;
}
}
