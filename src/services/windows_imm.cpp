#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>
namespace x64arm {
namespace {
std::vector<std::uint32_t> imm_clauses(std::u16string_view text){return {0,static_cast<std::uint32_t>(text.size())};}
std::vector<std::uint8_t> imm_bytes(std::u16string_view text,bool wide){
    if(wide){std::vector<std::uint8_t> bytes(text.size()*2);for(std::size_t i=0;i<text.size();++i){bytes[i*2]=static_cast<std::uint8_t>(text[i]);bytes[i*2+1]=static_cast<std::uint8_t>(text[i]>>8);}return bytes;}
    const auto value=utf16_to_utf8(text);return {value.begin(),value.end()};
}
std::uint32_t imm_offset(std::u16string_view text,std::uint32_t at,bool wide){if(!wide && at && at<text.size() && text[at]>=0xdc00 && text[at]<=0xdfff && text[at-1]>=0xd800 && text[at-1]<=0xdbff)--at;return wide?at:static_cast<std::uint32_t>(utf16_to_utf8(text.substr(0,at)).size());}
std::vector<std::uint8_t> imm_candidate_bytes(const GuiImeCandidateList& list,bool wide) {
    if(list.text.size()>1024)throw HostCallFault("IME 候選數量超過上限");
    const auto count=static_cast<std::uint32_t>(list.text.size());
    std::vector<std::uint8_t> bytes(24+4*std::max(count,1U));
    const auto put=[&](unsigned at,std::uint32_t value){for(unsigned i=0;i<4;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
    put(4,list.style);put(8,count);put(12,list.selection);put(16,list.page_start);put(20,list.page_size);
    for(unsigned i=0;i<count;++i){
        auto text=imm_bytes(list.text[i],wide);text.resize(text.size()+(wide?2U:1U));
        if(text.size()>1048576 || bytes.size()>1048576-text.size())throw HostCallFault("IME 候選資料超過上限");
        put(24+i*4,static_cast<std::uint32_t>(bytes.size()));bytes.insert(bytes.end(),text.begin(),text.end());
    }
    put(0,static_cast<std::uint32_t>(bytes.size()));return bytes;
}
bool imm_boundary(std::u16string_view text,std::uint32_t at){return at<=text.size() && !(at && at<text.size() && text[at]>=0xdc00 && text[at]<=0xdfff && text[at-1]>=0xd800 && text[at-1]<=0xdbff);}
void imm_font(GuestMemory& memory,GuiImmContext& context,std::uint64_t pointer,bool set,bool wide){
    if(wide){if(set)memory.read(GuestAddress{pointer},context.font);else memory.write(GuestAddress{pointer},context.font);return;}
    std::array<std::uint8_t,60> bytes{};
    if(set){
        memory.read(GuestAddress{pointer},bytes);std::copy_n(bytes.begin(),28,context.font.begin());std::fill(context.font.begin()+28,context.font.end(),0);
        std::string face;for(unsigned i=28;i<60 && bytes[i];++i)face.push_back(static_cast<char>(bytes[i]));const auto text=utf8_to_utf16_lossy(face);
        for(std::size_t i=0;i<std::min<std::size_t>(text.size(),31);++i){context.font[28+i*2]=static_cast<std::uint8_t>(text[i]);context.font[29+i*2]=static_cast<std::uint8_t>(text[i]>>8);}
    }else {
        std::copy_n(context.font.begin(),28,bytes.begin());std::u16string face;
        for(unsigned i=28;i<92;i+=2){const auto c=static_cast<char16_t>(context.font[i]|(context.font[i+1]<<8));if(!c)break;face.push_back(c);}
        const auto text=utf16_to_utf8(face);std::copy_n(text.begin(),std::min<std::size_t>(31,text.size()),bytes.begin()+28);memory.write(GuestAddress{pointer},bytes);
    }
}
std::vector<std::uint8_t> imm_composition_bytes(const GuiImeData& data){
    std::vector<std::uint8_t> bytes(100);
    const auto put=[&](unsigned index,std::uint32_t value){for(unsigned i=0;i<4;++i)bytes[index*4+i]=static_cast<std::uint8_t>(value>>(i*8));};
    const auto append=[&](unsigned length,unsigned offset,std::span<const std::uint8_t> part,unsigned unit=1){
        put(length,static_cast<std::uint32_t>(part.size()/unit));put(offset,static_cast<std::uint32_t>(bytes.size()));bytes.insert(bytes.end(),part.begin(),part.end());while(bytes.size()%4)bytes.push_back(0);
    };
    const auto clauses=[&](unsigned length,unsigned offset,const std::vector<std::uint32_t>& values){append(length,offset,{reinterpret_cast<const std::uint8_t*>(values.data()),values.size()*4});};
    append(1,2,data.reading_attributes);clauses(3,4,data.reading_clauses);append(5,6,imm_bytes(data.reading,true),2);
    append(7,8,data.attributes);clauses(9,10,data.clauses);append(11,12,imm_bytes(data.text,true),2);put(13,data.cursor);put(14,data.delta);
    clauses(15,16,data.result_reading_clauses);append(17,18,imm_bytes(data.result_reading,true),2);clauses(19,20,data.result_clauses);append(21,22,imm_bytes(data.result,true),2);
    put(0,static_cast<std::uint32_t>(bytes.size()));return bytes;
}
std::vector<std::uint8_t> imm_candidate_info(const GuiImeData& data){
    if(data.candidate_lists.size()>32)throw std::length_error("IME 候選資料超過上限");std::vector<std::uint8_t> bytes(144);
    const auto put=[&](unsigned at,std::uint32_t value){for(unsigned i=0;i<4;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
    put(4,static_cast<std::uint32_t>(data.candidate_lists.size()));for(unsigned i=0;i<data.candidate_lists.size();++i){put(8+i*4,static_cast<std::uint32_t>(bytes.size()));const auto list=imm_candidate_bytes(data.candidate_lists[i],true);bytes.insert(bytes.end(),list.begin(),list.end());while(bytes.size()%4)bytes.push_back(0);}put(0,static_cast<std::uint32_t>(bytes.size()));return bytes;
}
void imm_commit_buffer(GuestMemory& memory,GuiImmBuffer& buffer,GuiImeData& data){
    if(buffer.kind>1 || !buffer.size)return;std::vector<std::uint8_t> bytes(buffer.size);memory.read(GuestAddress{buffer.storage},bytes);if(bytes==buffer.snapshot)return;
    const auto word=[&](std::size_t at){if(at>bytes.size() || bytes.size()-at<4)throw std::invalid_argument("IME 資料範圍錯誤");std::uint32_t value=0;for(unsigned i=0;i<4;++i)value|=std::uint32_t{bytes[at+i]}<<(i*8);return value;};
    const auto total=word(0);if(total>bytes.size() || total<(buffer.kind?144U:100U))throw std::invalid_argument("IME 資料長度錯誤");
    const auto range=[&](std::uint32_t offset,std::size_t count){if(offset>total || count>total-offset)throw std::invalid_argument("IME 資料範圍錯誤");return std::span(bytes).subspan(offset,count);};
    const auto text=[&](std::uint32_t offset,unsigned count){if(count>32767)throw std::invalid_argument("IME 文字超過上限");const auto part=range(offset,std::size_t(count)*2);std::u16string value(count,0);for(unsigned i=0;i<count;++i)value[i]=static_cast<char16_t>(part[i*2]|(part[i*2+1]<<8));return value;};
    auto changed=data;
    if(!buffer.kind){
        const auto string=[&](unsigned length,unsigned offset){return text(word(offset*4),word(length*4));};
        const auto attributes=[&](unsigned length,unsigned offset,std::u16string_view value){const auto part=range(word(offset*4),word(length*4));if(part.empty())return std::vector<std::uint8_t>(value.size(),3);if(part.size()!=value.size())throw std::invalid_argument("IME 屬性長度錯誤");return std::vector<std::uint8_t>(part.begin(),part.end());};
        const auto clauses=[&](unsigned length,unsigned offset,std::u16string_view value){const auto count=word(length*4),at=word(offset*4);if(!count)return imm_clauses(value);if(count%4 || count>131072)throw std::invalid_argument("IME 分段長度錯誤");(void)range(at,count);std::vector<std::uint32_t> list;for(unsigned i=0;i<count;i+=4){const auto index=word(at+i);if(!imm_boundary(value,index) || (!list.empty() && (index<list.back() || (index==list.back() && !value.empty()))))throw std::invalid_argument("IME 分段位置錯誤");list.push_back(index);}if(list.front()!=0 || list.back()!=value.size())throw std::invalid_argument("IME 分段邊界錯誤");return list;};
        changed.reading=string(5,6);changed.text=string(11,12);changed.result_reading=string(17,18);changed.result=string(21,22);
        changed.reading_attributes=attributes(1,2,changed.reading);changed.attributes=attributes(7,8,changed.text);
        changed.reading_clauses=clauses(3,4,changed.reading);changed.clauses=clauses(9,10,changed.text);changed.result_reading_clauses=clauses(15,16,changed.result_reading);changed.result_clauses=clauses(19,20,changed.result);
        changed.cursor=word(52);changed.delta=word(56);if(!imm_boundary(changed.text,changed.cursor) || !imm_boundary(changed.text,changed.delta))throw std::invalid_argument("IME 游標位置錯誤");
        changed.composing=!changed.text.empty();changed.has_result=!changed.result.empty();changed.native_edit=false;changed.original=changed.text;
    }else {
        const auto count=word(4);if(count>32)throw std::invalid_argument("IME 候選數量錯誤");changed.candidate_lists.clear();
        for(unsigned i=0;i<count;++i){const auto at=word(8+i*4),size=word(at),length=word(at+8);if(length>1024 || size<24+std::size_t(length)*4)throw std::invalid_argument("IME 候選資料錯誤");(void)range(at,size);
            GuiImeCandidateList list;list.style=word(at+4);list.selection=word(at+12);list.page_start=word(at+16);list.page_size=word(at+20);
            for(unsigned j=0;j<length;++j){const auto offset=word(at+24+j*4);if(offset>=size)throw std::invalid_argument("IME 候選位置錯誤");std::u16string value;bool terminated=false;for(auto k=offset;k+1<size;k+=2){const auto part=range(at+k,2);const auto c=static_cast<char16_t>(part[0]|(part[1]<<8));if(!c){terminated=true;break;}value.push_back(c);}if(!terminated)throw std::invalid_argument("IME 候選文字未結束");list.text.push_back(std::move(value));}
            changed.candidate_lists.push_back(std::move(list));
        }
    }
    data=std::move(changed);buffer.snapshot=std::move(bytes);
}
}
void WindowsServices::imm_release_context_storage(GuestMemory& memory,const GuestState& state,std::uint64_t handle){
    const auto found=gui_.input_contexts.find(handle);if(found==gui_.input_contexts.end())return;
    for(const auto buffer:found->second.buffers)if(const auto item=gui_.input_buffers.find(buffer);item!=gui_.input_buffers.end()){
        (void)free_heap(memory,item->second.storage,state);gui_.input_buffers.erase(item);
    }
    if(found->second.storage)(void)free_heap(memory,found->second.storage,state);
}
std::optional<std::uint64_t> WindowsServices::imm_control_message(GuestMemory& memory,const GuiMessage& message,bool wide){
    auto window=gui_.windows.find(message.window);
    if(window==gui_.windows.end() || gui_control_class(window->second)!=u"ime" || message.message!=0x283)return std::nullopt;
    auto& ime=window->second;const auto command=static_cast<std::uint32_t>(message.wparam);
    if(command==0xf)return (static_cast<std::uint32_t>(ime.x)&0xffffU)|((static_cast<std::uint32_t>(ime.y)&0xffffU)<<16);
    if(command==0x10){ime.x=static_cast<std::int16_t>(message.lparam);ime.y=static_cast<std::int16_t>(message.lparam>>16);if(ime.visible)present_gui(message.window);return 0;}
    if(command==0x21 || command==0x22){
        if(!native_gui_){last_error_=50;return 1;}
        ime.visible=command==0x22;ime.style=(ime.style&~0x10000000U)|(ime.visible?0x10000000U:0U);
        if(ime.visible){
            const auto methods=native_input_methods(false);const auto current=std::find_if(methods.begin(),methods.end(),[](const auto& method){return method.current;});
            ime.title=current==methods.end()?u"輸入法":current->description;
            GuiDraw text;text.kind=GuiDraw::Kind::Text;text.x=6;text.y=4;text.color=0;text.text=ime.title;
            ime.drawing={std::move(text)};present_gui(message.window);
        }
        show_native_window(message.window,ime.visible);return 0;
    }
    std::uint64_t target=0;
    if(gui_.windows.contains(gui_.focus) && gui_.windows.at(gui_.focus).thread==ime.thread && gui_.focus!=message.window)target=gui_.focus;
    else if(const auto found=gui_.input_window_targets.find(ime.thread);found!=gui_.input_window_targets.end() && gui_.windows.contains(found->second))target=found->second;
    const auto handle=imm_window_context(target);if(!handle){last_error_=6;return 1;}
    auto& context=gui_.input_contexts.at(handle);const auto pointer=message.lparam;
    if(command==7 || command==8){
        if(!pointer){last_error_=87;return 1;}const auto index=static_cast<std::uint32_t>(memory.load(GuestAddress{pointer},32));
        if(index>=4){last_error_=87;return 1;}
        if(command==8){std::array<std::uint8_t,32> bytes{};memory.read(GuestAddress{pointer},bytes);context.candidates[index]=bytes;return 0;}
        const auto found=context.candidates.find(index);if(found==context.candidates.end())return 1;memory.write(GuestAddress{pointer},found->second);return 0;
    }
    if(command==9 || command==10){if(!pointer){last_error_=87;return 1;}imm_font(memory,context,pointer,command==10,wide);return 0;}
    if(command==11 || command==12){if(!pointer){last_error_=87;return 1;}if(command==12)memory.read(GuestAddress{pointer},context.composition);else memory.write(GuestAddress{pointer},context.composition);return 0;}
    last_error_=50;return 1;
}
std::uint64_t WindowsServices::imm_window_context(std::uint64_t hwnd){
    if(!gui_.windows.contains(hwnd))return 0;
    if(const auto found=gui_.input_associations.find(hwnd);found!=gui_.input_associations.end())return found->second;
    const auto thread=gui_.windows.at(hwnd).thread;auto& handle=gui_.input_defaults[thread];
    if(!handle){if(gui_.input_contexts.size()>=4096)return 0;handle=gui_.next_object++;GuiImmContext context;context.thread=thread;context.default_context=true;gui_.input_contexts.emplace(handle,std::move(context));}
    return handle;
}
void WindowsServices::imm_notify_composition(std::uint64_t handle,std::uint32_t flags,bool start,bool end){
    const auto context=gui_.input_contexts.find(handle);if(context==gui_.input_contexts.end())return;
    std::uint64_t target=0;
    if(gui_.focus && imm_window_context(gui_.focus)==handle)target=gui_.focus;
    if(!target){const auto last=gui_.input_window_targets.find(context->second.thread);if(last!=gui_.input_window_targets.end() && imm_window_context(last->second)==handle)target=last->second;}
    if(!target)for(const auto& [hwnd,w]:gui_.windows)if(w.thread==context->second.thread && gui_control_class(w)!=u"ime" && imm_window_context(hwnd)==handle){target=hwnd;break;}
    if(!target)return;
    const auto push=[&](std::uint32_t message,std::uint32_t bits){GuiMessage msg{target,0,bits,message};msg.ime=context->second.data;msg.ime_context=handle;msg.thread=context->second.thread;enqueue_gui_message(std::move(msg));};
    if(start)push(0x10d,0);push(0x10f,flags);if(end)push(0x10e,0);
    if(native_gui_){const auto& form=context->second.composition;const auto coord=[&](unsigned at){std::uint32_t value=0;for(unsigned i=0;i<4;++i)value|=std::uint32_t{form[at+i]}<<(i*8);return std::bit_cast<std::int32_t>(value);};
        (void)native_gui_->ime_mark(target,context->second.data,{coord(4),coord(8)});}
}
bool WindowsServices::imm_native_message(GuiMessage& message){
    if(!message.ime || message.message!=0x10f)return false;
    const auto handle=imm_window_context(message.window);if(!handle)return true;
    auto& context=gui_.input_contexts.at(handle);auto data=*message.ime;
    if(data.text.size()>32767 || data.result.size()>32767 || data.candidate_lists.size()>32)return true;
    if(!context.data.composing && data.composing)data.original=data.text;
    else if(data.composing)data.original=context.data.original;
    const bool start=!context.data.composing && data.composing,end=context.data.composing && !data.composing;
    context.data=std::move(data);context.open=true;
    message.ime=context.data;message.ime_context=handle;message.thread=context.thread;
    if(start){GuiMessage msg{message.window,0,0,0x10d};msg.ime=context.data;msg.ime_context=handle;enqueue_gui_message(std::move(msg));}
    enqueue_gui_message(message);
    if(end){GuiMessage msg{message.window,0,0,0x10e};msg.ime=context.data;msg.ime_context=handle;enqueue_gui_message(std::move(msg));}
    return true;
}
void WindowsServices::imm_default_result(const GuiMessage& message){
    if(message.message!=0x10f || !(message.lparam&0x800U))return;
    const auto handle=imm_window_context(message.window);if(!handle)return;
    const auto& data=gui_.input_contexts.at(handle).data;if(data.native_edit)return;
    for(const auto c:data.result)enqueue_gui_message({message.window,c,1,0x102});
}
std::span<const WindowsServiceDescriptor> imm_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{{"imm32.dll","ImmGetCandidateListA",4},{"imm32.dll","ImmGetCandidateListW",4},{"imm32.dll","ImmGetCandidateListCountA",2},{"imm32.dll","ImmGetCandidateListCountW",2},{"imm32.dll","ImmSetCompositionStringA",6},{"imm32.dll","ImmSetCompositionStringW",6},{"imm32.dll","ImmNotifyIME",4},{"imm32.dll","ImmGetIMEFileNameA",3},{"imm32.dll","ImmGetIMEFileNameW",3},
        {"imm32.dll","ImmGetDefaultIMEWnd",1},{"imm32.dll","ImmGetProperty",2},{"imm32.dll","ImmGetDescriptionA",3},{"imm32.dll","ImmGetDescriptionW",3},
        {"imm32.dll","ImmLockIMC",1},{"imm32.dll","ImmUnlockIMC",1},{"imm32.dll","ImmGetIMCLockCount",1},
        {"imm32.dll","ImmCreateIMCC",1},{"imm32.dll","ImmDestroyIMCC",1},{"imm32.dll","ImmLockIMCC",1},{"imm32.dll","ImmUnlockIMCC",1},
        {"imm32.dll","ImmReSizeIMCC",2},{"imm32.dll","ImmGetIMCCSize",1},{"imm32.dll","ImmGetIMCCLockCount",1}};return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_imm(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;std::uint64_t result=0;
    const auto d=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    const auto fail=[&](std::uint32_t error){last_error_=error;result=0;};
    try {
        const auto current=[&](std::uint64_t hwnd){
            const auto associated=gui_.input_associations.find(hwnd);if(associated!=gui_.input_associations.end())return associated->second;
            const auto thread=gui_.windows.at(hwnd).thread;auto& handle=gui_.input_defaults[thread];
            if(!handle){handle=gui_.next_object++;GuiImmContext context;context.thread=thread;context.default_context=true;gui_.input_contexts.emplace(handle,std::move(context));}
            return handle;
        };
#include "imm_memory_handlers.inc"
        if(imm_memory_handled){}
        else if(symbol=="ImmIsIME" || symbol=="ImmGetProperty" || symbol.starts_with("ImmGetDescription") || symbol.starts_with("ImmGetIMEFileName")){
            const auto methods=native_input_methods(symbol.starts_with("ImmGetIMEFileName"));const auto method=std::find_if(methods.begin(),methods.end(),[&](const auto& item){return item.layout==d(0);});
            if(symbol=="ImmIsIME")result=method!=methods.end();
            else if(method==methods.end())fail(2);
            else if(symbol=="ImmGetProperty"){
                // 回報共用 Unicode／原生 caret 橋接真正提供的能力。
                switch(d(1)){
                    case 4:result=0x00090000;break; // AT_CARET | UNICODE
                    case 0x14:result=1;break; // SCS_CAP_COMPSTR；不宣告自動讀音或 reconversion。
                    case 8:case 0xc:case 0x10:case 0x18:result=0;break;
                    case 0xfffffffcU:result=0x00040000;break;
                    default:fail(87);break;
                }
            }
            else if(symbol.starts_with("ImmGetIMEFileName") && method->filename.empty())fail(2);
            else {const auto& text=symbol.starts_with("ImmGetDescription")?method->description:method->filename;
                const bool wide=symbol.ends_with('W');const auto bytes=imm_bytes(text,wide);const auto count=static_cast<std::uint32_t>(wide?bytes.size()/2:bytes.size());
                if(!d(2))result=count;
                else if(!arg(1))fail(87);
                else {const auto copied=std::min(count,d(2)-1),unit=wide?2U:1U;memory.check(GuestAddress{arg(1)},(copied+1)*unit,Access::Write);memory.write(GuestAddress{arg(1)},std::span(bytes).first(copied*unit));memory.store(GuestAddress{arg(1)+copied*unit},static_cast<std::uint8_t>(unit*8),0);result=copied;}
            }
        }
        else if(symbol=="ImmGetDefaultIMEWnd"){
            const auto target=gui_.windows.find(arg(0));
            if(arg(0) && target==gui_.windows.end())fail(1400);
            else {
                const auto thread=arg(0)?target->second.thread:threads_.current;
                if(arg(0))gui_.input_window_targets[thread]=arg(0);
                const auto existing=gui_.input_windows.find(thread);
                if(existing!=gui_.input_windows.end() && gui_.windows.contains(existing->second))result=existing->second;
                else if(gui_.windows.size()>=4096)fail(8);
                else {
                    GuiWindow window;window.thread=thread;window.class_name=window.control_class=u"ime";window.builtin=true;
                    window.procedure=stock_gui_proc(u"ime",true);window.class_atom=gui_class_record(u"ime")->atom;
                    window.style=0x80000000U;window.ex_style=0x08000080U;window.width=240;window.height=28;
                    result=allocate_gui_window();gui_.windows.emplace(result,std::move(window));gui_.input_windows[thread]=result;publish_gui_window(result);
                    if(native_gui_)native_gui_->create(result,gui_.windows.at(result));
                }
            }
        }
        else if(symbol=="ImmCreateContext"){
            if(gui_.input_contexts.size()>=4096)fail(8);else {result=gui_.next_object++;GuiImmContext context;context.thread=threads_.current;gui_.input_contexts.emplace(result,std::move(context));}
        }else if(symbol=="ImmGetContext" || symbol=="ImmReleaseContext" || symbol.starts_with("ImmAssociateContext")){
            const auto window=gui_.windows.find(arg(0));
            if(window==gui_.windows.end())fail(1400);
            else if(window->second.thread!=threads_.current)fail(5);
            else if(symbol=="ImmGetContext"){result=current(arg(0));if(result)++gui_.input_borrows[{arg(0),result}];}
            else if(symbol=="ImmReleaseContext"){
                const auto borrow=gui_.input_borrows.find({arg(0),arg(1)});if(borrow==gui_.input_borrows.end())fail(6);else {if(!--borrow->second){gui_.input_borrows.erase(borrow);if(gui_.input_contexts.contains(arg(1))){auto& data=gui_.input_contexts.at(arg(1)).data;data.has_result=false;data.result.clear();data.result_reading.clear();data.result_clauses.clear();data.result_reading_clauses.clear();}}result=1;}
            }else {
                const bool extended=symbol=="ImmAssociateContextEx";const auto flags=extended?d(2):0;
                const auto context=gui_.input_contexts.find(arg(1));
                if(flags&~0x31U)fail(87);
                else if(!(flags&0x10U) && arg(1) && (context==gui_.input_contexts.end() || context->second.thread!=threads_.current))fail(6);
                else {
                    const auto previous=current(arg(0));std::vector<std::uint64_t> targets;
                    if(!(flags&1U))targets.push_back(arg(0));
                    else for(const auto& [handle,w]:gui_.windows){if(w.thread!=window->second.thread || handle==arg(0))continue;auto parent=w.parent;std::set<std::uint64_t> visited;
                        while(parent && visited.insert(parent).second){if(parent==arg(0)){targets.push_back(handle);break;}const auto ancestor=gui_.windows.find(parent);if(ancestor==gui_.windows.end())break;parent=ancestor->second.parent;}}
                    for(const auto handle:targets){if((flags&0x20U) && !current(handle))continue;if(flags&0x10U)gui_.input_associations.erase(handle);else gui_.input_associations[handle]=arg(1);}
                    result=extended?1:previous;
                }
            }
        }else {
            const auto found=gui_.input_contexts.find(arg(0));
            if(found==gui_.input_contexts.end() || found->second.thread!=threads_.current){fail(6);if(symbol.starts_with("ImmGetCompositionString"))result=static_cast<std::uint32_t>(-2);}
            else {auto& context=found->second;
                if(symbol=="ImmDestroyContext"){
                    const bool busy=context.default_context || std::any_of(gui_.input_associations.begin(),gui_.input_associations.end(),[&](const auto& v){return v.second==arg(0);}) || std::any_of(gui_.input_borrows.begin(),gui_.input_borrows.end(),[&](const auto& v){return v.first.second==arg(0);});
                    if(busy)fail(170);else {imm_release_context_storage(memory,state,arg(0));gui_.input_contexts.erase(found);result=1;}
                }else if(symbol.starts_with("ImmGetCandidateList")) {
                    const bool wide=symbol.ends_with('W');const auto& lists=context.data.candidate_lists;
                    if(symbol.starts_with("ImmGetCandidateListCount")) {
                        if(!arg(1))fail(87);
                        else {memory.check(GuestAddress{arg(1)},4,Access::Write);for(const auto& list:lists)result+=imm_candidate_bytes(list,wide).size();memory.store(GuestAddress{arg(1)},32,lists.size());}
                    }else if(d(1)<lists.size()) {
                        const auto bytes=imm_candidate_bytes(lists[d(1)],wide);
                        if(!d(3))result=bytes.size();
                        else if(!arg(2))fail(87);
                        else if(d(3)<bytes.size())fail(122);
                        else {memory.write(GuestAddress{arg(2)},bytes);result=bytes.size();}
                    }
                }
                else if(symbol=="ImmGetOpenStatus")result=context.open;
                else if(symbol=="ImmSetOpenStatus"){
                    if(arg(1) && !native_gui_)fail(50);
                    else {context.open=arg(1)!=0;if(!context.open && context.data.composing){context.data={};imm_notify_composition(arg(0),0,false,true);}result=1;}
                }
                else if(symbol=="ImmGetConversionStatus"){
                    for(unsigned i=1;i<=2;++i)if(arg(i))memory.check(GuestAddress{arg(i)},4,Access::Write);
                    if(arg(1))memory.store(GuestAddress{arg(1)},32,context.conversion);if(arg(2))memory.store(GuestAddress{arg(2)},32,context.sentence);result=1;
                }else if(symbol=="ImmSetConversionStatus"){if(arg(1) || arg(2))fail(50);else {context.conversion=context.sentence=0;result=1;}}
                else if(symbol.starts_with("ImmGetCompositionString")){
                    const auto& data=context.data;const bool wide=symbol.ends_with('W');const auto index=d(1);std::vector<std::uint8_t> bytes;
                    const auto clauses=[&](const auto& values,std::u16string_view text){for(const auto at:values){const auto value=imm_offset(text,at,wide);for(unsigned i=0;i<4;++i)bytes.push_back(static_cast<std::uint8_t>(value>>(i*8)));}};
                    const auto attrs=[&](const auto& values,std::u16string_view text){if(wide){bytes=values;return;}for(std::uint32_t i=0;i<text.size();){const auto next=i+((text[i]>=0xd800 && text[i]<=0xdbff && i+1<text.size())?2:1);const auto length=imm_offset(text,next,false)-imm_offset(text,i,false);bytes.insert(bytes.end(),length,values[i]);i=next;}};
                    if((index==0x80 || index==0x100) && data.composing)result=imm_offset(data.text,index==0x80?data.cursor:data.delta,wide);
                    else if((index<0x200 && !data.composing) || (index>=0x200 && !data.has_result))result=UINT32_MAX;
                    else {switch(index){case 1:bytes=imm_bytes(data.reading,wide);break;case 2:attrs(data.reading_attributes,data.reading);break;case 4:clauses(data.reading_clauses,data.reading);break;
                        case 8:bytes=imm_bytes(data.text,wide);break;case 0x10:attrs(data.attributes,data.text);break;case 0x20:clauses(data.clauses,data.text);break;
                        case 0x200:bytes=imm_bytes(data.result_reading,wide);break;case 0x400:clauses(data.result_reading_clauses,data.result_reading);break;
                        case 0x800:bytes=imm_bytes(data.result,wide);break;case 0x1000:clauses(data.result_clauses,data.result);break;default:fail(87);result=UINT32_MAX-1;break;}
                        if(result!=UINT32_MAX-1){if(!d(3))result=bytes.size();else if(!arg(2)){fail(87);result=UINT32_MAX-1;}else {auto size=std::min<std::size_t>(bytes.size(),d(3));if(wide && (index==1 || index==8 || index==0x200 || index==0x800))size&=~std::size_t{1};memory.write(GuestAddress{arg(2)},std::span(bytes).first(size));result=size;}}
                    }
                }
                else if(symbol.starts_with("ImmSetCompositionString")){
                    const bool wide=symbol.ends_with('W');const auto index=d(1);auto changed=context.data;bool valid=true;
                    if(!arg(2) && !arg(4))valid=false;
                    for(unsigned i:{2U,4U})if((!arg(i) && d(i+1)) || d(i+1)>65534)valid=false;
                    if(index!=9 && index!=0x12 && index!=0x24){fail(50);valid=false;}
                    const auto set=[&](unsigned input,bool reading){
                        if(!arg(input) || !valid)return;const auto length=d(input+1);auto& text=reading?changed.reading:changed.text;auto& attributes=reading?changed.reading_attributes:changed.attributes;auto& clauses=reading?changed.reading_clauses:changed.clauses;
                        if(index==9){if(wide && (length&1)){valid=false;return;}std::vector<std::uint8_t> bytes(length);memory.read(GuestAddress{arg(input)},bytes);
                            if(wide){text.resize(length/2);for(std::size_t i=0;i<text.size();++i)text[i]=static_cast<char16_t>(bytes[i*2]|(bytes[i*2+1]<<8));}
                            else text=utf8_to_utf16(std::string(bytes.begin(),bytes.end()));
                            if(text.size()>32767){valid=false;return;}attributes.assign(text.size(),3);clauses=imm_clauses(text);
                        }else if(!changed.composing){valid=false;return;}
                        else if(index==0x12){std::vector<std::uint8_t> bytes(length);memory.read(GuestAddress{arg(input)},bytes);
                            if(wide){if(length!=text.size()){valid=false;return;}attributes=std::move(bytes);}
                            else {if(length!=imm_offset(text,static_cast<std::uint32_t>(text.size()),false)){valid=false;return;}attributes.clear();for(std::uint32_t i=0;i<text.size();++i)attributes.push_back(bytes[imm_offset(text,i,false)]);}
                            if(std::any_of(attributes.begin(),attributes.end(),[](auto value){return value>5;})){valid=false;return;}
                        }else {if(length<8 || length%4){valid=false;return;}std::vector<std::uint32_t> values;
                            for(unsigned i=0;i<length;i+=4){auto at=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(input)+i},32));
                                if(!wide){std::uint32_t j=0;while(j<=text.size() && imm_offset(text,j,false)<at)++j;if(j>text.size() || imm_offset(text,j,false)!=at){valid=false;return;}at=j;}
                                if(!imm_boundary(text,at) || (!values.empty() && at<=values.back())){valid=false;return;}values.push_back(at);}
                            if(values.front()!=0 || values.back()!=text.size()){valid=false;return;}clauses=std::move(values);
                        }
                        for(std::size_t c=1;c<clauses.size();++c)for(auto i=clauses[c-1];i<clauses[c];++i)if(attributes[i]!=attributes[clauses[c-1]])valid=false;
                    };
                    try{set(2,false);set(4,true);}catch(const std::invalid_argument&){valid=false;}
                    if(!valid){if(index==9 || index==0x12 || index==0x24)fail(87);}
                    else {const bool start=!context.data.composing;changed.composing=true;changed.has_result=changed.native_edit=false;changed.result.clear();changed.result_reading.clear();changed.cursor=static_cast<std::uint32_t>(changed.text.size());changed.delta=0;if(index==9)changed.original=changed.text;
                        context.data=std::move(changed);imm_notify_composition(arg(0),index==9?0x1bfU:index,start,false);
                        result=1;
                    }
                }
                else if(symbol=="ImmNotifyIME"){
                    const auto action=d(1),index=d(2);if(action!=0x15)fail(50);
                    else if(index==2 && context.data.composing){std::uint64_t hwnd=0;for(const auto& [h,w]:gui_.windows)if(w.thread==context.thread && imm_window_context(h)==arg(0)){hwnd=h;break;}if(!native_gui_ || !hwnd || !native_gui_->ime_convert(hwnd))fail(50);else result=1;}
                    else if(index==2)fail(87);
                    else if(index==1 || index==3 || index==4){const bool end=context.data.composing;
                        if(index==1){context.data.result=context.data.text;context.data.result_reading=context.data.reading;context.data.result_clauses=context.data.clauses;context.data.result_reading_clauses=context.data.reading_clauses;context.data.has_result=end;context.data.composing=context.data.native_edit=false;context.data.text.clear();context.data.reading.clear();imm_notify_composition(arg(0),0x1e00,false,end);}
                        else if(index==4){context.data={};imm_notify_composition(arg(0),0,false,end);}
                        else {if(!end)fail(87);else {context.data.text=context.data.original;context.data.attributes.assign(context.data.text.size(),3);context.data.clauses=imm_clauses(context.data.text);context.data.cursor=static_cast<std::uint32_t>(context.data.text.size());context.data.delta=0;imm_notify_composition(arg(0),0x1b8);}}
                        if(index!=3 || end)result=1;
                    }else fail(87);
                }
                else if(symbol=="ImmGetCompositionWindow" || symbol=="ImmSetCompositionWindow"){
                    if(!arg(1))fail(87);else {if(symbol.starts_with("ImmSet"))memory.read(GuestAddress{arg(1)},context.composition);else memory.write(GuestAddress{arg(1)},context.composition);result=1;}
                }else if(symbol=="ImmSetCandidateWindow" || symbol=="ImmGetCandidateWindow"){
                    const bool set=symbol=="ImmSetCandidateWindow";const auto pointer=arg(set?1:2);
                    if(!pointer)fail(87);else {const auto index=set?static_cast<std::uint32_t>(memory.load(GuestAddress{pointer},32)):d(1);
                        if(index>=4)fail(87);else if(set){std::array<std::uint8_t,32> bytes{};memory.read(GuestAddress{pointer},bytes);context.candidates[index]=bytes;result=1;}
                        else if(const auto item=context.candidates.find(index);item!=context.candidates.end()){memory.write(GuestAddress{pointer},item->second);result=1;}
                    }
                }else if(symbol.starts_with("ImmSetCompositionFont") || symbol.starts_with("ImmGetCompositionFont")){
                    const bool set=symbol.starts_with("ImmSet"),font_wide=symbol.ends_with('W');
                    if(!arg(1))fail(87);else {imm_font(memory,context,arg(1),set,font_wide);result=1;}
                }else fail(50);
            }
        }
    }catch(const std::bad_alloc&){fail(8);}catch(const std::length_error&){fail(8);}catch(const std::domain_error&){fail(170);}catch(const std::invalid_argument&){fail(87);}
    arg.return_to(state,result);return std::nullopt;
}
}
