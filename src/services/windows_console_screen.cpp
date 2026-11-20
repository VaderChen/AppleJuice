#include "x64arm/windows_services.hpp"
#include "x64arm/native_console.hpp"
#include "windows_console_encoding.hpp"
#include "windows_locale_data.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <cstdio>

namespace x64arm {
// 主控台螢幕緩衝區、字型、別名與命令歷程家族：共用 windows_console.cpp 的 ConsoleBuffer 與同步流程。
// COORD 以值傳遞（低 16 位 X、高 16 位 Y，皆為有號）；SMALL_RECT 為四個有號 16 位元的含端點矩形。
std::span<const WindowsServiceDescriptor> console_screen_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"kernel32.dll","WriteConsoleOutputA",5,0,0,0xc},{"kernel32.dll","WriteConsoleOutputW",5,0,0,0xc},
        {"kernel32.dll","ReadConsoleOutputA",5,0,0,0xc},{"kernel32.dll","ReadConsoleOutputW",5,0,0,0xc},
        {"kernel32.dll","WriteConsoleOutputCharacterA",5,0,0,0xc},{"kernel32.dll","WriteConsoleOutputCharacterW",5,0,0,0xc},
        {"kernel32.dll","WriteConsoleOutputAttribute",5,0,0,0xc},
        {"kernel32.dll","ScrollConsoleScreenBufferA",5,0,0,8},{"kernel32.dll","ScrollConsoleScreenBufferW",5,0,0,8},
        {"kernel32.dll","GetConsoleScreenBufferInfoEx",2},{"kernel32.dll","SetConsoleScreenBufferInfoEx",2},
        {"kernel32.dll","GetLargestConsoleWindowSize",1},{"kernel32.dll","SetConsoleDisplayMode",3,0,0,2},
        {"kernel32.dll","GetCurrentConsoleFont",3,0,0,2},{"kernel32.dll","GetCurrentConsoleFontEx",3,0,0,2},{"kernel32.dll","SetCurrentConsoleFontEx",3,0,0,2},
        {"kernel32.dll","GetConsoleFontSize",2,0,0,2},{"kernel32.dll","GetNumberOfConsoleFonts",0},
        {"kernel32.dll","GetConsoleFontInfo",4,0,0,6},{"kernel32.dll","SetConsoleFont",2,0,0,2},
        {"kernel32.dll","GetNumberOfConsoleMouseButtons",1},{"kernel32.dll","GetConsoleProcessList",2,0,0,2},
        {"kernel32.dll","GetConsoleKeyboardLayoutNameA",1},{"kernel32.dll","GetConsoleKeyboardLayoutNameW",1},
        {"kernel32.dll","VerifyConsoleIoHandle",1},{"kernel32.dll","GetConsoleInputWaitHandle",0},
        {"kernel32.dll","GetConsoleSelectionInfo",1},{"kernel32.dll","GetConsoleHistoryInfo",1},{"kernel32.dll","SetConsoleHistoryInfo",1},
        {"kernel32.dll","AddConsoleAliasA",3},{"kernel32.dll","AddConsoleAliasW",3},
        {"kernel32.dll","GetConsoleAliasA",4,0,0,4},{"kernel32.dll","GetConsoleAliasW",4,0,0,4},
        {"kernel32.dll","GetConsoleAliasesA",3,0,0,2},{"kernel32.dll","GetConsoleAliasesW",3,0,0,2},
        {"kernel32.dll","GetConsoleAliasesLengthA",1},{"kernel32.dll","GetConsoleAliasesLengthW",1},
        {"kernel32.dll","GetConsoleAliasExesA",2,0,0,2},{"kernel32.dll","GetConsoleAliasExesW",2,0,0,2},
        {"kernel32.dll","GetConsoleAliasExesLengthA",0},{"kernel32.dll","GetConsoleAliasExesLengthW",0},
        {"kernel32.dll","SetConsoleCommandHistoryMode",1,0,0,1},
        {"kernel32.dll","SetConsoleNumberOfCommandsA",2,0,0,1},{"kernel32.dll","SetConsoleNumberOfCommandsW",2,0,0,1},
        {"kernel32.dll","GetConsoleCommandHistoryA",3,0,0,2},{"kernel32.dll","GetConsoleCommandHistoryW",3,0,0,2},
        {"kernel32.dll","GetConsoleCommandHistoryLengthA",1},{"kernel32.dll","GetConsoleCommandHistoryLengthW",1},
        {"kernel32.dll","ExpungeConsoleCommandHistoryA",1},{"kernel32.dll","ExpungeConsoleCommandHistoryW",1},
        {"kernel32.dll","SetConsoleInputExeNameA",1},{"kernel32.dll","SetConsoleInputExeNameW",1},
        {"kernel32.dll","GetConsoleInputExeNameA",2,0,0,1},{"kernel32.dll","GetConsoleInputExeNameW",2,0,0,1},
        {"kernel32.dll","CreateConsoleScreenBuffer",5,0,0,7},{"kernel32.dll","SetConsoleActiveScreenBuffer",1},
        {"kernel32.dll","CloseConsoleHandle",1},{"kernel32.dll","DuplicateConsoleHandle",4,0,0,0xe},{"kernel32.dll","OpenConsoleW",4,0,0,0xe},
        {"kernel32.dll","SetStdHandleEx",3,0,0,1}
    };
    return table;
}
std::array<std::uint16_t,2> console_maximum_window(const ConsoleBuffer& buffer){
    return buffer.window?buffer.window->maximum_size():std::array<std::uint16_t,2>{buffer.view_columns,buffer.view_rows};
}
void console_write_screen_info(GuestMemory& memory,std::uint64_t address,const ConsoleBuffer& buffer){
    memory.check(GuestAddress{address},22,Access::Write);
    const auto maximum=console_maximum_window(buffer);
    const std::array<unsigned,11> values{buffer.columns,buffer.rows,buffer.x,buffer.y,buffer.attributes,buffer.left,buffer.top,
        buffer.left+buffer.view_columns-1U,buffer.top+buffer.view_rows-1U,std::min(maximum[0],buffer.columns),std::min(maximum[1],buffer.rows)};
    for(unsigned i=0;i<values.size();++i)memory.store(GuestAddress{address+2*i},16,values[i]);
}
bool console_same_name(std::u16string_view left,std::u16string_view right){
    // LOCALE_INVARIANT＋NORM_IGNORECASE：與 Windows 主控台比對別名與執行檔名稱的方式相同，涵蓋非 ASCII 字母。
    if(left.size()!=right.size())return false;
    const auto compared=windows_nls_compare(left,right,0x7f,1);
    return compared ? *compared==0 : left==right;
}
static auto console_find_history(ConsoleBuffer& buffer,std::u16string_view exe){
    return std::find_if(buffer.histories.begin(),buffer.histories.end(),[&](const auto& item){return console_same_name(item.exe,exe);});
}
static auto console_create_history(ConsoleBuffer& buffer,std::u16string_view exe){
    if(buffer.histories.size()>=buffer.history_buffers)buffer.histories.erase(buffer.histories.begin());
    buffer.histories.push_back({std::u16string(exe),{},buffer.history_size});
    return buffer.histories.end()-1;
}
void console_remember_command(ConsoleBuffer& buffer,std::u16string_view exe,std::u16string_view command){
    if(command.empty() || !buffer.history_buffers)return;
    auto found=console_find_history(buffer,exe);
    if(found==buffer.histories.end())found=console_create_history(buffer,exe);
    auto& commands=found->commands;
    if(!found->capacity || (!commands.empty() && commands.back()==command))return;
    if(buffer.history_flags&1U)commands.erase(std::remove(commands.begin(),commands.end(),command),commands.end());
    if(commands.size()==found->capacity)commands.pop_front();
    commands.emplace_back(command);
}
std::u16string WindowsServices::console_input_exe() const {
    if(!console_input_exe_name_.empty())return console_input_exe_name_;
    std::u16string path;if(const auto* image=module_image(runtime_.image.base))path=image->path;
    const auto at=path.find_last_of(u"\\/");return at==path.npos?path:path.substr(at+1);
}

