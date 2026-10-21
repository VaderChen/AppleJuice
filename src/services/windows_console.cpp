#include "x64arm/windows_services.hpp"
#include "x64arm/native_console.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <thread>
#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace x64arm {
namespace {
std::uint32_t terminal_write(int descriptor,std::string_view text){
#if !defined(_WIN32)
    for(std::size_t position=0;position<text.size();){
        const auto count=::write(descriptor,text.data()+position,text.size()-position);
        if(count<0){if(errno==EINTR)continue;return errno==EPIPE?109U:5U;}if(!count)return 109;
        position+=static_cast<std::size_t>(count);
    }return 0;
#else
    (void)descriptor;(void)text;return 50;
#endif
}
std::string attributes(std::uint16_t value){
    constexpr std::array<unsigned,8> colors{0,4,2,6,1,5,3,7};
    const auto foreground=(value&8?90:30)+colors[value&7],background=(value&128?100:40)+colors[(value>>4)&7];
    return "\x1b[0;"+std::to_string(foreground)+";"+std::to_string(background)+
        ((value&0x4000)?";7":"")+((value&0x8000)?";4":"")+"m";
}
std::vector<ConsoleRecord> converted(std::deque<ConsoleRecord>& records,std::deque<ConsoleRecord>& pending,bool wide,std::size_t count){
    std::vector<ConsoleRecord> result;result.reserve(std::min(count,records.size()));
    while(!pending.empty() && result.size()<count){result.push_back(pending.front());pending.pop_front();}
    while(!records.empty() && result.size()<count){
        auto record=records.front();records.pop_front();const auto character=console_field(record,14,2);
        if(wide || console_field(record,0,2)!=1 || character<128){result.push_back(record);continue;}
        std::u16string text(1,static_cast<char16_t>(character));
        if(character>=0xd800 && character<=0xdbff && !records.empty()){
            const auto& next=records.front();const auto low=console_field(next,14,2);
            if(console_field(next,0,2)==1 && low>=0xdc00 && low<=0xdfff &&
               console_field(next,4)==console_field(record,4) && console_field(next,10,2)==console_field(record,10,2) &&
               console_field(next,16)==console_field(record,16)){text+=static_cast<char16_t>(low);records.pop_front();}
        }
        std::string bytes;try{bytes=utf16_to_utf8(text);}catch(const std::invalid_argument&){bytes="\xef\xbf\xbd";}
        for(auto byte:bytes){auto item=record;set_console_field(item,14,static_cast<unsigned char>(byte),2);
            if(result.size()<count)result.push_back(item);else pending.push_back(item);}
    }return result;
}
}
std::span<const WindowsServiceDescriptor> console_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"kernel32.dll","AllocConsole",0},{"kernel32.dll","AttachConsole",1},{"kernel32.dll","FreeConsole",0},
        {"kernel32.dll","GenerateConsoleCtrlEvent",2,0,0,3},
        {"internal-console","DispatchControl",1,0,0,1},
        {"kernel32.dll","GetConsoleWindow",0},{"kernel32.dll","SetConsoleScreenBufferSize",2},{"kernel32.dll","SetConsoleWindowInfo",3},
        {"crt","_cputs",1},{"crt","_cputws",1},
        {"crt","_getch",0},{"crt","_getche",0},{"crt","_getwch",0},{"crt","_getwche",0},{"crt","_kbhit",0},
        {"crt","_putch",1},{"crt","_putwch",1},{"crt","_ungetch",1},{"crt","_ungetwch",1},
        {"crt","_getch_nolock",0},{"crt","_getche_nolock",0},{"crt","_getwch_nolock",0},{"crt","_getwche_nolock",0},
        {"crt","_putch_nolock",1},{"crt","_putwch_nolock",1},{"crt","_ungetch_nolock",1},{"crt","_ungetwch_nolock",1},
        {"kernel32.dll","FillConsoleOutputAttribute",5},{"kernel32.dll","FillConsoleOutputCharacterA",5},{"kernel32.dll","FillConsoleOutputCharacterW",5},
        {"kernel32.dll","GetConsoleCursorInfo",2},{"kernel32.dll","SetConsoleCursorInfo",2},{"kernel32.dll","SetConsoleCursorPosition",2},
        {"kernel32.dll","SetConsoleTextAttribute",2},{"kernel32.dll","GetConsoleDisplayMode",1},
        {"kernel32.dll","GetConsoleTitleA",2},{"kernel32.dll","GetConsoleTitleW",2},{"kernel32.dll","SetConsoleTitleA",1},{"kernel32.dll","SetConsoleTitleW",1},
        {"kernel32.dll","FlushConsoleInputBuffer",1},{"kernel32.dll","GetNumberOfConsoleInputEvents",2},
        {"kernel32.dll","ReadConsoleInputA",4},{"kernel32.dll","ReadConsoleInputW",4},
        {"kernel32.dll","WriteConsoleInputA",4},{"kernel32.dll","WriteConsoleInputW",4},
        {"kernel32.dll","PeekConsoleInputA",4},{"kernel32.dll","PeekConsoleInputW",4},
        {"kernel32.dll","ReadConsoleOutputCharacterA",5},{"kernel32.dll","ReadConsoleOutputCharacterW",5},{"kernel32.dll","ReadConsoleOutputAttribute",5}
    };return table;
}
std::uint32_t WindowsServices::queue_console_control(GuestMemory& memory,std::uint32_t event){
    if(event>1)return 87;
    if(!console_identity_)return 6;
    if(!event && runtime_.ignore_ctrl_c)return 0;
    auto handlers=runtime_.console_handlers;
    const auto saved_error=last_error_;
    const auto handle=create_guest_thread(threads_.items.at(threads_.current).context,memory,
        resolve("internal-console","DispatchControl").value,event,65536,0);
    const auto error=handle?0U:last_error_;last_error_=saved_error;
    if(!handle)return error;
    threads_.items.at(thread_id(handle)).console_handlers=std::move(handlers);
    close_transfer_handle(memory,handle);return 0;
}
bool WindowsServices::console_input_ready(std::uint64_t handle,unsigned timeout){
    if(handle!=0x100 || !console_handle(handle))return true;
    auto& endpoint=*consoles_[0];auto& buffer=*endpoint.buffer;
    if(buffer.window){
        for(const auto& record:buffer.window->poll(buffer.records.empty()?timeout:0)){
            if(console_field(record,0,2)==4){buffer.resize_viewport(static_cast<std::uint16_t>(console_field(record,4,2)),static_cast<std::uint16_t>(console_field(record,6,2)));(void)buffer.window->update(buffer);}
            buffer.push(record);
        }
        buffer.eof=buffer.window->closed();buffer.fullscreen=buffer.window->fullscreen();
        return !buffer.records.empty() || !buffer.ansi_records.empty() || buffer.eof;
    }
#if !defined(_WIN32)
    winsize size{};if(!::ioctl(endpoint.descriptor,TIOCGWINSZ,&size) && size.ws_col && size.ws_row && (size.ws_col!=buffer.view_columns || size.ws_row!=buffer.view_rows)){
        buffer.resize_viewport(size.ws_col,size.ws_row);ConsoleRecord record{};set_console_field(record,0,4,2);set_console_field(record,4,buffer.columns,2);set_console_field(record,6,buffer.rows,2);buffer.push(record);
    }
    if(buffer.records.size()+buffer.ansi_records.size()>=65536)return true;
    termios current{};if(::tcgetattr(endpoint.descriptor,&current)){buffer.eof=true;return true;}
    // 事件讀取停用宿主行編輯，但保留 TTY 原有的控制訊號政策。
    auto raw=current;raw.c_lflag&=~static_cast<tcflag_t>(ICANON|ECHO);
    raw.c_iflag&=~static_cast<tcflag_t>(ICRNL|INLCR|IGNCR);raw.c_cc[VMIN]=0;raw.c_cc[VTIME]=0;
    if(::tcsetattr(endpoint.descriptor,TCSANOW,&raw)){buffer.eof=true;return true;}
    pollfd descriptor{endpoint.descriptor,POLLIN,0};int ready;
    do{ready=::poll(&descriptor,1,buffer.records.empty()?static_cast<int>(timeout):0);}while(ready<0 && errno==EINTR);
    std::array<char,4096> bytes{};ssize_t count=-1;
    if(ready>0 && (descriptor.revents&(POLLIN|POLLHUP))){
        do{count=::read(endpoint.descriptor,bytes.data(),std::min<std::size_t>(bytes.size(),65536-buffer.records.size()-buffer.ansi_records.size()));}while(count<0 && errno==EINTR);
    }
    const bool restore=::tcsetattr(endpoint.descriptor,TCSANOW,&current)==0;
    endpoint.changed=true;
    if(!restore || ready<0 || (descriptor.revents&POLLNVAL))buffer.eof=true;
    if(count>0)buffer.ingest(std::string_view(bytes.data(),static_cast<std::size_t>(count)));
    else if(count==0 || (ready>0 && (descriptor.revents&POLLHUP))){buffer.eof=true;buffer.ingest({},true);}
#else
    (void)timeout;buffer.eof=true;
#endif
    return !buffer.records.empty() || !buffer.ansi_records.empty() || buffer.eof;
}
bool WindowsServices::console_text_ready(bool line,int timeout){
    if(!console_handle(0x100))return true;
    auto& input=runtime_.standard_input;auto& buffer=*consoles_[0]->buffer;
    if(input.position<input.bytes.size())return true;
    input.bytes.clear();input.position=0;
    if(!buffer.ansi_records.empty()){
        while(!buffer.ansi_records.empty()){const auto record=buffer.ansi_records.front();buffer.ansi_records.pop_front();
            if(console_field(record,0,2)==1 && console_field(record,4))input.bytes.push_back(static_cast<std::uint8_t>(console_field(record,14,1)));}
        if(!input.bytes.empty())return true;
    }
    (void)console_input_ready(0x100,static_cast<unsigned>(std::max(timeout,0)));
    const bool editing=line || (buffer.input_mode&2);std::u16string available;
    int terminal_eof=-1;
#if !defined(_WIN32)
    termios mode{};
    if(editing && consoles_[0]->descriptor>=0 && !::tcgetattr(consoles_[0]->descriptor,&mode) &&
       (mode.c_lflag&ICANON) && mode.c_cc[VEOF]!=_POSIX_VDISABLE)terminal_eof=mode.c_cc[VEOF];
#endif
    while(!buffer.records.empty()){
        auto record=buffer.records.front();buffer.records.pop_front();
        if(console_field(record,0,2)!=1 || !console_field(record,4))continue;
        const auto c=static_cast<char16_t>(console_field(record,14,2));if(!c)continue;
        auto repeats=console_field(record,8,2);if(!repeats)continue;
        while(repeats--){
            // 輪詢期間收到的 VEOF 尚未經宿主行編輯：有資料時提交該行，空行才回 EOF。
            if(terminal_eof>=0 && c==terminal_eof){
                if(buffer.edited_line.empty())buffer.eof=true;
                else {available=std::move(buffer.edited_line);buffer.edited_line.clear();}
                if(repeats){set_console_field(record,8,repeats,2);buffer.records.push_front(record);}break;
            }
            if(editing && (c==u'\b' || c==127)){
                if(!buffer.edited_line.empty()){buffer.edited_line.pop_back();if(!buffer.edited_line.empty() && buffer.edited_line.back()>=0xd800 && buffer.edited_line.back()<=0xdbff)buffer.edited_line.pop_back();
                    if(buffer.input_mode&4){if(!buffer.echo_input.empty())buffer.echo_input.clear();else (void)write_standard_output(1,std::array<std::uint8_t,3>{8,32,8});}}
                continue;
            }
            if(editing && (c==u'\r' || c==u'\n')){
                buffer.edited_line+=u"\r\n";available=std::move(buffer.edited_line);buffer.edited_line.clear();
                if(buffer.input_mode&4)(void)write_standard_output(1,std::array<std::uint8_t,2>{13,10});
                if(repeats){set_console_field(record,8,repeats,2);buffer.records.push_front(record);}break;
            }
            (editing?buffer.edited_line:available)+=c;
            if(buffer.input_mode&4){
                buffer.echo_input+=c;
                if(c>=0xd800 && c<=0xdbff)continue;
                std::string bytes;try{bytes=utf16_to_utf8(buffer.echo_input);}catch(const std::invalid_argument&){
                    for(auto unit:buffer.echo_input)bytes+=unit>=0xd800 && unit<=0xdfff?"\xef\xbf\xbd":utf16_to_utf8(std::u16string(1,unit));
                }
                buffer.echo_input.clear();(void)write_standard_output(1,std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size()));
            }
        }
        if(editing && (!available.empty() || buffer.eof))break;
    }
    if(buffer.eof && available.empty() && !buffer.edited_line.empty()){available=std::move(buffer.edited_line);buffer.edited_line.clear();}
    if(!available.empty()){
        std::string text;try{text=utf16_to_utf8(available);}catch(const std::invalid_argument&){
            // 客體可以注入單獨的 surrogate；文字串流以 U+FFFD 回填，不暴露宿主例外。
            for(auto c:available){if(c>=0xd800 && c<=0xdfff)text+="\xef\xbf\xbd";else text+=utf16_to_utf8(std::u16string(1,c));}
        }
        input.bytes.assign(text.begin(),text.end());
    }
    live_input_eof_=buffer.eof;return !input.bytes.empty() || buffer.eof;
}
std::uint32_t WindowsServices::console_sync(unsigned stream,std::size_t start,std::size_t count){
    if(stream>=3 || !consoles_[stream])return 6;auto& endpoint=*consoles_[stream];auto& buffer=*endpoint.buffer;
    if(buffer.window){
        if(buffer.window->closed())return 109;if(!buffer.window->update(buffer))return 5;
        if(buffer.window_handle && gui_.windows.contains(buffer.window_handle)){
            auto& model=gui_.windows.at(buffer.window_handle);model.width=buffer.view_columns*9U;model.height=buffer.view_rows*18U;model.title=buffer.title;
            present_gui(buffer.window_handle);
        }
        return 0;
    }
    std::string text;
    // API 自己保存螢幕資料；VT 輸出只更新指定區域，最後還原實際游標與屬性。
    std::uint16_t active=UINT16_MAX;
    for(auto position=start;position<std::min(buffer.cells.size(),start+count);++position){
        const auto row=position/buffer.columns,column=position%buffer.columns;
        if(row<buffer.top || row>=buffer.top+buffer.view_rows || column<buffer.left || column>=buffer.left+buffer.view_columns)continue;
        if(position==start || column==buffer.left)text+="\x1b["+std::to_string(row-buffer.top+1)+";"+std::to_string(column-buffer.left+1)+"H";
        const auto& cell=buffer.cells[position];if(cell.attributes!=active){active=cell.attributes;text+=attributes(active);}
        auto c=cell.character;std::u16string units(1,c<32?u' ':c);
        if(c>=0xd800 && c<=0xdbff && column+1<buffer.left+buffer.view_columns && position+1<std::min(buffer.cells.size(),start+count)){
            const auto next=buffer.cells[position+1].character;if(next>=0xdc00 && next<=0xdfff){units+=next;++position;}
        }
        try{text+=utf16_to_utf8(units);}catch(const std::invalid_argument&){text+="\xef\xbf\xbd";}
    }
    const bool cursor=buffer.x>=buffer.left && buffer.x<buffer.left+buffer.view_columns && buffer.y>=buffer.top && buffer.y<buffer.top+buffer.view_rows;
    text+=attributes(buffer.attributes);
    if(cursor)text+="\x1b["+std::to_string(buffer.y-buffer.top+1)+";"+std::to_string(buffer.x-buffer.left+1)+"H";
    text+=buffer.cursor_visible && cursor?"\x1b[?25h":"\x1b[?25l";
    text+=buffer.cursor_size>=50?"\x1b[2 q":"\x1b[4 q";
    return terminal_write(endpoint.descriptor,text);
}
std::optional<std::uint32_t> WindowsServices::invoke_console_control(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);auto name=service.symbol;if(name.ends_with("_nolock"))name.remove_suffix(7);std::uint64_t result=0;
    if(service.family=="internal-console"){
        std::vector<GuestCallback> callbacks;auto handlers=std::move(threads_.items.at(threads_.current).console_handlers);
        for(const auto handler:handlers)callbacks.push_back({handler,{static_cast<std::uint32_t>(arg(0))}});
        arg.return_to(state,0);
        if(callbacks.empty())return finish_process(state,memory,0xc000013aU);
        start_callbacks(state,memory,std::move(callbacks),Continuation::ConsoleControl,0);return std::nullopt;
    }
    const auto d=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    const auto fail=[&](std::uint32_t error){last_error_=error;result=0;};
    const auto current=std::find_if(consoles_.begin(),consoles_.end(),[](const auto& endpoint){return endpoint!=nullptr;});
    const bool attached=current!=consoles_.end();
    const auto reset_standard=[&]{
        standard_open_={true,true,true};runtime_.standard_handles={0x100,0x101,0x102};
        runtime_.standard_input.bytes.clear();runtime_.standard_input.position=0;runtime_.console_input.clear();
        runtime_.console_handlers.clear();runtime_.ignore_ctrl_c=false;live_input_eof_=false;
        runtime_.conio_byte_pushback.reset();runtime_.conio_wide_pushback.reset();
    };
    const auto output_character=[&](std::uint16_t value,bool wide){
        if(!attached)return 6U;
        const auto endpoint=consoles_[1]?consoles_[1]:consoles_[2]?consoles_[2]:*current;auto& screen=*endpoint->buffer;
        std::string text;
        if(wide){
            std::u16string units;if(screen.conio_output_high){units+=screen.conio_output_high;screen.conio_output_high=0;if(value<0xdc00 || value>0xdfff)units[0]=0xfffd;}
            if(value>=0xd800 && value<=0xdbff){screen.conio_output_high=static_cast<char16_t>(value);if(units.empty())return 0U;}
            else units+=static_cast<char16_t>(value);
            try{text=utf16_to_utf8(units);}catch(const std::invalid_argument&){text="\xef\xbf\xbd";}
        }else text.push_back(static_cast<char>(static_cast<std::uint8_t>(value)));
        if(screen.window){if(screen.window->closed())return 109U;screen.write(text);return screen.window->update(screen)?0U:5U;}
        const auto error=terminal_write(endpoint->descriptor,text);if(!error)screen.write(text);return error;
    };
    try{
        if(name=="_ungetch" || name=="_ungetwch"){
            const bool wide=name=="_ungetwch";const auto value=wide?static_cast<std::uint32_t>(static_cast<std::uint16_t>(arg(0))):d(0);
            result=wide?0xffffU:UINT32_MAX;
            if(wide){if(value!=0xffffU && !runtime_.conio_wide_pushback){runtime_.conio_wide_pushback=static_cast<std::uint16_t>(value);result=value;}}
            else if(value!=UINT32_MAX && !runtime_.conio_byte_pushback){runtime_.conio_byte_pushback=static_cast<std::uint8_t>(value);result=static_cast<std::uint8_t>(value);}
        }else if(name=="_putch" || name=="_putwch"){
            const bool wide=name=="_putwch";const auto value=static_cast<std::uint16_t>(arg(0));
            const auto error=output_character(wide?value:static_cast<std::uint8_t>(value),wide);
            if(error){memory.store(GuestAddress{runtime_.errno_cell},32,error==109?32:9);result=wide?0xffffU:UINT32_MAX;}
            else result=wide?value:d(0);
        }else if(name=="_getch" || name=="_getche" || name=="_getwch" || name=="_getwche" || name=="_kbhit"){
            const bool peek=name=="_kbhit",wide=name=="_getwch" || name=="_getwche",echo=name=="_getche" || name=="_getwche";
            const bool pushed=wide?runtime_.conio_wide_pushback.has_value():runtime_.conio_byte_pushback.has_value();
            if(peek && (runtime_.conio_byte_pushback || runtime_.conio_wide_pushback))result=1;
            else if(!peek && pushed){result=wide?*runtime_.conio_wide_pushback:*runtime_.conio_byte_pushback;if(wide)runtime_.conio_wide_pushback.reset();else runtime_.conio_byte_pushback.reset();}
            else if(!consoles_[0])result=peek?0:wide?0xffffU:UINT32_MAX;
            else{
                auto& buffer=*consoles_[0]->buffer;(void)console_input_ready(0x100);
                const auto key=[](const ConsoleRecord& r){return console_field(r,0,2)==1 && console_field(r,4) && console_field(r,8,2) && (console_field(r,14,2) || (console_field(r,12,2) && console_field(r,10,2)!=16 && console_field(r,10,2)!=17 && console_field(r,10,2)!=18));};
                if(peek)result=!buffer.conio_bytes.empty() || !buffer.conio_units.empty() || std::any_of(buffer.ansi_records.begin(),buffer.ansi_records.end(),key) || std::any_of(buffer.records.begin(),buffer.records.end(),key);
                else{
                    auto& pending=wide?buffer.conio_units:buffer.conio_bytes;
                    while(pending.empty() && (!buffer.ansi_records.empty() || !buffer.records.empty())){
                        auto records=converted(buffer.records,buffer.ansi_records,wide,1);const auto& r=records.front();if(!key(r))continue;
                        const auto count=console_field(r,8,2),character=console_field(r,14,wide?2:1),scan=console_field(r,12,2);
                        for(unsigned i=0;i<count;++i){if(character)pending.push_back({static_cast<std::uint16_t>(character),true});else{pending.push_back({static_cast<std::uint16_t>((console_field(r,16)&0x100)?0xe0:0),false});pending.push_back({static_cast<std::uint16_t>(scan),false});}}
                    }
                    if(pending.empty() && !buffer.eof){auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::ConsoleInput;thread.wait.handles={0x100};schedule(state,memory);return std::nullopt;}
                    result=wide?0xffffU:UINT32_MAX;if(!pending.empty()){const auto character=pending.front();pending.pop_front();result=character.value;
                        if(echo && character.echo){const auto error=output_character(character.value,wide);if(error){memory.store(GuestAddress{runtime_.errno_cell},32,error==109?32:9);result=wide?0xffffU:UINT32_MAX;}}}
                    threads_.items.at(threads_.current).wait={};
                }
            }
        }else if(name=="GetConsoleWindow"){
            if(attached){
                auto& screen=*(*current)->buffer;
                if(!screen.window || !screen.window->closed()){
                    if(!screen.window_handle || !gui_.windows.contains(screen.window_handle)){
                        if(screen.window && !native_gui_ && gui_.windows.empty())enable_native_gui({});
                        GuiWindow model;model.console=(*current)->buffer;model.class_name=u"ConsoleWindowClass";model.title=screen.title;
                        model.thread=threads_.current;model.desktop_object=thread_desktop_object(model.thread);model.dpi_context=effective_dpi_context();
                        model.procedure=resolve("user32.dll","DefWindowProcW").value;model.width=screen.view_columns*9U;model.height=screen.view_rows*18U;
                        model.x=model.y=120;model.visible=screen.window!=nullptr;model.style=model.visible?0x10cf0000U:0;
                        if(!screen.window)model.parent=gui_message_parent;
                        const auto handle=allocate_gui_window();gui_.windows.emplace(handle,model);gui_.z_order.insert(gui_.z_order.begin(),handle);
                        try{if(native_gui_)native_gui_->create(handle,model);publish_gui_window(handle);screen.window_handle=handle;if(model.visible && native_gui_)show_native_window(handle,true);}
                        catch(...){erase_window(handle);throw;}
                    }
                    result=screen.window_handle;
                }
            }
        }else if(name=="_cputs" || name=="_cputws"){
            if(!arg(0)){
                memory.store(GuestAddress{runtime_.errno_cell},32,22);
                if(current_invalid_parameter_handler()){start_callbacks(state,memory,{{current_invalid_parameter_handler(),{0,0,0,0,0}}},Continuation::Return,UINT64_MAX);return std::nullopt;}
                return 0xc0000409U;
            }
            const auto text=name=="_cputws"?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)})):read_ansi(memory,arg(0));
            std::uint32_t error=6;
            if(attached){
                // CONOUT$ 與 stdout 的重新導向獨立；直接使用已附接的主控台。
                const auto endpoint=consoles_[1]?consoles_[1]:consoles_[2]?consoles_[2]:*current;
                auto& screen=*endpoint->buffer;
                if(screen.window){error=screen.window->closed()?109:0;if(!error){screen.write(text);error=screen.window->update(screen)?0:5;}}
                else {error=terminal_write(endpoint->descriptor,text);if(!error)screen.write(text);}
            }
            if(error){memory.store(GuestAddress{runtime_.errno_cell},32,error==109?32:9);result=UINT64_MAX;}
        }else if(name=="AllocConsole"){
            if(attached)fail(5);else if(!runtime_.network.external_io)fail(50);
            else {auto window=create_console_window();if(!window)fail(50);
                else {auto buffer=std::make_shared<ConsoleBuffer>();buffer->window=std::move(window);
                    for(auto& endpoint:consoles_){endpoint=std::make_shared<NativeConsole>();endpoint->buffer=buffer;}
                    live_stdio_={-1,-1,-1};reset_standard();const auto error=console_sync(1);if(error){consoles_={};fail(error);}else {publish_console_identity(true,true);result=1;}
                }
            }
        }else if(name=="AttachConsole"){
            if(attached)fail(5);else if(!runtime_.network.external_io)fail(50);
            else if(d(0)!=UINT32_MAX){std::vector<std::uint32_t> ids;const auto error=session_process_ids(ids);
                if(error)fail(error);else if(std::find(ids.begin(),ids.end(),d(0))==ids.end())fail(87);else fail(50);
            }else {
#if !defined(_WIN32)
                auto descriptors=parent_stdio_;int owned=-1;
                if(!std::any_of(descriptors.begin(),descriptors.end(),[](int fd){return fd>=0 && ::isatty(fd);})){
                    owned=::open("/dev/tty",O_RDWR|O_NOCTTY|O_CLOEXEC);descriptors={owned,owned,owned};
                }
                if(owned<0 && std::any_of(descriptors.begin(),descriptors.end(),[](int fd){return fd<0;}))fail(6);
                else {try{set_live_standard_io(descriptors[0],descriptors[1],descriptors[2]);}
                    catch(...){if(owned>=0)::close(owned);throw;}
                    if(owned>=0)::close(owned);
                    if(!std::any_of(consoles_.begin(),consoles_.end(),[](const auto& endpoint){return endpoint!=nullptr;}))fail(6);
                    else {reset_standard();result=1;}
                }
#else
                fail(50);
#endif
            }
        }else if(name=="GenerateConsoleCtrlEvent"){
            const auto error=generate_console_control(memory,d(0),d(1));if(error)fail(error);else result=1;
        }else if(name=="FreeConsole"){
            for(const auto& endpoint:consoles_)if(endpoint && endpoint->buffer->window_handle){const auto handle=endpoint->buffer->window_handle;endpoint->buffer->window_handle=0;erase_window(handle);}
            consoles_={};live_stdio_={-1,-1,-1};standard_open_={false,false,false};runtime_.standard_handles={0,0,0};
            runtime_.console_input.clear();runtime_.standard_input.bytes.clear();runtime_.standard_input.position=0;
            runtime_.console_handlers.clear();runtime_.ignore_ctrl_c=false;live_input_eof_=true;result=1;
            publish_console_identity(false);
        }else if(name.starts_with("GetConsoleTitle") || name.starts_with("SetConsoleTitle") || name=="GetConsoleDisplayMode"){
            if(!attached)fail(6);else {
                const auto stream=static_cast<unsigned>(current-consoles_.begin());auto& buffer=*(*current)->buffer;
                if(name=="GetConsoleDisplayMode"){if(!arg(0))fail(87);else {if(buffer.window)buffer.fullscreen=buffer.window->fullscreen();memory.store(GuestAddress{arg(0)},32,buffer.fullscreen?1:0);result=1;}}
                else if(name.starts_with("Set")){if(!arg(0))fail(87);else {
                    auto title=name.ends_with('W')?read_utf16(memory,GuestAddress{arg(0)},65536):utf8_to_utf16_lossy(read_ansi(memory,arg(0),65536));
                    buffer.title=std::move(title);std::uint32_t error=0;
                    if(buffer.window)error=console_sync(stream);
                    else {auto text=utf16_to_utf8(buffer.title);for(auto& c:text)if(c==7 || c==27 || c==0)c=' ';error=terminal_write((*current)->descriptor,"\x1b]0;"+text+"\x07");}
                    if(error)fail(error);else result=1;
                }}else {
                    const auto count=d(1);const bool wide=name.ends_with('W');
                    if(!arg(0) || !count || count>65536)fail(87);
                    else {const auto title=wide?std::string{}:utf16_to_utf8(buffer.title);const auto length=wide?buffer.title.size():title.size();
                        const auto copied=std::min<std::size_t>(count-1,length);memory.check(GuestAddress{arg(0)},(copied+1)*(wide?2U:1U),Access::Write);
                        if(wide){for(std::size_t i=0;i<copied;++i)memory.store(GuestAddress{arg(0)+2*i},16,buffer.title[i]);memory.store(GuestAddress{arg(0)+2*copied},16,0);}
                        else {memory.write(GuestAddress{arg(0)},std::span(reinterpret_cast<const std::uint8_t*>(title.data()),copied));memory.store(GuestAddress{arg(0)+copied},8,0);}result=length;
                    }
                }
            }
        }else if(!console_handle(arg(0)))fail(6);
        else {
            const auto stream=static_cast<unsigned>(arg(0)-0x100);auto& buffer=*consoles_[stream]->buffer;
            const bool input=name.find("Input")!=name.npos;
            if((input && stream!=0) || (!input && stream==0 && name!="GetNumberOfConsoleInputEvents"))fail(6);
            else if(name=="FlushConsoleInputBuffer"){
                if(stream)fail(6);else {
#if !defined(_WIN32)
                    if(consoles_[0]->descriptor>=0 && ::tcflush(consoles_[0]->descriptor,TCIFLUSH)){fail(6);arg.return_to(state,result);return std::nullopt;}
#endif
                    if(buffer.window)(void)buffer.window->poll(0);buffer.records.clear();buffer.ansi_records.clear();buffer.terminal_input.clear();buffer.edited_line.clear();buffer.echo_input.clear();
                    runtime_.console_input.clear();runtime_.standard_input.bytes.clear();runtime_.standard_input.position=0;result=1;
                }
            }else if(name=="GetNumberOfConsoleInputEvents"){
                if(stream)fail(6);else if(!arg(1))fail(87);else {memory.check(GuestAddress{arg(1)},4,Access::Write);(void)console_input_ready(arg(0));memory.store(GuestAddress{arg(1)},32,buffer.records.size()+buffer.ansi_records.size());result=1;}
            }else if(name.find("ConsoleInput")!=name.npos){
                const auto count=d(2);const bool writing=name.starts_with("Write"),peek=name.starts_with("Peek"),wide=name.ends_with('W');
                if(!arg(3) || (count && !arg(1)) || (!count && !writing))fail(87);
                else if(count>65536)fail(8);
                else {
                    memory.check(GuestAddress{arg(3)},4,Access::Write);
                    if(writing){
                        if(count>65536-buffer.records.size()-buffer.ansi_records.size())fail(8);
                        else {memory.check(GuestAddress{arg(1)},static_cast<std::size_t>(count)*20,Access::Read);std::vector<ConsoleRecord> records(count);
                            for(unsigned i=0;i<count;++i){memory.read(GuestAddress{arg(1)+20ULL*i},records[i]);const auto type=console_field(records[i],0,2);
                                if(type!=1 && type!=2 && type!=4 && type!=8 && type!=16){fail(87);arg.return_to(state,result);return std::nullopt;}
                                if(!wide && type==1){const auto byte=console_field(records[i],14,1);if(byte>=128){fail(1113);arg.return_to(state,result);return std::nullopt;}set_console_field(records[i],14,byte,2);}
                            }
                            buffer.records.insert(buffer.records.end(),records.begin(),records.end());memory.store(GuestAddress{arg(3)},32,count);result=1;
                        }
                    }else {
                        (void)console_input_ready(arg(0));
                        if(!peek && buffer.records.empty() && buffer.ansi_records.empty() && !buffer.eof){
                            auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::ConsoleInput;thread.wait.handles={arg(0)};
                            schedule(state,memory);return std::nullopt;
                        }
                        if(!peek && buffer.eof && buffer.records.empty() && buffer.ansi_records.empty())fail(109);
                        else {auto remaining=buffer.records,pending=buffer.ansi_records;const auto records=converted(remaining,pending,wide,count);
                            memory.check(GuestAddress{arg(1)},records.size()*20,Access::Write);
                            for(std::size_t i=0;i<records.size();++i)memory.write(GuestAddress{arg(1)+20*i},records[i]);
                            memory.store(GuestAddress{arg(3)},32,records.size());if(!peek){buffer.records=std::move(remaining);buffer.ansi_records=std::move(pending);}
                            threads_.items.at(threads_.current).wait={};result=1;
                        }
                    }
                }
            }else if(name=="GetConsoleCursorInfo" || name=="SetConsoleCursorInfo"){
                if(!arg(1))fail(87);
                else if(name.starts_with("Get")){memory.check(GuestAddress{arg(1)},8,Access::Write);memory.store(GuestAddress{arg(1)},32,buffer.cursor_size);memory.store(GuestAddress{arg(1)+4},32,buffer.cursor_visible?1:0);result=1;}
                else {const auto size=memory.load(GuestAddress{arg(1)},32),visible=memory.load(GuestAddress{arg(1)+4},32);
                    if(!size || size>100)fail(87);else {buffer.cursor_size=static_cast<std::uint32_t>(size);buffer.cursor_visible=visible!=0;const auto error=console_sync(stream);if(error)fail(error);else result=1;}
                }
            }else if(name=="SetConsoleScreenBufferSize"){
                const auto width=static_cast<std::int16_t>(d(1)),height=static_cast<std::int16_t>(d(1)>>16);
                if(width<=0 || height<=0 || width<buffer.view_columns || height<buffer.view_rows)fail(87);
                else if(static_cast<std::uint64_t>(width)*static_cast<std::uint64_t>(height)>std::min<std::uint64_t>(1024U*1024U,memory.limit()/sizeof(ConsoleCell)))fail(8);
                else {
                    buffer.resize(static_cast<std::uint16_t>(width),static_cast<std::uint16_t>(height),true);
                    ConsoleRecord event{};set_console_field(event,0,4,2);set_console_field(event,4,buffer.columns,2);set_console_field(event,6,buffer.rows,2);buffer.push(event);
                    const auto error=console_sync(stream,0,buffer.cells.size());if(error)fail(error);else result=1;
                }
            }else if(name=="SetConsoleWindowInfo"){
                if(!arg(2))fail(87);
                else {
                    std::array<std::int32_t,4> rectangle{};
                    const std::array<std::int32_t,4> previous{buffer.left,buffer.top,buffer.left+buffer.view_columns-1,buffer.top+buffer.view_rows-1};
                    for(unsigned i=0;i<4;++i)rectangle[i]=static_cast<std::int16_t>(memory.load(GuestAddress{arg(2)+2*i},16))+(d(1)?0:previous[i]);
                    const auto width=rectangle[2]-rectangle[0]+1,height=rectangle[3]-rectangle[1]+1;
                    const auto maximum=buffer.window?buffer.window->maximum_size():std::array<std::uint16_t,2>{32767,32767};
                    if(rectangle[0]<0 || rectangle[1]<0 || width<2 || height<2 || rectangle[2]>=buffer.columns || rectangle[3]>=buffer.rows || width>maximum[0] || height>maximum[1])fail(87);
                    else {
                        std::uint32_t error=0;
#if !defined(_WIN32)
                        if(!buffer.window){winsize size{};size.ws_col=static_cast<std::uint16_t>(width);size.ws_row=static_cast<std::uint16_t>(height);
                            if(::ioctl(consoles_[stream]->descriptor,TIOCSWINSZ,&size))error=6;
                            else error=terminal_write(consoles_[stream]->descriptor,"\x1b[8;"+std::to_string(height)+";"+std::to_string(width)+"t");}
#endif
                        if(error)fail(error);
                        else {buffer.left=static_cast<std::uint16_t>(rectangle[0]);buffer.top=static_cast<std::uint16_t>(rectangle[1]);buffer.view_columns=static_cast<std::uint16_t>(width);buffer.view_rows=static_cast<std::uint16_t>(height);
                            error=console_sync(stream,0,buffer.cells.size());if(error)fail(error);else result=1;}
                    }
                }
            }else if(name=="SetConsoleCursorPosition"){
                const auto x=static_cast<std::int16_t>(d(1)),y=static_cast<std::int16_t>(d(1)>>16);
                if(x<0 || y<0 || x>=buffer.columns || y>=buffer.rows)fail(87);
                else {const auto left=buffer.left,top=buffer.top;buffer.x=static_cast<std::uint16_t>(x);buffer.y=static_cast<std::uint16_t>(y);buffer.reveal_cursor();
                    const auto error=console_sync(stream,0,left!=buffer.left || top!=buffer.top?buffer.cells.size():0);if(error)fail(error);else result=1;}
            }else if(name=="SetConsoleTextAttribute"){
                buffer.attributes=static_cast<std::uint16_t>(d(1));const auto error=console_sync(stream);if(error)fail(error);else result=1;
            }else if(name=="GetConsoleScreenBufferInfo"){
                if(!arg(1))fail(87);else {memory.check(GuestAddress{arg(1)},22,Access::Write);memory.write(GuestAddress{arg(1)},std::vector<std::uint8_t>(22));
                    const auto maximum=buffer.window?buffer.window->maximum_size():std::array<std::uint16_t,2>{buffer.view_columns,buffer.view_rows};
                    for(const auto [offset,value]:std::array<std::pair<unsigned,unsigned>,11>{{{0,buffer.columns},{2,buffer.rows},{4,buffer.x},{6,buffer.y},{8,buffer.attributes},{10,buffer.left},{12,buffer.top},{14,buffer.left+buffer.view_columns-1U},{16,buffer.top+buffer.view_rows-1U},{18,std::min(maximum[0],buffer.columns)},{20,std::min(maximum[1],buffer.rows)}}}){
                        memory.store(GuestAddress{arg(1)+offset},16,value);
                    }result=1;
                }
            }else {
                const bool fill=name.starts_with("Fill"),attribute=name.ends_with("Attribute"),wide=name.ends_with('W');
                const auto coord=d(3);const auto x=static_cast<std::int16_t>(coord),y=static_cast<std::int16_t>(coord>>16);
                if(!arg(4) || (!fill && d(2) && !arg(1)) || x<0 || y<0 || x>=buffer.columns || y>=buffer.rows)fail(87);
                else {memory.check(GuestAddress{arg(4)},4,Access::Write);const auto start=static_cast<std::size_t>(y)*buffer.columns+static_cast<unsigned>(x),count=std::min<std::size_t>(d(2),buffer.cells.size()-start);
                    if(fill){const auto character=static_cast<char16_t>(wide?d(1)&0xffffU:d(1)&0xffU);
                        if(!wide && !attribute && character>=128)fail(1113);
                        else {for(auto i=start;i<start+count;++i){if(attribute)buffer.cells[i].attributes=static_cast<std::uint16_t>(d(1));else buffer.cells[i].character=character;}
                            const auto error=console_sync(stream,start,count);if(error)fail(error);else {memory.store(GuestAddress{arg(4)},32,count);result=1;}
                        }
                    }else {
                        const unsigned unit=attribute || wide?2:1;memory.check(GuestAddress{arg(1)},count*unit,Access::Write);
                        if(!wide && !attribute && std::any_of(buffer.cells.begin()+static_cast<std::ptrdiff_t>(start),buffer.cells.begin()+static_cast<std::ptrdiff_t>(start+count),[](const auto& cell){return cell.character>=128;}))fail(1113);
                        else {for(std::size_t i=0;i<count;++i)memory.store(GuestAddress{arg(1)+i*unit},static_cast<std::uint8_t>(unit*8),attribute?buffer.cells[start+i].attributes:buffer.cells[start+i].character);
                            memory.store(GuestAddress{arg(4)},32,count);result=1;}
                    }
                }
            }
        }
    }catch(const MemoryFault&){if(name.starts_with("_cput")){memory.store(GuestAddress{runtime_.errno_cell},32,22);result=UINT64_MAX;}else fail(87);}
    catch(const std::bad_alloc&){if(name.starts_with("_cput")){memory.store(GuestAddress{runtime_.errno_cell},32,12);result=UINT64_MAX;}else fail(8);}
    catch(const std::invalid_argument&){if(name.starts_with("_cput")){memory.store(GuestAddress{runtime_.errno_cell},32,42);result=UINT64_MAX;}else fail(1113);}
    arg.return_to(state,result);return std::nullopt;
}
}
