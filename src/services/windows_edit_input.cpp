#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>

namespace x64arm {
namespace {
using EditLines=std::vector<std::array<std::uint32_t,2>>;
std::uint32_t edit_position(std::u16string_view text,std::uint32_t position,bool unicode,bool to_utf16){
    if(unicode)return static_cast<std::uint32_t>(std::min<std::size_t>(position,text.size()));
    if(!to_utf16)return static_cast<std::uint32_t>(utf16_to_utf8(text.substr(0,std::min<std::size_t>(position,text.size()))).size());
    std::uint32_t bytes=0,index=0;
    while(index<text.size()){
        const auto c=text[index];const bool pair=c>=0xd800 && c<=0xdbff && index+1<text.size() && text[index+1]>=0xdc00 && text[index+1]<=0xdfff;
        const auto count=pair?4U:c<0x80?1U:c<0x800?2U:3U;
        if(count>position-bytes)break;bytes+=count;index+=pair?2:1;
    }return index;
}
EditLines edit_lines(const GuiWindow& window,std::uint64_t handle,NativeGui* native,const GuiFont& font){
    const auto& text=window.title;const auto length=static_cast<std::uint32_t>(text.size());
    if(!(window.style&4U))return {{0,length}};
    if(native)if(auto lines=native->text_line_ranges(handle))return *lines;
    const auto metrics=native?native->font_metrics(font).value_or(fallback_font_metrics(font)):fallback_font_metrics(font);
    const auto cell=std::max(1,metrics.average_width);const auto width=std::max<std::int64_t>(1,static_cast<std::int64_t>(window.width)-10);
    const bool wrap=!(window.style&(0x80U|0x100000U));EditLines lines;std::uint32_t begin=0;
    while(begin<=length){
        auto hard_end=begin;while(hard_end<length && text[hard_end]!=u'\r' && text[hard_end]!=u'\n')++hard_end;
        auto first=begin;
        if(!wrap || first==hard_end)lines.push_back({first,hard_end});
        else while(first<hard_end){
            auto index=first,last_break=first;std::int64_t used=0;
            while(index<hard_end){const auto c=text[index];const bool pair=c>=0xd800 && c<=0xdbff && index+1<hard_end && text[index+1]>=0xdc00 && text[index+1]<=0xdfff;
                const auto advance=c==u'\t'?cell*8-used%(cell*8):(pair || (c>=0x2e80 && c<=0xa4cf) || c>=0xff01)?cell*2:cell;
                if(used+advance>width && index>first)break;used+=advance;index+=pair?2:1;
                if(c==u' ' || c==u'\t' || c==u'-' || (c>=0x2e80 && c<=0xa4cf))last_break=index;
            }
            if(index<hard_end && last_break>first)index=last_break;
            lines.push_back({first,index});first=index;
        }
        if(hard_end==length)break;begin=hard_end+1;if(text[hard_end]==u'\r' && begin<length && text[begin]==u'\n')++begin;
    }return lines;
}
}
std::optional<std::uint64_t> WindowsServices::gui_edit_input(GuestMemory& memory,const GuiMessage& message,bool wide){
    auto& window=gui_.windows.at(message.window);const auto code=message.message;
    auto& start=window.edit_start;auto& end=window.edit_end;const auto length=static_cast<std::uint32_t>(window.title.size());start=std::min(start,length);end=std::min(end,length);
    const auto position=[&](std::uint32_t at,bool to_utf16=false){return edit_position(window.title,at,wide,to_utf16);};
    const auto notify=[&](unsigned event){if(gui_.windows.contains(window.parent))enqueue_gui_message({window.parent,(std::uint64_t{event}<<16)|(window.id&65535U),message.window,0x111});};
    const auto previous=[&](unsigned at){if(at && at>1 && window.title[at-1]>=0xdc00 && window.title[at-1]<=0xdfff && window.title[at-2]>=0xd800 && window.title[at-2]<=0xdbff)return at-2;return at?at-1:0;};
    const auto following=[&](unsigned at){if(at<length && at+1<length && window.title[at]>=0xd800 && window.title[at]<=0xdbff && window.title[at+1]>=0xdc00 && window.title[at+1]<=0xdfff)return at+2;return std::min(at+1,length);};
    const auto replace=[&](std::u16string text,bool undo){
        if(!window.enabled || (window.style&0x800U))return;
        const auto first=std::min(start,end),last=std::max(start,end);const auto retained=length-(last-first);
        const auto available=window.text_limit>retained?window.text_limit-retained:0;
        if(text.size()>available){text.resize(available);if(!text.empty() && text.back()>=0xd800 && text.back()<=0xdbff)text.pop_back();notify(0x501);}
        if(window.style&8U)for(auto& c:text)c=unicode_case_unit(c,true);else if(window.style&16U)for(auto& c:text)c=unicode_case_unit(c,false);
        const auto old=window.title;if(undo)window.edit_undo=old;else window.edit_undo.reset();window.title.replace(first,last-first,text);
        start=end=first+static_cast<std::uint32_t>(text.size());window.edit_modified=true;notify(0x400);present_gui(message.window);if(window.title!=old)notify(0x300);
    };
    const auto paste=[&]{
        if(job_ui_restrictions()&2U)return;
        if(gui_.clipboard_open_thread && gui_.clipboard_open_thread!=threads_.current)return;
        std::optional<std::u16string> text=gui_.clipboard_native_text;
        if(!gui_.clipboard_stale){const auto entry=gui_.clipboard.find(13);if(entry!=gui_.clipboard.end()){const auto block=gui_.global_memory.find(entry->second);
            if(block!=gui_.global_memory.end() && block->second.address)text=read_utf16(memory,GuestAddress{block->second.address},static_cast<std::size_t>(block->second.size/2));}}
        if(!text && native_gui_ && interactive_station())text=native_gui_->clipboard_text();if(!text)return;
        if(!(window.style&4U)){const auto at=text->find_first_of(u"\r\n");if(at!=text->npos)text->resize(at);}replace(*text,true);
    };
    const auto copy=[&]()->bool{
        if((job_ui_restrictions()&4U) || (window.style&0x20U) || start==end || (gui_.clipboard_open_thread && gui_.clipboard_open_thread!=threads_.current))return false;
        const auto text=window.title.substr(std::min(start,end),std::max(start,end)-std::min(start,end));
        if(native_gui_ && interactive_station() && !native_gui_->set_clipboard_text(text))return false;
        // 共用 clipboard 入口下一次存取會釋放舊 HGLOBAL；文字與原生 clipboard 同步。
        gui_.clipboard_stale=true;gui_.clipboard_native_text=text;gui_.clipboard_owner=message.window;refresh_clipboard(true);notify_clipboard();return true;
    };
    const auto undo=[&]{if(!window.edit_undo || (window.style&0x800U))return false;const auto old=window.title;window.title=*window.edit_undo;window.edit_undo=old;start=end=static_cast<std::uint32_t>(window.title.size());window.edit_modified=true;notify(0x400);present_gui(message.window);notify(0x300);return true;};
    if(code==0xb0){if(message.wparam)memory.check(GuestAddress{message.wparam},4,Access::Write);if(message.lparam)memory.check(GuestAddress{message.lparam},4,Access::Write);
        const auto first=position(std::min(start,end)),last=position(std::max(start,end));if(message.wparam)memory.store(GuestAddress{message.wparam},32,first);if(message.lparam)memory.store(GuestAddress{message.lparam},32,last);return first>65535 || last>65535?UINT32_MAX:first|(last<<16);}
    if(code==0xb1){const auto first=static_cast<std::uint32_t>(message.wparam),last=static_cast<std::uint32_t>(message.lparam);if(first==UINT32_MAX)start=end;else {start=position(first,true);end=position(last,true);}present_gui(message.window);return 0;}
    if(code==0xba || code==0xbb || code==0xc1 || code==0xc4 || code==0xc9){
        const auto lines=edit_lines(window,message.window,native_gui_.get(),gui_font(gui_,window.font));
        const auto line_at=[&](std::uint32_t at){std::size_t index=0;while(index+1<lines.size() && lines[index+1][0]<=at)++index;return index;};
        const auto parameter=static_cast<std::uint32_t>(message.wparam);
        if(code==0xba)return lines.size();
        if(code==0xbb){const auto line=parameter==UINT32_MAX?line_at(end):static_cast<std::size_t>(parameter);return line<lines.size()?position(lines[line][0]):UINT64_MAX;}
        if(code==0xc9)return line_at(parameter==UINT32_MAX?std::min(start,end):position(parameter,true));
        if(code==0xc1){
            if(parameter!=UINT32_MAX && parameter>position(length))return 0;
            if(!(window.style&4U))return position(length);
            if(parameter!=UINT32_MAX){const auto line=lines[line_at(position(parameter,true))];return position(line[1])-position(line[0]);}
            const auto first=std::min(start,end),last=std::max(start,end);const auto left=lines[line_at(first)],right=lines[line_at(last>first?last-1:last)];
            return position(std::min(first,left[1]))-position(left[0])+position(right[1])-position(std::min(last,right[1]));
        }
        const auto capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{message.lparam},16));const auto line=(window.style&4U)?static_cast<std::size_t>(parameter):0;
        if(line>=lines.size() || !capacity)return 0;auto text=window.title.substr(lines[line][0],lines[line][1]-lines[line][0]);
        if(wide){if(text.size()>capacity){text.resize(capacity);if(!text.empty() && text.back()>=0xd800 && text.back()<=0xdbff)text.pop_back();}
            auto bytes=utf16_bytes(text);bytes.resize(text.size()*2);if(!bytes.empty())memory.write(GuestAddress{message.lparam},bytes);return text.size();}
        auto text_bytes=utf16_to_utf8(text);if(text_bytes.size()>capacity){auto at=capacity;while(at && (static_cast<unsigned char>(text_bytes[at])&0xc0U)==0x80U)--at;text_bytes.resize(at);}
        if(!text_bytes.empty())memory.write(GuestAddress{message.lparam},std::span{reinterpret_cast<const std::uint8_t*>(text_bytes.data()),text_bytes.size()});return text_bytes.size();
    }
    if(code==0xb8)return window.edit_modified?1:0;
    if(code==0xb9){window.edit_modified=message.wparam!=0;return 0;}
    if(code==0xc2){replace(message.lparam?(wide?read_utf16(memory,GuestAddress{message.lparam}):utf8_to_utf16_lossy(read_ansi(memory,message.lparam))):std::u16string{},message.wparam!=0);return 0;}
    if(code==0xc6)return window.edit_undo?1:0;
    if(code==0xc7 || code==0x304)return undo()?1:0;
    if(code==0xcd){window.edit_undo.reset();return 0;}
    if(code==0xcf){window.style=(window.style&~0x800U)|(message.wparam?0x800U:0);present_gui(message.window);return 1;}
    if(code==0x301){(void)copy();return 0;}
    if(code==0x302){paste();return 0;}
    if(code==0x300){if(!(window.style&0x800U) && copy())replace({},true);return 0;}
    if(code==0x303){replace({},true);return 0;}
    if(code==0xc){start=end=0;window.edit_undo.reset();window.edit_modified=false;return std::nullopt;}
    if(code==0x100){
        const auto key=static_cast<unsigned>(message.wparam);const bool shift=gui_.keyboard[16]&0x80U;
        if(key==0x2e){if(start==end)end=following(end);replace({},true);return 0;}
        if(key==0x25 || key==0x27 || key==0x24 || key==0x23){unsigned at=end;
            if(key==0x24)at=0;else if(key==0x23)at=length;else if(start!=end && !shift)at=key==0x25?std::min(start,end):std::max(start,end);else at=key==0x25?previous(end):following(end);
            if(!shift)start=at;end=at;present_gui(message.window);return 0;
        }
        return std::nullopt;
    }
    if(code!=0x102)return std::nullopt;
    const auto c=static_cast<char16_t>(message.wparam);
    if(c==1){start=0;end=length;present_gui(message.window);return 0;}if(c==3){(void)copy();return 0;}if(c==22){paste();return 0;}if(c==24){if(!(window.style&0x800U) && copy())replace({},true);return 0;}if(c==26)return undo()?1:0;
    if(c==8){if(start==end)start=previous(start);replace({},true);return 0;}
    if(c==13){if(window.style&4U)replace(u"\r\n",true);return 0;}
    if(c<32 || c==127)return 0;if((window.style&0x2000U) && (c<u'0' || c>u'9'))return 0;
    replace(std::u16string(1,c),true);return 0;
}
}