namespace {
// 非作用中螢幕的操作期間取代原生視窗：更新與輪詢都不顯示，查詢沿用原視窗的最大尺寸與全螢幕狀態。
class HiddenConsoleWindow final : public ConsoleWindow {
public:
    HiddenConsoleWindow(ConsoleWindow* real,std::array<std::uint16_t,2> maximum):real_(real),maximum_(maximum){}
    bool update(const ConsoleSnapshot&) override {return true;}
    std::vector<ConsoleRecord> poll(unsigned) override {return {};}
    bool closed() const override {return real_ && real_->closed();}
    bool fullscreen() const override {return real_ && real_->fullscreen();}
    std::array<std::uint16_t,2> maximum_size() const override {return maximum_;}
private:
    ConsoleWindow* real_;std::array<std::uint16_t,2> maximum_;
};
// 以螢幕緩衝區 handle 為第一個引數的主控台輸出服務。
bool console_output_service(std::string_view name){
    static constexpr std::string_view names[]{
        "WriteConsoleA","WriteConsoleW","WriteConsoleOutputA","WriteConsoleOutputW","WriteConsoleOutputCharacterA","WriteConsoleOutputCharacterW",
        "WriteConsoleOutputAttribute","ReadConsoleOutputA","ReadConsoleOutputW","ReadConsoleOutputCharacterA","ReadConsoleOutputCharacterW",
        "ReadConsoleOutputAttribute","FillConsoleOutputCharacterA","FillConsoleOutputCharacterW","FillConsoleOutputAttribute",
        "ScrollConsoleScreenBufferA","ScrollConsoleScreenBufferW","SetConsoleCursorPosition","SetConsoleTextAttribute",
        "GetConsoleScreenBufferInfo","GetConsoleScreenBufferInfoEx","SetConsoleScreenBufferInfoEx","SetConsoleScreenBufferSize",
        "SetConsoleWindowInfo","GetConsoleCursorInfo","SetConsoleCursorInfo","GetConsoleMode","SetConsoleMode",
        "GetCurrentConsoleFont","GetCurrentConsoleFontEx","SetCurrentConsoleFontEx","GetConsoleFontSize","GetLargestConsoleWindowSize",
        "SetConsoleDisplayMode","WriteFile","FlushFileBuffers","GetFileType","VerifyConsoleIoHandle"};
    return std::find(std::begin(names),std::end(names),name)!=std::end(names);
}
// 交換作用中內容與保存的螢幕內容；標題與全螢幕屬於整個主控台，不隨螢幕緩衝區交換。
void exchange_screen(ConsoleBuffer& live,ConsoleSnapshot& snapshot,std::uint32_t& mode){
    const auto title=live.title;const bool fullscreen=live.fullscreen;
    std::swap(static_cast<ConsoleSnapshot&>(live),snapshot);std::swap(live.output_mode,mode);
    live.title=title;live.fullscreen=fullscreen;snapshot.title=title;
}
}
ConsoleBuffer* WindowsServices::console_output_buffer(){
    for(const unsigned stream:{1U,2U,0U})if(consoles_[stream])return consoles_[stream]->buffer.get();
    return nullptr;
}
struct WindowsServices::ConsoleScreen {ConsoleSnapshot snapshot;std::uint32_t output_mode{3};};
struct WindowsServices::ConsoleScreenState {
    std::map<std::uint64_t,std::shared_ptr<ConsoleScreen>> handles;   // nullptr 代表原始螢幕（0x101／0x102）的複本 handle
    std::shared_ptr<ConsoleScreen> active,original,swapped;            // active 為 nullptr 即原始螢幕作用中；original 保存非作用中的原始內容
    std::unique_ptr<ConsoleWindow> saved_window;std::uint64_t saved_window_handle{},next{0x104};bool swapped_in{};
};
WindowsServices::ConsoleScreenState& WindowsServices::console_screens(){
    if(!console_screen_state_)console_screen_state_=std::make_shared<ConsoleScreenState>();return *console_screen_state_;
}
bool WindowsServices::console_screen_handle(std::uint64_t handle) const{return console_screen_state_ && console_screen_state_->handles.contains(handle);}
bool WindowsServices::console_screen_swapped() const{return console_screen_state_ && console_screen_state_->swapped_in;}
unsigned WindowsServices::console_output_stream() const{return consoles_[1]?1U:consoles_[2]?2U:0U;}

