#include "x64arm/windows_services.hpp"
#include "x64arm/windows_window_state.hpp"
#include "x64arm/windows_gui_abi.hpp"
#include "x64arm/windows_paths.hpp"
#include "x64arm/windows_fonts.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>

namespace x64arm {
std::span<const WindowsServiceDescriptor> common_dialog_service_descriptors() {
    static constexpr WindowsServiceDescriptor table[]{
        {"comdlg32.dll","ChooseFontA",1},{"comdlg32.dll","ChooseFontW",1},
        {"comdlg32.dll","ChooseColorA",1},{"comdlg32.dll","ChooseColorW",1},
        {"comdlg32.dll","GetOpenFileNameA",1},{"comdlg32.dll","GetOpenFileNameW",1},
        {"comdlg32.dll","GetSaveFileNameA",1},{"comdlg32.dll","GetSaveFileNameW",1},
        {"comdlg32.dll","CommDlgExtendedError",0},
        {"comdlg32.dll","__CommonDialogProcA",4},{"comdlg32.dll","__CommonDialogProcW",4},
        {"oledlg.dll","OleUIBusyA",1},{"oledlg.dll","OleUIBusyW",1},
        {"oledlg.dll","__OleBusyDialogProcA",4},{"oledlg.dll","__OleBusyDialogProcW",4}
    };return table;
}
namespace {
using Kind=GuiCommonDialog::Kind;
struct CommonDialogAbi {
    unsigned size,legacy_size,owner,flags,font,point_size,font_type,min_size,max_size,color,custom_colors;
    unsigned filter,filter_index,file,file_capacity,file_title,title_capacity,directory,title,extension,file_offset,extension_offset,hook,font_color,style;
    std::uint8_t bits;
    CommonDialogAbi(Kind kind,bool small)
        : size(kind==Kind::Font?(small?60:104):kind==Kind::Colour?(small?36:72):(small?88:152)),
          legacy_size(small?76:136),owner(small?4:8),flags(kind==Kind::Font?(small?20:36):kind==Kind::Colour?(small?20:40):(small?52:96)),
          font(small?12:24),point_size(small?16:32),font_type(small?48:88),min_size(small?52:92),max_size(small?56:96),
          color(small?12:24),custom_colors(small?16:32),filter(small?12:24),filter_index(small?24:44),file(small?28:48),file_capacity(small?32:56),
          file_title(small?36:64),title_capacity(small?40:72),directory(small?44:80),title(small?48:88),extension(small?60:104),
          file_offset(small?56:100),extension_offset(small?58:102),
          hook(kind==Kind::Font?(small?32:56):kind==Kind::Colour?(small?28:56):(small?68:120)),font_color(small?24:40),style(small?44:80),bits(small?32:64) {}
};
std::u16string number(std::int32_t value){return utf8_to_utf16_lossy(std::to_string(value));}
std::optional<std::int32_t> integer(const std::u16string& text){
    const auto s=utf16_to_utf8(text);std::int32_t v{};const auto r=std::from_chars(s.data(),s.data()+s.size(),v);
    return r.ec==std::errc{} && r.ptr==s.data()+s.size()?std::optional{v}:std::nullopt;
}
std::u16string slash(std::u16string s){std::replace(s.begin(),s.end(),u'/',u'\\');return s;}
std::u16string parent_path(const std::u16string& path){const auto n=path.find_last_of(u'\\');return n==std::u16string::npos?u".":n==2 && path[1]==u':'?path.substr(0,3):path.substr(0,n);}
std::u16string join_path(const std::u16string& dir,const std::u16string& name){return dir.empty() || dir==u"."?name:dir+(dir.back()==u'\\'?u"":u"\\")+name;}
bool match_filter(std::u16string_view name,std::u16string_view pattern){
    const auto lower=[](char16_t c){return c>=u'A' && c<=u'Z'?static_cast<char16_t>(c+32):c;};
    std::size_t n=0,p=0,star=std::u16string_view::npos,retry=0;
    if(pattern==u"*.*")return true;
    while(n<name.size()){
        if(p<pattern.size() && (pattern[p]==u'?' || lower(pattern[p])==lower(name[n]))){++n;++p;}
        else if(p<pattern.size() && pattern[p]==u'*'){star=p++;retry=n;}
        else if(star!=std::u16string_view::npos){p=star+1;n=++retry;}else return false;
    }
    while(p<pattern.size() && pattern[p]==u'*')++p;return p==pattern.size();
}
bool matches(std::u16string_view name,std::u16string_view patterns){
    for(std::size_t start=0;start<=patterns.size();){auto end=patterns.find(u';',start);if(end==patterns.npos)end=patterns.size();if(match_filter(name,patterns.substr(start,end-start)))return true;if(end==patterns.size())break;start=end+1;}return false;
}
}
std::optional<std::uint32_t> WindowsServices::invoke_gui_busy(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const bool wide=service.symbol.ends_with('W');
    const GuiControlAbi abi(x86_);
    const auto pointer=[&](std::uint64_t p,unsigned index){return memory.load(GuestAddress{p+8+index*abi.unit},abi.bits);};
    if(service.symbol.starts_with("__OleBusyDialogProc")){
        const auto handle=arg(0);auto window=gui_.windows.find(handle);
        if(window==gui_.windows.end()){arg.return_to(state,0);return std::nullopt;}
        if(arg(1)==0x110){
            const auto p=arg(3);GuiCommonDialog data;data.kind=Kind::Busy;data.request=p;data.wide=wide;
            data.flags=static_cast<std::uint32_t>(memory.load(GuestAddress{p+4},32));data.hook=pointer(p,2);
            data.blocking_task=pointer(p,7);data.dialog_output=pointer(p,8);
            const auto owner=gui_.windows.find(window->second.owner);data.owner_enabled=owner==gui_.windows.end() || owner->second.enabled;
            if(data.dialog_output)memory.store(GuestAddress{data.dialog_output},abi.bits,handle);
            const auto caption=pointer(p,1);
            if(caption)window->second.title=wide?read_utf16(memory,GuestAddress{caption}):utf8_to_utf16_lossy(read_ansi(memory,caption));
            for(const auto& [id,disabled]:{std::pair{2U,(data.flags&9U)!=0},std::pair{604U,(data.flags&2U)!=0},std::pair{600U,(data.flags&4U)!=0}}){
                const auto child=gui_child(handle,id);if(child){gui_set_window_enabled(gui_.windows.at(child),!disabled);present_gui(child);}
            }
            window->second.default_id=(data.flags&4U)?((data.flags&2U)?2:604):600;present_gui(handle);
            gui_.common_dialogs[handle]=std::move(data);
        }
        const auto found=gui_.common_dialogs.find(handle);
        if(found==gui_.common_dialogs.end() || found->second.kind!=Kind::Busy){arg.return_to(state,0);return std::nullopt;}
        std::vector<GuestCallback> callbacks;if(found->second.hook)callbacks.push_back({found->second.hook,{handle,arg(1),arg(2),arg(3)}});
        start_callbacks(state,memory,std::move(callbacks),Continuation::GuiBusyHook);return std::nullopt;
    }
    const auto p=arg(0);const auto fail=[&](std::uint32_t error){arg.return_to(state,error);};
    if(!p){fail(101);return std::nullopt;}
    std::uint32_t error=102;std::uint64_t allocation=0;
    try {
        memory.check(GuestAddress{p},8+9*abi.unit,Access::Write);
        if(memory.load(GuestAddress{p},32)!=8+9*abi.unit){fail(103);return std::nullopt;}
        const auto flags=memory.load(GuestAddress{p+4},32),owner=pointer(p,0),caption=pointer(p,1),hook=pointer(p,2);
        const auto instance=pointer(p,4),name=pointer(p,5),resource=pointer(p,6),task=pointer(p,7),output=pointer(p,8);
        error=104;if(owner && !gui_.windows.contains(owner)){fail(error);return std::nullopt;}
        error=105;const auto title=caption?(wide?read_utf16(memory,GuestAddress{caption}):utf8_to_utf16_lossy(read_ansi(memory,caption))):std::u16string{u"Busy"};
        error=106;if(hook && !handles(GuestAddress{hook}))memory.check(GuestAddress{hook},1,Access::Execute);
        error=107;if(instance && !resource_image(instance)){fail(error);return std::nullopt;}
        error=108;if(name>=65536){if(wide)(void)read_utf16(memory,GuestAddress{name});else (void)read_ansi(memory,name);}
        error=116;if(task){const auto local=threads_.items.find(task==process_id()?1:task);
            if((local==threads_.items.end() || local->second.status==ThreadStatus::Exited) && !(task<=UINT32_MAX && runtime_.network.external_io && remote_process_available(static_cast<std::uint32_t>(task)))){fail(error);return std::nullopt;}}
        error=102;if(output)memory.check(GuestAddress{output},abi.unit,Access::Write);
        std::uint64_t source=resource;
        if(resource){
            error=109;const auto global=gui_.global_memory.find(resource);if(global!=gui_.global_memory.end())source=global->second.address;
            if(!source || source%4){fail(error);return std::nullopt;}memory.check(GuestAddress{source},18,Access::Read);
        }else if(name){
            error=110;const auto entry=gui_resource(memory,instance,name,5,wide);if(!entry){fail(error);return std::nullopt;}
            const auto* image=resource_image(entry,true);if(!image){fail(111);return std::nullopt;}
            source=image->base+memory.load(GuestAddress{entry},32);
        }else{
            // 系統模板仍交由共用 DLGTEMPLATE 解析、建立控制項與模態訊息迴圈。
            std::vector<std::uint8_t> bytes;
            const auto put=[&](std::uint32_t value,unsigned count){for(unsigned n=0;n<count;++n)bytes.push_back(static_cast<std::uint8_t>(value>>(n*8)));};
            const auto text=[&](std::u16string_view value){for(auto c:value)put(c,2);put(0,2);};
            put(0x90c80040U,4);put(0x00000001U,4);put(4,2);put(30,2);put(30,2);put(310,2);put(100,2);put(0,2);put(0,2);text(title);put(9,2);text(u"Segoe UI");
            const auto item=[&](std::uint16_t id,std::uint16_t cls,std::u16string_view label,unsigned x,unsigned y,unsigned width,unsigned height,std::uint32_t extra){
                while(bytes.size()%4)put(0,1);put(0x50000000U|extra,4);put(0,4);put(x,2);put(y,2);put(width,2);put(height,2);put(id,2);put(0xffff,2);put(cls,2);text(label);put(0,2);
            };
            item(602,0x82,(flags&8U)?u"另一個應用程式沒有回應。請切換至該應用程式，或重試目前的操作。":u"另一個應用程式正在忙碌。請切換至該應用程式處理問題，或選擇重試／取消。",12,12,286,40,0);
            item(604,0x80,u"切換至…",65,67,75,18,0x10000U);item(600,0x80,u"重試",145,67,70,18,0x10001U);item(2,0x80,u"取消",220,67,70,18,0x10000U);
            error=114;allocation=allocate_heap(memory,bytes.size());if(!allocation){fail(error);return std::nullopt;}memory.write(GuestAddress{allocation},bytes);source=allocation;
        }
        if(output)memory.store(GuestAddress{output},abi.bits,0);
        start_callbacks(state,memory,{{resolve("user32.dll",wide?"DialogBoxIndirectParamW":"DialogBoxIndirectParamA").value,{instance,source,owner,resolve("oledlg.dll",wide?"__OleBusyDialogProcW":"__OleBusyDialogProcA").value,p}}},Continuation::GuiBusyResult,allocation);
        return std::nullopt;
    }catch(const MemoryFault&){if(allocation)(void)free_heap(memory,allocation,state);fail(error);return std::nullopt;}
     catch(const HostCallFault&){if(allocation)(void)free_heap(memory,allocation,state);fail(error);return std::nullopt;}
     catch(const std::invalid_argument&){if(allocation)(void)free_heap(memory,allocation,state);fail(error);return std::nullopt;}
}
std::optional<std::uint32_t> WindowsServices::resume_gui_busy(GuestState& state,GuestMemory& memory,const CallbackFrame& frame,std::uint64_t value){
    if(frame.continuation==Continuation::GuiBusyResult){if(frame.value)(void)free_heap(memory,frame.value,state);complete_call(state,memory,value==UINT64_MAX || static_cast<std::uint32_t>(value)==UINT32_MAX?112:value);return std::nullopt;}
    const WindowsCall arg(frame.saved,memory,4);const auto handle=arg(0);const auto message=static_cast<std::uint32_t>(arg(1));
    const auto found=gui_.common_dialogs.find(handle);auto window=gui_.windows.find(handle);
    if(found==gui_.common_dialogs.end() || window==gui_.windows.end()){complete_call(state,memory,0);return std::nullopt;}
    const auto data=found->second;
    if(data.kind!=Kind::Busy){
        if(!value){const GuiMessage message_data{handle,arg(2),arg(3),message};
            if(common_dialog_message(memory,message_data,true))value=message==0x401 || message==0x465?0U:1U;
            else if(message==0x110)value=1;
            else {start_callbacks(state,memory,{{resolve("user32.dll",data.wide?"DefWindowProcW":"DefWindowProcA").value,{handle,message,arg(2),arg(3)}}},Continuation::ReturnCallback);return std::nullopt;}
        }
        complete_call(state,memory,value);return std::nullopt;
    }
    // 清除輸出 HWND 是壽命管理，不受 hook 是否攔截 WM_NCDESTROY 影響。
    if(message==0x82 && data.dialog_output)memory.store(GuestAddress{data.dialog_output},x86_?32:64,0);
    if(value){complete_call(state,memory,value);return std::nullopt;}
    const auto end=[&](std::uint64_t result){window->second.ended=true;window->second.dialog_result=result;};
    const auto id=arg(2)&65535;
    if(message==gui_registered_message(u"OLEUI_MSG_CLOSEBUSYDIALOG")){end(119);value=1;}
    else if(message==0x10 || (message==0x111 && id==2)){if(!(data.flags&9U))end(2);value=1;}
    else if(message==0x111 && (id==600 || id==604) && (arg(2)>>16)==0){
        value=1;
        if(!(data.flags&(id==600?4U:2U))){
            end(id==604 && !data.blocking_task && (data.flags&8U)?117:118);
            if(id==604 && data.blocking_task){
                const auto task=data.blocking_task==process_id()?1:data.blocking_task;
                const auto target=std::find_if(gui_.windows.begin(),gui_.windows.end(),[&](const auto& item){return item.first!=handle && !item.second.parent && item.second.visible && item.second.thread==task;});
                if(target!=gui_.windows.end()){start_callbacks(state,memory,{{resolve("user32.dll","SetForegroundWindow").value,{target->first}}},Continuation::Return,1);return std::nullopt;}
            }
        }
    }else if(message==0x110)value=1;
    complete_call(state,memory,value);return std::nullopt;
}
void WindowsServices::common_dialog_refresh(std::uint64_t handle){
    auto& data=gui_.common_dialogs.at(handle);auto& list=gui_.windows.at(gui_child(handle,412));list.items.clear();list.selection=-1;
    auto& location=gui_.windows.at(gui_child(handle,410));location.title=data.directory;present_gui(gui_child(handle,410));
    const auto entries=filesystem_.list(utf16_to_utf8(data.directory));
    auto& status=gui_.windows.at(gui_child(handle,499));status.title=entries.error?u"無法存取此目錄，請選擇已映射的工作目錄或 C:\\。":u"";
    if(data.directory!=u"." && data.directory!=u"C:\\")list.items.push_back({gui_.next_item++,0,1,u"..",0});
    std::u16string filter=u"*";const auto filterbox=gui_child(handle,413);
    if(filterbox){const auto index=gui_.windows.at(filterbox).selection;if(index>=0 && static_cast<std::size_t>(index)<data.filters.size())filter=data.filters[static_cast<std::size_t>(index)];}
    for(const auto& entry:entries.entries){const auto name=utf8_to_utf16_lossy(entry.name);const bool dir=(entry.metadata.attributes&16)!=0;if(dir || matches(name,filter))list.items.push_back({gui_.next_item++,0,dir?1U:0U,name,0});}
    present_gui(gui_child(handle,412));present_gui(gui_child(handle,499));
}
std::optional<std::uint32_t> WindowsServices::invoke_common_dialog(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(service.family=="oledlg.dll")return invoke_gui_busy(state,memory,service);
    const WindowsCall arg(state,memory,service);
    if(service.symbol=="CommDlgExtendedError"){arg.return_to(state,gui_.common_dialog_error);return std::nullopt;}
    if(service.symbol.starts_with("__CommonDialogProc")){
        const auto found=gui_.common_dialogs.find(arg(0));if(found==gui_.common_dialogs.end()){arg.return_to(state,0);return std::nullopt;}
        std::vector<GuestCallback> callbacks;if(found->second.hook)callbacks.push_back({found->second.hook,{arg(0),arg(1),arg(2),arg(3)}});
        start_callbacks(state,memory,std::move(callbacks),Continuation::GuiBusyHook);return std::nullopt;
    }
    gui_.common_dialog_error=0;const auto p=arg(0);const bool wide=service.symbol.ends_with('W');
    const auto fail=[&](std::uint32_t e){gui_.common_dialog_error=e;arg.return_to(state,0);};
    if(!p){fail(2);return std::nullopt;}
    const auto size=memory.load(GuestAddress{p},32);GuiCommonDialog data;data.request=p;data.wide=wide;
    if(service.symbol.starts_with("ChooseFont"))data.kind=Kind::Font;else if(service.symbol.starts_with("ChooseColor"))data.kind=Kind::Colour;
    else data.kind=service.symbol.starts_with("GetSave")?Kind::SaveFile:Kind::OpenFile;
    const bool file=data.kind==Kind::SaveFile || data.kind==Kind::OpenFile;
    const CommonDialogAbi abi(data.kind,x86_);
    if(size!=abi.size && !(file && size==abi.legacy_size)){fail(1);return std::nullopt;}
    memory.check(GuestAddress{p},size,Access::Write);
    const auto load=[&](unsigned offset,unsigned width=0){return memory.load(GuestAddress{p+offset},static_cast<std::uint8_t>(width?width:abi.bits));};
    const auto read=[&](std::uint64_t address){return address?(wide?read_utf16(memory,GuestAddress{address}):utf8_to_utf16_lossy(read_ansi(memory,address))):std::u16string{};};
    data.flags=static_cast<std::uint32_t>(load(abi.flags,32));
    // 模板尚未經共用解析器接入，明確回報而不忽略呼叫者的 UI 契約。
    if(data.flags&(file?0xc0U:data.kind==Kind::Font?0x30U:0x60U)){fail(2);return std::nullopt;}
    if(data.flags&(file?0x20U:data.kind==Kind::Font?8U:16U)){
        data.hook=load(abi.hook);if(!data.hook){fail(0xb);return std::nullopt;}
        if(!handles(GuestAddress{data.hook}))memory.check(GuestAddress{data.hook},1,Access::Execute);
    }
    if(data.kind==Kind::Font && (data.flags&0x2000) && (load(abi.min_size,32)>load(abi.max_size,32) || load(abi.max_size,32)>512)){fail(2);return std::nullopt;}
    if(data.kind==Kind::Font){if(!load(abi.font)){fail(2);return std::nullopt;}memory.check(GuestAddress{load(abi.font)},wide?92:60,Access::Write);}
    if(data.kind==Kind::Font && (data.flags&0x80U) && !load(abi.style)){fail(2);return std::nullopt;}
    if(data.kind==Kind::Colour && load(abi.custom_colors))memory.check(GuestAddress{load(abi.custom_colors)},64,Access::Write);
    if(file){if(!load(abi.file) || !load(abi.file_capacity,32) || load(abi.file_capacity,32)>1048576){fail(2);return std::nullopt;}memory.check(GuestAddress{load(abi.file)},load(abi.file_capacity,32)*(wide?2:1),Access::Write);}
    std::u16string initial_face,initial_file;
    // 固定陣列和 lpstrFile 必須在宣告容量內終止；先驗證再建立原生視窗。
    try {
        if(data.kind==Kind::Font && (data.flags&0x40))initial_face=wide?read_utf16(memory,GuestAddress{load(abi.font)+28},31):utf8_to_utf16_lossy(read_ansi(memory,load(abi.font)+28,31));
        if(file)initial_file=wide?read_utf16(memory,GuestAddress{load(abi.file)},load(abi.file_capacity,32)-1):utf8_to_utf16_lossy(read_ansi(memory,load(abi.file),load(abi.file_capacity,32)-1));
    }catch(const HostCallFault&){fail(file?0x3002:2);return std::nullopt;}catch(const std::invalid_argument&){fail(file?0x3002:2);return std::nullopt;}
    // Headless 回傳使用者取消；原生模式才建立可操作的共用對話框。
    if(!native_gui_ && !data.hook){arg.return_to(state,0);return std::nullopt;}
    GuiWindow dialog;dialog.dpi_context=effective_dpi_context();dialog.thread=threads_.current;dialog.class_name=u"#32770";dialog.dialog=true;dialog.modal=true;dialog.builtin=true;dialog.visible=true;dialog.owner=load(abi.owner);dialog.width=600;dialog.height=file?480:data.kind==Kind::Font && (data.flags&0x100U)?400:data.kind==Kind::Colour?460:300;
    dialog.procedure=resolve("comdlg32.dll",wide?"__CommonDialogProcW":"__CommonDialogProcA").value;
    dialog.unicode=wide;
    dialog.title=file?(read(load(abi.title)).empty()?(data.kind==Kind::SaveFile?u"Save file":u"Open file"):read(load(abi.title))):data.kind==Kind::Font?u"Font":u"Colour";
    data.owner_enabled=!gui_.windows.contains(dialog.owner) || gui_.windows.at(dialog.owner).enabled;
    const auto h=allocate_gui_window();gui_.windows[h]=dialog;if(native_gui_)native_gui_->create(h,dialog);
    const auto add=[&](std::uint64_t id,std::u16string cls,std::u16string text,int x,int y,int width,int height,std::uint32_t style=0){GuiWindow w;w.dpi_context=effective_dpi_context();w.thread=threads_.current;w.parent=h;w.id=id;w.class_name=std::move(cls);w.title=std::move(text);w.x=x;w.y=y;w.width=static_cast<std::uint32_t>(width);w.height=static_cast<std::uint32_t>(height);w.style=style;w.visible=true;w.builtin=true;const auto child=allocate_gui_window();gui_.windows[child]=w;if(native_gui_)native_gui_->create(child,w);return child;};
    const auto row=[&](unsigned id,const std::u16string& label,const std::u16string& text,int y){add(0,u"static",label,20,y,130,26);return add(id,u"edit",text,160,y,410,26);};
    if(data.kind==Kind::Font){
        std::u16string face=u"Menlo";int points=10,weight=400;bool italic=false;
        if(data.flags&0x40){const auto lf=load(abi.font);const auto height=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{lf},32)));points=static_cast<int>(std::clamp((std::llabs(static_cast<long long>(height))*72+48)/96,1LL,512LL));weight=static_cast<int>(memory.load(GuestAddress{lf+16},32));italic=memory.load(GuestAddress{lf+20},8)!=0;face=initial_face;}
        if((data.flags&0x80U) && load(abi.style)){auto style=read(load(abi.style));for(auto& c:style)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);weight=style.find(u"bold")!=style.npos || style.find(u"粗")!=style.npos?700:400;italic=style.find(u"italic")!=style.npos || style.find(u"oblique")!=style.npos || style.find(u"斜")!=style.npos;}
        add(0,u"static",u"字型",20,25,130,26);const auto font=add(401,u"combobox",face,160,25,410,28,2);
        auto& fonts=gui_.windows.at(font);std::set<std::u16string> families;
        if(native_gui_)for(auto& family:native_gui_->font_families((data.flags&0x4000)!=0))families.insert(std::move(family));
        else for(const auto& available:native_font_faces({}))if(!(data.flags&0x4000U) || available.fixed_pitch)families.insert(available.family);
        for(const auto& family:families)fonts.items.push_back({gui_.next_item++,0,0,family,0});present_gui(font);
        row(402,u"大小（pt）",number(points),70);const auto bold=add(403,u"button",u"粗體",160,115,170,28,3);gui_.windows.at(bold).check=weight>=700;present_gui(bold);
        const auto it=add(404,u"button",u"斜體",350,115,170,28,3);gui_.windows.at(it).check=italic;present_gui(it);
        if(data.flags&0x100U){const auto lf=load(abi.font),color=load(abi.font_color,32);const auto under=add(408,u"button",u"底線",160,150,170,28,3),strike=add(409,u"button",u"刪除線",350,150,170,28,3);gui_.windows.at(under).check=memory.load(GuestAddress{lf+21},8)!=0;gui_.windows.at(strike).check=memory.load(GuestAddress{lf+22},8)!=0;present_gui(under);present_gui(strike);row(405,u"文字紅（0–255）",number(static_cast<int>(color&255)),190);row(406,u"文字綠（0–255）",number(static_cast<int>((color>>8)&255)),225);row(407,u"文字藍（0–255）",number(static_cast<int>((color>>16)&255)),260);}
    }else if(data.kind==Kind::Colour){const auto color=(data.flags&1)?load(abi.color,32):0;row(401,u"紅（0–255）",number(static_cast<int>(color&255)),25);row(402,u"綠（0–255）",number(static_cast<int>((color>>8)&255)),70);row(403,u"藍（0–255）",number(static_cast<int>((color>>16)&255)),115);
        const auto custom=load(abi.custom_colors);for(unsigned n=0;n<16;++n){const auto value=custom?memory.load(GuestAddress{custom+n*4},32):0xffffffU;const auto text=number(static_cast<int>(value&255))+u","+number(static_cast<int>((value>>8)&255))+u","+number(static_cast<int>((value>>16)&255));add(420+n,u"button",text,20+static_cast<int>(n%4)*138,175+static_cast<int>(n/4)*32,130,28);}
        row(437,u"自訂色格（1–16）",u"1",315);const auto append=add(436,u"button",u"加入自訂色",370,350,200,28);gui_set_window_enabled(gui_.windows.at(append),custom!=0 && !(data.flags&4U));present_gui(append);
    }else{
        auto initial=slash(initial_file);data.directory=slash(read(load(abi.directory)));if(data.directory.empty())data.directory=u".";
        if(const auto cut=initial.find_last_of(u'\\');cut!=initial.npos){data.directory=parent_path(initial);initial=initial.substr(cut+1);}
        data.extension=read(load(abi.extension));while(!data.extension.empty() && data.extension.front()==u'.')data.extension.erase(0,1);
        row(410,u"目錄",data.directory,15);add(414,u"button",u"前往",480,50,90,28);add(415,u"button",u"工作目錄",20,50,120,28);add(416,u"button",u"C:\\",150,50,90,28);add(417,u"button",u"上一層",250,50,90,28);
        add(412,u"listbox",{},20,90,550,205,1);row(411,u"檔案名稱",initial,315);
        add(0,u"static",u"檔案類型",20,355,130,26);const auto filter=add(413,u"combobox",{},160,355,410,28,3);auto& filters=gui_.windows.at(filter);
        auto at=load(abi.filter);for(unsigned i=0;at && i<128;++i){const auto label=read(at);if(label.empty())break;at+=(wide?label.size():utf16_to_utf8(label).size())*(wide?2:1)+(wide?2:1);const auto pattern=read(at);at+=(wide?pattern.size():utf16_to_utf8(pattern).size())*(wide?2:1)+(wide?2:1);data.filters.push_back(pattern);filters.items.push_back({gui_.next_item++,0,0,label,0});}
        if(filters.items.empty()){data.filters.push_back(u"*");filters.items.push_back({gui_.next_item++,0,0,u"所有檔案",0});}const auto filter_index=load(abi.filter_index,32);filters.selection=filter_index>=1 && filter_index<=filters.items.size()?static_cast<int>(filter_index-1):0;filters.title=filters.items[static_cast<std::size_t>(filters.selection)].text;present_gui(filter);
    }
    const auto footer=static_cast<int>(dialog.height)-55;if(data.kind==Kind::Font && (data.flags&0x200U))add(1026,u"button",u"套用",20,footer,90,28);add(499,u"static",u"",20,footer-38,550,32);add(1,u"button",u"OK",370,footer,90,28,1);add(2,u"button",u"Cancel",480,footer,90,28);
    gui_.common_dialogs[h]=std::move(data);if(file)common_dialog_refresh(h);
    std::vector<GuestCallback> callbacks;if(gui_.common_dialogs.at(h).hook)callbacks.push_back({dialog.procedure,{h,0x110,0,p}});
    start_callbacks(state,memory,std::move(callbacks),Continuation::GuiModalInit,h);return std::nullopt;
}
bool WindowsServices::common_dialog_message(GuestMemory& memory,const GuiMessage& msg,bool from_hook){
    if(shell_open_with_message(msg))return true;
    const auto found=gui_.common_dialogs.find(msg.window);if(found==gui_.common_dialogs.end())return false;
    if(found->second.kind==Kind::Busy)return false; // 先讓對話框 hook 決定是否攔截。
    if(found->second.hook && !from_hook)return false;
    auto& data=found->second;auto& dialog=gui_.windows.at(msg.window);const auto p=data.request;const bool wide=data.wide;const CommonDialogAbi abi(data.kind,x86_);
    if(msg.message==0x10){dialog.ended=true;dialog.dialog_result=0;return true;}
    const bool query_font=data.kind==Kind::Font && msg.message==0x401;
    if(msg.message!=0x111 && !query_font && !(data.kind==Kind::Font && msg.message==0x465))return false;
    const auto id=query_font?1U:msg.wparam&65535,notice=query_font?0U:msg.wparam>>16;
    const auto control=[&](unsigned n)->GuiWindow&{return gui_.windows.at(gui_child(msg.window,n));};
    const auto status=[&](const std::u16string& s){control(499).title=s;present_gui(gui_child(msg.window,499));};
    const auto load=[&](unsigned offset,unsigned width=0){return memory.load(GuestAddress{p+offset},static_cast<std::uint8_t>(width?width:abi.bits));};
    const auto store=[&](unsigned offset,unsigned width,std::uint64_t value){memory.store(GuestAddress{p+offset},static_cast<std::uint8_t>(width),value);};
    if(data.kind==Kind::Font && msg.message==0x465){
        const auto font=msg.lparam;if(!font)return true;memory.check(GuestAddress{font},wide?92:60,Access::Read);
        control(401).title=wide?read_utf16(memory,GuestAddress{font+28},31):utf8_to_utf16_lossy(read_ansi(memory,font+28,31));
        const auto height=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{font},32)));
        control(402).title=number(static_cast<std::int32_t>(std::clamp((std::llabs(static_cast<long long>(height))*72+48)/96,1LL,512LL)));
        control(403).check=memory.load(GuestAddress{font+16},32)>=700;control(404).check=memory.load(GuestAddress{font+20},8)!=0;
        if(data.flags&0x100U){control(408).check=memory.load(GuestAddress{font+21},8)!=0;control(409).check=memory.load(GuestAddress{font+22},8)!=0;present_gui(gui_child(msg.window,408));present_gui(gui_child(msg.window,409));}
        for(unsigned n=401;n<=404;++n)present_gui(gui_child(msg.window,n));return true;
    }
    if(id==2 || id==3){dialog.ended=true;dialog.dialog_result=0;return true;}
    const bool file=data.kind==Kind::OpenFile || data.kind==Kind::SaveFile;
    if(data.kind==Kind::Colour && ((id>=420 && id<436) || id==436)){
        const auto colors=load(abi.custom_colors);if(!colors)return true;
        if(id==436){if(data.flags&4U)return true;const auto index=integer(control(437).title),r=integer(control(401).title),g=integer(control(402).title),b=integer(control(403).title);if(!index || *index<1 || *index>16 || !r || !g || !b || *r<0 || *r>255 || *g<0 || *g>255 || *b<0 || *b>255){status(u"請輸入有效色格與 RGB。 ");return true;}memory.store(GuestAddress{colors+static_cast<unsigned>(*index-1)*4},32,static_cast<std::uint32_t>(*r|(*g<<8)|(*b<<16)));control(419+static_cast<unsigned>(*index)).title=number(*r)+u","+number(*g)+u","+number(*b);present_gui(gui_child(msg.window,419+static_cast<unsigned>(*index)));}
        else {const auto color=memory.load(GuestAddress{colors+(id-420)*4},32);for(unsigned n=0;n<3;++n){control(401+n).title=number(static_cast<int>((color>>(n*8))&255));present_gui(gui_child(msg.window,401+n));}}
        return true;
    }
    if(file){
        if(id==412 && notice==1){const auto& list=control(412);if(list.selection>=0 && static_cast<std::size_t>(list.selection)<list.items.size()){control(411).title=list.items[static_cast<std::size_t>(list.selection)].text;present_gui(gui_child(msg.window,411));}return true;}
        if((id==413 && notice==1) || id==414 || id==415 || id==416 || id==417){
            if(id==414)data.directory=slash(control(410).title);else if(id==415)data.directory=u".";else if(id==416)data.directory=u"C:\\";else if(id==417)data.directory=parent_path(data.directory);
            if(data.directory.empty())data.directory=u".";common_dialog_refresh(msg.window);return true;
        }
    }
    if(id!=1 || notice)return true;
    if(data.kind==Kind::Font){
        const auto face=control(401).title;const auto points=integer(control(402).title);
        const auto min=(data.flags&0x2000)?std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(load(abi.min_size,32))):1,max=(data.flags&0x2000)?std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(load(abi.max_size,32))):512;
        if(face.empty() || !points || *points<min || *points>max || *points<1 || *points>512){status(u"請輸入有效字型與大小。 ");return true;}
        if(data.flags&(0x10000U|0x4000U)){
            auto fold=[](std::u16string text){for(auto& c:text)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);return text;};
            const auto family=fold(face);const std::vector<std::u16string> aliases{u"consolas",u"lucida console",u"fixedsys",u"terminal",u"courier new"};
            const bool known=std::any_of(control(401).items.begin(),control(401).items.end(),[&](const auto& item){return fold(item.text)==family;}) || std::find(aliases.begin(),aliases.end(),family)!=aliases.end();
            if(!known){status(u"請從清單選擇可用字型。 ");return true;}
        }
        const auto encoded=utf16_to_utf8(face);if((wide?face.size():encoded.size())>=32){status(u"字型名稱過長。 ");return true;}
        std::uint32_t effects_color=0;
        if(!query_font && (data.flags&0x100U)){const auto r=integer(control(405).title),g=integer(control(406).title),b=integer(control(407).title);if(!r || !g || !b || *r<0 || *r>255 || *g<0 || *g>255 || *b<0 || *b>255){status(u"文字 RGB 每個值必須介於 0 和 255。 ");return true;}effects_color=static_cast<std::uint32_t>(*r|(*g<<8)|(*b<<16));}
        const auto lf=query_font?msg.lparam:load(abi.font);if(!lf)return true;memory.check(GuestAddress{lf},wide?92:60,Access::Write);std::vector<std::uint8_t> bytes(wide?92:60);if(data.flags&0x40)memory.read(GuestAddress{load(abi.font)},bytes);memory.write(GuestAddress{lf},bytes);
        memory.store(GuestAddress{lf},32,static_cast<std::uint32_t>(-((*points*96+36)/72)));memory.store(GuestAddress{lf+4},32,0);memory.store(GuestAddress{lf+16},32,control(403).check?700:400);memory.store(GuestAddress{lf+20},8,control(404).check?1:0);
        memory.write(GuestAddress{lf+28},std::vector<std::uint8_t>(wide?64:32));if(wide)memory.write(GuestAddress{lf+28},utf16_bytes(face));else {memory.write(GuestAddress{lf+28},{reinterpret_cast<const std::uint8_t*>(encoded.c_str()),encoded.size()+1});}
        if(data.flags&0x100U){memory.store(GuestAddress{lf+21},8,control(408).check!=0);memory.store(GuestAddress{lf+22},8,control(409).check!=0);if(!query_font)store(abi.font_color,32,effects_color);}
        if(!query_font && (data.flags&0x80U) && load(abi.style)){const auto style=control(403).check?(control(404).check?u"粗斜體":u"粗體"):(control(404).check?u"斜體":u"一般");if(wide)memory.write(GuestAddress{load(abi.style)},utf16_bytes(style));else{const auto text=utf16_to_utf8(style);memory.write(GuestAddress{load(abi.style)},{reinterpret_cast<const std::uint8_t*>(text.c_str()),text.size()+1});}}
        if(query_font)return true;store(abi.point_size,32,static_cast<std::uint32_t>(*points*10));store(abi.font_type,16,0x2000U | (control(403).check?0x100U:0U) | (control(404).check?0x200U:0U) | (!control(403).check && !control(404).check?0x400U:0U)); // SCREEN／BOLD／ITALIC／REGULAR_FONTTYPE
    }else if(data.kind==Kind::Colour){
        const auto r=integer(control(401).title),g=integer(control(402).title),b=integer(control(403).title);if(!r || !g || !b || *r<0 || *r>255 || *g<0 || *g>255 || *b<0 || *b>255){status(u"RGB 每個值必須介於 0 和 255。 ");return true;}store(abi.color,32,static_cast<std::uint32_t>(*r|(*g<<8)|(*b<<16)));
    }else{
        auto name=slash(control(411).title);if(name.empty()){status(u"請選擇或輸入檔案名稱。 ");return true;}
        if(name==u".."){data.directory=parent_path(data.directory);common_dialog_refresh(msg.window);return true;}
        auto path=name.starts_with(u"\\") || (name.size()>1 && name[1]==u':')?name:join_path(data.directory,name);
        try{path=normalized_windows_path(std::move(path),runtime_.current_directory);}catch(const std::invalid_argument&){status(u"請輸入有效的已映射路徑。 ");return true;}
        auto info=filesystem_.information(utf16_to_utf8(path));if(!info.error && (info.metadata.attributes&16)){data.directory=path;control(411).title.clear();present_gui(gui_child(msg.window,411));common_dialog_refresh(msg.window);return true;}
        const auto leaf=name.substr(name.find_last_of(u'\\')==name.npos?0:name.find_last_of(u'\\')+1);
        if(leaf.find(u'.')==leaf.npos && !data.extension.empty()){path+=u"."+data.extension;info=filesystem_.information(utf16_to_utf8(path));}
        const auto parent=filesystem_.information(utf16_to_utf8(parent_path(path)));
        if(parent.error || !(parent.metadata.attributes&16) || (info.error && info.error!=2 && info.error!=3)){status(u"路徑無法存取，請使用已映射的目錄。 ");return true;}
        if((data.flags&0x1000) && info.error){status(u"指定的檔案不存在。 ");return true;}
        if(data.kind==Kind::SaveFile && (data.flags&2) && !info.error && data.overwrite_path!=path){data.overwrite_path=path;status(u"檔案已存在；再次按 OK 確認覆寫。 ");return true;}
        const auto utf8=utf16_to_utf8(path);const auto length=wide?path.size():utf8.size();if(length+1>load(abi.file_capacity,32)){gui_.common_dialog_error=0x3003;if(load(abi.file_capacity,32)*(wide?2:1)>=2)memory.store(GuestAddress{load(abi.file)},16,std::min<std::size_t>(length+1,65535));dialog.ended=true;dialog.dialog_result=0;return true;}
        const auto basename=path.substr(path.find_last_of(u'\\')==path.npos?0:path.find_last_of(u'\\')+1);const auto title=load(abi.file_title),capacity=load(abi.title_capacity,32);const auto out=load(abi.file);
        if(title && capacity)memory.check(GuestAddress{title},capacity*(wide?2:1),Access::Write);
        if(wide)memory.write(GuestAddress{out},utf16_bytes(path));else memory.write(GuestAddress{out},{reinterpret_cast<const std::uint8_t*>(utf8.c_str()),utf8.size()+1});
        if(title && capacity){if(wide)memory.write(GuestAddress{title},utf16_bytes(basename.substr(0,capacity-1)));else{const auto t=utf16_to_utf8(basename).substr(0,capacity-1);memory.write(GuestAddress{title},{reinterpret_cast<const std::uint8_t*>(t.c_str()),t.size()+1});}}
        const auto offset=wide?path.size()-basename.size():utf8.size()-utf16_to_utf8(basename).size();const auto ext=wide?path.find_last_of(u'.'):utf8.find_last_of('.');
        store(abi.file_offset,16,offset);store(abi.extension_offset,16,ext==std::string::npos || ext<offset?0:ext+1);store(abi.filter_index,32,static_cast<std::uint32_t>(control(413).selection+1));
    }
    dialog.ended=true;dialog.dialog_result=1;return true;
}
}
