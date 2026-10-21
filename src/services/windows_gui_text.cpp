#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>

namespace x64arm {
namespace {
constexpr std::uint64_t text_limit=1048576;
bool text_items(const GuiWindow& window) {
    const auto& cls=gui_control_class(window);
    if(cls==u"listbox")return !(window.style&0x30U) || (window.style&0x40U);
    if(cls==u"combobox")return !(window.style&0x30U) || (window.style&0x200U);
    return true;
}
std::vector<std::uint8_t> encoded_text(std::u16string_view text,bool wide) {
    if(wide)return utf16_bytes(text);
    const auto ansi=utf16_to_utf8(text);std::vector<std::uint8_t> bytes(ansi.begin(),ansi.end());bytes.push_back(0);return bytes;
}
std::u16string bounded_text(const GuestMemory& memory,std::uint64_t address,std::uint64_t count,bool wide) {
    if(wide){auto text=read_utf16_counted(memory,GuestAddress{address},static_cast<std::size_t>(count));const auto end=text.find(u'\0');if(end!=std::u16string::npos)text.resize(end);return text;}
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count));memory.read(GuestAddress{address},bytes);
    std::string text(bytes.begin(),bytes.end());const auto end=text.find('\0');if(end!=std::string::npos)text.resize(end);return utf8_to_utf16_lossy(text);
}
}
std::uint64_t WindowsServices::gui_window_procedure(std::uint64_t handle,bool wide) {
    const auto& window=gui_.windows.at(handle);
    if(!window.procedure || window.unicode==wide)return window.procedure;
    for(const auto& [token,reference]:gui_procedure_references_)
        if(reference.procedure==window.procedure && reference.wide==window.unicode)return token;
    if(gui_procedure_references_.size()>=16384)throw HostCallFault("視窗程序轉換參照超過上限");
    const auto token=gui_.next_object++;gui_procedure_references_.emplace(token,GuiProcedureReference{handle,window.procedure,window.unicode});return token;
}
void WindowsServices::discard_gui_text_thunk(GuestMemory& memory,const GuestState& state,std::uint64_t token) {
    const auto found=gui_text_thunks_.find(token);if(found==gui_text_thunks_.end())return;
    release_gui_scratch(memory,state,found->second.scratch);gui_text_thunks_.erase(found);
}
bool WindowsServices::begin_gui_text_thunk(GuestState& state,GuestMemory& memory,const GuiMessage& message,bool source_wide,
        Continuation completion,std::uint64_t value,std::uint64_t procedure,std::optional<bool> encoding) {
    const auto window=gui_.windows.find(message.window);if(window==gui_.windows.end())return false;
    if(!procedure)procedure=window->second.procedure;
    if(!procedure)return false;
    bool target_wide=encoding.value_or(window->second.unicode);
    if(const auto reference=gui_procedure_references_.find(procedure);reference!=gui_procedure_references_.end()){
        target_wide=reference->second.wide;procedure=reference->second.procedure;
    }
    if(source_wide==target_wide)return false;
    const auto m=message.message;
    GuiTextThunk operation;operation.message=message;operation.procedure=procedure;operation.source_wide=source_wide;operation.target_wide=target_wide;
    operation.completion=completion;operation.value=value;operation.thread=threads_.current;
    if(m==12 || m==0xc2 || m==0x1a || m==0x1b)operation.kind=GuiTextThunk::Kind::Input;
    else if(m==13 || m==0x30c){operation.kind=GuiTextThunk::Kind::Output;operation.capacity=message.wparam;}
    else if(m==14)operation.kind=GuiTextThunk::Kind::Length;
    else if(m==0xc4){operation.kind=GuiTextThunk::Kind::Line;operation.capacity=memory.load(GuestAddress{message.lparam},16);}
    else if(m==1 || m==0x81)operation.kind=GuiTextThunk::Kind::Creation;
    else if(text_items(window->second)){
        if(m==0x143 || m==0x145 || m==0x14a || m==0x14c || m==0x14d || m==0x158 ||
           m==0x180 || m==0x181 || m==0x18c || m==0x18d || m==0x18f || m==0x196 || m==0x1a2)operation.kind=GuiTextThunk::Kind::Input;
        else if(m==0x148 || m==0x189)operation.kind=GuiTextThunk::Kind::Item;
        else if(m==0x149 || m==0x18a)operation.kind=GuiTextThunk::Kind::Length;
        else return false;
    }else return false;
    if(m==13 || m==14){const auto& cls=gui_control_class(window->second);if(cls==u"static" && ((window->second.style&31U)==3 || (window->second.style&31U)==14))return false;}
    if(operation.capacity>text_limit)throw HostCallFault("視窗文字轉換容量超過上限");
    if(gui_text_thunks_.size()>=128)throw HostCallFault("視窗文字轉換重入超過上限");
    const auto token=gui_.next_object++;gui_text_thunks_.emplace(token,std::move(operation));
    try {
        auto& active=gui_text_thunks_.at(token);auto converted=message;
        if(active.kind==GuiTextThunk::Kind::Input){
            if(message.lparam){const auto text=source_wide?read_utf16(memory,GuestAddress{message.lparam}):utf8_to_utf16_lossy(read_ansi(memory,message.lparam));
                converted.lparam=allocate_gui_scratch(memory,active.scratch,encoded_text(text,target_wide));}
        }else if(active.kind==GuiTextThunk::Kind::Creation){
            const auto size=x86_?48U:80U;std::vector<std::uint8_t> bytes(size);memory.read(GuestAddress{message.lparam},bytes);active.buffer=allocate_gui_scratch(memory,active.scratch,bytes);converted.lparam=active.buffer;
            for(const auto offset:{x86_?36U:56U,x86_?40U:64U}){
                const auto original=memory.load(GuestAddress{message.lparam+offset},x86_?32:64);if(original<=65535)continue;
                const auto text=source_wide?read_utf16(memory,GuestAddress{original}):utf8_to_utf16_lossy(read_ansi(memory,original));
                const auto address=allocate_gui_scratch(memory,active.scratch,encoded_text(text,target_wide));memory.store(GuestAddress{active.buffer+offset},x86_?32:64,address);
            }
        }else if(active.kind==GuiTextThunk::Kind::Item){
            active.phase=1;converted.message=m==0x148?0x149U:0x18aU;converted.lparam=0;
        }else if(active.kind==GuiTextThunk::Kind::Output || active.kind==GuiTextThunk::Kind::Line){
            active.target_capacity=target_wide?active.capacity:std::min<std::uint64_t>(text_limit,active.capacity*3);
            if(active.kind==GuiTextThunk::Kind::Line)active.target_capacity=std::min<std::uint64_t>(active.target_capacity,65535);
            const auto bytes=active.target_capacity*(target_wide?2U:1U)+(active.kind==GuiTextThunk::Kind::Line?2U:0U);
            active.buffer=allocate_gui_scratch(memory,active.scratch,{},static_cast<std::size_t>(std::max<std::uint64_t>(bytes,2)));
            converted.lparam=active.buffer;if(active.kind==GuiTextThunk::Kind::Output)converted.wparam=active.target_capacity;
            else memory.store(GuestAddress{active.buffer},16,active.target_capacity);
        }
        start_callbacks(state,memory,{{procedure,{converted.window,converted.message,converted.wparam,converted.lparam}}},Continuation::GuiTextThunk,token);return true;
    }catch(...){discard_gui_text_thunk(memory,state,token);throw;}
}
std::optional<std::uint32_t> WindowsServices::resume_gui_text_thunk(GuestState& state,GuestMemory& memory,const CallbackFrame& frame,std::uint64_t result) {
    const auto found=gui_text_thunks_.find(frame.value);if(found==gui_text_thunks_.end())throw HostCallFault("視窗文字轉換缺少狀態");
    try {
        auto& operation=found->second;auto returned=result;const bool replying=frame.continuation!=Continuation::GuiTextThunk;
        constexpr std::uint32_t replied_phase=0x80000000U;
        const auto sent=operation.completion==Continuation::GuiSentMessage?gui_.sent_messages.find(operation.value):gui_.sent_messages.end();
        const bool abandoned=sent!=gui_.sent_messages.end() && sent->second.abandoned && !sent->second.sender && !sent->second.done;
        if(operation.phase&replied_phase)returned=operation.capacity;
        else {
        const bool negative=x86_?static_cast<std::uint32_t>(result)>INT32_MAX:result>INT64_MAX;
        if(operation.kind==GuiTextThunk::Kind::Item && operation.phase==1 && !negative && !replying && !abandoned){
            if(result>=text_limit)throw HostCallFault("控制項文字轉換長度超過上限");
            operation.target_capacity=result+1;operation.buffer=allocate_gui_scratch(memory,operation.scratch,{},static_cast<std::size_t>(operation.target_capacity*(operation.target_wide?2U:1U)));
            operation.phase=2;const auto& message=operation.message;
            start_callbacks(state,memory,{{operation.procedure,{message.window,message.message,message.wparam,operation.buffer}}},Continuation::GuiTextThunk,frame.value);return {};
        }
        if(operation.kind==GuiTextThunk::Kind::Length && !negative && !operation.source_wide && operation.target_wide){
            if(result>text_limit)throw HostCallFault("視窗文字長度超過上限");returned=result*3;
        }
        if(!abandoned && !negative && (operation.kind==GuiTextThunk::Kind::Output || operation.kind==GuiTextThunk::Kind::Line || (operation.kind==GuiTextThunk::Kind::Item && operation.phase==2))){
            auto text=bounded_text(memory,operation.buffer,std::min(result,operation.target_capacity),operation.target_wide);
            const bool terminated=operation.kind!=GuiTextThunk::Kind::Line;
            const auto maximum=operation.kind==GuiTextThunk::Kind::Item?text_limit:operation.capacity-(terminated && operation.capacity?1U:0U);
            if(operation.source_wide){
                if(text.size()>maximum){text.resize(static_cast<std::size_t>(maximum));if(!text.empty() && text.back()>=0xd800 && text.back()<=0xdbff)text.pop_back();}
                returned=text.size();if(terminated && (operation.capacity || operation.kind==GuiTextThunk::Kind::Item))memory.write(GuestAddress{operation.message.lparam},utf16_bytes(text));
                else if(!terminated){auto bytes=utf16_bytes(text);bytes.resize(text.size()*2);memory.write(GuestAddress{operation.message.lparam},bytes);}
            }else {
                auto bytes=utf16_to_utf8(text);if(bytes.size()>maximum){auto end=static_cast<std::size_t>(maximum);while(end && (static_cast<unsigned char>(bytes[end])&0xc0U)==0x80U)--end;bytes.resize(end);}
                returned=bytes.size();if(terminated && (operation.capacity || operation.kind==GuiTextThunk::Kind::Item))bytes.push_back(0);
                if(!bytes.empty())memory.write(GuestAddress{operation.message.lparam},std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size()});
            }
        }
        if(!abandoned && operation.kind==GuiTextThunk::Kind::Creation){
            const auto size=x86_?48U:80U;std::vector<std::uint8_t> bytes(size);memory.read(GuestAddress{operation.buffer},bytes);
            for(const auto offset:{x86_?36U:56U,x86_?40U:64U}){
                std::vector<std::uint8_t> original(x86_?4U:8U);memory.read(GuestAddress{operation.message.lparam+offset},original);std::copy(original.begin(),original.end(),bytes.begin()+offset);
            }
            memory.write(GuestAddress{operation.message.lparam},bytes);
        }
        }
        // 提前回覆先完成回寫，之後發送者可釋放原始緩衝區；接收程序仍持有轉換後的私有資料。
        if(replying){operation.capacity=returned;operation.phase|=replied_phase;return {};}
        const auto completion=operation.completion;const auto value=operation.value;discard_gui_text_thunk(memory,state,frame.value);
        CallbackFrame forwarded=frame;forwarded.continuation=completion;forwarded.value=value;
        if(completion==Continuation::GuiSentMessage)return resume_gui_send(state,memory,forwarded,returned);
        if(completion==Continuation::GuiModalDispatch)return resume_gui(state,memory,forwarded,returned);
        if(completion==Continuation::GuiCreationNcCreate || completion==Continuation::GuiCreationCreate)return resume_gui_creation(state,memory,forwarded,returned);
        if(completion==Continuation::GuiAsyncSend)return resume_gui_async(state,memory,forwarded,returned);
        complete_call(state,memory,completion==Continuation::Return?value:returned);return {};
    }catch(...){discard_gui_text_thunk(memory,state,frame.value);throw;}
}
}
