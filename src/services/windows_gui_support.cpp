#include "x64arm/windows_services.hpp"
#include "x64arm/windows_icon.hpp"
#include "windows_slist.hpp"
#include "x64arm/windows_userdraw.hpp"
#include "x64arm/windows_geometry.hpp"
#include "x64arm/windows_window_state.hpp"
#include "x64arm/windows_gui_abi.hpp"
#include "x64arm/unicode.hpp"
#include "directmusic_interfaces.hpp"
#include "dsound_interfaces.hpp"
#include "wasapi_interfaces.hpp"
#include "dinput_interfaces.hpp"
#include <algorithm>
#include <bit>
#include <limits>

namespace x64arm {
std::uint32_t WindowsServices::system_metric(std::uint32_t index,std::uint32_t dpi) {
    switch(index){
    case 0:case 1:case 78:case 79:{const auto area=gui_screen_area();return static_cast<std::uint32_t>((index==0 || index==78)?area[2]-area[0]:area[3]-area[1]);}
    case 16:case 17:{const auto area=gui_work_area();return static_cast<std::uint32_t>(index==16?area[2]-area[0]:area[3]-area[1]);}
    case 68:return gui_scale_signed_metric(gui_.mouse_drag_width,dpi);
    case 69:return gui_scale_signed_metric(gui_.mouse_drag_height,dpi);
    default:return gui_system_metric(index,dpi);
    }
}

std::span<const WindowsServiceDescriptor> gui_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"user32.dll","WinHelpA",4},{"user32.dll","WinHelpW",4},
        {"user32.dll","DefFrameProcA",5},{"user32.dll","DefFrameProcW",5},
        {"user32.dll","DefMDIChildProcA",4},{"user32.dll","DefMDIChildProcW",4},
        {"user32.dll","CreateMDIWindowA",10},{"user32.dll","CreateMDIWindowW",10},
        {"user32.dll","TranslateMDISysAccel",2},
        {"user32.dll","IsWindowUnicode",1},{"user32.dll","GetNextDlgTabItem",3},{"user32.dll","GetNextDlgGroupItem",3},
        {"dwmapi.dll","DwmIsCompositionEnabled",1},{"dwmapi.dll","DwmEnableComposition",1},
        {"dwmapi.dll","DwmExtendFrameIntoClientArea",2},{"dwmapi.dll","DwmEnableBlurBehindWindow",2},
        {"dwmapi.dll","DwmSetWindowAttribute",4},{"dwmapi.dll","DwmGetWindowAttribute",4},
        {"dwmapi.dll","DwmDefWindowProc",5},{"dwmapi.dll","DwmGetCompositionTimingInfo",2},{"dwmapi.dll","DwmFlush",0},
        {"user32.dll","RegisterTouchWindow",2},{"user32.dll","UnregisterTouchWindow",1},{"user32.dll","IsTouchWindow",2},
        {"user32.dll","GetTouchInputInfo",4},{"user32.dll","CloseTouchInputHandle",1},
        {"imm32.dll","ImmCreateContext",0},{"imm32.dll","ImmDestroyContext",1},{"imm32.dll","ImmAssociateContext",2},{"imm32.dll","ImmAssociateContextEx",3},
        {"imm32.dll","ImmGetOpenStatus",1},{"imm32.dll","ImmSetOpenStatus",2},{"imm32.dll","ImmGetConversionStatus",3},{"imm32.dll","ImmSetConversionStatus",3},
        {"imm32.dll","ImmGetCompositionWindow",2},{"imm32.dll","ImmGetCompositionFontA",2},{"imm32.dll","ImmGetCompositionFontW",2},
        {"imm32.dll","ImmSetCandidateWindow",2},{"imm32.dll","ImmGetCandidateWindow",3},{"imm32.dll","ImmGetCompositionStringA",4},{"imm32.dll","ImmGetCompositionStringW",4},{"imm32.dll","ImmIsIME",1},
        {"imm32.dll","ImmGetContext",1},{"imm32.dll","ImmReleaseContext",2},{"imm32.dll","ImmSetCompositionWindow",2},{"imm32.dll","ImmSetCompositionFontA",2},{"imm32.dll","ImmSetCompositionFontW",2},
        {"kernel32.dll","InterlockedFlushSList",1},{"kernel32.dll","InterlockedPushEntrySList",2},{"kernel32.dll","InterlockedPopEntrySList",1},{"kernel32.dll","QueryDepthSList",1},{"kernel32.dll","InitializeSListHead",1}, {"kernel32.dll","FindResourceA",3},{"kernel32.dll","FindResourceExA",4},{"kernel32.dll","FindResourceExW",4},{"kernel32.dll","FindResourceW",3},
        {"kernel32.dll","LoadResource",2},{"kernel32.dll","LockResource",1},{"kernel32.dll","SizeofResource",2},
        {"kernel32.dll","MulDiv",3},
        {"ole32.dll","CoInitialize",1},{"ole32.dll","CoInitializeEx",2},{"ole32.dll","CoUninitialize",0},{"ole32.dll","CoCreateInstance",5,0,0,4},{"ole32.dll","CoGetClassObject",5,0,0,2},
        {"comctl32.dll","PropertySheetA",1},{"comctl32.dll","PropertySheetW",1},
        {"comctl32.dll","InitCommonControls",0},{"comctl32.dll","InitCommonControlsEx",1},
        {"user32.dll","DialogBoxParamA",5},{"user32.dll","DialogBoxParamW",5},{"user32.dll","EndDialog",2},
        {"user32.dll","DialogBoxIndirectParamA",5},{"user32.dll","DialogBoxIndirectParamW",5},{"user32.dll","CreateDialogIndirectParamA",5},{"user32.dll","CreateDialogIndirectParamW",5},
        {"user32.dll","MessageBoxA",4},{"user32.dll","MessageBoxW",4},
        {"user32.dll","MessageBoxIndirectA",1},{"user32.dll","MessageBoxIndirectW",1},{"user32.dll","MessageBeep",1},
        {"comctl32.dll","MakeDragList",1},{"comctl32.dll","LBItemFromPt",3},{"comctl32.dll","DrawInsert",3},
        {"ole32.dll","OleDuplicateData",3},
        {"shell32.dll","DragAcceptFiles",2},{"user32.dll","ClipCursor",1},{"user32.dll","GetClipCursor",1},{"user32.dll","AttachThreadInput",3},{"user32.dll","GetScrollPos",2},{"shell32.dll","DragQueryFileA",4},{"shell32.dll","DragQueryFileW",4},{"shell32.dll","DragQueryPoint",2},{"shell32.dll","DragFinish",1},
        {"kernel32.dll","GlobalAlloc",2},{"kernel32.dll","GlobalFree",1},{"kernel32.dll","GlobalLock",1},{"kernel32.dll","GlobalUnlock",1},
        {"kernel32.dll","GlobalSize",1},{"kernel32.dll","GlobalFlags",1},{"kernel32.dll","GlobalHandle",1},
        {"kernel32.dll","GlobalReAlloc",3},
        {"user32.dll","GetClipboardSequenceNumber",0},{"user32.dll","GetOpenClipboardWindow",0},
        {"user32.dll","CountClipboardFormats",0},{"user32.dll","EnumClipboardFormats",1},{"user32.dll","GetUpdatedClipboardFormats",3},
        {"user32.dll","GetClipboardFormatNameA",3},{"user32.dll","GetClipboardFormatNameW",3},{"user32.dll","GetPriorityClipboardFormat",2},
        {"user32.dll","AddClipboardFormatListener",1},{"user32.dll","RemoveClipboardFormatListener",1},
        {"user32.dll","UserHandleGrantAccess",3},{"user32.dll","OpenClipboard",1},{"user32.dll","CloseClipboard",0},{"user32.dll","EmptyClipboard",0},
        {"user32.dll","SetClipboardData",2},{"user32.dll","GetClipboardData",1},{"user32.dll","GetClipboardOwner",0},
        {"user32.dll","IsClipboardFormatAvailable",1},{"user32.dll","RegisterClipboardFormatA",1},{"user32.dll","RegisterClipboardFormatW",1},
        {"kernel32.dll","GlobalMemoryStatus",1},{"kernel32.dll","GlobalMemoryStatusEx",1},
        {"kernel32.dll","GetThreadTimes",5},{"kernel32.dll","GetProcessTimes",5},{"kernel32.dll","QueryProcessCycleTime",2},{"user32.dll","GetCursorPos",1},{"user32.dll","SetCursorPos",2},{"user32.dll","GetCursorInfo",1},{"user32.dll","GetCursor",0},
        {"user32.dll","LoadImageA",6},{"user32.dll","LoadImageW",6},
        {"user32.dll","DisableProcessWindowsGhosting",0},
        {"user32.dll","ShutdownBlockReasonCreate",2},{"user32.dll","ShutdownBlockReasonQuery",3},{"user32.dll","ShutdownBlockReasonDestroy",1},
        {"user32.dll","InSendMessage",0},{"user32.dll","InSendMessageEx",1},{"user32.dll","ReplyMessage",1},
        {"user32.dll","GetWindowDisplayAffinity",2},{"user32.dll","SetWindowDisplayAffinity",2},
        {"user32.dll","GetClassLongA",2},{"user32.dll","GetClassLongW",2},{"user32.dll","SetClassLongA",3},{"user32.dll","SetClassLongW",3},
        {"user32.dll","GetClassLongPtrA",2},{"user32.dll","GetClassLongPtrW",2},{"user32.dll","SetClassLongPtrA",3},{"user32.dll","SetClassLongPtrW",3},
        {"user32.dll","GetClassWord",2},{"user32.dll","SetClassWord",3},
        {"user32.dll","GetClassNameA",3},{"user32.dll","GetClassNameW",3},
        {"user32.dll","SetTimer",4},{"user32.dll","SetCoalescableTimer",5},{"user32.dll","KillTimer",2},{"user32.dll","GetQueueStatus",1},{"user32.dll","MsgWaitForMultipleObjects",5},{"user32.dll","MsgWaitForMultipleObjectsEx",5},
        {"user32.dll","GetKeyboardLayout",1},{"user32.dll","GetKeyboardLayoutNameA",1},{"user32.dll","GetKeyboardLayoutNameW",1},{"user32.dll","GetKeyboardLayoutList",2},{"user32.dll","TrackMouseEvent",1},{"comctl32.dll","_TrackMouseEvent",1},{"user32.dll","GetAsyncKeyState",1},{"user32.dll","GetLastInputInfo",1},{"user32.dll","GetKeyboardState",1},{"user32.dll","SetKeyboardState",1},
        {"user32.dll","CreateCaret",4},{"user32.dll","DestroyCaret",0},{"user32.dll","ShowCaret",1},{"user32.dll","HideCaret",1},{"user32.dll","SetCaretPos",2},
        {"user32.dll","SetScrollInfo",4},{"user32.dll","GetScrollInfo",3},{"user32.dll","SetCursor",1},{"user32.dll","ShowCursor",1},
        {"user32.dll","GetCapture",0},{"user32.dll","SetCapture",1},{"user32.dll","ReleaseCapture",0},
        {"user32.dll","CreateDialogParamA",5},{"user32.dll","CreateDialogParamW",5},
        {"user32.dll","DefDlgProcA",4},{"user32.dll","DefDlgProcW",4},{"user32.dll","IsDialogMessageA",2},{"user32.dll","IsDialogMessageW",2},
        {"user32.dll","GetDlgItemInt",4},{"user32.dll","SetDlgItemInt",4},
        {"user32.dll","GetDlgCtrlID",1},{"user32.dll","GetDlgItem",2},{"user32.dll","SendDlgItemMessageA",5},{"user32.dll","SendDlgItemMessageW",5},
        {"user32.dll","EnumWindows",2},{"user32.dll","EnumChildWindows",3},{"user32.dll","EnumThreadWindows",3},
        {"user32.dll","SetDlgItemTextA",3},{"user32.dll","SetDlgItemTextW",3},{"user32.dll","GetDlgItemTextA",4},{"user32.dll","GetDlgItemTextW",4},
        {"user32.dll","CheckDlgButton",3},{"user32.dll","CheckRadioButton",4},{"user32.dll","IsDlgButtonChecked",2},
        {"user32.dll","GetWindowTextA",3},{"user32.dll","GetWindowTextW",3},{"user32.dll","GetWindowTextLengthA",1},{"user32.dll","GetWindowTextLengthW",1},
        {"user32.dll","SetWindowTextA",2},{"user32.dll","SetWindowTextW",2},
        {"user32.dll","GetWindowRect",2},{"user32.dll","GetDesktopWindow",0},
        {"user32.dll","GetShellWindow",0},{"user32.dll","SetShellWindow",1},{"user32.dll","SetShellWindowEx",2},{"user32.dll","GetWindowInfo",2},
        {"user32.dll","LogicalToPhysicalPoint",2},{"user32.dll","PhysicalToLogicalPoint",2},{"user32.dll","LogicalToPhysicalPointForPerMonitorDPI",2},{"user32.dll","PhysicalToLogicalPointForPerMonitorDPI",2},{"user32.dll","MapDialogRect",2},
        {"user32.dll","MapWindowPoints",4},{"user32.dll","MoveWindow",6},{"user32.dll","SetWindowPos",7},{"user32.dll","OffsetRect",3},{"user32.dll","ScreenToClient",2},{"user32.dll","ClientToScreen",2},
        {"user32.dll","BeginDeferWindowPos",1},{"user32.dll","DeferWindowPos",8},{"user32.dll","EndDeferWindowPos",1},
        {"user32.dll","EnableWindow",2},{"user32.dll","IsWindowEnabled",1},{"user32.dll","IsWindow",1},{"user32.dll","IsWindowVisible",1},{"user32.dll","IsIconic",1},{"user32.dll","IsZoomed",1},
        {"user32.dll","GetAncestor",2},{"user32.dll","GetParent",1},{"user32.dll","SetParent",2},{"user32.dll","IsChild",2},{"user32.dll","SetFocus",1},{"user32.dll","GetFocus",0},
        {"user32.dll","FindWindowExA",4},{"user32.dll","FindWindowExW",4},{"user32.dll","FindWindowA",2},{"user32.dll","FindWindowW",2},
        {"user32.dll","GetForegroundWindow",0},{"user32.dll","GetActiveWindow",0},{"user32.dll","GetLastActivePopup",1},{"user32.dll","SetForegroundWindow",1},{"user32.dll","SetActiveWindow",1},
        {"user32.dll","LoadIconA",2},{"user32.dll","LoadIconW",2},{"user32.dll","DestroyIcon",1},{"user32.dll","DestroyCursor",1},
        {"user32.dll","CreateIconIndirect",1},{"user32.dll","CopyIcon",1},{"user32.dll","GetIconInfo",2},
        {"user32.dll","BringWindowToTop",1},{"user32.dll","WindowFromDC",1},{"user32.dll","GetDC",1},{"user32.dll","GetWindowDC",1},{"user32.dll","ReleaseDC",2},{"user32.dll","GetSysColor",1},{"user32.dll","GetSysColorBrush",1},
        {"user32.dll","GetDoubleClickTime",0},{"user32.dll","GetCaretBlinkTime",0},{"user32.dll","GetMessageTime",0},{"user32.dll","GetMessagePos",0},{"user32.dll","GetMessageExtraInfo",0},{"user32.dll","SetMessageExtraInfo",1},
        {"user32.dll","GetWindowThreadProcessId",2},{"user32.dll","GetGuiResources",2},
        {"user32.dll","RegisterWindowMessageA",1},{"user32.dll","RegisterWindowMessageW",1},
        {"gdi32.dll","GetCharWidth32A",4},{"gdi32.dll","GetCharWidth32W",4},
        {"gdi32.dll","GetCharABCWidthsFloatA",4},{"gdi32.dll","GetCharABCWidthsFloatW",4},
        {"gdi32.dll","GetObjectA",3},{"gdi32.dll","GetObjectW",3},
        {"gdi32.dll","CreateFontA",14},{"gdi32.dll","CreateFontW",14},{"gdi32.dll","CreateFontIndirectA",1},{"gdi32.dll","CreateFontIndirectW",1},
        {"gdi32.dll","GetTextExtentPoint32A",4},{"gdi32.dll","GetTextExtentPoint32W",4},
        {"gdi32.dll","GetTextExtentPointA",4},{"gdi32.dll","GetTextExtentPointW",4},
        {"gdi32.dll","GetTextExtentExPointA",7},{"gdi32.dll","GetTextExtentExPointW",7},
        {"gdi32.dll","GetTextMetricsA",2},{"gdi32.dll","GetTextMetricsW",2},{"gdi32.dll","GetDeviceCaps",2}
    };return table;
}
namespace {
std::int32_t s32(std::uint64_t v){return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(v));}
std::u16string lower(std::u16string s){for(auto& c:s)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);return s;}
}
std::uint64_t WindowsServices::gui_resource(const GuestMemory& memory,std::uint64_t module,std::uint64_t name,std::uint64_t type,bool wide,bool first,int language) const {
    const auto* image=resource_image(module?module:runtime_.image.base);if(!image || image->size<256)return 0;
    const auto base=image->base,size=image->size;
    const auto optional=base+memory.load(GuestAddress{base+60},32)+24;
    const auto directory=memory.load(GuestAddress{optional},16)==0x10b?112U:128U;
    const auto root=base+memory.load(GuestAddress{optional+directory},32),extent=memory.load(GuestAddress{optional+directory+4},32);
    if(root<base || root-base>size || extent>size-(root-base) || extent<16)return 0;
    const auto check=[&](std::uint64_t offset,std::uint64_t length){if(offset>extent || length>extent-offset)throw HostCallFault("資源目錄超出 PE 範圍");};
    const auto match=[&](std::uint32_t entry,std::uint64_t requested){
        if(!(entry&0x80000000U))return requested<=65535 && entry==requested;
        if(requested<=65535)return false;
        const auto offset=entry&0x7fffffffU;check(offset,2);const auto length=memory.load(GuestAddress{root+offset},16);check(offset+2,length*2);std::u16string text;
        for(std::uint64_t i=0;i<length;++i)text.push_back(static_cast<char16_t>(memory.load(GuestAddress{root+offset+2+i*2},16)));
        return lower(text)==lower(wide?read_utf16(memory,GuestAddress{requested}):utf8_to_utf16_lossy(read_ansi(memory,requested)));
    };
    std::uint64_t offset=0;
    for(unsigned level=0;level<3;++level){
        check(offset,16);const auto count=memory.load(GuestAddress{root+offset+12},16)+memory.load(GuestAddress{root+offset+14},16);check(offset+16,count*8);bool found=false;
        for(std::uint64_t i=0;i<count;++i){
            const auto at=root+offset+16+i*8;const auto key=static_cast<std::uint32_t>(memory.load(GuestAddress{at},32));
            if(level<2 && !(level==1 && first) && !match(key,level==0?type:name))continue;
            if(level==2 && language>=0 && key!=static_cast<std::uint32_t>(language))continue;
            const auto next=static_cast<std::uint32_t>(memory.load(GuestAddress{at+4},32));
            if(level==2){if(next&0x80000000U)return 0;check(next,16);const auto rva=memory.load(GuestAddress{root+next},32),length=memory.load(GuestAddress{root+next+4},32);if(rva>size || length>size-rva)return 0;return root+next;}
            if(!(next&0x80000000U))return 0;offset=next&0x7fffffffU;found=true;break;
        }
        if(!found)return 0;
    }
    return 0;
}
#include "gui_window_position.inc"
std::uint64_t WindowsServices::load_gui_icon(GuestMemory& memory,std::uint64_t instance,std::uint64_t name,bool wide,std::uint32_t width,std::uint32_t height,bool intrinsic,bool first,bool cursor,std::uint32_t flags,bool prefer_larger){
    const auto fail=[&](std::uint32_t error){last_error_=error;return UINT64_C(0);};
    if(width>4096 || height>4096)return fail(87);
    try {
        if(cursor && !instance && name<=65535){
            switch(name){case 32512:case 32513:case 32514:case 32515:case 32516:case 32640:case 32641:case 32642:case 32643:case 32644:case 32645:case 32649:case 32650:case 32651:return 0xd000+name;default:return fail(1814);}
        }
        if(!cursor && !instance && name>=32512 && name<=32518){
#ifndef __APPLE__
            // 可攜 headless 核心沿用既有系統圖示識別值；實際 native 圖示解碼由 macOS adapter 提供。
            return store_gui_icon(GuiIcon{{},width?width:32,height?height:32,static_cast<std::uint32_t>(name)},flags);
#else
            static constexpr std::array<std::uint32_t,7> stock{2,80,23,78,79,2,77};auto icon=native_stock_icon(stock[static_cast<std::size_t>(name-32512)],width?width:32);if(!icon)return fail(50);icon->system=static_cast<std::uint32_t>(name);resize_icon(*icon,width?width:32,height?height:32,(flags&1)!=0);return store_gui_icon(std::move(*icon),flags);
#endif
        }
        const auto group=gui_resource(memory,instance,name,cursor?12:14,wide,first);
        if(!group){
            const auto animated=gui_resource(memory,instance,name,cursor?21:22,wide,first);if(!animated)return fail(1814);
            const auto length=memory.load(GuestAddress{animated+4},32);if(length>64*1024*1024)return fail(8);std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
            memory.read(GuestAddress{resource_image(animated,true)->base+memory.load(GuestAddress{animated},32)},bytes);
            return store_gui_icon(decode_icon_file(bytes,cursor,width,height,flags,prefer_larger),flags);
        }
        const auto size=memory.load(GuestAddress{group+4},32);if(size>65536)return fail(1814);std::vector<std::uint8_t> directory(static_cast<std::size_t>(size));
        memory.read(GuestAddress{resource_image(group,true)->base+memory.load(GuestAddress{group},32)},directory);
        auto choice=choose_icon(directory,cursor,false,width,height,(flags&1)!=0,prefer_larger);
        if(intrinsic && !width && !height){
            const auto w=cursor?directory.at(6)+(std::uint32_t{directory.at(7)}<<8):directory.at(6)?directory[6]:256U;
            const auto h=cursor?(directory.at(8)+(std::uint32_t{directory.at(9)}<<8))/2:directory.at(7)?directory[7]:256U;
            choice=choose_icon(directory,cursor,false,w,h,(flags&1)!=0);
        }
        const auto entry=gui_resource(memory,instance,choice.id,cursor?1:3,true);if(!entry)return fail(1814);
        const auto length=memory.load(GuestAddress{entry+4},32);if(length>16*1024*1024)return fail(8);std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
        memory.read(GuestAddress{resource_image(entry,true)->base+memory.load(GuestAddress{entry},32)},bytes);
        auto icon=decode_icon_resource(bytes,!cursor,width?width:intrinsic?choice.width:32,height?height:intrinsic?choice.height:32,flags);
        if(flags&0x8000U){icon.resource_instance=resource_image(group,true)->base;if(name<=65535)icon.resource_name_id=static_cast<std::uint32_t>(name);
            else icon.resource_name=wide?read_utf16(memory,GuestAddress{name}):utf8_to_utf16_lossy(read_ansi(memory,name));}
        return store_gui_icon(std::move(icon),flags);
    }catch(const std::invalid_argument&){return fail(193);}catch(const std::bad_alloc&){return fail(8);}
}
std::optional<std::uint32_t> WindowsServices::invoke_gui_support(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(service.symbol=="WinHelpA" || service.symbol=="WinHelpW")return invoke_winhelp(state,memory,service);
    if(service.symbol.starts_with("DefFrameProc") || service.symbol.starts_with("DefMDIChildProc") ||
       service.symbol.starts_with("CreateMDIWindow") || service.symbol=="TranslateMDISysAccel")return invoke_mdi(state,memory,service);
    const auto input_connected=[&](std::uint64_t target){std::set<std::uint64_t> visited{threads_.current};std::vector<std::uint64_t> pending{threads_.current};
        for(std::size_t i=0;i<pending.size();++i){if(pending[i]==target)return true;for(const auto& edge:gui_.input_attachments){const auto next=edge.first==pending[i]?edge.second:edge.second==pending[i]?edge.first:0;if(next && visited.insert(next).second)pending.push_back(next);}}return false;};

    if(service.symbol=="UserHandleGrantAccess")return invoke_job(state,memory,service);
    if(invoke_remote_gui(state,memory,service))return std::nullopt;
    if(service.family=="imm32.dll")return invoke_imm(state,memory,service);
    if(service.symbol.starts_with("PropertySheet"))return invoke_property_sheet(state,memory,service);
    if(service.symbol=="IsWindowUnicode" || service.symbol=="GetNextDlgTabItem" || service.symbol=="GetNextDlgGroupItem")return invoke_ui_helpers(state,memory,service);
    if(intercept_gui_cbt(state,memory,service))return std::nullopt;
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;const bool wide=symbol.ends_with('W');std::uint64_t result=0;
    const auto d=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    const auto read=[&](std::uint64_t p){return p?(wide?read_utf16(memory,GuestAddress{p}):utf8_to_utf16_lossy(read_ansi(memory,p))):std::u16string{};};
    const auto selected_font=[&](){const auto dc=gui_.contexts.find(arg(0));return gui_font(gui_,dc==gui_.contexts.end()?0xb011:dc->second.font);};
    const auto font_metrics=[&](const GuiFont& font){return native_gui_?native_gui_->font_metrics(font).value_or(fallback_font_metrics(font)):fallback_font_metrics(font);};
    const auto text_width=[&](const GuiFont& font,const std::u16string& text){const auto metrics=font_metrics(font);const auto fallback=static_cast<std::int32_t>(text.size())*metrics.average_width;return native_gui_?native_gui_->text_width(font,text).value_or(fallback):fallback;};
    const auto fail=[&](std::uint32_t error){last_error_=error;result=0;};
    const auto send=[&](const GuiMessage& msg)->bool{
        if(route_gui_send(state,memory,msg,wide))return true;
        if(!gui_.windows.contains(msg.window)){fail(1400);return false;}
        if(begin_gui_text_thunk(state,memory,msg,wide))return true;
        if(const auto value=default_gui_control(memory,msg,wide)){result=*value;return false;}
        const auto& w=gui_.windows.at(msg.window);const auto proc=w.procedure;
        if(proc){start_callbacks(state,memory,{{proc,{msg.window,msg.message,msg.wparam,msg.lparam}}},Continuation::ReturnCallback);return true;}
        return false;
    };
    if(symbol=="InSendMessage" || symbol=="InSendMessageEx" || symbol=="ReplyMessage"){
        auto active=gui_.sent_messages.end();std::uint64_t text_token=0;
        for(auto frame=runtime_.callbacks.rbegin();frame!=runtime_.callbacks.rend();++frame){
            auto token=frame->continuation==Continuation::GuiSentMessage?frame->value:0;
            if(frame->continuation==Continuation::GuiTextThunk){const auto text=gui_text_thunks_.find(frame->value);if(text!=gui_text_thunks_.end() && text->second.completion==Continuation::GuiSentMessage)token=text->second.value;}
            if(!token)continue;const auto sent=gui_.sent_messages.find(token);
            if(sent!=gui_.sent_messages.end() && sent->second.active && !sent->second.rpc && sent->second.receiver==threads_.current &&
               ((sent->second.sender && sent->second.sender!=threads_.current) || sent->second.done)){active=sent;if(frame->continuation==Continuation::GuiTextThunk)text_token=frame->value;break;}
        }
        const auto flags=active==gui_.sent_messages.end()?0U:(active->second.kind==GuiSentMessage::Kind::Synchronous?1U:active->second.kind==GuiSentMessage::Kind::Notify?2U:4U)|(active->second.done?8U:0U);
        if(symbol=="ReplyMessage"){
            if(active!=gui_.sent_messages.end() && active->second.kind==GuiSentMessage::Kind::Synchronous){if(!active->second.done){
                auto reply=arg(0);
                if(!text_token)for(auto frame=runtime_.callbacks.rbegin();frame!=runtime_.callbacks.rend();++frame){
                    if(frame->continuation!=Continuation::GuiTextThunk)continue;const auto text=gui_text_thunks_.find(frame->value);if(text==gui_text_thunks_.end())continue;
                    const auto& message=text->second.message;const auto& original=active->second.message;
                    if(message.window==original.window && message.message==original.message && message.lparam==original.lparam)text_token=frame->value;
                }
                if(text_token){
                    std::vector<std::uint64_t> chain{text_token};
                    for(std::size_t depth=0;depth<128;++depth){const auto& parent=gui_text_thunks_.at(chain.back());std::uint64_t child=0;if(!parent.buffer)break;
                        for(const auto& frame:runtime_.callbacks){if(frame.continuation!=Continuation::GuiTextThunk || std::find(chain.begin(),chain.end(),frame.value)!=chain.end())continue;
                            const auto text=gui_text_thunks_.find(frame.value);if(text!=gui_text_thunks_.end() && text->second.message.window==parent.message.window && text->second.message.message==parent.message.message && text->second.message.lparam==parent.buffer){child=frame.value;break;}}
                        if(!child)break;chain.push_back(child);
                    }
                    for(auto token=chain.rbegin();token!=chain.rend();++token){CallbackFrame frame;frame.value=*token;frame.continuation=Continuation::ReturnCallback;(void)resume_gui_text_thunk(state,memory,frame,reply);reply=gui_text_thunks_.at(*token).capacity;}
                }
                active->second.result=reply;active->second.done=true;
            }result=1;}
        }else if(symbol=="InSendMessage")result=flags!=0;
        else if(arg(0))fail(87);else result=flags;
    }else if(symbol=="GetGuiResources"){
        const auto flags=d(1);const auto process=arg(0);
        if(flags!=0 && flags!=1 && flags!=2 && flags!=4)fail(87);
        else if(process==(x86_?UINT32_MAX-1ULL:UINT64_MAX-1))fail(50); // 尚未共享 session 全域 GUI 資源表。
        else if(!current_process_handle(process,0x1000) && !current_process_handle(process,0x400)){
            if(current_process_handle(process,0) || process_handle_pid(process,0x1000))fail(5);else fail(6);
        }else {gui_.observe_resources();const auto index=(flags==1 || flags==4)?1U:0U;result=std::min<std::uint64_t>(UINT32_MAX,flags>=2?gui_.resource_peaks[index]:gui_.resource_counts()[index]);last_error_=0;}
    }else if(service.family=="dwmapi.dll"){
        // 本後端提供一般視窗，尚無 DWM redirection／玻璃合成能力；維持一致的停用狀態。
        if(symbol=="DwmIsCompositionEnabled"){
            if(!arg(0))result=0x80070057U;else memory.store(GuestAddress{arg(0)},32,0);
        } else if(symbol=="DwmDefWindowProc")result=0; // FALSE：交回客體一般視窗程序處理。
        else if(symbol=="DwmEnableComposition")result=d(0)>1?0x80070057U:d(0)?0x80263001U:0U;
        else result=0x80263001U; // DWM_E_COMPOSITIONDISABLED；不回填不存在的合成資訊。
    }
    else if(symbol.starts_with("FindWindow")){
        const bool extended=symbol.starts_with("FindWindowEx");
        const auto parent=extended && arg(0)!=gui_.desktop?arg(0):0,after=extended?arg(1):0;
        const auto cls=arg(extended?2:0),caption=arg(extended?3:1);
        const auto class_name=cls>65535?lower(read(cls)):std::u16string{};
        const auto title=caption?lower(read(caption)):std::u16string{};
        if((parent && !gui_.windows.contains(parent)) || (after && (!gui_.windows.contains(after) || gui_.windows.at(after).parent!=parent)))fail(1400);
        else {bool seen=!after;
            for(const auto handle:gui_.z_order){
                const auto found=gui_.windows.find(handle);if(found==gui_.windows.end())continue;const auto& w=found->second;
                if(!seen){if(handle==after)seen=true;continue;}
                if(w.desktop_object!=thread_desktop_object(threads_.current) || w.parent!=parent || (cls && (cls<=65535?w.class_atom!=cls:lower(w.class_name)!=class_name)) || (caption && lower(w.title)!=title))continue;
                result=handle;break;
            }
        }
    }
    else if(symbol=="GetClassNameA" || symbol=="GetClassNameW"){
        const auto found=gui_.windows.find(arg(0));
        if(found==gui_.windows.end())fail(1400);
        else if(s32(arg(2))<=0)fail(87);
        else {
            auto name=found->second.class_name;
            if(!found->second.class_atom || gui_.system_classes.contains(found->second.class_name)){
                for(const auto& [key,value]:std::array<std::pair<std::u16string_view,std::u16string_view>,7>{{
                    {u"button",u"Button"},{u"static",u"Static"},{u"edit",u"Edit"},{u"listbox",u"ListBox"},
                    {u"combobox",u"ComboBox"},{u"combolbox",u"ComboLBox"},{u"scrollbar",u"ScrollBar"}}})if(name==key){name=value;break;}
            }
            if(wide){result=std::min<std::uint64_t>(name.size(),d(2)-1U);memory.write(GuestAddress{arg(1)},utf16_bytes(name.substr(0,result)));}
            else {const auto text=utf16_to_utf8(name);result=std::min<std::uint64_t>(text.size(),d(2)-1U);std::vector<std::uint8_t> bytes(text.begin(),text.begin()+static_cast<std::ptrdiff_t>(result));bytes.push_back(0);memory.write(GuestAddress{arg(1)},bytes);}
        }
    }
    else if(symbol.starts_with("GetClassLong") || symbol.starts_with("SetClassLong") || symbol=="GetClassWord" || symbol=="SetClassWord" || symbol.starts_with("GetClassInfo") || symbol.starts_with("UnregisterClass")){
#include "windows_class_properties.inc"
    }
    else if(symbol=="InitializeSListHead" || symbol=="InterlockedFlushSList" || symbol=="InterlockedPushEntrySList" || symbol=="InterlockedPopEntrySList" || symbol=="QueryDepthSList") {
        const auto header=arg(0);
        if(symbol=="InitializeSListHead") {slist::check(memory,header,x86_,Access::Write);memory.write(GuestAddress{header},std::vector<std::uint8_t>(slist::size(x86_)));}
        else if(symbol=="QueryDepthSList")result=slist::depth(memory,header,x86_);
        else if(symbol=="InterlockedFlushSList")result=slist::flush(memory,header,x86_);
        else if(symbol=="InterlockedPushEntrySList")result=slist::push(memory,header,x86_,arg(1));
        else result=slist::pop(memory,header,x86_);
    }
    else if(symbol=="MulDiv"){
        const auto a=static_cast<std::int64_t>(s32(arg(0))),b=static_cast<std::int64_t>(s32(arg(1))),c=static_cast<std::int64_t>(s32(arg(2)));result=UINT64_MAX;
        if(c){const auto product=a*b;const auto negative=(product<0)!=(c<0);const auto magnitude=(product<0?-product:product)+(c<0?-c:c)/2;const auto value=magnitude/(c<0?-c:c)*(negative?-1:1);if(value>=INT32_MIN && value<=INT32_MAX)result=static_cast<std::uint32_t>(value);}
    } else if(symbol=="FindResourceExW" || symbol=="FindResourceExA"){
        // FindResourceEx(hModule, type, name, language)：LANG_NEUTRAL 取第一個語系，其他語系須完全符合。
        const auto language=static_cast<std::uint16_t>(arg(3));
        result=gui_resource(memory,arg(0),arg(2),arg(1),wide,false,language?static_cast<int>(language):-1);
        if(!result)last_error_=language && gui_resource(memory,arg(0),arg(2),arg(1),wide)?1815:1814;
    } else if(symbol.starts_with("FindResource")){result=gui_resource(memory,arg(0),arg(1),arg(2),wide);if(!result)last_error_=1814;}
    else if(symbol=="LoadResource" || symbol=="SizeofResource"){
        const auto* image=resource_image(arg(0)?arg(0):runtime_.image.base);
        if(!image || arg(1)<image->base || arg(1)-image->base>image->size || image->size-(arg(1)-image->base)<16)fail(6);
        else result=symbol=="LoadResource"?image->base+memory.load(GuestAddress{arg(1)},32):memory.load(GuestAddress{arg(1)+4},32);
    } else if(symbol=="LockResource")result=arg(0);
    else if(symbol=="CoInitialize" || symbol=="CoInitializeEx"){
        if(begin_initialize_spies(state,memory,service))return std::nullopt;
        const auto mode=symbol=="CoInitialize"?2U:d(1);
        result=arg(0)?0x80070057U:initialize_com_apartment(mode);
    } else if(symbol=="CoUninitialize"){
        if(begin_initialize_spies(state,memory,service))return std::nullopt;
        const auto leaving=com_apartment(threads_.current);
        auto& com=gui_.com_initializations[threads_.current];if(com>=16){com-=16;if(com<16){
            auto callbacks=retire_apartment_shutdowns(memory,threads_.current,leaving);
            {auto classes=revoke_com_classes(memory,threads_.current);callbacks.insert(callbacks.end(),classes.begin(),classes.end());}
            auto clipboard=retire_ole_clipboard(state,memory,threads_.current);callbacks.insert(callbacks.end(),clipboard.begin(),clipboard.end());
            auto context=retire_com_context(state,memory,threads_.current);callbacks.insert(callbacks.end(),context.begin(),context.end());
            auto marshaling=retire_com_marshal(memory,threads_.current);callbacks.insert(callbacks.end(),marshaling.begin(),marshaling.end());
            auto proxies=retire_com_proxy_thread(state,memory,threads_.current,true);callbacks.insert(callbacks.end(),proxies.begin(),proxies.end());
            com=0;if(gui_.com_main_sta==threads_.current)gui_.com_main_sta=0;
            auto filters=revoke_com_message_filter(memory,threads_.current);callbacks.insert(callbacks.end(),filters.begin(),filters.end());
            auto errors=retire_com_error(state,memory,threads_.current);callbacks.insert(callbacks.end(),errors.begin(),errors.end());
            if(!callbacks.empty()){start_callbacks(state,memory,std::move(callbacks),Continuation::Return,0);return std::nullopt;}
        }}
    }
    else if(symbol=="CoCreateInstance" || symbol=="CoGetClassObject"){if(begin_registered_com(state,memory,service))return std::nullopt;if(begin_msxml_com(state,memory,service))return std::nullopt;if(begin_ribbon_com(state,memory,service))return std::nullopt;if(dmusic::is_class(memory,arg(0)))return invoke_directmusic(state,memory,service);if(dsound::is_class(memory,arg(0)))return invoke_dsound(state,memory,service);if(wasapi::is_class(memory,arg(0)))return invoke_wasapi(state,memory,service);if(dinput::is_class(memory,arg(0)))return invoke_dinput(state,memory,service);if(symbol=="CoGetClassObject"){memory.store(GuestAddress{arg(4)},x86_?32:64,0);arg.return_to(state,0x80040154U);return std::nullopt;}return invoke_wmi(state,memory,service);}
    else if(symbol.starts_with("InitCommonControls")){if(symbol.ends_with("Ex")){if(memory.load(GuestAddress{arg(0)},32)!=8)fail(87);else result=1;}}
    else if(symbol=="DisableProcessWindowsGhosting"){
        // 此 void API 停用的是 Windows 的替身視窗；AppKit 後端本來不建立替身。
        // 保留不可逆的程序政策，既有及後續 HWND 都繼續使用客體自己的視窗。
        gui_.ghosting_disabled=true;
    }
    else if(symbol=="ShutdownBlockReasonCreate" || symbol=="ShutdownBlockReasonQuery" || symbol=="ShutdownBlockReasonDestroy"){
        const auto found=gui_.windows.find(arg(0));
        if(found==gui_.windows.end())fail(shared_gui_window(arg(0))?5:1400);
        else if(found->second.thread!=threads_.current)fail(5);
        else if(symbol=="ShutdownBlockReasonCreate"){
            if(!arg(1))fail(87);
            else {found->second.shutdown_reason=read_utf16(memory,GuestAddress{arg(1)});result=1;}
        }else if(symbol=="ShutdownBlockReasonDestroy"){
            found->second.shutdown_reason.reset();result=1;
        }else if(!arg(2))fail(87);
        else if(!found->second.shutdown_reason)fail(1168);
        else {
            const auto& reason=*found->second.shutdown_reason;
            const auto capacity=memory.load(GuestAddress{arg(2)},32);
            memory.check(GuestAddress{arg(2)},4,Access::Write);
            if(!arg(1)){memory.store(GuestAddress{arg(2)},32,reason.size()+1);result=1;}
            else if(capacity<=reason.size()){memory.store(GuestAddress{arg(2)},32,reason.size());fail(122);}
            else {
                memory.check(GuestAddress{arg(1)},(reason.size()+1)*2,Access::Write);
                for(std::size_t i=0;i<reason.size();++i)memory.store(GuestAddress{arg(1)+2*i},16,reason[i]);
                memory.store(GuestAddress{arg(1)+2*reason.size()},16,0);
                memory.store(GuestAddress{arg(2)},32,reason.size()+1);result=1;
            }
        }
    }
    else if(symbol=="GetWindowDisplayAffinity" || symbol=="SetWindowDisplayAffinity"){
        const auto found=gui_.windows.find(arg(0));
        if(found==gui_.windows.end())fail(1400);
        else if(found->second.style&0x40000000U)fail(87);
        else if(symbol=="GetWindowDisplayAffinity"){
            if(!arg(1))fail(87);else {memory.store(GuestAddress{arg(1)},32,found->second.display_affinity);result=1;}
        }else if(d(1)!=0 && d(1)!=1 && d(1)!=0x11)fail(87);
        else {found->second.display_affinity=d(1);present_gui(arg(0));result=1;}
    }
    else if(symbol.starts_with("LoadImage") || symbol.starts_with("LoadIcon")){
        return invoke_icon_api(state,memory,service);
    }
    else if(symbol=="RegisterTouchWindow" || symbol=="UnregisterTouchWindow" || symbol=="IsTouchWindow"){
        const auto found=gui_.windows.find(arg(0));
        if(found==gui_.windows.end())fail(1400);
        else if(symbol=="IsTouchWindow"){
            result=found->second.touch_flags.has_value();
            if(arg(1))memory.store(GuestAddress{arg(1)},32,found->second.touch_flags.value_or(0));
        } else if(found->second.thread!=threads_.current)fail(5);
        else if(symbol=="RegisterTouchWindow"){
            if(d(1)&~3U)fail(87);else {found->second.touch_flags=d(1);result=1;}
        } else {found->second.touch_flags.reset();result=1;}
    }else if(symbol=="GetTouchInputInfo" || symbol=="CloseTouchInputHandle"){
        // 沒有實體觸控來源便不產生 WM_TOUCH 或 HTOUCHINPUT；任意值不是有效封包。
        fail(6);
    }else if(symbol=="SetTimer" || symbol=="SetCoalescableTimer"){
        const auto elapsed=std::clamp(d(2),10U,0x7fffffffU);
        const auto tolerance=symbol=="SetCoalescableTimer"?d(4):0U;
        if(tolerance!=UINT32_MAX && tolerance>0x7ffffff5U)fail(87);
        else if(tolerance!=UINT32_MAX && elapsed>0x7fffffffU-tolerance){
            auto caller=state;arg.return_to(caller,0);begin_exception(caller,memory,0xc000000dU,0,{});state=caller;return std::nullopt;
        }
        else if(arg(0) && !gui_.windows.contains(arg(0)))fail(1400);
        else if(arg(0) && gui_.windows.at(arg(0)).thread!=threads_.current)fail(5);
        else {
            auto id=arg(1);const auto existing=gui_.timers.find({arg(0),id});
            // 有 HWND 的 ID 屬於該視窗，包含 ID 0；只有無 HWND 的新 timer 才配置 ID。
            if(!arg(0) && (!id || existing==gui_.timers.end() || existing->second.thread!=threads_.current))id=gui_.next_object++;
            const auto interval=static_cast<std::uint64_t>(elapsed)*10000;
            const auto slack=tolerance==UINT32_MAX?0:static_cast<std::uint64_t>(tolerance)*10000;
            const auto owner=arg(0)?gui_.windows.at(arg(0)).thread:threads_.current;
            const auto deadline=gui_.timer_deadline(runtime_.clock_ticks+interval,slack,{arg(0),id});
            gui_.timers[{arg(0),id}]={deadline,interval,arg(3),owner,slack};
            result=id?id:1;gui_.message_threads.insert(owner);
        }
    }else if(symbol=="KillTimer"){
        const auto timer=gui_.timers.find({arg(0),arg(1)});
        if(timer!=gui_.timers.end() && (arg(0) || timer->second.thread==threads_.current)){gui_.timers.erase(timer);result=1;}
    }
    else if(symbol=="GetWindowThreadProcessId"){
        const auto window=gui_.windows.find(arg(0));if(window==gui_.windows.end())fail(1400);else {if(arg(1))memory.store(GuestAddress{arg(1)},32,process_id());result=thread_identifier(window->second.thread);}
    }
    else if(symbol=="GetQueueStatus"){
        pump_gui(memory,false);std::uint32_t bits=pending_gui_send(threads_.current)?0x40U:0,changed=bits;
        for(auto& message:gui_.queue)if(gui_message_matches(message,threads_.current)){const auto category=gui_queue_category(message);bits|=category;if(!message.queue_seen)changed|=category;if(category&d(0))message.queue_seen=true;}
        result=(static_cast<std::uint64_t>(bits&d(0))<<16)|(changed&d(0));
    }
    else if(symbol=="MsgWaitForMultipleObjects" || symbol=="MsgWaitForMultipleObjectsEx"){
        const bool extended=symbol.ends_with("Ex");const auto count=d(0),flags=extended?d(4):(d(2)?1U:0U),timeout=d(extended?2:3),mask=d(extended?3:4);
        if(count>63 || (count && !arg(1)) || (flags&~7U)){fail(87);result=UINT32_MAX;}
        else {
            const auto key=std::pair{threads_.current,state.reg(Gpr::Rsp)};
            const auto deadline=gui_.message_wait_deadlines.try_emplace(key,timeout==UINT32_MAX?UINT64_MAX:runtime_.clock_ticks+static_cast<std::uint64_t>(timeout)*10000).first->second;result=258;
            if(dispatch_gui_send(state,memory,0,false))return std::nullopt;
            pump_gui(memory,false);bool ready=false,invalid=false;std::vector<std::uint64_t> handles;std::vector<bool> signaled;std::unique_ptr<SharedEventLock> shared_lock;
            for(unsigned i=0;i<count;++i){
                const auto h=memory.load(GuestAddress{arg(1)+i*(x86_?4:8)},x86_?32:64);handles.push_back(h);const auto f=runtime_.wait_objects.find(h);const auto thread=thread_id(h);const auto process=process_signaled(h);
                if(f==runtime_.wait_objects.end() && !thread && !process.has_value()){last_error_=6;invalid=true;break;}
                if((f!=runtime_.wait_objects.end() && !(f->second.access&0x100000)) || ((thread || process_object_type(h)==u"Thread") && !(thread_access(h)&0x100000))){last_error_=5;invalid=true;break;}
                if(!shared_lock && f!=runtime_.wait_objects.end() && runtime_.wait_lock_descriptor(f->second)>=0)shared_lock=std::make_unique<SharedEventLock>(runtime_.wait_lock_descriptor(f->second));
                signaled.push_back(f!=runtime_.wait_objects.end()?runtime_.wait_ready(f->second,handles,false,threads_.current):thread?threads_.items.at(thread).status==ThreadStatus::Exited:*process);
            }
            if(!invalid && (flags&1U)){for(std::size_t i=0;i<handles.size();++i){const auto object=runtime_.wait_objects.find(handles[i]);if(object!=runtime_.wait_objects.end())signaled[i]=runtime_.wait_ready(object->second,handles,true,threads_.current);}}
            const bool input=std::any_of(gui_.queue.begin(),gui_.queue.end(),[&](const auto& message){return gui_message_matches(message,threads_.current) && (gui_queue_category(message)&mask) && ((flags&4) || !message.queue_seen);});
            if(invalid){result=UINT32_MAX;ready=true;}
            else if((flags&2) && !threads_.items.at(threads_.current).apcs.empty()){
                shared_lock.reset();gui_.message_wait_deadlines.erase(key);threads_.items.at(threads_.current).wait={};
                (void)dispatch_user_apc(state,memory,0xc0);return std::nullopt;
            }
            else if(flags&1){if(input && std::all_of(signaled.begin(),signaled.end(),[](bool value){return value;})){result=0;ready=true;for(const auto h:handles){const auto f=runtime_.wait_objects.find(h);if(f!=runtime_.wait_objects.end() && runtime_.acquire_wait_object(f->second,threads_.current))result=0x80;}}}
            else {for(unsigned i=0;i<count;++i)if(signaled[i]){result=i;ready=true;const auto f=runtime_.wait_objects.find(handles[i]);if(f!=runtime_.wait_objects.end() && runtime_.acquire_wait_object(f->second,threads_.current))result=0x80U+i;break;}if(!ready && input){result=count;ready=true;}}
            shared_lock.reset();
            if(!ready && runtime_.clock_ticks<deadline){ThreadWait wait;wait.handles=std::move(handles);wait.deadline=deadline;wait.all=flags&1;wait.alertable=(flags&2)!=0;wait.gui_mask=mask;wait.gui_new_only=!(flags&4);wait.gui_callbacks=false;wait_gui_input(state,memory,std::move(wait));return std::nullopt;}
            gui_.message_wait_deadlines.erase(key);
        }
    }
    else if(symbol=="GetKeyboardLayout"){
        // ActivateKeyboardLayout 選定的 HKL 優先；其次為宿主目前輸入法。
        const auto thread=d(0)?thread_from_identifier(d(0)):threads_.current;
        if(const auto selected=thread?selected_keyboard_layout(thread):0)result=selected;
        else {result=0x04090409;if(native_gui_)for(const auto& method:native_input_methods())if(method.current){result=method.layout;break;}}
    }
    else if(symbol=="GetKeyboardLayoutNameA" || symbol=="GetKeyboardLayoutNameW"){
        if(!arg(0))fail(87);
        else {std::uint32_t layout=0x409;if(native_gui_)for(const auto& method:native_input_methods())if(method.current){layout=method.layout;break;}
            if(const auto selected=selected_keyboard_layout(threads_.current))layout=static_cast<std::uint32_t>(((selected>>16)&0xffffU)==(selected&0xffffU)?selected&0xffffU:selected);
            std::u16string name(8,u'0');for(unsigned i=0;i<8;++i)name[7-i]=u"0123456789ABCDEF"[(layout>>(i*4))&15];
            if(wide)memory.write(GuestAddress{arg(0)},utf16_bytes(name));else {const auto bytes=utf16_to_utf8(name);std::vector<std::uint8_t> value(bytes.begin(),bytes.end());value.push_back(0);memory.write(GuestAddress{arg(0)},value);}result=1;}
    }else if(symbol=="GetKeyboardLayoutList"){
        if(s32(arg(0))<0 || (d(0) && !arg(1)))fail(87);
        else {std::vector<std::uint32_t> layouts{0x04090409};if(native_gui_)for(const auto& method:native_input_methods())layouts.push_back(method.layout);
            if(!d(0))result=layouts.size();else {result=std::min<std::size_t>(layouts.size(),d(0));memory.check(GuestAddress{arg(1)},result*(x86_?4:8),Access::Write);for(unsigned i=0;i<result;++i)memory.store(GuestAddress{arg(1)+i*(x86_?4:8)},x86_?32:64,layouts[i]);}}
    }
    else if(symbol=="GetKeyboardState"){memory.write(GuestAddress{arg(0)},gui_.keyboard);result=1;}
    else if(symbol=="SetKeyboardState"){memory.read(GuestAddress{arg(0)},gui_.keyboard);for(unsigned i=0;i<256;++i)gui_.input_keyboard[i]=static_cast<std::uint8_t>((gui_.input_keyboard[i]&0x80U)|(gui_.keyboard[i]&1U));result=1;}
    else if(symbol=="CreateCaret"){gui_.caret_window=arg(0);gui_.caret_bitmap=arg(1);gui_.caret_visible=false;result=gui_.windows.contains(arg(0))?1:0;}
    else if(symbol=="DestroyCaret"){gui_.caret_window=0;gui_.caret_visible=false;result=1;}
    else if(symbol=="ShowCaret" || symbol=="HideCaret"){if(!arg(0) || arg(0)==gui_.caret_window){gui_.caret_visible=symbol=="ShowCaret";result=1;}}
    else if(symbol=="SetCaretPos"){gui_.caret_x=s32(arg(0));gui_.caret_y=s32(arg(1));result=gui_.caret_window?1:0;}
    else if(symbol=="GetCursor")result=gui_.cursor;
    else if(symbol=="ShowCursor" || symbol=="SetCursor"){
        const bool set=symbol=="SetCursor";const auto handle=set?arg(0):gui_.cursor;GuiIcon icon;const GuiIcon* selected=nullptr;bool valid=true;
        if(handle){const auto found=gui_.icons.find(handle);if(found!=gui_.icons.end() && !found->second.is_icon)selected=&found->second;
            else if(handle>=0xd000 && handle<0x1d000){icon.is_icon=false;icon.system=static_cast<std::uint32_t>(handle-0xd000);switch(icon.system){case 32512:case 32513:case 32514:case 32515:case 32516:case 32640:case 32641:case 32642:case 32643:case 32644:case 32645:case 32649:case 32650:case 32651:selected=&icon;break;default:valid=false;break;}}
            else valid=false;
        }
        const bool changed=set && handle!=gui_.cursor;const auto start=changed?runtime_.clock_ticks:gui_.cursor_animation_start;
        const auto step=selected?icon_animation_step(*selected,runtime_.clock_ticks>=start?runtime_.clock_ticks-start:0):0;
        if(selected && !selected->animation_sequence.empty())selected=&selected->animation_frames.at(selected->animation_sequence[step]);
        const auto count=set?gui_.cursor_count:static_cast<std::int32_t>(std::clamp<std::int64_t>(static_cast<std::int64_t>(gui_.cursor_count)+(d(0)?1:-1),INT32_MIN,INT32_MAX));
        if(!valid)fail(1402);
        else if(native_gui_ && interactive_station() && !native_gui_->set_cursor(selected,count>=0 && handle!=0))fail(50);
        else {gui_.cursor_animation_start=start;gui_.cursor_animation_step=step;if(set){result=gui_.cursor;gui_.cursor=handle;}else{gui_.cursor_count=count;result=static_cast<std::uint64_t>(static_cast<std::int64_t>(count));}}
    }
    else if(symbol=="SetScrollInfo" || symbol=="GetScrollInfo" || symbol=="GetScrollPos"){
        if(!gui_.windows.contains(arg(0)))fail(1400);else if(d(1)>2)fail(87);
        else {auto& bytes=gui_.scroll_info[{arg(0),d(1)}];const auto load=[&](unsigned at){std::uint32_t value=0;for(unsigned i=0;i<4;++i)value|=std::uint32_t{bytes[at+i]}<<(8*i);return value;};
            const auto store=[&](unsigned at,std::uint32_t value){for(unsigned i=0;i<4;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(8*i));};
            if(!load(0)){store(0,28);store(12,100);}
            if(symbol=="GetScrollPos")result=load(20);
            else {const auto p=arg(2);if(!p || memory.load(GuestAddress{p},32)!=28)fail(87);else {const auto mask=static_cast<std::uint32_t>(memory.load(GuestAddress{p+4},32));
                if(mask&~0x1fU)fail(87);
                else if(symbol=="SetScrollInfo"){
                    if(mask&1){store(8,static_cast<std::uint32_t>(memory.load(GuestAddress{p+8},32)));store(12,static_cast<std::uint32_t>(memory.load(GuestAddress{p+12},32)));}
                    if(mask&2)store(16,static_cast<std::uint32_t>(memory.load(GuestAddress{p+16},32)));if(mask&4)store(20,static_cast<std::uint32_t>(memory.load(GuestAddress{p+20},32)));
                    const auto low=static_cast<std::int32_t>(load(8)),high=std::max(low,static_cast<std::int32_t>(load(12)));store(12,static_cast<std::uint32_t>(high));
                    const auto page=std::min<std::uint64_t>(load(16),static_cast<std::uint64_t>(static_cast<std::int64_t>(high)-low+1));store(16,static_cast<std::uint32_t>(page));
                    const auto upper=static_cast<std::int64_t>(high)-static_cast<std::int64_t>(page?page-1:0);store(20,static_cast<std::uint32_t>(std::clamp<std::int64_t>(static_cast<std::int32_t>(load(20)),low,upper)));result=load(20);
                }else {memory.check(GuestAddress{p},28,Access::Write);for(const auto pair:std::array<std::pair<unsigned,unsigned>,5>{{{1,8},{1,12},{2,16},{4,20},{16,24}}})if(mask&pair.first)memory.store(GuestAddress{p+pair.second},32,load(pair.second));result=1;}
            }}
        }
    }
    else if(symbol=="GetCapture" || symbol=="SetCapture" || symbol=="ReleaseCapture"){
        const auto previous=gui_.capture;const auto owner=gui_.windows.find(previous);
        const bool own=owner!=gui_.windows.end() && owner->second.thread==threads_.current;
        if(symbol=="GetCapture")result=own?previous:0;
        else {
            const auto next=symbol=="ReleaseCapture"?0:arg(0);const auto target=gui_.windows.find(next);
            if(next && (target==gui_.windows.end() || target->second.thread!=threads_.current))fail(target==gui_.windows.end()?1400:5);
            else {
                result=symbol=="ReleaseCapture"?1:previous;
                // NULL 只釋放目前執行緒持有的 capture，不奪走其他執行緒的 capture。
                if((next || own) && previous!=next){
                    gui_.capture=next;++gui_.capture_generation;
                    if(owner!=gui_.windows.end()){
                        const auto notify=resolve("user32.dll",own?"SendMessageW":"SendNotifyMessageW").value;
                        start_callbacks(state,memory,{{notify,{previous,0x215,0,next}}},Continuation::Return,result);return std::nullopt;
                    }
                }
            }
        }
    }
    else if(symbol=="EndDialog"){const auto f=gui_.windows.find(arg(0));if(f!=gui_.windows.end() && f->second.modal){f->second.ended=true;f->second.dialog_result=arg(1);result=1;}else fail(1400);}
    else if(symbol.starts_with("MessageBox")){
        auto owner=arg(0),body=arg(1),caption=arg(2);auto style=d(3);
        if(symbol.starts_with("MessageBoxIndirect")){
            const auto p=arg(0);if(!p || memory.load(GuestAddress{p},32)!=(x86_?40:80)){fail(87);arg.return_to(state,0);return std::nullopt;}
            memory.check(GuestAddress{p},x86_?40:80,Access::Read);const auto bits=static_cast<std::uint8_t>(x86_?32:64);owner=memory.load(GuestAddress{p+(x86_?4:8)},bits);body=memory.load(GuestAddress{p+(x86_?12:24)},bits);caption=memory.load(GuestAddress{p+(x86_?16:32)},bits);style=static_cast<std::uint32_t>(memory.load(GuestAddress{p+(x86_?20:40)},32));
            // Help callback 與自訂 icon 尚未接入；回報失敗，不誤讀未啟用欄位。
            if(style&(0x4000U|0x80U)){fail(50);arg.return_to(state,0);return std::nullopt;}
        }
        GuiWindow w;w.dpi_context=effective_dpi_context();w.thread=threads_.current;w.title=read(caption);w.class_name=u"#32770";w.dialog=true;w.modal=true;w.builtin=true;w.owner=owner;w.width=500;w.height=190;w.style=0x10000000;w.visible=true;w.extra.resize(40);
        w.procedure=resolve("user32.dll",wide?"DefDlgProcW":"DefDlgProcA").value;const auto h=allocate_gui_window();
        std::vector<GuiWindowCreation> plans{{h,std::move(w),wide}};
        const auto add=[&](GuiWindow child){child.thread=threads_.current;child.parent=h;child.style|=0x50000000;child.visible=true;child.builtin=true;child.control_class=child.class_name;child.procedure=stock_gui_proc(child.class_name,wide);plans.push_back({allocate_gui_window(),std::move(child),wide});};
        GuiWindow label;label.dpi_context=effective_dpi_context();label.class_name=u"static";label.title=read(body);label.x=20;label.y=20;label.width=460;label.height=110;add(label);
        std::vector<std::pair<unsigned,std::u16string>> buttons;switch(style&15){case 0:buttons={{1,u"OK"}};break;case 1:buttons={{1,u"OK"},{2,u"Cancel"}};break;case 2:buttons={{3,u"Abort"},{4,u"Retry"},{5,u"Ignore"}};break;case 3:buttons={{6,u"Yes"},{7,u"No"},{2,u"Cancel"}};break;case 4:buttons={{6,u"Yes"},{7,u"No"}};break;case 5:buttons={{4,u"Retry"},{2,u"Cancel"}};break;case 6:buttons={{2,u"Cancel"},{10,u"Try Again"},{11,u"Continue"}};break;default:throw HostCallFault("MessageBox 按鈕種類尚未支援");}
        for(std::size_t i=0;i<buttons.size();++i){GuiWindow b;b.dpi_context=effective_dpi_context();b.class_name=u"button";b.id=buttons[i].first;b.title=buttons[i].second;b.x=static_cast<std::int32_t>(480-buttons.size()*100+i*100);b.y=145;b.width=90;b.height=28;b.style=i==((style>>8)&3)?1:0;add(b);}
        return start_gui_creation(state,memory,std::move(plans));
    }
    else if(symbol.starts_with("CreateDialog") || symbol.starts_with("DialogBox") || symbol.starts_with("DefDlgProc"))return invoke_gui_dialog(state,memory,service);
    else if(symbol.starts_with("IsDialogMessage"))return invoke_gui_dialog(state,memory,service);
    else if(symbol=="GetDlgCtrlID"){
        const auto found=gui_.windows.find(arg(0));
        if(found==gui_.windows.end())last_error_=1400;
        else result=static_cast<std::uint32_t>(found->second.id);
    }
    else if(symbol=="EnumWindows" || symbol=="EnumChildWindows" || symbol=="EnumThreadWindows"){
        const bool children=symbol=="EnumChildWindows",thread_only=symbol=="EnumThreadWindows",offset=children || thread_only;const auto callback=arg(offset?1:0),parameter=arg(offset?2:1);
        const auto selected_thread=thread_only?thread_from_identifier(d(0)):threads_.current;
        auto parent=children?arg(0):0;if(parent==gui_.desktop)parent=0;
        if(!callback)fail(87);else if(parent && !gui_.windows.contains(parent))fail(1400);else if(thread_only && !selected_thread)fail(87);
        else{
            auto order=gui_.z_order;for(const auto& [handle,window]:gui_.windows){(void)window;if(std::find(order.begin(),order.end(),handle)==order.end())order.push_back(handle);}
            std::vector<GuestCallback> callbacks;std::set<std::uint64_t> visited;
            const auto collect=[&](auto&& recurse,std::uint64_t root)->void{
                for(const auto handle:order){const auto found=gui_.windows.find(handle);if(found==gui_.windows.end() || handle==gui_.desktop || found->second.parent!=root || !visited.insert(handle).second)continue;
                    if(found->second.desktop_object!=thread_desktop_object(selected_thread))continue;
                    if(thread_only && found->second.thread!=selected_thread)continue;callbacks.push_back({callback,{handle,parameter}});if(children && parent)recurse(recurse,handle);
                }
            };
            collect(collect,parent);if(callbacks.empty())result=1;else{start_callbacks(state,memory,std::move(callbacks),Continuation::WindowEnumeration);return std::nullopt;}
        }
    }
    else if(symbol=="GetDlgItemInt" || symbol=="SetDlgItemInt"){
        const bool get=symbol=="GetDlgItemInt";if(get && arg(2))memory.store(GuestAddress{arg(2)},32,0);
        const auto child=gui_child(arg(0),d(1));if(!child)fail(1400);
        else{
            const auto text=get?std::u16string{}:utf8_to_utf16(d(3)?std::to_string(static_cast<std::int32_t>(d(2))):std::to_string(d(2)));
            const auto scratch=allocate_heap(memory,get?64:(text.size()+1)*2);
            if(!scratch)fail(8);else{
                if(!get)memory.write(GuestAddress{scratch},utf16_bytes(text));
                start_callbacks(state,memory,{{resolve("user32.dll","SendMessageW").value,{child,get?13U:12U,get?32U:0U,scratch}}},get?Continuation::GuiIntegerText:Continuation::ReturnCallbackFreeHeap,scratch);return std::nullopt;
            }
        }
    }
    else if(symbol=="GetDlgItem")result=gui_child(arg(0),d(1));
    else if(symbol.starts_with("SendDlgItemMessage")){if(send({gui_child(arg(0),d(1)),arg(3),arg(4),d(2)}))return std::nullopt;}
    else if(symbol.starts_with("SetDlgItemText") || symbol.starts_with("GetDlgItemText") || symbol.starts_with("SetWindowText") || symbol.starts_with("GetWindowText")){
        const bool dlg=symbol.find("DlgItem")!=std::string_view::npos,set=symbol.starts_with("Set"),length=symbol.find("Length")!=std::string_view::npos;
        const auto h=dlg?gui_child(arg(0),d(1)):arg(0);const auto f=gui_.windows.find(h);if(f==gui_.windows.end())fail(1400);
        else if(f->second.procedure){
            const GuiMessage message{h,set || length?0:d(dlg?3:2),length?0:arg(dlg?2:1),set?12U:length?14U:13U};
            if(send(message))return std::nullopt;
        }
        else if(set){f->second.title=read(arg(dlg?2:1));present_gui(h);result=1;}
        else {
            const auto& value=f->second.title;const auto text=utf16_to_utf8(value);
            if(length)result=wide?value.size():text.size();
            else {const auto capacity=d(dlg?3:2);const auto pointer=arg(dlg?2:1);if(capacity){result=std::min<std::uint64_t>(wide?value.size():text.size(),capacity-1);if(wide)memory.write(GuestAddress{pointer},utf16_bytes(value.substr(0,result)));else {std::vector<std::uint8_t> bytes(text.begin(),text.begin()+static_cast<std::ptrdiff_t>(result));bytes.push_back(0);memory.write(GuestAddress{pointer},bytes);}}}
        }
    } else if(symbol=="CheckRadioButton"){
        for(auto& [h,w]:gui_.windows)if(w.parent==arg(0) && w.id>=d(1) && w.id<=d(2)){w.check=w.id==d(3)?1:0;present_gui(h);}result=1;
    } else if(symbol=="CheckDlgButton" || symbol=="IsDlgButtonChecked"){
        const auto h=gui_child(arg(0),d(1));if(h){auto& w=gui_.windows.at(h);if(symbol=="CheckDlgButton"){w.check=d(2);present_gui(h);result=1;}else result=w.check;}
    } else if(symbol=="SetParent" || symbol=="IsChild"){
        const auto message_parent=x86_?UINT64_C(0xfffffffd):gui_message_parent;
        auto parent=arg(symbol=="SetParent"?1:0);if(parent==message_parent)parent=gui_message_parent;if(parent==gui_.desktop)parent=0;
        const auto child=arg(symbol=="SetParent"?0:1);const auto found=gui_.windows.find(child);
        if(symbol=="IsChild"){
            std::set<std::uint64_t> visited;auto cursor=child;
            while(cursor && visited.insert(cursor).second){const auto w=gui_.windows.find(cursor);if(w==gui_.windows.end())break;cursor=w->second.parent;if(cursor && cursor==parent){result=1;break;}}
        }else if(found==gui_.windows.end() || (parent && parent!=gui_message_parent && !gui_.windows.contains(parent)))fail(1400);
        else {
            bool cycle=parent==child;auto cursor=parent;std::set<std::uint64_t> visited;
            while(cursor && visited.insert(cursor).second){if(cursor==child){cycle=true;break;}const auto w=gui_.windows.find(cursor);if(w==gui_.windows.end())break;cursor=w->second.parent;}
            if(cycle)fail(87);
            else {auto& w=found->second;const auto before=gui_client_origin(gui_,w.parent),after=gui_client_origin(gui_,parent);
                const auto x=static_cast<std::int64_t>(w.x)+before[0]-after[0],y=static_cast<std::int64_t>(w.y)+before[1]-after[1];
                if(x<INT32_MIN || x>INT32_MAX || y<INT32_MIN || y>INT32_MAX)fail(87);
                else {result=w.parent;const auto previous=w.parent;w.parent=parent;w.x=static_cast<std::int32_t>(x);w.y=static_cast<std::int32_t>(y);
                    present_gui(child);if(native_gui_)show_native_window(child,w.visible);invalidate_window(child);if(previous && gui_.windows.contains(previous))invalidate_window(previous);if(parent && gui_.windows.contains(parent))invalidate_window(parent);
                }
            }
        }
    } else if(symbol=="GetAncestor"){
        const auto first=gui_.windows.find(arg(0));
        if(d(1)<1 || d(1)>3)fail(87);
        else if(arg(0)==gui_.desktop)result=0;
        else if(first==gui_.windows.end())fail(1400);
        else if(d(1)==1)result=first->second.parent?first->second.parent:gui_.desktop;
        else {
            result=arg(0);std::set<std::uint64_t> visited;
            while(visited.insert(result).second){
                const auto current=gui_.windows.find(result);if(current==gui_.windows.end())break;
                const auto parent=current->second.parent?current->second.parent:(d(1)==3?current->second.owner:0);
                if(!parent || parent==gui_.desktop || !gui_.windows.contains(parent))break;
                result=parent;
            }
        }
    } else if(symbol=="GetDesktopWindow")result=gui_.desktop;
    else if(symbol=="GetShellWindow")result=query_shell_window();
    else if(symbol=="SetShellWindow" || symbol=="SetShellWindowEx")result=register_shell_window(arg(0),symbol=="SetShellWindowEx"?arg(1):arg(0));
    else if(symbol=="GetWindowInfo" || symbol=="LogicalToPhysicalPoint" || symbol=="PhysicalToLogicalPoint" || symbol=="LogicalToPhysicalPointForPerMonitorDPI" || symbol=="PhysicalToLogicalPointForPerMonitorDPI"){
        std::optional<SharedWindowInfo> info;
        if(gui_.windows.contains(arg(0)))info=describe_gui_window(arg(0));
        else if(arg(0)==gui_.desktop){SharedWindowInfo desktop{};desktop.handle=gui_.desktop;desktop.width=desktop.outer_width=1440;desktop.height=desktop.outer_height=900;desktop.style=0x96000000;desktop.visible=desktop.enabled=1;desktop.class_atom=32769;info=desktop;}
        else info=shared_gui_window(arg(0));
        if(!info)fail(1400);
        else if(!arg(1))fail(87);
        else if(symbol=="GetWindowInfo"){
            if(memory.load(GuestAddress{arg(1)},32)!=60)fail(87);
            else {const auto& w=*info;std::vector<std::uint8_t> bytes(60);const auto put=[&](unsigned offset,std::uint32_t value,unsigned size=4){for(unsigned i=0;i<size;++i)bytes[offset+i]=static_cast<std::uint8_t>(value>>(8*i));};
                put(0,60);put(4,static_cast<std::uint32_t>(w.x));put(8,static_cast<std::uint32_t>(w.y));put(12,static_cast<std::uint32_t>(w.x)+w.outer_width);put(16,static_cast<std::uint32_t>(w.y)+w.outer_height);
                put(20,static_cast<std::uint32_t>(w.client_x));put(24,static_cast<std::uint32_t>(w.client_y));put(28,static_cast<std::uint32_t>(w.client_x)+w.width);put(32,static_cast<std::uint32_t>(w.client_y)+w.height);
                put(36,w.style);put(40,w.ex_style);put(44,w.active?1:0);put(48,(w.outer_width-std::min(w.outer_width,w.width))/2);
                put(52,static_cast<std::uint32_t>(std::max<std::int64_t>(0,std::int64_t{w.y}+w.outer_height-w.client_y-w.height)));
                put(56,w.class_atom,2);put(58,0x400,2); // Win32 視窗相容版本；不是宿主作業系統版本。
                memory.write(GuestAddress{arg(1)},bytes);result=1;
            }
        }else {const auto& w=*info;const auto x=s32(memory.load(GuestAddress{arg(1)},32)),y=s32(memory.load(GuestAddress{arg(1)+4},32));
            if(!w.width || !w.height || x<w.client_x || y<w.client_y || std::int64_t{x}>std::int64_t{w.client_x}+w.width || std::int64_t{y}>std::int64_t{w.client_y}+w.height)fail(87);
            else {memory.check(GuestAddress{arg(1)},8,Access::Write);result=1;} // 共用視窗模型目前為 96 DPI，兩個座標空間相同。
        }
    }
    else if(symbol=="GetWindowRect"){
        std::int32_t x=0,y=0;std::uint32_t width=1440,height=900;
        if(arg(0)!=gui_.desktop){const auto f=gui_.windows.find(arg(0));if(f==gui_.windows.end()){fail(1400);arg.return_to(state,0);return std::nullopt;}const auto origin=gui_client_origin(gui_,f->second.parent);x=static_cast<std::int32_t>(f->second.x+origin[0]);y=static_cast<std::int32_t>(f->second.y+origin[1]);const auto outer=gui_outer_size(f->second);width=outer[0];height=outer[1];}
        memory.store(GuestAddress{arg(1)},32,static_cast<std::uint32_t>(x));memory.store(GuestAddress{arg(1)+4},32,static_cast<std::uint32_t>(y));memory.store(GuestAddress{arg(1)+8},32,static_cast<std::uint32_t>(x)+width);memory.store(GuestAddress{arg(1)+12},32,static_cast<std::uint32_t>(y)+height);result=1;
    } else if(symbol=="MapWindowPoints"){
        const auto valid=[&](std::uint64_t h){return !h || h==gui_.desktop || gui_.windows.contains(h);};
        if(!valid(arg(0)) || !valid(arg(1)))fail(1400);
        else if(d(3)>output_limit/8 || (d(3) && !arg(2)))fail(87);
        else {
            const auto from=gui_client_origin(gui_,arg(0)),to=gui_client_origin(gui_,arg(1));
            const auto mirrored=[&](std::uint64_t h){return gui_.windows.contains(h) && (gui_.windows.at(h).ex_style&0x400000U);};
            const bool fm=mirrored(arg(0)),tm=mirrored(arg(1));
            const auto fx=from[0]+(fm?gui_.windows.at(arg(0)).width:0),tx=to[0]+(tm?gui_.windows.at(arg(1)).width:0);
            const auto dx=fx-tx,dy=from[1]-to[1];
            memory.check(GuestAddress{arg(2)},static_cast<std::size_t>(d(3))*8,Access::Read);
            memory.check(GuestAddress{arg(2)},static_cast<std::size_t>(d(3))*8,Access::Write);
            for(std::uint64_t i=0;i<d(3);++i){
                const auto at=arg(2)+i*8;const auto x=s32(memory.load(GuestAddress{at},32)),y=s32(memory.load(GuestAddress{at+4},32));
                const auto mapped=dx+(fm?-static_cast<std::int64_t>(x):x);
                memory.store(GuestAddress{at},32,static_cast<std::uint32_t>(tm?-mapped:mapped));
                memory.store(GuestAddress{at+4},32,static_cast<std::uint32_t>(y+dy));
            }
            if(d(3)==2 && fm!=tm){const auto left=memory.load(GuestAddress{arg(2)},32),right=memory.load(GuestAddress{arg(2)+8},32);memory.store(GuestAddress{arg(2)},32,right);memory.store(GuestAddress{arg(2)+8},32,left);}
            result=(static_cast<std::uint32_t>(dx)&65535U)|((static_cast<std::uint32_t>(dy)&65535U)<<16);
        }
    } else if(symbol=="ScreenToClient" || symbol=="ClientToScreen"){
        auto h=arg(0);std::int64_t x=0,y=0;if(!gui_.windows.contains(h))fail(1400);else {
            const auto origin=gui_client_origin(gui_,h);x=origin[0];y=origin[1];
            if(symbol=="ScreenToClient"){x=-x;y=-y;}for(unsigned i=0;i<2;++i)memory.store(GuestAddress{arg(1)+i*4},32,static_cast<std::uint32_t>(s32(memory.load(GuestAddress{arg(1)+i*4},32))+(i?y:x)));result=1;
        }
    } else if(symbol=="MapDialogRect" || symbol=="OffsetRect"){
        const auto p=arg(symbol=="MapDialogRect"?1:0);for(unsigned i=0;i<4;++i){const auto v=s32(memory.load(GuestAddress{p+i*4},32));const auto n=symbol=="MapDialogRect"?(static_cast<std::int64_t>(v)*(gui_.windows.contains(arg(0))?(i%2?gui_.windows.at(arg(0)).dialog_base_y:gui_.windows.at(arg(0)).dialog_base_x):(i%2?16:8)))/(i%2?8:4):static_cast<std::int64_t>(v)+s32(arg(i%2?2:1));memory.store(GuestAddress{p+i*4},32,static_cast<std::uint32_t>(n));}result=1;
    } else if(symbol=="BeginDeferWindowPos"){
        if(s32(arg(0))<0)fail(87);
        else if(d(0)>65536)fail(8);
        else try {
            GuiWindowPositionBatch batch;batch.thread=threads_.current;batch.items.reserve(d(0));
            const auto handle=gui_.next_object++;gui_.deferred_positions.emplace(handle,std::move(batch));result=handle;
        }catch(const std::bad_alloc&){fail(8);}
    } else if(symbol=="DeferWindowPos"){
        const auto batch=gui_.deferred_positions.find(arg(0));const auto window=gui_.windows.find(arg(1));
        if(batch==gui_.deferred_positions.end())fail(1405);
        else if(batch->second.thread!=threads_.current)fail(5);
        else {
            auto& pending=batch->second;const auto flags=d(7);
            auto after=arg(2);if(x86_ && after>=UINT32_MAX-1)after=static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(after)));
            std::uint32_t error=0;
            if(window==gui_.windows.end() || window->second.destroying)error=1400;
            else if(window->second.thread!=threads_.current)error=5;
            else if(!pending.items.empty() && pending.parent!=window->second.parent)error=87;
            else if(!(flags&4U) && after>1 && after<UINT64_MAX-1 && (!gui_.windows.contains(after) || gui_.windows.at(after).parent!=window->second.parent))error=1400;
            if(error){gui_.deferred_positions.erase(batch);fail(error);}
            else try {
                pending.parent=window->second.parent;
                const auto found=std::find_if(pending.items.begin(),pending.items.end(),[&](const auto& item){return item.window==arg(1);});
                GuiWindowPosition item;item.window=arg(1);item.after=after;item.x=s32(arg(3));item.y=s32(arg(4));
                item.width=static_cast<std::uint32_t>(std::max(0,s32(arg(5))));item.height=static_cast<std::uint32_t>(std::max(0,s32(arg(6))));item.flags=flags;
                if(found==pending.items.end())pending.items.push_back(item);
                else {
                    if(!(flags&4U))found->after=after;if(!(flags&2U)){found->x=item.x;found->y=item.y;}if(!(flags&1U)){found->width=item.width;found->height=item.height;}
                    // 多次指定同一 HWND 合併忽略旗標，保留最後有效的位置與尺寸。
                    constexpr auto ignore=1U|2U|4U|8U|0x10U|0x200U|0x400U;
                    found->flags=((found->flags&flags)&ignore)|((found->flags|flags)&~ignore);
                    if(flags&0x40U)found->flags&=~0x80U;if(flags&0x80U)found->flags&=~0x40U;
                }
                result=arg(0);
            }catch(const std::bad_alloc&){gui_.deferred_positions.erase(batch);fail(8);}
        }
    } else if(symbol=="EndDeferWindowPos"){
        const auto batch=gui_.deferred_positions.find(arg(0));
        if(batch==gui_.deferred_positions.end())fail(1405);
        else if(batch->second.thread!=threads_.current)fail(5);
        else {auto items=std::move(batch->second.items);gui_.deferred_positions.erase(batch);return start_gui_window_pos(state,memory,std::move(items));}
    } else if(symbol=="MoveWindow" || symbol=="SetWindowPos"){
        const bool move=symbol=="MoveWindow";GuiWindowPosition item;item.window=arg(0);
        item.flags=move?(4U|(d(5)?0U:8U)):d(6);item.after=move?0:arg(1);
        if(x86_ && item.after>=UINT32_MAX-1)item.after=static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(item.after)));
        item.x=s32(arg(move?1:2));item.y=s32(arg(move?2:3));
        item.width=static_cast<std::uint32_t>(std::max(0,s32(arg(move?3:4))));item.height=static_cast<std::uint32_t>(std::max(0,s32(arg(move?4:5))));
        return start_gui_window_pos(state,memory,{item});
    } else if(symbol=="IsWindowVisible"){
        auto handle=arg(0);
        // WS_VISIBLE 只沿 parent 逐層判斷；owned popup 並不是 owner 的 child。
        for(std::size_t depth=0;handle && depth<=gui_.windows.size();++depth){
            if(handle==gui_.desktop){result=1;break;}
            const auto found=gui_.windows.find(handle);if(found==gui_.windows.end() || !(found->second.style&0x10000000U))break;
            handle=found->second.parent;if(!handle){result=1;break;}
        }
    } else if(symbol=="EnableWindow"){
        const auto window=gui_.windows.find(arg(0));
        if(window==gui_.windows.end() || window->second.destroying)fail(1400);
        else if(arg(1) && window->second.enabled)result=0;
        else {
            if(gui_.control_operations.size()>=128)throw HostCallFault("視窗啟用回呼超過上限");
            GuiControlOperation operation;operation.thread=threads_.current;operation.wide=window->second.unicode;
            operation.message={arg(0),arg(1)!=0,0,0xa};operation.enable=GuiControlOperation::Enable{arg(1)!=0,false};
            const auto token=gui_.next_object++;gui_.control_operations.emplace(token,std::move(operation));
            try{CallbackFrame frame;frame.value=token;(void)resume_gui_control(state,memory,frame,0);}
            catch(...){discard_gui_control(token);throw;}
            return std::nullopt;
        }
    } else if(symbol=="IsWindowEnabled" || symbol=="IsWindow" || symbol=="GetParent" || symbol=="IsIconic" || symbol=="IsZoomed"){
        const auto f=gui_.windows.find(arg(0));if(f!=gui_.windows.end()){if(symbol=="GetParent")result=f->second.parent?f->second.parent:f->second.owner;else if(symbol=="IsWindow")result=1;else if(symbol=="IsWindowEnabled")result=f->second.enabled?1:0;else if(symbol=="IsIconic")result=f->second.show_command==2;else if(symbol=="IsZoomed")result=f->second.show_command==3;}
    } else if(symbol=="SetFocus" || symbol=="GetFocus"){
        result=gui_.focus;if(result && !input_connected(gui_.windows.at(result).thread))result=0;
        if(symbol=="SetFocus"){
            const auto found=gui_.windows.find(arg(0));
            if(arg(0) && found==gui_.windows.end())fail(1400);
            else if(found!=gui_.windows.end() && (!gui_window_input_enabled(gui_,arg(0)) || !input_connected(found->second.thread)))fail(87);
            else if(result!=arg(0)){
                std::vector<GuestCallback> callbacks;
                if(arg(0)){const auto root=gui_root_window(arg(0));if(root!=gui_.active)callbacks=activate_gui_window(root);}
                gui_.focus=arg(0);if(native_gui_)native_gui_->focus(arg(0));
                const auto old=gui_.windows.find(result);if(old!=gui_.windows.end() && old->second.procedure)callbacks.push_back({old->second.procedure,{result,8,arg(0),0}});
                if(found!=gui_.windows.end() && found->second.procedure)callbacks.push_back({found->second.procedure,{arg(0),7,result,0}});
                start_callbacks(state,memory,std::move(callbacks),Continuation::Return,result);return std::nullopt;
            }
        }
    } else if(symbol=="GetLastActivePopup"){
        const auto window=gui_.windows.find(arg(0));if(window==gui_.windows.end())fail(1400);
        else {
            result=arg(0);
            if(!window->second.parent && !window->second.owner){
                auto popup=window->second.last_active_popup;
                for(std::size_t depth=0;popup && depth<gui_.windows.size();++depth){
                    if(popup==arg(0)){result=window->second.last_active_popup;break;}
                    const auto owned=gui_.windows.find(popup);if(owned==gui_.windows.end() || owned->second.parent)break;popup=owned->second.owner;
                }
            }
        }
    } else if(symbol=="GetForegroundWindow" || symbol=="GetActiveWindow" || symbol=="SetForegroundWindow" || symbol=="SetActiveWindow"){
        result=gui_.active;if(result && gui_.windows.at(result).desktop_object!=thread_desktop_object(threads_.current))result=0;
        if(symbol=="GetActiveWindow" && result && !input_connected(gui_.windows.at(result).thread))result=0;
        if(symbol!="GetForegroundWindow" && symbol!="GetActiveWindow"){
            const auto found=gui_.windows.find(arg(0));
            if(found==gui_.windows.end() && (arg(0) || symbol=="SetForegroundWindow"))fail(1400);
            else if(found!=gui_.windows.end() && (!input_connected(found->second.thread) || found->second.desktop_object!=gui_.input_desktop || (symbol=="SetForegroundWindow" && gui_.foreground_locked)))fail(5);
            else {
                const auto previous=gui_.active;auto callbacks=activate_gui_window(arg(0));
                start_callbacks(state,memory,std::move(callbacks),Continuation::Return,symbol=="SetForegroundWindow"?1:previous);return std::nullopt;
            }
        }
    }
    else if(symbol=="CreateIconIndirect"){
        if(!arg(0))fail(87);
        else {
            const auto unit=x86_?4U:8U,mask_at=x86_?12U:16U;memory.check(GuestAddress{arg(0)},x86_?20:32,Access::Read);
            const auto mask_handle=memory.load(GuestAddress{arg(0)+mask_at},x86_?32:64),color_handle=memory.load(GuestAddress{arg(0)+mask_at+unit},x86_?32:64);
            const auto mask=gui_.bitmaps.find(mask_handle),color=gui_.bitmaps.find(color_handle);
            if(mask==gui_.bitmaps.end() || (color_handle && color==gui_.bitmaps.end()))fail(6);
            else if(mask->second.bits_per_pixel!=1 || (!color_handle && mask->second.height%2))fail(87);
            else if(std::any_of(gui_.contexts.begin(),gui_.contexts.end(),[&](const auto& item){return item.second.bitmap==mask_handle || (color_handle && item.second.bitmap==color_handle);}))fail(170);
            else {
                if(color_handle){import_dib_bitmap(memory,color_handle);if(rasters_.contains(color_handle))sync_raster_bitmap(memory,color_handle);}
                GuiIcon icon;icon.is_icon=memory.load(GuestAddress{arg(0)},32)!=0;icon.monochrome=!color_handle;icon.width=mask->second.width;icon.height=mask->second.height/(icon.monochrome?2U:1U);
                icon.hotspot_x=icon.is_icon?icon.width/2:static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)+4},32));icon.hotspot_y=icon.is_icon?icon.height/2:static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)+8},32));
                if(!icon.width || !icon.height || icon.width>256 || icon.height>256 || (!icon.is_icon && (icon.hotspot_x>=icon.width || icon.hotspot_y>=icon.height)) || (color_handle && (color->second.width!=icon.width || color->second.height!=icon.height)))fail(87);
                else {
                    const auto count=static_cast<std::size_t>(icon.width)*icon.height;icon.mask_pixels.reserve(mask->second.pixels.size());for(auto pixel:mask->second.pixels)icon.mask_pixels.push_back((pixel&0xffffffU)?1:0);
                    const bool alpha=color_handle && color->second.bits_per_pixel==32 && std::any_of(color->second.pixels.begin(),color->second.pixels.end(),[](auto pixel){return pixel>>24;});
                    icon.alpha=alpha;icon.rgba.resize(count);if(color_handle)icon.color_pixels=color->second.pixels;
                    for(std::size_t i=0;i<count;++i){const auto pixel=icon.monochrome?(icon.mask_pixels[count+i]?0xffffffU:0U):icon.color_pixels[i];const auto coverage=alpha?pixel>>24:icon.mask_pixels[i]?0U:255U;
                        auto rgb=pixel&0xffffffU;if(alpha && coverage && coverage<255){rgb=0;for(unsigned c=0;c<3;++c)rgb|=std::min(255U,((pixel>>(c*8))&255U)*255U/coverage)<<(c*8);}
                        icon.rgba[i]=rgb|(coverage<<24);}
                    finish_icon(icon);result=store_gui_icon(std::move(icon));
                }
            }
        }
    }
    else if(symbol=="CopyIcon"){
        const auto icon=gui_.icons.find(arg(0));if(icon==gui_.icons.end())fail(1402);else {auto copy=icon->second;copy.shared=false;result=gui_.next_object++;gui_.icons.emplace(result,std::move(copy));}
    }
    else if(symbol=="GetIconInfo"){
        const auto found=gui_.icons.find(arg(0));if(found==gui_.icons.end())fail(1402);else if(!arg(1))fail(87);
        else {
            const auto& icon=found->second;const auto count=static_cast<std::size_t>(icon.width)*icon.height;
            if(icon.mask_pixels.size()!=count*(icon.monochrome?2U:1U) || (!icon.monochrome && icon.color_pixels.size()!=count))fail(50);
            else {
                const auto unit=x86_?4U:8U,mask_at=x86_?12U:16U;memory.check(GuestAddress{arg(1)},x86_?20:32,Access::Write);
                GuiBitmap mask{icon.width,icon.height*(icon.monochrome?2U:1U),std::vector<std::uint32_t>(icon.mask_pixels.size())};mask.bits_per_pixel=1;mask.stride=(icon.width+15)/16*2;
                for(std::size_t i=0;i<mask.pixels.size();++i)mask.pixels[i]=icon.mask_pixels[i]?0xffffffU:0;
                GuiBitmap color{icon.width,icon.height,icon.color_pixels};color.raw_alpha=true;const auto mask_handle=gui_.next_object++,color_handle=icon.monochrome?0:gui_.next_object++;
                gui_.bitmaps.emplace(mask_handle,std::move(mask));try{if(color_handle)gui_.bitmaps.emplace(color_handle,std::move(color));}catch(...){gui_.bitmaps.erase(mask_handle);throw;}
                memory.write(GuestAddress{arg(1)},std::vector<std::uint8_t>(x86_?20:32));memory.store(GuestAddress{arg(1)},32,icon.is_icon);memory.store(GuestAddress{arg(1)+4},32,icon.hotspot_x);memory.store(GuestAddress{arg(1)+8},32,icon.hotspot_y);
                memory.store(GuestAddress{arg(1)+mask_at},x86_?32:64,mask_handle);memory.store(GuestAddress{arg(1)+mask_at+unit},x86_?32:64,color_handle);result=1;
            }
        }
    }
    else if(symbol=="DestroyIcon" || symbol=="DestroyCursor"){
        const auto icon=gui_.icons.find(arg(0));if(icon==gui_.icons.end())fail(1402);else if((symbol=="DestroyIcon")!=icon->second.is_icon)fail(87);else if(icon->second.shared)result=1;else{if(gui_.cursor==arg(0)){gui_.cursor=0;if(native_gui_ && interactive_station())(void)native_gui_->set_cursor(nullptr,false);}gui_.icons.erase(icon);result=1;}
    }
    else if(symbol=="BringWindowToTop"){
        if(!gui_.windows.contains(arg(0)))fail(1400);
        else {start_callbacks(state,memory,{{resolve("user32.dll","SetWindowPos").value,{arg(0),0,0,0,0,0,3}}},Continuation::ReturnCallback);return std::nullopt;}
    }else if(symbol=="WindowFromDC"){
        if(arg(0)==0x10000000U)result=gui_.desktop;
        else if(arg(0)>=0x10000000U && gui_.windows.contains(arg(0)-0x10000000U))result=arg(0)-0x10000000U;
        else if(!gui_.contexts.contains(arg(0)))fail(6);
    }else if(symbol=="GetWindowDC"){
        const auto found=gui_.windows.find(arg(0));
        if(arg(0) && arg(0)!=gui_.desktop && found==gui_.windows.end())fail(1400);
        else{
            result=0x10000000U+(arg(0)==gui_.desktop?0:arg(0));GuiDc context{};
            // 非工作區由原生外框繪製：視窗 DC 的 (left,top) 對應工作區原點，量測文字與工作區繪製保持正確，
            // 外框範圍的繪製落在表面之外而被裁掉。
            if(found!=gui_.windows.end()){
                const auto insets=gui_frame_insets(found->second);
                if(insets.left || insets.top || insets.right || insets.bottom){context.viewport_origin={-static_cast<std::int32_t>(insets.left),-static_cast<std::int32_t>(insets.top)};context.window_frame=true;}
            }
            gui_.contexts[result]=std::move(context);gui_.dc_objects[result]=0xb011;gui_.saved_contexts.erase(result);
        }
    }
    else if(symbol=="GetDC"){
        if(arg(0) && !gui_.windows.contains(arg(0)))fail(1400);
        else {
            result=0x10000000+arg(0);gui_.dc_objects.try_emplace(result,0xb011);
            if(const auto context=gui_.contexts.find(result);context!=gui_.contexts.end() && context->second.window_frame){context->second=GuiDc{};gui_.dc_objects[result]=0xb011;}
        }
    }
    else if(symbol=="ReleaseDC"){
        auto found=gui_.contexts.find(arg(1));
        if(found!=gui_.contexts.end()){
            if(found->second.window_frame){found->second=GuiDc{};gui_.dc_objects[arg(1)]=0xb011;}
            found->second.clipped=false;found->second.excluded.clear();
        }
        result=1;
    }
    else if(symbol=="GetSysColor")result=gui_system_color(d(0));
    else if(symbol=="GetSysColorBrush"){result=0xa000+d(0);gui_.brushes[result]=gui_system_color(d(0));}
    else if(symbol=="GetDoubleClickTime" || symbol=="GetCaretBlinkTime")result=500;
    else if(symbol=="GetMessageTime")result=threads_.items.at(threads_.current).message_time;
    else if(symbol=="GetMessagePos")result=threads_.items.at(threads_.current).message_position;
    else if(symbol=="GetMessageExtraInfo")result=threads_.items.at(threads_.current).message_extra;
    else if(symbol=="SetMessageExtraInfo"){auto& extra=threads_.items.at(threads_.current).message_extra;result=extra;extra=arg(0);}
    else if(symbol.starts_with("RegisterWindowMessage")){result=gui_registered_message(read(arg(0)));}
    else if(symbol.starts_with("GetCharWidth32") || symbol.starts_with("GetCharABCWidthsFloat")){
        if(d(2)<d(1) || d(2)>65535 || (!wide && d(2)>255))fail(87);
        else {const bool abc=symbol.starts_with("GetCharABC");memory.check(GuestAddress{arg(3)},static_cast<std::size_t>(d(2)-d(1)+1)*(abc?12:4),Access::Write);const auto font=selected_font();
            for(std::uint64_t i=0;i<=d(2)-d(1);++i){const auto p=arg(3)+i*(abc?12:4);const auto character=wide?std::u16string(1,static_cast<char16_t>(d(1)+i)):cp1252_to_utf16(std::string(1,static_cast<char>(d(1)+i)));const auto width=text_width(font,character);
                if(abc){memory.store(GuestAddress{p},32,0);memory.store(GuestAddress{p+4},32,std::bit_cast<std::uint32_t>(static_cast<float>(width)));memory.store(GuestAddress{p+8},32,0);}else memory.store(GuestAddress{p},32,static_cast<std::uint32_t>(width));}result=1;}
    }
    else if(symbol=="SelectObject"){auto& value=gui_.dc_objects[arg(0)];result=value?value:0xb011;value=arg(1);}
    else if(symbol=="CreateFontA" || symbol=="CreateFontW"){
        std::vector<std::uint8_t> bytes(wide?92:60);for(unsigned i=0;i<5;++i)for(unsigned j=0;j<4;++j)bytes[i*4+j]=static_cast<std::uint8_t>(d(i)>>(j*8));for(unsigned i=5;i<13;++i)bytes[20+i-5]=static_cast<std::uint8_t>(d(i));
        if(arg(13)){const auto face=read(arg(13)).substr(0,31);if(wide){const auto data=utf16_bytes(face);std::copy(data.begin(),data.end(),bytes.begin()+28);}else {const auto text=utf16_to_utf8(face);std::copy_n(text.begin(),std::min<std::size_t>(31,text.size()),bytes.begin()+28);}}
        result=gui_.next_object++;gui_.fonts[result]=std::move(bytes);if(const auto font=gui_font(gui_,result);font.native)gui_.font_natives[result]=font.native;
    }
    else if(symbol.starts_with("CreateFontIndirect")){const auto bytes=wide?92U:60U;std::vector<std::uint8_t> data(bytes);memory.read(GuestAddress{arg(0)},data);result=gui_.next_object++;gui_.fonts[result]=std::move(data);if(const auto font=gui_font(gui_,result);font.native)gui_.font_natives[result]=font.native;}
    else if(symbol.starts_with("GetObject")){
        const auto bitmap=gui_.bitmaps.find(arg(0));
        if(bitmap!=gui_.bitmaps.end()){
            const auto& item=bitmap->second;const auto bitmap_size=x86_?24U:32U,dib_size=x86_?84U:104U;const auto size=item.dib_address && s32(arg(1))>=static_cast<std::int32_t>(dib_size)?dib_size:bitmap_size;
            if(!arg(2))result=bitmap_size;
            else if(s32(arg(1))>=static_cast<std::int32_t>(size)){
                // BITMAP / DIBSECTION 依客體指標寬度對齊；A/W 佈局相同。
                std::array<std::uint8_t,104> bytes{};
                const auto put=[&](unsigned at,unsigned count,std::uint64_t value){for(unsigned i=0;i<count;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
                put(4,4,item.width);put(8,4,item.height);put(12,4,item.stride?item.stride:((item.width*item.bits_per_pixel+15)/16*2));put(16,2,1);put(18,2,item.bits_per_pixel);put(x86_?20:24,x86_?4:8,item.dib_address);
                if(size==dib_size){put(bitmap_size,4,40);put(bitmap_size+4,4,item.width);put(bitmap_size+8,4,static_cast<std::uint32_t>(item.signed_height));put(bitmap_size+12,2,1);put(bitmap_size+14,2,item.bits_per_pixel);put(bitmap_size+16,4,item.dib_compression);put(bitmap_size+20,4,static_cast<std::uint64_t>(item.stride)*item.height);put(bitmap_size+32,4,item.dib_palette.size());if(item.dib_compression==3)for(unsigned i=0;i<3;++i)put(bitmap_size+40+i*4,4,item.dib_masks[i]);put(x86_?76:88,x86_?4:8,item.dib_section);put(x86_?80:96,4,item.dib_offset);}
                memory.write(GuestAddress{arg(2)},std::span(bytes).first(size));result=size;
            }
        }else if(gui_.palettes.contains(arg(0)) || arg(0)==0xb00f){
            if(!arg(2))result=2;
            else if(s32(arg(1))>=2){memory.store(GuestAddress{arg(2)},16,arg(0)==0xb00f?20:gui_.palettes.at(arg(0)).entries.size());result=2;}
        }else if(gui_.fonts.contains(arg(0)) || (arg(0)>=0xb00a && arg(0)<=0xb011 && arg(0)!=0xb00f)){
            const auto size=wide?92U:60U;std::vector<std::uint8_t> data(size);if(gui_.fonts.contains(arg(0))){
                const auto& src=gui_.fonts.at(arg(0));std::copy_n(src.begin(),std::min<std::size_t>(28,src.size()),data.begin());
                const auto family=gui_font(gui_,arg(0)).face;
                if(wide){auto count=std::min<std::size_t>(31,family.size());if(count && family[count-1]>=0xd800 && family[count-1]<=0xdbff)--count;const auto bytes=utf16_bytes(family.substr(0,count));std::copy(bytes.begin(),bytes.end(),data.begin()+28);}
                else {const auto text=utf16_to_utf8(family);auto count=std::min<std::size_t>(31,text.size());while(count && count<text.size() && (static_cast<unsigned char>(text[count])&0xc0)==0x80)--count;std::copy_n(text.begin(),count,data.begin()+28);}
            }else {data[0]=0xf3;data[1]=data[2]=data[3]=0xff;data[16]=0x90;data[17]=1;}
            if(!arg(2))result=size;else if(d(1)>=size){memory.write(GuestAddress{arg(2)},data);result=size;}
        }else if(gui_.pens.contains(arg(0))){
            const auto extended=gui_.extended_pens.find(arg(0));const bool ext=extended!=gui_.extended_pens.end();const unsigned size=ext?(x86_?24:28):16;
            if(!arg(2))result=size;else if(d(1)>=size){std::vector<std::uint8_t> bytes(size);const auto put=[&](unsigned at,unsigned value){for(unsigned i=0;i<4;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
                put(0,ext?extended->second.style:gui_.pen_widths.at(arg(0))?0:5);put(4,ext?extended->second.width:static_cast<unsigned>(gui_.pen_widths.at(arg(0))));if(ext)put(8,extended->second.brush_style);put(12,gui_.pens.at(arg(0)));memory.write(GuestAddress{arg(2)},bytes);result=size;}
        }else fail(gui_.brushes.contains(arg(0))?50:6);
    } else if(symbol.starts_with("GetTextExtent")){
        const bool ex=symbol.starts_with("GetTextExtentEx");const auto count=d(2);if(count>1048576)throw HostCallFault("GDI 文字長度超過上限");memory.check(GuestAddress{arg(1)},static_cast<std::size_t>(count)*(wide?2:1),Access::Read);
        const auto p=arg(ex?6:3);memory.check(GuestAddress{p},8,Access::Write);
        if(ex){if(arg(4))memory.check(GuestAddress{arg(4)},4,Access::Write);if(arg(5))memory.check(GuestAddress{arg(5)},static_cast<std::size_t>(count)*4,Access::Write);}
        const auto font=selected_font();const auto metrics=font_metrics(font);std::u16string text;std::vector<std::size_t> ends;
        if(wide){for(std::uint64_t i=0;i<count;++i)text.push_back(static_cast<char16_t>(memory.load(GuestAddress{arg(1)+i*2},16)));}
        else {std::string bytes(count,'\0');for(std::uint64_t i=0;i<count;++i)bytes[static_cast<std::size_t>(i)]=static_cast<char>(memory.load(GuestAddress{arg(1)+i},8));
            for(std::size_t at=0;at<bytes.size();){const auto first=static_cast<unsigned char>(bytes[at]);std::size_t n=first<0x80?1:first>=0xc2 && first<=0xdf?2:first>=0xe0 && first<=0xef?3:first>=0xf0 && first<=0xf4?4:1;
                if(n>bytes.size()-at || std::any_of(bytes.begin()+static_cast<std::ptrdiff_t>(at+1),bytes.begin()+static_cast<std::ptrdiff_t>(at+std::min(n,bytes.size()-at)),[](unsigned char c){return (c&0xc0)!=0x80;}))n=1;
                text+=utf8_to_utf16_lossy(std::string_view(bytes).substr(at,n));for(std::size_t j=0;j<n;++j)ends.push_back(text.size());at+=n;}}
        const auto width=text_width(font,text);memory.store(GuestAddress{p},32,static_cast<std::uint32_t>(width));memory.store(GuestAddress{p+4},32,static_cast<std::uint32_t>(metrics.height));
        if(ex){std::vector<std::int32_t> advances;if(native_gui_)if(const auto native=native_gui_->text_advances(font,text))advances=*native;
            if(advances.size()!=text.size()){advances.resize(text.size());std::int32_t extent=0;for(std::size_t i=0;i<text.size();++i){extent+=text_width(font,std::u16string(1,text[i]));advances[i]=extent;}}
            std::uint32_t fit=0;for(std::size_t i=0;i<count;++i){const auto end=wide?i+1:ends[i];const auto extent=end?advances[end-1]:0;if(arg(5))memory.store(GuestAddress{arg(5)+i*4},32,static_cast<std::uint32_t>(extent));if(extent<=s32(arg(3)) && (wide || i+1==count || ends[i+1]!=end))fit=static_cast<std::uint32_t>(i+1);}
            if(arg(4))memory.store(GuestAddress{arg(4)},32,fit);}result=1;
    } else if(symbol.starts_with("GetTextMetrics")){
        const auto font=selected_font();const auto metrics=font_metrics(font);const auto p=arg(1);memory.write(GuestAddress{p},std::vector<std::uint8_t>(wide?60:56));
        for(const auto [offset,value]:std::array<std::pair<unsigned,std::int32_t>,10>{{{0,metrics.height},{4,metrics.ascent},{8,metrics.descent},{16,metrics.external_leading},{20,metrics.average_width},{24,metrics.maximum_width},{28,font.weight?font.weight:400},{32,0},{36,96},{40,96}}})memory.store(GuestAddress{p+offset},32,static_cast<std::uint32_t>(value));
        const auto chars=wide?2U:1U;memory.store(GuestAddress{p+44},static_cast<std::uint8_t>(chars*8),32);memory.store(GuestAddress{p+44+chars},static_cast<std::uint8_t>(chars*8),wide?65535:255);memory.store(GuestAddress{p+44+chars*2},static_cast<std::uint8_t>(chars*8),'?');memory.store(GuestAddress{p+44+chars*3},static_cast<std::uint8_t>(chars*8),' ');
        const auto flags=p+44+chars*4;memory.store(GuestAddress{flags},8,font.italic?1:0);memory.store(GuestAddress{flags+1},8,font.underline?1:0);memory.store(GuestAddress{flags+2},8,font.strikeout?1:0);memory.store(GuestAddress{flags+3},8,(font.pitch_family&0xf0)|4|(metrics.fixed_pitch?0:1));memory.store(GuestAddress{flags+4},8,font.charset);result=1;
    } else if(symbol=="GetDeviceCaps"){if(const auto cap=printer_device_cap(arg(0),d(1)))result=*cap;else switch(d(1)){case 8:result=1440;break;case 10:result=900;break;case 12:result=32;break;case 14:result=1;break;case 88:case 90:result=96;break;case 38:result=0;break;default:result=0;}}
    else return invoke_gui_extra(state,memory,service);
    arg.return_to(state,result);return std::nullopt;
}
}
