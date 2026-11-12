// 依 Windows SDK 相容標頭的公開 COM ABI 產生：IID、vtable 順序與參數數量。
#pragma once
#include <array>
#include <span>
#include <string_view>
namespace x64arm::webbrowser {
using Guid=std::array<std::uint8_t,16>;
struct Method {std::string_view name;unsigned arguments;};
struct Interface {std::string_view name;Guid iid;std::span<const Method> methods;};
inline constexpr Method IWebBrowser2_methods[]{{"QueryInterface",3},{"AddRef",1},{"Release",1},{"GetTypeInfoCount",2},{"GetTypeInfo",4},{"GetIDsOfNames",6},{"Invoke",9},{"GoBack",1},{"GoForward",1},{"GoHome",1},{"GoSearch",1},{"Navigate",6},{"Refresh",1},{"Refresh2",2},{"Stop",1},{"get_Application",2},{"get_Parent",2},{"get_Container",2},{"get_Document",2},{"get_TopLevelContainer",2},{"get_Type",2},{"get_Left",2},{"put_Left",2},{"get_Top",2},{"put_Top",2},{"get_Width",2},{"put_Width",2},{"get_Height",2},{"put_Height",2},{"get_LocationName",2},{"get_LocationURL",2},{"get_Busy",2},{"Quit",1},{"ClientToWindow",3},{"PutProperty",3},{"GetProperty",3},{"get_Name",2},{"get_HWND",2},{"get_FullName",2},{"get_Path",2},{"get_Visible",2},{"put_Visible",2},{"get_StatusBar",2},{"put_StatusBar",2},{"get_StatusText",2},{"put_StatusText",2},{"get_ToolBar",2},{"put_ToolBar",2},{"get_MenuBar",2},{"put_MenuBar",2},{"get_FullScreen",2},{"put_FullScreen",2},{"Navigate2",6},{"QueryStatusWB",3},{"ExecWB",5},{"ShowBrowserBar",4},{"get_ReadyState",2},{"get_Offline",2},{"put_Offline",2},{"get_Silent",2},{"put_Silent",2},{"get_RegisterAsBrowser",2},{"put_RegisterAsBrowser",2},{"get_RegisterAsDropTarget",2},{"put_RegisterAsDropTarget",2},{"get_TheaterMode",2},{"put_TheaterMode",2},{"get_AddressBar",2},{"put_AddressBar",2},{"get_Resizable",2},{"put_Resizable",2}};
inline constexpr Guid IWebBrowser2_iid{0x61,0x16,0xc,0xd3,0xaf,0xcd,0xd0,0x11,0x8a,0x3e,0x0,0xc0,0x4f,0xc9,0xe2,0x6e};
inline constexpr Method IOleObject_methods[]{{"QueryInterface",3},{"AddRef",1},{"Release",1},{"SetClientSite",2},{"GetClientSite",2},{"SetHostNames",3},{"Close",2},{"SetMoniker",3},{"GetMoniker",4},{"InitFromData",4},{"GetClipboardData",3},{"DoVerb",7},{"EnumVerbs",2},{"Update",1},{"IsUpToDate",1},{"GetUserClassID",2},{"GetUserType",3},{"SetExtent",3},{"GetExtent",3},{"Advise",3},{"Unadvise",2},{"EnumAdvise",2},{"GetMiscStatus",3},{"SetColorScheme",2}};
inline constexpr Guid IOleObject_iid{0x12,0x1,0x0,0x0,0x0,0x0,0x0,0x0,0xc0,0x0,0x0,0x0,0x0,0x0,0x0,0x46};
inline constexpr Method IOleInPlaceObject_methods[]{{"QueryInterface",3},{"AddRef",1},{"Release",1},{"GetWindow",2},{"ContextSensitiveHelp",2},{"InPlaceDeactivate",1},{"UIDeactivate",1},{"SetObjectRects",3},{"ReactivateAndUndo",1}};
inline constexpr Guid IOleInPlaceObject_iid{0x13,0x1,0x0,0x0,0x0,0x0,0x0,0x0,0xc0,0x0,0x0,0x0,0x0,0x0,0x0,0x46};
inline constexpr Method IPersistStreamInit_methods[]{{"QueryInterface",3},{"AddRef",1},{"Release",1},{"GetClassID",2},{"IsDirty",1},{"Load",2},{"Save",3},{"GetSizeMax",2},{"InitNew",1}};
inline constexpr Guid IPersistStreamInit_iid{0x80,0x23,0xd5,0x7f,0x7,0x4e,0x1b,0x10,0xae,0x2d,0x8,0x0,0x2b,0x2e,0xc7,0x13};
inline constexpr Method IConnectionPointContainer_methods[]{{"QueryInterface",3},{"AddRef",1},{"Release",1},{"EnumConnectionPoints",2},{"FindConnectionPoint",3}};
inline constexpr Guid IConnectionPointContainer_iid{0x84,0xb2,0x96,0xb1,0xb4,0xba,0x1a,0x10,0xb6,0x9c,0x0,0xaa,0x0,0x34,0x1d,0x7};
inline constexpr Method IConnectionPoint_methods[]{{"QueryInterface",3},{"AddRef",1},{"Release",1},{"GetConnectionInterface",2},{"GetConnectionPointContainer",2},{"Advise",3},{"Unadvise",2},{"EnumConnections",2}};
inline constexpr Guid IConnectionPoint_iid{0x86,0xb2,0x96,0xb1,0xb4,0xba,0x1a,0x10,0xb6,0x9c,0x0,0xaa,0x0,0x34,0x1d,0x7};
inline constexpr Method IOleInPlaceActiveObject_methods[]{{"QueryInterface",3},{"AddRef",1},{"Release",1},{"GetWindow",2},{"ContextSensitiveHelp",2},{"TranslateAccelerator",2},{"OnFrameWindowActivate",2},{"OnDocWindowActivate",2},{"ResizeBorder",4},{"EnableModeless",2}};
inline constexpr Guid IOleInPlaceActiveObject_iid{0x17,0x1,0x0,0x0,0x0,0x0,0x0,0x0,0xc0,0x0,0x0,0x0,0x0,0x0,0x0,0x46};
inline constexpr Method IClassFactory_methods[]{{"QueryInterface",3},{"AddRef",1},{"Release",1},{"CreateInstance",4},{"LockServer",2}};
inline constexpr Guid IClassFactory_iid{0x1,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0xc0,0x0,0x0,0x0,0x0,0x0,0x0,0x46};
inline constexpr Interface interfaces[]{{"IWebBrowser2",IWebBrowser2_iid,IWebBrowser2_methods},{"IOleObject",IOleObject_iid,IOleObject_methods},{"IOleInPlaceObject",IOleInPlaceObject_iid,IOleInPlaceObject_methods},{"IPersistStreamInit",IPersistStreamInit_iid,IPersistStreamInit_methods},{"IConnectionPointContainer",IConnectionPointContainer_iid,IConnectionPointContainer_methods},{"IConnectionPoint",IConnectionPoint_iid,IConnectionPoint_methods},{"IOleInPlaceActiveObject",IOleInPlaceActiveObject_iid,IOleInPlaceActiveObject_methods},{"IClassFactory",IClassFactory_iid,IClassFactory_methods}};
inline constexpr Guid clsid{0x61,0xf9,0x56,0x88,0xa,0x34,0xd0,0x11,0xa9,0x6b,0x0,0xc0,0x4f,0xd7,0x5,0xa2};
inline constexpr Guid events_iid{0xa0,0x15,0xa7,0x34,0x87,0x65,0xd0,0x11,0x92,0x4a,0x0,0x20,0xaf,0xc7,0xac,0x4d};
}
