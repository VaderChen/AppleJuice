// DxDiag（dxdiagn.dll）：CLSID_DxDiagProvider 的 IDxDiagProvider／IDxDiagContainer 與 IClassFactory，
// 供 CoCreateInstance、CoGetClassObject 與 dxdiagn!DllGetClassObject／DllCanUnloadNow 使用。
// - 容器樹在第一次 Initialize 時依目前虛擬環境建立，之後的根容器共用同一棵樹（容器物件持有樹的參考）。
// - 系統資訊與 GetVersionEx、GlobalMemoryStatusEx、WMI 共用來源；顯示裝置與 DXGI／WMI 共用配接器身分（UMA：獨立顯存 0，
//   顯示記憶體＝共用 GPU 記憶體配額）。64 位元數值與官方相同以十進位 BSTR 回傳，DWORD 為 VT_UI4、旗標為 VT_BOOL。
// - 子容器名稱可用「.」串接路徑（例如 DxDiag_DisplayDevices.0）；名稱或索引不存在回 E_INVALIDARG，
//   緩衝不足回 DXDIAG_E_INSUFFICIENT_BUFFER 並寫入空字串。
// - 尚未列舉的子系統（DirectSound 裝置、DirectInput、檔案清單等）保留官方容器名稱但沒有子項，不虛構裝置。
#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/gpu_memory.hpp"
#include "x64arm/processor_information.hpp"
#include "x64arm/windows_version.hpp"
#include "dxdiag_interfaces.hpp"
#include "virtual_firmware.hpp"
#include "windows_display_identity.hpp"
#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace x64arm {
namespace {
constexpr std::uint32_t s_ok=0,s_false=1,e_pointer=0x80004003U,e_invalidarg=0x80070057U,e_nointerface=0x80004002U,e_outofmemory=0x8007000eU;
constexpr std::uint32_t co_e_notinitialized=0x800401f0U,class_e_noaggregation=0x80040110U,class_e_classnotavailable=0x80040111U,regdb_e_classnotreg=0x80040154U;
enum Kind : unsigned { Factory,Provider,Container };
constexpr std::array<std::string_view,3> families{"internal-dxdiag-factory","internal-dxdiag-provider","internal-dxdiag-container"};
struct Value { std::uint16_t type{}; std::uint32_t number{}; std::u16string text; };
struct Node {
    std::u16string name;
    std::vector<std::pair<std::u16string,Value>> props;
    std::vector<Node> children;
    Node& child(std::u16string child_name){children.push_back(Node{std::move(child_name),{},{}});return children.back();}
    void text(std::u16string key,std::u16string value){props.emplace_back(std::move(key),Value{8,0,std::move(value)});}
    void text(std::u16string key,std::string_view value){text(std::move(key),utf8_to_utf16(std::string(value)));}
    void number(std::u16string key,std::uint32_t value){props.emplace_back(std::move(key),Value{19,value,{}});}
    void flag(std::u16string key,bool value){props.emplace_back(std::move(key),Value{11,value?1U:0U,{}});}
    void big(std::u16string key,std::uint64_t value){text(std::move(key),utf8_to_utf16(std::to_string(value)));}
};
std::u16string dx_format(const char* pattern,auto... values){char buffer[256];std::snprintf(buffer,sizeof buffer,pattern,values...);return utf8_to_utf16(buffer);}
}
struct DxDiagState {
    struct Object { unsigned kind{}; std::uint32_t references{1}; bool initialized{}; std::shared_ptr<const Node> root; const Node* node{}; };
    std::map<std::uint64_t,Object> objects;
    std::array<std::uint64_t,3> tables{};
    std::shared_ptr<const Node> tree;
    std::uint32_t locks{};
};

std::span<const WindowsServiceDescriptor> dxdiag_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"dxdiagn.dll","DllGetClassObject",3},{"dxdiagn.dll","DllCanUnloadNow",0},
        {"internal-dxdiag-factory","QueryInterface",3},{"internal-dxdiag-factory","AddRef",1},{"internal-dxdiag-factory","Release",1},
        {"internal-dxdiag-factory","CreateInstance",4},{"internal-dxdiag-factory","LockServer",2},
        {"internal-dxdiag-provider","QueryInterface",3},{"internal-dxdiag-provider","AddRef",1},{"internal-dxdiag-provider","Release",1},
        {"internal-dxdiag-provider","Initialize",2},{"internal-dxdiag-provider","GetRootContainer",2},
        {"internal-dxdiag-container","QueryInterface",3},{"internal-dxdiag-container","AddRef",1},{"internal-dxdiag-container","Release",1},
        {"internal-dxdiag-container","GetNumberOfChildContainers",2},{"internal-dxdiag-container","EnumChildContainerNames",4},
        {"internal-dxdiag-container","GetChildContainer",3},{"internal-dxdiag-container","GetNumberOfProps",2},
        {"internal-dxdiag-container","EnumPropNames",4},{"internal-dxdiag-container","GetProp",3}
    };return table;
}