bool WindowsServices::console_screen_enter(GuestState& state,GuestMemory& memory){
    auto& screens=console_screens();
    if(screens.swapped_in || !console_output_buffer() || !console_output_service(name(state.rip)))return false;
    const auto slot=x86_?state.reg(Gpr::Rsp)+4:0;const auto handle=x86_?memory.load(GuestAddress{slot},32):state.reg(Gpr::Rcx);
    std::shared_ptr<ConsoleScreen> screen;
    if(handle!=0x101 && handle!=0x102){
        const auto found=screens.handles.find(handle);if(found==screens.handles.end())return false;screen=found->second;
        // 被呼叫端擁有引數槽與 RCX：改成原始輸出 handle，既有主控台流程即作用在換入的內容上。
        if(x86_)memory.store(GuestAddress{slot},32,0x101);else state.reg(Gpr::Rcx)=0x101;
    }
    if(screen==screens.active)return false;
    auto& stored=screen?*screen:*screens.original;auto& live=*console_output_buffer();
    const auto maximum=console_maximum_window(live);exchange_screen(live,stored.snapshot,stored.output_mode);
    screens.saved_window=std::move(live.window);live.window=std::make_unique<HiddenConsoleWindow>(screens.saved_window.get(),maximum);
    screens.saved_window_handle=live.window_handle;live.window_handle=0;
    screens.swapped=screen?screen:screens.original;screens.swapped_in=true;return true;
}
void WindowsServices::console_screen_leave(){
    auto& screens=console_screens();
    if(!screens.swapped_in)return;screens.swapped_in=false;auto* live=console_output_buffer();auto stored=std::move(screens.swapped);
    if(!live){screens.saved_window.reset();return;}
    live->window=std::move(screens.saved_window);live->window_handle=screens.saved_window_handle;screens.saved_window_handle=0;
    if(stored)exchange_screen(*live,stored->snapshot,stored->output_mode);
}
std::uint32_t WindowsServices::activate_console_screen(std::shared_ptr<ConsoleScreen> target){
    auto& screens=console_screens();
    // 作用中的內容寫回其保存處（原始螢幕保存在 screens.original），再載入目標並整頁重繪；標題與全螢幕屬於整個主控台。
    if(target==screens.active)return 0;auto* live=console_output_buffer();if(!live)return 6;
    if(!screens.original)screens.original=std::make_shared<ConsoleScreen>();
    auto& outgoing=screens.active?*screens.active:*screens.original;
    auto& incoming=target?*target:*screens.original;
    const auto title=live->title;const bool fullscreen=live->fullscreen;
    outgoing.snapshot=static_cast<const ConsoleSnapshot&>(*live);outgoing.output_mode=live->output_mode;
    static_cast<ConsoleSnapshot&>(*live)=incoming.snapshot;live->output_mode=incoming.output_mode;live->title=title;live->fullscreen=fullscreen;
    screens.active=std::move(target);
    return console_sync(console_output_stream(),0,live->cells.size());
}
void WindowsServices::close_console_screen(std::uint64_t handle){
    auto& screens=console_screens();
    const auto found=screens.handles.find(handle);if(found==screens.handles.end())return;const auto screen=found->second;screens.handles.erase(found);console_screen_count_=screens.handles.size();
    runtime_.handle_flags.erase(handle);
    // 關閉作用中螢幕的最後一個 handle 時回到原始螢幕。
    const bool referenced=std::any_of(screens.handles.begin(),screens.handles.end(),[&](const auto& item){return item.second==screen;});
    if(screen && screen==screens.active && !referenced)(void)activate_console_screen(nullptr);
}
std::optional<std::uint32_t> WindowsServices::invoke_console_screen(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    auto& screens=console_screens();
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;std::uint64_t result=0;
    const auto d=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    const auto fail=[&](std::uint32_t error){last_error_=error;result=0;};
    const bool wide=name.ends_with('W');
    const auto current=std::find_if(consoles_.begin(),consoles_.end(),[](const auto& endpoint){return endpoint!=nullptr;});
    ConsoleBuffer* attached=current!=consoles_.end()?(*current)->buffer.get():nullptr;
    const auto x_of=[](std::uint32_t value){return static_cast<std::int32_t>(static_cast<std::int16_t>(value));};
    const auto y_of=[](std::uint32_t value){return static_cast<std::int32_t>(static_cast<std::int16_t>(value>>16));};
    const auto pack=[](std::int32_t x,std::int32_t y){return static_cast<std::uint32_t>(static_cast<std::uint16_t>(x))|(static_cast<std::uint32_t>(static_cast<std::uint16_t>(y))<<16);};
    struct Rect {std::int32_t left,top,right,bottom;};
    const auto read_rect=[&](std::uint64_t address){
        memory.check(GuestAddress{address},8,Access::Read);
        const auto field=[&](unsigned i){return static_cast<std::int32_t>(static_cast<std::int16_t>(memory.load(GuestAddress{address+2*i},16)));};
        return Rect{field(0),field(1),field(2),field(3)};
    };
    const auto write_rect=[&](std::uint64_t address,Rect value){
        memory.check(GuestAddress{address},8,Access::Write);
        const std::array<std::int32_t,4> fields{value.left,value.top,value.right,value.bottom};
        for(unsigned i=0;i<4;++i)memory.store(GuestAddress{address+2*i},16,static_cast<std::uint16_t>(fields[i]));
    };
    // A 版本的字元以主控台碼頁轉換；碼頁 API 讀寫輸入用輸入碼頁，螢幕內容用輸出碼頁。
    const auto text_in=[&](std::uint64_t pointer,std::uint32_t page)->std::u16string{
        if(!pointer)return {};
        return wide?read_utf16(memory,GuestAddress{pointer},65536):console_decode(read_ansi(memory,pointer,65536),page);
    };
    // 不含結尾 NUL 的編碼；put_strings 為每個字串補上 NUL，長度一律以位元組回報（W 版本為字元×2）。
    const auto encoded=[&](std::u16string_view text,std::uint32_t page)->std::vector<std::uint8_t>{
        if(wide){std::vector<std::uint8_t> bytes;bytes.reserve(text.size()*2);for(const auto c:text){bytes.push_back(static_cast<std::uint8_t>(c));bytes.push_back(static_cast<std::uint8_t>(c>>8));}return bytes;}
        const auto bytes=console_encode(text,page);return {bytes.begin(),bytes.end()};
    };
    const auto put_strings=[&](const std::vector<std::u16string>& items,std::uint64_t output,std::uint64_t capacity,std::uint32_t page,bool& fits)->std::uint64_t{
        std::vector<std::uint8_t> bytes;
        for(const auto& item:items){auto part=encoded(item,page);bytes.insert(bytes.end(),part.begin(),part.end());bytes.insert(bytes.end(),wide?2U:1U,0);}
        fits=bytes.size()<=capacity;
        if(fits && output && !bytes.empty())memory.write(GuestAddress{output},bytes);
        return bytes.size();
    };
    try{
        // 不需要主控台 handle 的查詢。
        if(name=="GetNumberOfConsoleMouseButtons"){
            if(!arg(0))fail(87);else if(!attached)fail(6);else {memory.store(GuestAddress{arg(0)},32,system_metric(memory,43));result=1;}
            arg.return_to(state,result);return std::nullopt;
        }
        if(name=="GetNumberOfConsoleFonts"){result=attached?1:0;if(!attached)fail(6);arg.return_to(state,result);return std::nullopt;}
        if(name.starts_with("GetConsoleKeyboardLayoutName")){
            if(!arg(0))fail(87);else if(!attached)fail(6);
            else {const auto layout=keyboard_layout_name();auto bytes=encoded(layout,65001);bytes.insert(bytes.end(),wide?2U:1U,0);memory.write(GuestAddress{arg(0)},bytes);result=1;}
            arg.return_to(state,result);return std::nullopt;
        }
        if(name=="VerifyConsoleIoHandle"){result=console_handle(arg(0))?1:0;arg.return_to(state,result);return std::nullopt;}
        if(name=="CreateConsoleScreenBuffer"){
            // (存取權, 共用模式, SECURITY_ATTRIBUTES*, CONSOLE_TEXTMODE_BUFFER, NULL)：新緩衝區沿用作用中螢幕的大小、視窗、色表與字型，
            // 內容為以目前屬性填滿的空白、游標在左上角，輸出模式沿用主控台目前的模式（終端模式下 VT 處理必須開著）。
            const auto invalid=x86_?UINT64_C(0xffffffff):UINT64_MAX;
            if(d(3)!=1 || arg(4)){fail(87);result=invalid;}
            else if(!attached){fail(6);result=invalid;}
            else if(screens.next>0xffc){fail(8);result=invalid;}
            else {
                auto screen=std::make_shared<ConsoleScreen>();screen->snapshot=static_cast<const ConsoleSnapshot&>(*console_output_buffer());auto& view=screen->snapshot;
                std::fill(view.cells.begin(),view.cells.end(),ConsoleCell{u' ',view.attributes});screen->output_mode=console_output_buffer()->output_mode;view.x=view.y=0;view.left=view.top=0;view.cursor_visible=true;view.cursor_size=25;
                const auto handle=screens.next;screens.next+=4;screens.handles.emplace(handle,std::move(screen));console_screen_count_=screens.handles.size();
                bool inherit=false;if(arg(2))inherit=memory.load(GuestAddress{arg(2)+(x86_?8U:16U)},32)!=0;runtime_.handle_flags[handle]=inherit?1U:0U;result=handle;
            }
            arg.return_to(state,result);return std::nullopt;
        }
        if(name=="SetConsoleActiveScreenBuffer"){
            if(arg(0)==0x101 || arg(0)==0x102){if(!console_handle(arg(0)))fail(6);else if(const auto error=activate_console_screen(nullptr))fail(error);else result=1;}
            else if(const auto found=screens.handles.find(arg(0));found!=screens.handles.end()){if(const auto error=activate_console_screen(found->second))fail(error);else result=1;}
            else fail(6);
            arg.return_to(state,result);return std::nullopt;
        }
        if(name=="CloseConsoleHandle"){
            if(screens.handles.contains(arg(0))){close_console_screen(arg(0));result=1;}
            else if(arg(0)>=0x100 && arg(0)<=0x102)result=nested_service(state,memory,"kernel32.dll","CloseHandle",{arg(0)});
            else fail(6);
            arg.return_to(state,result);return std::nullopt;
        }
        if(name=="DuplicateConsoleHandle" || name=="OpenConsoleW"){
            // DuplicateConsoleHandle(handle, 存取權, 繼承, 選項)：螢幕緩衝區 handle 的複本指向同一份內容，DUPLICATE_CLOSE_SOURCE 關閉來源；
            // 標準 handle 交給 DuplicateHandle。OpenConsoleW(L"CONIN$"／L"CONOUT$", 存取權, 繼承, 0)：CONOUT$ 指向作用中的螢幕。
            const auto invalid=x86_?UINT64_C(0xffffffff):UINT64_MAX;std::uint64_t source=arg(0);const bool inherit=d(2)!=0;const auto options=name=="OpenConsoleW"?0U:d(3);
            if(name=="OpenConsoleW"){
                const auto device=arg(0)?read_utf16(memory,GuestAddress{arg(0)},16):std::u16string{};
                if(console_same_name(device,u"CONIN$"))source=0x100;
                else if(console_same_name(device,u"CONOUT$"))source=0x101;
                else {fail(87);arg.return_to(state,invalid);return std::nullopt;}
                if(!console_handle(source)){fail(6);arg.return_to(state,invalid);return std::nullopt;}
            }
            std::shared_ptr<ConsoleScreen> screen;bool is_screen=false;
            if(const auto found=screens.handles.find(source);found!=screens.handles.end()){screen=found->second;is_screen=true;}
            else if(name=="OpenConsoleW" && source==0x101 && screens.active){screen=screens.active;is_screen=true;}
            if(is_screen){
                if(screens.next>0xffc){fail(8);result=invalid;}
                else {const auto handle=screens.next;screens.next+=4;screens.handles.emplace(handle,screen);console_screen_count_=screens.handles.size();runtime_.handle_flags[handle]=inherit?1U:0U;
                    if(options&1U)close_console_screen(source);result=handle;}
            }else if(source>=0x100 && source<=0x102 && console_handle(source)){
                GuestScratch scratch(*this,memory,state);const auto output=scratch.allocate(8);const auto self=x86_?UINT64_C(0xffffffff):UINT64_MAX;
                if(output && nested_service(state,memory,"kernel32.dll","DuplicateHandle",{self,source,self,output,d(1),inherit?1U:0U,options|(name=="OpenConsoleW"?2U:0U)}))
                    result=memory.load(GuestAddress{output},x86_?32:64);
                else result=invalid;
            }else {fail(6);result=invalid;}
            arg.return_to(state,result);return std::nullopt;
        }
        if(name=="SetStdHandleEx"){
            // SetStdHandle 並回傳先前的 handle。
            const auto previous=nested_service(state,memory,"kernel32.dll","GetStdHandle",{d(0)});
            result=nested_service(state,memory,"kernel32.dll","SetStdHandle",{d(0),arg(1)});
            if(result && arg(2))memory.store(GuestAddress{arg(2)},x86_?32:64,previous);
            arg.return_to(state,result);return std::nullopt;
        }
        if(name=="GetConsoleInputWaitHandle"){
            if(console_handle(0x100))result=0x100;else fail(6);
            arg.return_to(state,result);return std::nullopt;
        }
        if(name=="GetConsoleProcessList"){
            std::vector<ConsoleMember> members;
            if(!arg(0) || !d(1))fail(87);
            else if(const auto error=console_members(members))fail(error);
            else {
                // 緩衝區不足時只回報所需數量、不寫入任何識別碼。
                if(members.size()<=d(1)){memory.check(GuestAddress{arg(0)},members.size()*4,Access::Write);for(std::size_t i=0;i<members.size();++i)memory.store(GuestAddress{arg(0)+4*i},32,members[i].pid);}
                result=members.size();
            }
            arg.return_to(state,result);return std::nullopt;
        }
        if(name.starts_with("SetConsoleInputExeName") || name.starts_with("GetConsoleInputExeName")){
            if(name.starts_with("Set")){
                if(!arg(0))fail(87);
                else {auto value=text_in(arg(0),attached?attached->input_codepage:65001);if(value.empty() || value.size()>260)fail(87);else {console_input_exe_name_=std::move(value);result=1;}}
            }else {
                // GetConsoleInputExeName(nBufferLength, lpBuffer)：長度以字元計（含 NUL），不足時回 ERROR_BUFFER_OVERFLOW。
                const auto exe=console_input_exe();auto bytes=encoded(exe,attached?attached->input_codepage:65001);
                const auto units=wide?exe.size():bytes.size();
                if(!arg(1))fail(87);else if(d(0)<units+1)fail(111);
                else {bytes.insert(bytes.end(),wide?2U:1U,0);memory.write(GuestAddress{arg(1)},bytes);result=1;}
            }
            arg.return_to(state,result);return std::nullopt;
        }
        if(!attached){fail(6);arg.return_to(state,result);return std::nullopt;}
        auto& console=*attached;const auto input_page=console.input_codepage;
        if(name=="GetConsoleSelectionInfo"){
            // 主控台視窗沒有選取功能：CONSOLE_NO_SELECTION，錨點與範圍為零。
            if(!arg(0))fail(87);else {memory.write(GuestAddress{arg(0)},std::vector<std::uint8_t>(16));result=1;}
        }else if(name=="GetConsoleHistoryInfo" || name=="SetConsoleHistoryInfo"){
            // CONSOLE_HISTORY_INFO：cbSize(16)、HistoryBufferSize、NumberOfHistoryBuffers、dwFlags。
            if(!arg(0) || memory.load(GuestAddress{arg(0)},32)!=16)fail(87);
            else if(name.starts_with("Get")){memory.store(GuestAddress{arg(0)+4},32,console.history_size);memory.store(GuestAddress{arg(0)+8},32,console.history_buffers);memory.store(GuestAddress{arg(0)+12},32,console.history_flags);result=1;}
            else {
                const auto size=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)+4},32)),buffers=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)+8},32)),flags=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)+12},32));
                if(size>999 || buffers>999 || (flags&~1U))fail(87);
                else {console.history_size=size;console.history_buffers=buffers;console.history_flags=flags;
                    for(auto& history:console.histories)history.resize(size);
                    while(console.histories.size()>buffers)console.histories.erase(console.histories.begin());
                    result=1;}
            }
        }else if(name.starts_with("AddConsoleAlias")){
            const auto source=text_in(arg(0),input_page),target=text_in(arg(1),input_page),exe=text_in(arg(2),input_page);
            if(source.empty() || exe.empty())fail(87);
            else {
                auto table=std::find_if(console.aliases.begin(),console.aliases.end(),[&](const auto& item){return console_same_name(item.exe,exe);});
                if(table==console.aliases.end() && !target.empty()){console.aliases.push_back({exe,{}});table=console.aliases.end()-1;}
                if(table!=console.aliases.end()){
                    auto& entries=table->entries;
                    const auto entry=std::find_if(entries.begin(),entries.end(),[&](const auto& item){return console_same_name(item.source,source);});
                    // 目標為 NULL 或空字串即移除別名；最後一個別名移除後一併移除該執行檔。
                    if(target.empty()){if(entry!=entries.end())entries.erase(entry);if(entries.empty())console.aliases.erase(table);}
                    else if(entry!=entries.end())entry->target=target;else entries.push_back({source,target});
                }
                result=1;
            }
        }else if(name.starts_with("GetConsoleAliasExes")){
            std::vector<std::u16string> names;for(const auto& table:console.aliases)names.push_back(table.exe);
            bool fits=false;
            if(name.find("Length")!=name.npos)result=put_strings(names,0,UINT64_MAX,input_page,fits);
            else if(!arg(0))fail(87);
            else {result=put_strings(names,arg(0),d(1),input_page,fits);if(!fits)fail(122);}
        }else if(name.starts_with("GetConsoleAliases")){
            const bool length=name.find("Length")!=name.npos;const auto exe=text_in(arg(length?0:2),input_page);
            std::vector<std::u16string> items;
            if(const auto table=std::find_if(console.aliases.begin(),console.aliases.end(),[&](const auto& item){return console_same_name(item.exe,exe);});table!=console.aliases.end())
                for(const auto& entry:table->entries)items.push_back(entry.source+u"="+entry.target);
            bool fits=false;
            if(length)result=put_strings(items,0,UINT64_MAX,input_page,fits);
            else if(!arg(0) || exe.empty())fail(87);
            else {result=put_strings(items,arg(0),d(1),input_page,fits);if(!fits)fail(122);}
        }else if(name.starts_with("GetConsoleAlias")){
            const auto source=text_in(arg(0),input_page),exe=text_in(arg(3),input_page);
            if(source.empty() || exe.empty() || !arg(1))fail(87);
            else {
                const std::u16string* target=nullptr;
                if(const auto table=std::find_if(console.aliases.begin(),console.aliases.end(),[&](const auto& item){return console_same_name(item.exe,exe);});table!=console.aliases.end())
                    for(const auto& entry:table->entries)if(console_same_name(entry.source,source)){target=&entry.target;break;}
                bool fits=false;
                if(!target)fail(31);   // ERROR_GEN_FAILURE：沒有此別名
                else {result=put_strings({*target},arg(1),d(2),input_page,fits);if(!fits){fail(122);}}
            }
        }else if(name=="SetConsoleCommandHistoryMode"){
            console.input_mode=(console.input_mode&~0x20U)|((d(0)&1U)<<5);
            result=1;
        }else if(name.starts_with("SetConsoleNumberOfCommands")){
            const auto exe=text_in(arg(1),input_page);
            if(exe.empty() || d(0)>INT32_MAX)fail(87);
            else{
                auto history=console_find_history(console,exe);
                if(history==console.histories.end() && console.history_buffers && console_same_name(exe,console_input_exe()))
                    history=console_create_history(console,exe);
                if(history!=console.histories.end()){
                    history->resize(d(0));
                    std::rotate(history,history+1,console.histories.end());
                }
                result=1;
            }
        }else if(name.starts_with("GetConsoleCommandHistory") || name.starts_with("ExpungeConsoleCommandHistory")){
            const bool length=name.find("Length")!=name.npos,expunge=name.starts_with("Expunge");
            const auto exe=text_in(arg(length || expunge?0:2),input_page);
            const auto history=console_find_history(console,exe);
            if(expunge){if(history!=console.histories.end())history->commands.clear();result=1;}
            else {
                std::vector<std::u16string> items;if(history!=console.histories.end())items.assign(history->commands.begin(),history->commands.end());
                bool fits=false;
                if(length)result=put_strings(items,0,UINT64_MAX,input_page,fits);
                else if(!arg(0))fail(87);
                else {result=put_strings(items,arg(0),d(1),input_page,fits);if(!fits)fail(122);}
            }
        }else if(!console_handle(arg(0)) || arg(0)==0x100)fail(6);
        else {
            const auto stream=static_cast<unsigned>(arg(0)-0x100);auto& buffer=*consoles_[stream]->buffer;
            const auto page=buffer.output_codepage;
            const auto sync_rows=[&](std::int32_t top,std::int32_t bottom){
                const auto first=static_cast<std::size_t>(std::max(top,0))*buffer.columns;
                const auto last=static_cast<std::size_t>(std::min<std::int32_t>(bottom,buffer.rows-1)+1)*buffer.columns;
                return console_sync(stream,first,last>first?last-first:0);
            };
            // CHAR_INFO：WCHAR／CHAR 與 WORD 屬性，共 4 位元組；A 版本一個儲存格只放一個位元組。
            const auto cell_from=[&](std::uint32_t raw)->ConsoleCell{
                ConsoleCell cell{};cell.attributes=static_cast<std::uint16_t>(raw>>16);
                if(wide)cell.character=static_cast<char16_t>(raw&0xffffU);
                else {const char byte=static_cast<char>(raw&0xffU);const auto decoded=console_decode({&byte,1},page);cell.character=decoded.size()==1?decoded.front():u'?';}
                return cell;
            };
            const auto cell_to=[&](const ConsoleCell& cell)->std::uint32_t{
                std::uint32_t character=cell.character;
                if(!wide){const auto bytes=console_encode(std::u16string_view(&cell.character,1),page);character=bytes.size()==1?static_cast<unsigned char>(bytes.front()):'?';}
                return character|(std::uint32_t{cell.attributes}<<16);
            };
            if(name.starts_with("WriteConsoleOutput") && !name.starts_with("WriteConsoleOutputCharacter") && name!="WriteConsoleOutputAttribute"){
                // 目標矩形先依來源緩衝區可用範圍裁切，再裁到螢幕；回傳實際寫入的矩形，完全在外時右下小於左上。
                if(!arg(1) || !arg(4))fail(87);
                else {
                    const auto width=x_of(d(2)),height=y_of(d(2)),from_x=x_of(d(3)),from_y=y_of(d(3));
                    auto region=read_rect(arg(4));
                    if(width<=0 || height<=0 || from_x<0 || from_y<0)fail(87);
                    else {
                        const auto right=std::min(region.right,region.left+width-from_x-1),bottom=std::min(region.bottom,region.top+height-from_y-1);
                        const Rect clipped{std::max(region.left,0),std::max(region.top,0),std::min<std::int32_t>(right,buffer.columns-1),std::min<std::int32_t>(bottom,buffer.rows-1)};
                        if(clipped.left>clipped.right || clipped.top>clipped.bottom){write_rect(arg(4),{region.left,region.top,region.left-1,region.top-1});result=1;}
                        else {
                            for(auto y=clipped.top;y<=clipped.bottom;++y){
                                const auto source=arg(1)+(static_cast<std::uint64_t>(from_y+y-region.top)*static_cast<std::uint64_t>(width)+static_cast<std::uint64_t>(from_x+clipped.left-region.left))*4;
                                const auto count=static_cast<std::size_t>(clipped.right-clipped.left+1);
                                std::vector<std::uint8_t> row(count*4);memory.read(GuestAddress{source},row);
                                for(std::size_t i=0;i<count;++i){
                                    const auto raw=std::uint32_t{row[4*i]}|(std::uint32_t{row[4*i+1]}<<8)|(std::uint32_t{row[4*i+2]}<<16)|(std::uint32_t{row[4*i+3]}<<24);
                                    buffer.cells[static_cast<std::size_t>(y)*buffer.columns+static_cast<std::size_t>(clipped.left)+i]=cell_from(raw);
                                }
                            }
                            write_rect(arg(4),clipped);
                            if(const auto error=sync_rows(clipped.top,clipped.bottom))fail(error);else result=1;
                        }
                    }
                }
            }else if(name.starts_with("ReadConsoleOutput")){
                if(!arg(1) || !arg(4))fail(87);
                else {
                    const auto width=x_of(d(2)),height=y_of(d(2)),to_x=x_of(d(3)),to_y=y_of(d(3));
                    auto region=read_rect(arg(4));
                    if(width<=0 || height<=0 || to_x<0 || to_y<0)fail(87);
                    else {
                        const auto right=std::min(region.right,region.left+width-to_x-1),bottom=std::min(region.bottom,region.top+height-to_y-1);
                        const Rect clipped{std::max(region.left,0),std::max(region.top,0),std::min<std::int32_t>(right,buffer.columns-1),std::min<std::int32_t>(bottom,buffer.rows-1)};
                        if(clipped.left>clipped.right || clipped.top>clipped.bottom)write_rect(arg(4),{region.left,region.top,region.left-1,region.top-1});
                        else {
                            for(auto y=clipped.top;y<=clipped.bottom;++y){
                                const auto target=arg(1)+(static_cast<std::uint64_t>(to_y+y-region.top)*static_cast<std::uint64_t>(width)+static_cast<std::uint64_t>(to_x+clipped.left-region.left))*4;
                                const auto count=static_cast<std::size_t>(clipped.right-clipped.left+1);std::vector<std::uint8_t> row(count*4);
                                for(std::size_t i=0;i<count;++i){
                                    const auto raw=cell_to(buffer.cells[static_cast<std::size_t>(y)*buffer.columns+static_cast<std::size_t>(clipped.left)+i]);
                                    for(unsigned b=0;b<4;++b)row[4*i+b]=static_cast<std::uint8_t>(raw>>(8*b));
                                }
                                memory.write(GuestAddress{target},row);
                            }
                            write_rect(arg(4),clipped);
                        }
                        result=1;
                    }
                }
            }else if(name.starts_with("WriteConsoleOutputCharacter") || name=="WriteConsoleOutputAttribute"){
                // 從座標起逐格寫入並自動換行，到緩衝區結尾為止；A 版本回報實際消耗的位元組數。
                const bool attribute=name=="WriteConsoleOutputAttribute";const auto x=x_of(d(3)),y=y_of(d(3));
                if(!arg(4) || (d(2) && !arg(1)) || x<0 || y<0 || x>=buffer.columns || y>=buffer.rows)fail(87);
                else {
                    memory.check(GuestAddress{arg(4)},4,Access::Write);
                    const auto start=static_cast<std::size_t>(y)*buffer.columns+static_cast<std::size_t>(x),room=buffer.cells.size()-start;
                    std::uint64_t written=0;std::size_t cells=0;
                    if(attribute){
                        const auto count=std::min<std::size_t>(d(2),room);std::vector<std::uint8_t> raw(count*2);if(count)memory.read(GuestAddress{arg(1)},raw);
                        for(std::size_t i=0;i<count;++i)buffer.cells[start+i].attributes=static_cast<std::uint16_t>(raw[2*i]|(raw[2*i+1]<<8));
                        written=cells=count;
                    }else {
                        std::u16string text;
                        if(wide){std::vector<std::uint8_t> raw(static_cast<std::size_t>(std::min<std::uint64_t>(d(2),room))*2);if(!raw.empty())memory.read(GuestAddress{arg(1)},raw);
                            for(std::size_t i=0;i<raw.size();i+=2)text+=static_cast<char16_t>(raw[i]|(raw[i+1]<<8));}
                        else {if(d(2)>output_limit)throw std::bad_alloc();std::string bytes(d(2),'\0');if(!bytes.empty())memory.read(GuestAddress{arg(1)},std::span(reinterpret_cast<std::uint8_t*>(bytes.data()),bytes.size()));text=console_decode(bytes,page);}
                        cells=std::min(text.size(),room);
                        for(std::size_t i=0;i<cells;++i)buffer.cells[start+i].character=text[i];
                        written=wide?cells:cells==text.size()?d(2):console_encode(std::u16string_view(text).substr(0,cells),page).size();
                    }
                    memory.store(GuestAddress{arg(4)},32,written);
                    const auto error=console_sync(stream,start,cells);
                    if(error)fail(error);else result=1;
                }
            }else if(name.starts_with("ScrollConsoleScreenBuffer")){
                // 來源矩形裁到螢幕後搬到目的座標；裁切矩形限制所有被改寫的儲存格；來源中未被覆蓋的部分以填充字元填滿。
                if(!arg(1) || !arg(4))fail(87);
                else {
                    const auto scroll=read_rect(arg(1));
                    const Rect source{std::max(scroll.left,0),std::max(scroll.top,0),std::min<std::int32_t>(scroll.right,buffer.columns-1),std::min<std::int32_t>(scroll.bottom,buffer.rows-1)};
                    Rect clip{0,0,buffer.columns-1,buffer.rows-1};
                    if(arg(2)){const auto given=read_rect(arg(2));clip={std::max(given.left,0),std::max(given.top,0),std::min(given.right,clip.right),std::min(given.bottom,clip.bottom)};}
                    if(source.left>source.right || source.top>source.bottom || clip.left>clip.right || clip.top>clip.bottom)fail(87);
                    else {
                        memory.check(GuestAddress{arg(4)},4,Access::Read);
                        const auto fill=cell_from(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(4)},32)));
                        const auto dx=x_of(d(3))-scroll.left,dy=y_of(d(3))-scroll.top;
                        const auto inside=[](const Rect& r,std::int32_t x,std::int32_t y){return x>=r.left && x<=r.right && y>=r.top && y<=r.bottom;};
                        const auto snapshot=buffer.cells;
                        for(auto y=source.top;y<=source.bottom;++y)for(auto x=source.left;x<=source.right;++x)
                            if(inside(clip,x,y))buffer.cells[static_cast<std::size_t>(y)*buffer.columns+static_cast<std::size_t>(x)]=fill;
                        for(auto y=source.top;y<=source.bottom;++y)for(auto x=source.left;x<=source.right;++x){
                            const auto tx=x+dx,ty=y+dy;
                            if(tx<0 || ty<0 || tx>=buffer.columns || ty>=buffer.rows || !inside(clip,tx,ty))continue;
                            buffer.cells[static_cast<std::size_t>(ty)*buffer.columns+static_cast<std::size_t>(tx)]=snapshot[static_cast<std::size_t>(y)*buffer.columns+static_cast<std::size_t>(x)];
                        }
                        if(const auto error=sync_rows(clip.top,clip.bottom))fail(error);else result=1;
                    }
                }
            }else if(name=="GetConsoleScreenBufferInfoEx" || name=="SetConsoleScreenBufferInfoEx"){
                // CONSOLE_SCREEN_BUFFER_INFOEX（96 位元組）：偏移 4 起與 CONSOLE_SCREEN_BUFFER_INFO 相同，之後為彈出屬性、全螢幕支援與 16 色色表。
                if(!arg(1) || memory.load(GuestAddress{arg(1)},32)!=96)fail(87);
                else if(name.starts_with("Get")){
                    console_write_screen_info(memory,arg(1)+4,buffer);
                    memory.store(GuestAddress{arg(1)+26},16,buffer.popup_attributes);memory.store(GuestAddress{arg(1)+28},32,buffer.window?1:0);
                    for(unsigned i=0;i<16;++i)memory.store(GuestAddress{arg(1)+32+4*i},32,buffer.colors[i]);
                    result=1;
                }else {
                    const auto info=arg(1);memory.check(GuestAddress{info},96,Access::Read);
                    const auto columns=static_cast<std::int16_t>(memory.load(GuestAddress{info+4},16)),rows=static_cast<std::int16_t>(memory.load(GuestAddress{info+6},16));
                    const auto cursor_x=static_cast<std::int16_t>(memory.load(GuestAddress{info+8},16)),cursor_y=static_cast<std::int16_t>(memory.load(GuestAddress{info+10},16));
                    const auto window=read_rect(info+14);
                    // Windows 主控台把 srWindow 的右下當成不含端點：視窗寬高為 Right-Left、Bottom-Top（與 Get 回報的含端點值差一）。
                    const auto view_width=window.right-window.left,view_height=window.bottom-window.top;
                    const auto maximum=console_maximum_window(buffer);
                    if(columns<=0 || rows<=0 || static_cast<std::uint64_t>(columns)*static_cast<std::uint64_t>(rows)>1024U*1024U || cursor_x<0 || cursor_y<0 || cursor_x>=columns || cursor_y>=rows ||
                       view_width<1 || view_height<1 || window.left<0 || window.top<0 || window.left+view_width>columns || window.top+view_height>rows || view_width>maximum[0] || view_height>maximum[1])fail(87);
                    else {
                        if(columns!=buffer.columns || rows!=buffer.rows){
                            buffer.resize(static_cast<std::uint16_t>(columns),static_cast<std::uint16_t>(rows),true);
                            ConsoleRecord event{};set_console_field(event,0,4,2);set_console_field(event,4,buffer.columns,2);set_console_field(event,6,buffer.rows,2);buffer.push(event);
                        }
                        buffer.left=static_cast<std::uint16_t>(window.left);buffer.top=static_cast<std::uint16_t>(window.top);
                        buffer.view_columns=static_cast<std::uint16_t>(view_width);buffer.view_rows=static_cast<std::uint16_t>(view_height);
                        buffer.x=static_cast<std::uint16_t>(cursor_x);buffer.y=static_cast<std::uint16_t>(cursor_y);
                        buffer.attributes=static_cast<std::uint16_t>(memory.load(GuestAddress{info+12},16));
                        buffer.popup_attributes=static_cast<std::uint16_t>(memory.load(GuestAddress{info+26},16));
                        std::array<std::uint32_t,16> colors{};for(unsigned i=0;i<16;++i)colors[i]=static_cast<std::uint32_t>(memory.load(GuestAddress{info+32+4*i},32))&0xffffffU;
                        std::uint32_t error=0;
                        if(colors!=buffer.colors){
                            buffer.colors=colors;
                            // 終端模式以 OSC 4 改寫 ANSI 調色盤（Windows 索引 RGB 位元與 ANSI 的 BGR 順序不同）。
                            if(!buffer.window){
                                constexpr std::array<unsigned,8> ansi{0,4,2,6,1,5,3,7};std::string text;
                                for(unsigned i=0;i<16;++i){const auto value=colors[i];char hex[32];
                                    std::snprintf(hex,sizeof hex,"\x1b]4;%u;rgb:%02x/%02x/%02x\x07",ansi[i&7]+(i&8),value&255U,(value>>8)&255U,(value>>16)&255U);text+=hex;}
                                error=console_terminal_write(stream,text);
                            }
                        }
                        if(!error)error=console_sync(stream,0,buffer.cells.size());
                        if(error)fail(error);else result=1;
                    }
                }
            }else if(name=="GetLargestConsoleWindowSize"){
                const auto maximum=console_maximum_window(buffer);result=pack(maximum[0],maximum[1]);
            }else if(name=="SetConsoleDisplayMode"){
                // CONSOLE_FULLSCREEN_MODE 1／CONSOLE_WINDOWED_MODE 2；回傳新的螢幕緩衝區大小。
                const auto mode=d(1);
                if(mode!=1 && mode!=2)fail(87);
                else if(!buffer.window)fail(50);
                else if(!buffer.window->set_fullscreen(mode==1))fail(5);
                else {buffer.fullscreen=mode==1;if(arg(2))memory.store(GuestAddress{arg(2)},32,pack(buffer.columns,buffer.rows));result=1;}
            }else if(name=="GetConsoleFontSize"){
                if(d(1))fail(87);else result=pack(buffer.font_width,buffer.font_height);
            }else if(name=="GetCurrentConsoleFont" || name=="GetConsoleFontInfo"){
                // CONSOLE_FONT_INFO：nFont 與 dwFontSize；目前只有一個字型（索引 0）。
                const bool list=name=="GetConsoleFontInfo";const auto output=arg(list?3:2);
                if(!output || (list && !d(2)))fail(87);
                else {memory.store(GuestAddress{output},32,0);memory.store(GuestAddress{output+4},32,pack(buffer.font_width,buffer.font_height));result=1;}
            }else if(name=="SetConsoleFont"){
                if(d(1))fail(87);else result=1;
            }else if(name=="GetCurrentConsoleFontEx" || name=="SetCurrentConsoleFontEx"){
                // CONSOLE_FONT_INFOEX（84 位元組）：cbSize、nFont、dwFontSize、FontFamily、FontWeight、FaceName[32]。
                const auto info=arg(2);
                if(!info || memory.load(GuestAddress{info},32)!=84)fail(87);
                else if(name.starts_with("Get")){
                    memory.check(GuestAddress{info},84,Access::Write);memory.store(GuestAddress{info+4},32,0);memory.store(GuestAddress{info+8},32,pack(buffer.font_width,buffer.font_height));
                    memory.store(GuestAddress{info+12},32,buffer.font_family);memory.store(GuestAddress{info+16},32,buffer.font_weight);
                    std::vector<std::uint8_t> face(64);const auto units=std::min<std::size_t>(buffer.font_face.size(),31);
                    for(std::size_t i=0;i<units;++i){face[2*i]=static_cast<std::uint8_t>(buffer.font_face[i]);face[2*i+1]=static_cast<std::uint8_t>(buffer.font_face[i]>>8);}
                    memory.write(GuestAddress{info+20},face);result=1;
                }else {
                    memory.check(GuestAddress{info},84,Access::Read);
                    const auto size=static_cast<std::uint32_t>(memory.load(GuestAddress{info+8},32));const auto width=x_of(size),height=y_of(size);
                    const auto weight=static_cast<std::uint32_t>(memory.load(GuestAddress{info+16},32));
                    std::u16string face;for(unsigned i=0;i<32;++i){const auto c=static_cast<char16_t>(memory.load(GuestAddress{info+20+2*i},16));if(!c)break;face+=c;}
                    if(height<0 || width<0 || height>255 || width>255 || weight>1000)fail(87);
                    else {
                        // 高度為 0 保留目前高度；寬度為 0 依 TrueType 字型以高度的一半推算。
                        const auto cell_height=height?height:buffer.font_height;
                        buffer.font_height=static_cast<std::uint16_t>(std::max(cell_height,4));
                        buffer.font_width=static_cast<std::uint16_t>(std::max(width?width:(cell_height+1)/2,2));
                        if(weight)buffer.font_weight=weight;
                        buffer.font_family=static_cast<std::uint32_t>(memory.load(GuestAddress{info+12},32));
                        if(!face.empty())buffer.font_face=std::move(face);
                        const auto error=console_sync(stream,0,buffer.cells.size());if(error)fail(error);else result=1;
                    }
                }
            }else throw HostCallFault("未知的主控台螢幕服務");
        }
    }catch(const MemoryFault&){fail(87);}
    catch(const std::bad_alloc&){fail(8);}
    catch(const std::invalid_argument&){fail(1113);}
    arg.return_to(state,result);return std::nullopt;
}
}