std::optional<std::uint32_t> WindowsServices::invoke_dxdiag(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(!dxdiag_)dxdiag_=std::make_shared<DxDiagState>();
    auto& dx=*dxdiag_;
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;const auto family=service.family;
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto done=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto guid=[&](std::uint64_t address){dxdiag::Guid value{};memory.read(GuestAddress{address},value);return value;};
    const auto table=[&](unsigned kind){
        if(!dx.tables[kind]){
            std::vector<std::uint64_t> entries;for(const auto& d:dxdiag_service_descriptors())if(d.family==families[kind])entries.push_back(resolve(d.family,d.symbol).value);
            const auto at=runtime_allocate(memory,{},entries.size()*unit);
            for(std::size_t i=0;i<entries.size();++i)memory.store(GuestAddress{at+i*unit},bits,entries[i]);
            dx.tables[kind]=at;
        }
        return dx.tables[kind];
    };
    const auto create=[&](DxDiagState::Object value)->std::uint64_t{
        if(dx.objects.size()>=65536)return 0;
        const auto vtable=table(value.kind);const auto pointer=allocate_heap(memory,unit);if(!pointer)return 0;
        memory.store(GuestAddress{pointer},bits,vtable);dx.objects.emplace(pointer,std::move(value));return pointer;
    };
    // 類別工廠與 provider 只接受各自的介面；COM 已初始化與 in-process 檢查由呼叫端（CoCreateInstance／CoGetClassObject）處理。
    const auto make=[&](unsigned kind,std::uint64_t riid,std::uint64_t output)->std::uint32_t{
        const auto iid=guid(riid);
        if(iid!=dxdiag::iid_unknown && iid!=(kind==Factory?dxdiag::iid_class_factory:dxdiag::iid_provider))return e_nointerface;
        DxDiagState::Object value;value.kind=kind;const auto pointer=create(std::move(value));if(!pointer)return e_outofmemory;
        memory.store(GuestAddress{output},bits,pointer);return s_ok;
    };
    try{
        if(symbol=="CoCreateInstance" || symbol=="CoGetClassObject"){
            // (rclsid, pUnkOuter, dwClsContext, riid, ppv)／(rclsid, dwClsContext, pServerInfo, riid, ppv)
            const bool creating=symbol=="CoCreateInstance";
            if(!arg(4))return done(e_pointer);memory.store(GuestAddress{arg(4)},bits,0);if(!arg(3))return done(e_invalidarg);
            if(!com_apartment(threads_.current))return done(co_e_notinitialized);
            if(!(static_cast<std::uint32_t>(arg(creating?2:1))&1U))return done(regdb_e_classnotreg);
            if(creating && arg(1))return done(class_e_noaggregation);
            return done(make(creating?Provider:Factory,arg(3),arg(4)));
        }
        if(family=="dxdiagn.dll"){
            if(symbol=="DllCanUnloadNow")return done(dx.objects.empty() && !dx.locks?s_ok:s_false);
            // DllGetClassObject(rclsid, riid, ppv)
            if(!arg(2))return done(e_pointer);memory.store(GuestAddress{arg(2)},bits,0);if(!arg(0) || !arg(1))return done(e_invalidarg);
            if(guid(arg(0))!=dxdiag::clsid_provider)return done(class_e_classnotavailable);
            return done(make(Factory,arg(1),arg(2)));
        }
        const auto found=dx.objects.find(arg(0));
        if(found==dx.objects.end())throw HostCallFault("DxDiag 介面物件已失效");
        auto& object=found->second;
        if(symbol=="QueryInterface"){
            if(!arg(2))return done(e_pointer);memory.store(GuestAddress{arg(2)},bits,0);if(!arg(1))return done(e_invalidarg);
            const auto iid=guid(arg(1));
            const auto own=object.kind==Factory?dxdiag::iid_class_factory:object.kind==Provider?dxdiag::iid_provider:dxdiag::iid_container;
            if(iid!=dxdiag::iid_unknown && iid!=own)return done(e_nointerface);
            if(object.references==UINT32_MAX)return done(e_outofmemory);
            ++object.references;memory.store(GuestAddress{arg(2)},bits,arg(0));return done(s_ok);
        }
        if(symbol=="AddRef"){if(object.references<UINT32_MAX)++object.references;return done(object.references);}
        if(symbol=="Release"){
            const auto remaining=--object.references;
            if(!remaining){dx.objects.erase(found);(void)free_heap(memory,arg(0),state);}
            return done(remaining);
        }
        if(object.kind==Factory){
            if(symbol=="LockServer"){if(arg(1))++dx.locks;else if(dx.locks)--dx.locks;return done(s_ok);}
            // CreateInstance(pUnkOuter, riid, ppv)
            if(!arg(3))return done(e_pointer);memory.store(GuestAddress{arg(3)},bits,0);if(!arg(2))return done(e_invalidarg);
            if(arg(1))return done(class_e_noaggregation);
            return done(make(Provider,arg(2),arg(3)));
        }
        if(object.kind==Provider){
            if(symbol=="Initialize"){
                // DXDIAG_INIT_PARAMS：dwSize、dwDxDiagHeaderVersion、bAllowWHQLChecks、pReserved。
                if(!arg(1))return done(e_pointer);
                if(memory.load(GuestAddress{arg(1)},32)!=(x86_?16U:24U) || memory.load(GuestAddress{arg(1)+4},32)!=dxdiag::dx9_sdk_version)return done(e_invalidarg);
                if(!dx.tree){
                    auto root=std::make_shared<Node>();
                    auto& sys_info=root->child(u"DxDiag_SystemInfo");
                    std::array<std::uint64_t,6> status{};if(const auto error=memory_status_fields(memory,status))return done(0x80070000U|error);
                    const auto cpu=guest_processor_information(memory);
                    sys_info.number(u"dwOSMajorVersion",windows_version_major);sys_info.number(u"dwOSMinorVersion",windows_version_minor);
                    sys_info.number(u"dwOSBuildNumber",windows_version_build);sys_info.number(u"dwOSPlatformID",2);
                    sys_info.number(u"dwDirectXVersionMajor",9);sys_info.number(u"dwDirectXVersionMinor",0);sys_info.text(u"szDirectXVersionLetter",std::u16string(u"c"));
                    sys_info.flag(u"bDebug",false);sys_info.flag(u"bNECPC98",false);sys_info.flag(u"bNetMeetingRunning",false);
                    sys_info.big(u"ullPhysicalMemory",status[0]);sys_info.big(u"ullUsedPageFile",status[2]-std::min(status[2],status[3]));sys_info.big(u"ullAvailPageFile",status[3]);
                    const auto mib=[](std::uint64_t bytes){return static_cast<unsigned long long>(bytes>>20);};
                    for(const auto suffix:{u"English",u"Localized"}){
                        const std::u16string tail(suffix);
                        sys_info.text(u"szPhysicalMemory"+tail,dx_format("%lluMB RAM",mib(status[0])));
                        sys_info.text(u"szPageFile"+tail,dx_format("%lluMB used, %lluMB available",mib(status[2]-std::min(status[2],status[3])),mib(status[3])));
                        const auto os=dx_format("Windows %u.%u 64-bit (%u.%u, Build %u)",windows_version_major,windows_version_minor,windows_version_major,windows_version_minor,windows_version_build);
                        sys_info.text(u"szOS"+tail,os);sys_info.text(u"szOSEx"+tail,os);sys_info.text(u"szOSExLong"+tail,os);
                    }
                    sys_info.text(u"szWindowsDir",std::u16string(u"C:\\Windows"));sys_info.text(u"szCSDVersion",std::u16string());
                    sys_info.text(u"szDirectXVersionEnglish",std::u16string(u"4.09.0000.0904"));sys_info.text(u"szDirectXVersionLongEnglish",std::u16string(u"DirectX 12"));
                    sys_info.text(u"szDxDiagVersion",dx_format("10.00.%u.0001",windows_version_build));
                    std::string processor=cpu.name;processor+=" ("+std::to_string(memory.processor_count())+" CPUs)";
                    if(cpu.nominal_mhz){char speed[32];std::snprintf(speed,sizeof speed,", ~%.1fGHz",cpu.nominal_mhz/1000.0);processor+=speed;}
                    sys_info.text(u"szProcessorEnglish",processor);
                    sys_info.text(u"szSystemManufacturerEnglish",virtual_firmware::manufacturer);sys_info.text(u"szSystemModelEnglish",virtual_firmware::product);
                    sys_info.text(u"szBIOSEnglish",virtual_firmware::bios);
                    // 顯示模式與 WMI Win32_VideoController 同一來源：主螢幕（原生模式優先），客體改過模式時以客體設定為準。
                    std::array<std::uint32_t,4> mode{};
                    const auto native=native_gui_?native_gui_->current_display_mode():std::optional<GuiDisplayMode>{};const auto& monitors=gui_monitors();
                    if(!monitors.empty()){const auto& monitor=monitors.front();
                        mode={monitor.primary && native?native->width:static_cast<std::uint32_t>(monitor.rect[2]-monitor.rect[0]),monitor.primary && native?native->height:static_cast<std::uint32_t>(monitor.rect[3]-monitor.rect[1]),32,
                              monitor.primary && native && native->refresh_denominator?(native->refresh_numerator+native->refresh_denominator/2)/native->refresh_denominator:monitor.refresh};}
                    if(gui_.display_mode)mode=*gui_.display_mode;
                    auto& display=root->child(u"DxDiag_DisplayDevices").child(u"0");
                    const auto shared=gpu_memory_stats().limit;
                    display.text(u"szDescription",std::u16string(display_identity::adapter_name));display.text(u"szChipType",std::u16string(display_identity::adapter_name));
                    display.text(u"szManufacturer",std::u16string(display_identity::vendor_name));display.text(u"szDeviceName",std::u16string(u"\\\\.\\DISPLAY1"));
                    display.text(u"szDACType",std::u16string(u"Integrated RAMDAC"));display.text(u"szMonitorName",std::u16string(u"Generic PnP Monitor"));
                    for(const auto suffix:{u"English",u"Localized"}){
                        const std::u16string tail(suffix);
                        display.text(u"szDisplayMemory"+tail,dx_format("%llu MB",mib(shared)));
                        if(mode[0] && mode[1])display.text(u"szDisplayMode"+tail,dx_format("%u x %u (%u bit) (%uHz)",mode[0],mode[1],mode[2],mode[3]));
                        display.text(u"szDriverDate"+tail,std::u16string(display_identity::date));
                    }
                    display.text(u"szDedicatedMemoryEnglish",std::u16string(u"0 MB"));display.text(u"szSharedMemoryEnglish",dx_format("%llu MB",mib(shared)));
                    display.number(u"dwWidth",mode[0]);display.number(u"dwHeight",mode[1]);display.number(u"dwBpp",mode[2]);display.number(u"dwRefreshRate",mode[3]);
                    display.text(u"szDriverName",std::u16string(display_identity::provider));display.text(u"szDriverVersion",std::u16string(display_identity::version));
                    display.text(u"szVendorId",dx_format("0x%04X",display_identity::vendor_id));display.text(u"szDeviceId",std::u16string(u"0x0000"));
                    display.text(u"szSubSysId",std::u16string(u"0x00000000"));display.text(u"szRevisionId",std::u16string(u"0x0000"));
                    display.text(u"szKeyDeviceID",u"Enum\\"+std::u16string(display_identity::adapter_hardware_id));
                    display.number(u"dwDDIVersion",12);display.flag(u"bDriverSigned",true);display.flag(u"bDriverBeta",false);display.flag(u"bDriverDebug",false);
                    display.flag(u"bDDAccelerationEnabled",true);display.flag(u"b3DAccelerationExists",true);display.flag(u"b3DAccelerationEnabled",true);
                    display.flag(u"bAGPEnabled",false);display.flag(u"bAGPExists",false);display.flag(u"bAGPExistenceValid",false);
                    auto& sound=root->child(u"DxDiag_DirectSound");sound.child(u"DxDiag_SoundDevices");sound.child(u"DxDiag_SoundCaptureDevices");
                    for(const auto name:{u"DxDiag_DirectMusic",u"DxDiag_DirectInput",u"DxDiag_DirectPlay",u"DxDiag_SystemDevices",u"DxDiag_DirectXFiles",u"DxDiag_DirectShowFilters",u"DxDiag_LogicalDisks"})
                        root->child(name);
                    dx.tree=std::move(root);
                }
                object.initialized=true;return done(s_ok);
            }
            // GetRootContainer(ppInstance)
            if(!arg(1))return done(e_invalidarg);memory.store(GuestAddress{arg(1)},bits,0);
            if(!object.initialized)return done(co_e_notinitialized);
            DxDiagState::Object value;value.kind=Container;value.root=dx.tree;value.node=dx.tree.get();
            const auto pointer=create(std::move(value));if(!pointer)return done(e_outofmemory);
            memory.store(GuestAddress{arg(1)},bits,pointer);return done(s_ok);
        }
        const auto& node=*object.node;
        if(symbol=="GetNumberOfChildContainers" || symbol=="GetNumberOfProps"){
            if(!arg(1))return done(e_invalidarg);
            memory.store(GuestAddress{arg(1)},32,symbol=="GetNumberOfProps"?node.props.size():node.children.size());return done(s_ok);
        }
        if(symbol=="EnumChildContainerNames" || symbol=="EnumPropNames"){
            // (dwIndex, pwszName, cchName)
            const bool props=symbol=="EnumPropNames";const auto index=static_cast<std::uint32_t>(arg(1)),capacity=static_cast<std::uint32_t>(arg(3));
            if(!arg(2) || !capacity)return done(e_invalidarg);
            if(index>=(props?node.props.size():node.children.size()))return done(e_invalidarg);
            const auto& name=props?node.props[index].first:node.children[index].name;
            if(name.size()>=capacity){memory.store(GuestAddress{arg(2)},16,0);return done(dxdiag::e_insufficient_buffer);}
            auto bytes=utf16_bytes(name);memory.write(GuestAddress{arg(2)},bytes);return done(s_ok);
        }
        if(symbol=="GetChildContainer"){
            if(!arg(1) || !arg(2))return done(e_invalidarg);memory.store(GuestAddress{arg(2)},bits,0);
            const auto path=read_utf16(memory,GuestAddress{arg(1)},1024);
            const Node* current=&node;std::size_t start=0;
            while(true){
                const auto end=path.find(u'.',start);const auto part=path.substr(start,end==std::u16string::npos?std::u16string::npos:end-start);
                if(part.empty())return done(e_invalidarg);
                const auto next=std::ranges::find_if(current->children,[&](const Node& c){return c.name==part;});
                if(next==current->children.end())return done(e_invalidarg);
                current=&*next;if(end==std::u16string::npos)break;start=end+1;
            }
            DxDiagState::Object value;value.kind=Container;value.root=object.root;value.node=current;
            const auto pointer=create(std::move(value));if(!pointer)return done(e_outofmemory);
            memory.store(GuestAddress{arg(2)},bits,pointer);return done(s_ok);
        }
        // GetProp(pwszPropName, pvarProp)：VARIANT 的 vt 在 +0、值在 +8。
        if(!arg(1) || !arg(2))return done(e_invalidarg);
        const auto name=read_utf16(memory,GuestAddress{arg(1)},1024);
        const auto prop=std::ranges::find_if(node.props,[&](const auto& p){return p.first==name;});
        if(prop==node.props.end())return done(e_invalidarg);
        const auto& value=prop->second;const unsigned variant_size=x86_?16U:24U;
        memory.check(GuestAddress{arg(2)},variant_size,Access::Write);
        std::uint64_t payload=0;
        if(value.type==8){auto bytes=utf16_bytes(value.text);bytes.resize(bytes.size()-2);payload=allocate_bstr(memory,bytes.size(),bytes);if(!payload)return done(e_outofmemory);}
        memory.fill(GuestAddress{arg(2)},0,variant_size);memory.store(GuestAddress{arg(2)},16,value.type);
        if(value.type==8)memory.store(GuestAddress{arg(2)+8},bits,payload);
        else if(value.type==11)memory.store(GuestAddress{arg(2)+8},16,value.number?0xffffU:0U);
        else memory.store(GuestAddress{arg(2)+8},32,value.number);
        return done(s_ok);
    }catch(const MemoryFault&){return done(e_pointer);}catch(const std::bad_alloc&){return done(e_outofmemory);}
}
}
