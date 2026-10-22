#include "x64arm/windows_services.hpp"
#include "x64arm/gpu_memory.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/security_descriptor.hpp"
#include "x64arm/registry_store.hpp"
#include "x64arm/serial.hpp"
#include "x64arm/windows_raw_input.hpp"
#include "x64arm/usb.hpp"
#include "x64arm/windows_guid.hpp"
#include "x64arm/security_descriptor.hpp"
#include "windows_display_identity.hpp"
#include "setup_driver_store.hpp"
#include "bluetooth_windows.hpp"
#include <algorithm>
#include <set>

namespace x64arm {
namespace {
struct DeviceNode {
    std::u16string id,enumerator,service,class_guid,interface_path,port_name;
    std::uint32_t parent{},status{},problem{};
    bool present{true};
    std::map<std::uint32_t,std::pair<std::u16string,RegistryValue>> properties;
    // 驅動軟體鍵（Control\Class\{guid}\0000）專屬值，例如 WDDM 的 OpenCLDriverName。
    std::map<std::u16string,RegistryValue> software;
    std::array<std::uint8_t,16> guid{};
    std::array<std::uint8_t,16> interface_guid{};
    std::vector<std::array<std::uint8_t,16>> interface_aliases;
    // SetupDiCreateDeviceInfo 建立、尚未 DIF_REGISTERDEVICE 的元素只屬於該集合；setup_created 表示可由 SetupAPI 移除。
    std::uint64_t pending_set{};bool setup_created{};
};
bool supports_interface(const DeviceNode& node,const std::array<std::uint8_t,16>& guid){return node.interface_guid==guid || std::find(node.interface_aliases.begin(),node.interface_aliases.end(),guid)!=node.interface_aliases.end();}
std::u16string folded(std::u16string value){for(auto& ch:value)if(ch>=u'A' && ch<=u'Z')ch+=32;return value;}
std::u16string guid_name(const std::array<std::uint8_t,16>& guid){
    constexpr char16_t digits[]=u"0123456789abcdef";constexpr std::array<unsigned,16> order{3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};std::u16string text=u"{";
    for(unsigned i=0;i<16;++i){if(i==4 || i==6 || i==8 || i==10)text+=u'-';const auto byte=guid[order[i]];text+=digits[byte>>4];text+=digits[byte&15U];}return text+u"}";
}
std::vector<std::uint8_t> property_bytes(const RegistryValue& value,bool wide){
    if(wide || (value.type!=1 && value.type!=7))return value.bytes;
    std::u16string text;for(std::size_t at=0;at+1<value.bytes.size();at+=2)text.push_back(static_cast<char16_t>(value.bytes[at]|(static_cast<unsigned>(value.bytes[at+1])<<8)));
    const auto encoded=utf16_to_utf8(text);return {encoded.begin(),encoded.end()};
}
void text_property(DeviceNode& node,std::uint32_t property,std::u16string name,std::u16string value,bool multiple=false){
    if(multiple)value.push_back(0);RegistryValue data;data.type=multiple?7U:1U;data.bytes=utf16_bytes(value);data.display_name=name;
    node.properties.emplace(property,std::pair{std::move(name),std::move(data)});
}
void number_property(DeviceNode& node,std::uint32_t property,std::u16string name,std::uint32_t value){
    RegistryValue data;data.type=4;for(unsigned shift=0;shift<32;shift+=8)data.bytes.push_back(static_cast<std::uint8_t>(value>>shift));data.display_name=name;
    node.properties.emplace(property,std::pair{std::move(name),std::move(data)});
}
#include "windows_device_properties.inc"
// Enum key 值名稱與 CM_DRP 編號（SPDRP+1）的對應；註冊裝置的屬性以這些名稱保存與重建。
constexpr std::array<std::pair<std::uint32_t,std::u16string_view>,19> setup_property_names{{
    {1,u"DeviceDesc"},{2,u"HardwareID"},{3,u"CompatibleIDs"},{5,u"Service"},{8,u"Class"},{9,u"ClassGUID"},{10,u"Driver"},{11,u"ConfigFlags"},{12,u"Mfg"},
    {13,u"FriendlyName"},{14,u"LocationInformation"},{16,u"Capabilities"},{18,u"UpperFilters"},{19,u"LowerFilters"},{0x1a,u"DeviceType"},{0x1b,u"Exclusive"},
    {0x1c,u"DeviceCharacteristics"},{0x1e,u"UINumberDescFormat"},{0x22,u"RemovalPolicy"}}};
std::u16string property_text(const DeviceNode& node,std::uint32_t property){
    std::u16string text;const auto found=node.properties.find(property);if(found==node.properties.end())return text;const auto& bytes=found->second.second.bytes;
    for(std::size_t i=0;i+1<bytes.size();i+=2){const auto c=static_cast<char16_t>(bytes[i]|(std::uint16_t{bytes[i+1]}<<8));if(!c)break;text.push_back(c);}return text;
}
// SetupAPI 建立的 root 裝置沒有宿主硬體：停用為 CM_PROB_DISABLED；已安裝驅動但無原生後端可載入為
// CM_PROB_DRIVER_FAILED_LOAD；尚未安裝驅動為 CM_PROB_FAILED_INSTALL。
std::uint32_t setup_problem(const DeviceNode& node){
    if(const auto flags=node.properties.find(11);flags!=node.properties.end() && flags->second.second.bytes.size()==4 && (flags->second.second.bytes[0]&1U))return 22;
    return node.properties.contains(10)?39U:28U;
}
// 寫入一個登錄鍵：display_path 為含根代碼的原始大小寫路徑；values 覆寫、remove 刪除，其餘值保留。
std::uint32_t write_registry_key(RegistryState& registry,const std::filesystem::path& file,const std::u16string& display_path,
                                 const std::vector<std::pair<std::u16string,RegistryValue>>& values,const std::vector<std::u16string>& remove,std::size_t limit){
    RegistryTransaction transaction(registry,file);if(transaction.error)return transaction.error;
    const auto before=registry;const auto path=folded(display_path);
    for(std::size_t end=1;;end=path.find(u'\\',end+1)){
        const auto key=path.substr(0,end==path.npos?path.size():end);
        if(registry.keys.try_emplace(key).second && key.size()>1){const auto start=key.find_last_of(u'\\')+1;registry.display_names[key]=display_path.substr(start,key.size()-start);}
        if(end==path.npos)break;
    }
    auto& entries=registry.keys[path];
    for(const auto& name:remove)if(const auto old=entries.find(folded(name));old!=entries.end()){registry.bytes-=old->second.bytes.size();--registry.values;entries.erase(old);}
    for(const auto& [name,value]:values){
        if(registry.bytes+value.bytes.size()>limit || registry.values>=4096){registry=before;return 8;}
        const auto key=folded(name);if(const auto old=entries.find(key);old!=entries.end()){registry.bytes-=old->second.bytes.size();--registry.values;}
        entries.insert_or_assign(key,value);registry.bytes+=value.bytes.size();++registry.values;
    }
    if(const auto error=transaction.commit();error){registry=before;return error;}return 0;
}
std::uint32_t erase_registry_tree(RegistryState& registry,const std::filesystem::path& file,const std::u16string& display_path){
    RegistryTransaction transaction(registry,file);if(transaction.error)return transaction.error;
    const auto before=registry;const auto path=folded(display_path),prefix=path+u"\\";
    const auto drop=[&](auto item){for(const auto& [value_name,value]:item->second){(void)value_name;registry.bytes-=value.bytes.size();--registry.values;}
        registry.security_descriptors.erase(item->first);registry.display_names.erase(item->first);registry.modified_times.erase(item->first);registry.classes.erase(item->first);registry.volatile_keys.erase(item->first);return registry.keys.erase(item);};
    for(auto item=registry.keys.lower_bound(prefix);item!=registry.keys.end() && item->first.starts_with(prefix);)item=drop(item);
    if(const auto item=registry.keys.find(path);item!=registry.keys.end())drop(item);
    if(const auto error=transaction.commit();error){registry=before;return error;}return 0;
}
// devnode 屬性寫入 Enum\<instance>：對應 CM_DRP 的值覆寫或刪除，客體自行寫入的其他值保留。
std::uint32_t write_enum_key(RegistryState& registry,const std::filesystem::path& file,const DeviceNode& node,std::size_t limit){
    std::vector<std::pair<std::u16string,RegistryValue>> values;std::vector<std::u16string> remove;
    for(const auto& [property,value_name]:setup_property_names){(void)property;remove.emplace_back(value_name);}
    for(const auto& [property,entry]:node.properties)if(property!=23)values.emplace_back(entry.first,entry.second);
    return write_registry_key(registry,file,std::u16string(1,2)+u"\\SYSTEM\\CurrentControlSet\\Enum\\"+node.id,values,remove,limit);
}
}
struct DeviceConfigurationState {
    std::map<std::uint32_t,DeviceNode> nodes;
    std::map<input::Guid,std::uint32_t> hid_nodes;
    std::map<std::u16string,std::uint32_t> bluetooth_nodes;
    // USB：IOKit registry ID／介面編號 → devnode，主控制器 locationID → devnode；拔除後保留為不存在。
    std::vector<std::pair<std::string,std::uint32_t>> usb_paths; // IOService 路徑 → USB devnode
    std::map<std::u16string,usb::Target> usb_targets;           // 摺疊後的裝置介面路徑 → 開啟目標
    std::map<std::uint64_t,std::uint32_t> usb_nodes;std::map<std::pair<std::uint64_t,unsigned>,std::uint32_t> usb_interfaces;std::map<std::uint32_t,std::uint32_t> usb_controllers;
    std::uint32_t next_dynamic_node{0x30000};
    struct InfoSet {
        struct Interface {std::uint32_t node{};std::array<std::uint8_t,16> guid{};};
        std::vector<std::uint32_t> nodes;
        std::map<std::uint64_t,Interface> interfaces;
        std::array<std::uint8_t,16> class_guid{};bool filtered{};
        std::map<std::pair<std::uint32_t,std::uint32_t>,std::vector<driver_store::Model>> driver_lists;
        std::map<std::uint32_t,std::vector<std::uint8_t>> install_params; // 0 為整個集合的類別安裝參數。
    };
    std::map<std::uint64_t,InfoSet> sets;
    std::uint64_t next_set{0x69000000};
    std::uint64_t next_interface{0x6b000000};
    std::uint64_t next_driver{0x6d000000};
    struct Event {std::uint32_t action{};std::vector<std::uint8_t> bytes;};
    struct Notification {
        std::uint32_t type{},flags{};std::array<std::uint8_t,16> guid{};
        std::u16string instance;std::uint64_t callback{},context{},window{},payload{},work{};
        bool wide{},closing{};std::map<std::uint32_t,bool> present;
        std::deque<Event> pending;
    };
    std::map<std::uint64_t,Notification> notifications;
    std::uint64_t next_notification{0x6c000000},poll_deadline{};
};
void WindowsServices::refresh_device_configuration(bool enumerate_hid){
        if(!device_configuration_){
            auto configuration=std::make_shared<DeviceConfigurationState>();DeviceNode root;root.id=u"HTREE\\ROOT\\0";root.enumerator=u"HTREE";root.status=0x0009;
            root.class_guid=u"{4d36e97d-e325-11ce-bfc1-08002be10318}";
            root.guid={0x7d,0xe9,0x36,0x4d,0x25,0xe3,0xce,0x11,0xbf,0xc1,0x08,0x00,0x2b,0xe1,0x03,0x18};
            text_property(root,1,u"DeviceDesc",u"x64arm Virtual Machine");text_property(root,8,u"Class",u"System");
            text_property(root,9,u"ClassGUID",root.class_guid);text_property(root,23,u"EnumeratorName",root.enumerator);configuration->nodes.emplace(1,std::move(root));
            // 客體只看到實際可用的相容層裝置，不冒充宿主 PCI／USB 硬體。
            // 只需名稱時不建立 Metal 裝置（首次約 50 ms）；名稱無法確定時沿用建立裝置取得的資訊。
            auto identity=metal_opengl_identity();
            if(!identity)if(const auto native=make_metal_opengl(nullptr,1,1)){const auto info=native->info();identity=std::pair{info.renderer,info.vendor};}
            if(identity){
                struct {std::string renderer,vendor;} info{identity->first,identity->second};DeviceNode display;display.id=display_identity::instance;display.enumerator=u"ROOT";display.service=u"x64armMetal";display.parent=1;display.status=0x000b;
                display.class_guid=display_identity::class_name;display.guid=display_identity::class_guid;
                // 顯示配接器的裝置介面：GUID_DISPLAY_DEVICE_ARRIVAL，並以 GUID_DEVINTERFACE_DISPLAY_ADAPTER 為別名；D3DKMTOpenAdapterFromDeviceName 接受此路徑。
                display.interface_guid={0x80,0x51,0xa0,0x1c,0x99,0xa6,0x0a,0x45,0x9a,0x0c,0xde,0x4f,0xbe,0x3d,0xdd,0x89};
                display.interface_aliases={{0x1d,0x20,0x45,0x5b,0xf2,0xf2,0x3b,0x4f,0x85,0xbb,0x30,0xff,0x1f,0x95,0x35,0x99}};
                display.interface_path=u"\\\\?\\ROOT#X64ARM_DISPLAY#0000#{1ca05180-a699-450a-9a0c-de4fbe3ddd89}";
                text_property(display,1,u"DeviceDesc",utf8_to_utf16(info.renderer));text_property(display,2,u"HardwareID",u"ROOT\\X64ARM_DISPLAY",true);
                text_property(display,3,u"CompatibleIDs",u"ROOT\\X64ARM_DISPLAY",true);text_property(display,5,u"Service",display.service);
                text_property(display,8,u"Class",u"Display");text_property(display,9,u"ClassGUID",display.class_guid);
                text_property(display,10,u"Driver",display.class_guid+u"\\0000");text_property(display,12,u"Mfg",utf8_to_utf16(info.vendor));
                text_property(display,13,u"FriendlyName",utf8_to_utf16(info.renderer));text_property(display,23,u"EnumeratorName",display.enumerator);
                number_property(display,11,u"ConfigFlags",0);number_property(display,16,u"Capabilities",0);
                // 與 Metal 顯示配接器同一身分公告內建 OpenCL ICD；Khronos 載入器經 CM_REGISTRY_SOFTWARE 讀取。
                for(const auto* name:{u"OpenCLDriverName",u"OpenCLDriverNameWow"}){
                    RegistryValue icd;icd.type=1;icd.bytes=utf16_bytes(u"OpenCL.dll");icd.display_name=name;display.software.emplace(folded(name),std::move(icd));
                }
                configuration->nodes.emplace(2,std::move(display));
            }
            device_configuration_=std::move(configuration);
        }
        // SetupAPI 註冊的 root 列舉裝置保存在 sandbox 登錄 Enum\ROOT；每次盤點與登錄同步，
        // 讓其他程序的註冊、屬性變更與移除反映到同一棵 devnode 樹。ROOT\X64ARM_* 依宿主狀態產生，不從登錄重建。
        {auto& config=*device_configuration_;RegistryTransaction view(runtime_.registry,registry_file_,&registry_store_cache_,true);/* 唯讀同步沿用登錄快取，檔案未變更時不重新解析。 */std::set<std::u16string> listed;
            const auto prefix=std::u16string(1,2)+u"\\system\\currentcontrolset\\enum\\root\\";
            for(auto item=runtime_.registry.keys.lower_bound(prefix);item!=runtime_.registry.keys.end() && item->first.starts_with(prefix);++item){
                const auto rest=item->first.substr(prefix.size());const auto slash=rest.find(u'\\');
                if(slash==rest.npos || !slash || slash+1>=rest.size() || rest.find(u'\\',slash+1)!=rest.npos || rest.starts_with(u"x64arm_"))continue;
                DeviceNode node;node.id=u"ROOT\\"+rest;for(auto& c:node.id)if(c>=u'a' && c<=u'z')c=static_cast<char16_t>(c-32);
                node.enumerator=u"ROOT";node.parent=1;node.present=true;node.status=0x0401U;node.problem=28U;node.setup_created=true;
                for(const auto& [property,value_name]:setup_property_names){const auto value=item->second.find(folded(std::u16string(value_name)));
                    if(value!=item->second.end())node.properties.insert_or_assign(property,std::pair{std::u16string(value_name),value->second});}
                if(const auto guid=node.properties.find(9);guid!=node.properties.end()){std::u16string text;const auto& bytes=guid->second.second.bytes;
                    for(std::size_t i=0;i+1<bytes.size();i+=2){const auto c=static_cast<char16_t>(bytes[i]|(std::uint16_t{bytes[i+1]}<<8));if(!c)break;text.push_back(c);}
                    if(const auto parsed=parse_windows_guid(text,true)){node.guid=*parsed;node.class_guid=folded(text);}}
                node.problem=setup_problem(node);
                text_property(node,23,u"EnumeratorName",node.enumerator);listed.insert(folded(node.id));
                const auto existing=std::find_if(config.nodes.begin(),config.nodes.end(),[&](const auto& entry){return !entry.second.pending_set && folded(entry.second.id)==folded(node.id);});
                if(existing!=config.nodes.end()){if(existing->second.setup_created)existing->second=std::move(node);continue;}
                if(config.nodes.size()>=65536)break;
                while(config.nodes.contains(config.next_dynamic_node)){if(config.next_dynamic_node==UINT32_MAX)throw std::bad_alloc{};++config.next_dynamic_node;}
                config.nodes.emplace(config.next_dynamic_node++,std::move(node));
            }
            std::erase_if(config.nodes,[&](const auto& entry){return entry.second.setup_created && !entry.second.pending_set && !listed.contains(folded(entry.second.id));});
        }
        // 列舉與 CreateFile 使用同一份 IOKit／嵌入端 COM 映射；devnode 與集合 token 保持穩定。
#include "windows_usb_devices.inc"
        scan_serial_ports();
        for(const auto& [number,path]:serial_ports_){
            const auto handle=0x10000U+number;std::error_code error;
            const bool character_device=std::filesystem::is_character_file(path,error);
            const bool opened=std::any_of(files_.begin(),files_.end(),[&](const auto& file){return file.second.serial && file.second.path==path;});
            const bool present=character_device && (native_serial_paths_.contains(path) || opened || NativeSerial::is_device(path));
            if(!present && !device_configuration_->nodes.contains(handle))continue;
            DeviceNode port;port.id=u"ROOT\\X64ARM_SERIAL\\"+utf8_to_utf16("COM"+std::to_string(number));
            port.enumerator=u"ROOT";port.service=u"x64armSerial";port.parent=1;port.present=present;
            port.status=present?0x000bU:0x0401U;port.problem=present?0U:45U;
            port.class_guid=u"{4d36e978-e325-11ce-bfc1-08002be10318}";
            port.guid={0x78,0xe9,0x36,0x4d,0x25,0xe3,0xce,0x11,0xbf,0xc1,0x08,0x00,0x2b,0xe1,0x03,0x18};
            port.interface_guid={0xe0,0xd1,0xe0,0x86,0x89,0x80,0xd0,0x11,0x9c,0xe4,0x08,0x00,0x3e,0x30,0x1f,0x73};
            port.port_name=utf8_to_utf16("COM"+std::to_string(number));port.interface_path=u"\\\\?\\"+port.port_name;
            const auto description=utf8_to_utf16(std::filesystem::path(path).filename().string());
            text_property(port,1,u"DeviceDesc",description);text_property(port,2,u"HardwareID",u"ROOT\\X64ARM_SERIAL",true);
            text_property(port,3,u"CompatibleIDs",u"ROOT\\X64ARM_SERIAL",true);text_property(port,5,u"Service",port.service);
            text_property(port,8,u"Class",u"Ports");text_property(port,9,u"ClassGUID",port.class_guid);
            text_property(port,10,u"Driver",port.class_guid+u"\\0000");text_property(port,12,u"Mfg",u"x64arm");
            text_property(port,13,u"FriendlyName",description+u" ("+port.port_name+u")");
            text_property(port,14,u"LocationInformation",utf8_to_utf16(path));text_property(port,23,u"EnumeratorName",port.enumerator);
            number_property(port,11,u"ConfigFlags",0);number_property(port,16,u"Capabilities",0);
            device_configuration_->nodes.insert_or_assign(handle,std::move(port));
        }
        if(enumerate_hid){
            prepare_input_backend();if(!raw_input_)raw_input_=std::make_shared<RawInputState>();poll_dinput();
            for(const auto& [raw_handle,item]:raw_input_->devices){(void)raw_handle;const auto& device=item.device;
                auto id=device_configuration_->hid_nodes.find(device.instance);
                if(id==device_configuration_->hid_nodes.end()){const auto number=0x20000U+static_cast<std::uint32_t>(device_configuration_->hid_nodes.size());id=device_configuration_->hid_nodes.emplace(device.instance,number).first;}
                DeviceNode node;node.id=u"HID\\"+item.name.substr(8);node.enumerator=u"HID";node.service=u"MacWinHID";node.parent=1;node.present=device.attached;node.status=device.attached?0x000bU:0x0401U;node.problem=device.attached?0U:45U;
                node.guid=hid_class_guid;node.class_guid=guid_name(hid_class_guid);node.interface_guid=hid_interface_guid;node.interface_path=item.name;
                // USB HID 掛在所屬 USB 介面（或單一介面裝置）節點下，與 Windows 的 USB→HID 拓撲一致；其餘 HID 留在根節點。
                {std::size_t best=0;for(const auto& [usb_path,usb_handle]:device_configuration_->usb_paths)
                    if(!usb_path.empty() && usb_path.size()>best && device.path.size()>usb_path.size() && device.path.starts_with(usb_path) && device.path[usb_path.size()]=='/'){best=usb_path.size();node.parent=usb_handle;}}
                // 硬體 ID：既有實例 ID、HID\VID&PID&REV、HID\VID&PID；相容 ID：依頂層 usage 的 HID_DEVICE_UP、HID_DEVICE。
                const auto hex=[](unsigned value){constexpr char16_t table[]=u"0123456789ABCDEF";std::u16string out(4,u'0');for(unsigned i=0;i<4;++i)out[3-i]=table[(value>>(4*i))&15U];return out;};
                const auto vidpid=u"HID\\VID_"+hex(device.vendor)+u"&PID_"+hex(device.product_id);
                text_property(node,1,u"DeviceDesc",utf8_to_utf16(device.name));text_property(node,2,u"HardwareID",node.id+u'\0'+vidpid+u"&REV_"+hex(device.version)+u'\0'+vidpid,true);
                text_property(node,3,u"CompatibleIDs",u"HID_DEVICE_UP:"+hex(device.usage_page)+u"_U:"+hex(device.usage)+u'\0'+u"HID_DEVICE"+u'\0'+u"HID\\MACWIN",true);
                text_property(node,5,u"Service",node.service);text_property(node,8,u"Class",u"HIDClass");text_property(node,9,u"ClassGUID",node.class_guid);text_property(node,10,u"Driver",node.class_guid+u"\\0000");
                if(!device.manufacturer.empty())text_property(node,12,u"Mfg",utf8_to_utf16(device.manufacturer));text_property(node,13,u"FriendlyName",utf8_to_utf16(device.name));text_property(node,14,u"LocationInformation",utf8_to_utf16(device.path));text_property(node,23,u"EnumeratorName",node.enumerator);
                number_property(node,11,u"ConfigFlags",0);number_property(node,16,u"Capabilities",0);device_configuration_->nodes.insert_or_assign(id->second,std::move(node));
            }
        }
    auto& config=*device_configuration_;
    for(const auto& [path,id]:config.bluetooth_nodes){(void)path;const auto found=config.nodes.find(id);if(found==config.nodes.end())continue;auto& node=found->second;node.present=false;node.status=0x2401;node.problem=45;}
    if(bluetooth_){
        for(const auto& item:bluetooth_->interfaces()){
            const auto key=folded(item.path);auto held=config.bluetooth_nodes.find(key);
            if(held==config.bluetooth_nodes.end()){
                while(config.nodes.contains(config.next_dynamic_node)){if(config.next_dynamic_node==UINT32_MAX)throw std::bad_alloc{};++config.next_dynamic_node;}
                if(config.next_dynamic_node==UINT32_MAX)throw std::bad_alloc{};held=config.bluetooth_nodes.emplace(key,config.next_dynamic_node++).first;
            }
            DeviceNode node;node.parent=1;node.present=true;node.status=0x000b;node.service=u"x64armBluetooth";
            const auto identity=bluetooth::windows::encoded_id(item.device);node.enumerator=item.kind==bluetooth::windows::Kind::Radio?u"BTH":u"BTHLE";
            node.id=node.enumerator+u"\\X64ARM\\"+identity;
            if(item.service){const auto handle=std::to_string(item.service->handle);node.id+=u"\\GATT_";node.id.append(handle.begin(),handle.end());
                const auto parent_path=key.substr(0,key.find(u"#service#"));if(const auto parent=config.bluetooth_nodes.find(parent_path);parent!=config.bluetooth_nodes.end())node.parent=parent->second;}
            node.guid=bluetooth::windows::device_class;node.class_guid=guid_name(node.guid);node.interface_path=item.path;node.interface_guid=item.guid;node.interface_aliases=item.aliases;
            const auto description=item.name.empty()?std::u16string(u"Bluetooth"):item.name;
            text_property(node,1,u"DeviceDesc",description);text_property(node,2,u"HardwareID",node.id,true);text_property(node,3,u"CompatibleIDs",node.enumerator+u"\\X64ARM",true);
            text_property(node,5,u"Service",node.service);text_property(node,8,u"Class",u"Bluetooth");text_property(node,9,u"ClassGUID",node.class_guid);
            text_property(node,13,u"FriendlyName",description);text_property(node,14,u"LocationInformation",utf8_to_utf16(item.device));text_property(node,23,u"EnumeratorName",node.enumerator);
            number_property(node,11,u"ConfigFlags",0);number_property(node,16,u"Capabilities",0);config.nodes.insert_or_assign(held->second,std::move(node));
        }
    }
    // 映射消失亦須更新既有 devnode，讓列舉與通知看到同一個移除狀態。
    for(auto& [id,node]:device_configuration_->nodes){
        if(node.enumerator==u"ROOT" && node.service==u"x64armSerial" && !serial_ports_.contains(id-0x10000U)){
            node.present=false;node.status=0x2401;node.problem=45;
        }
    }
}
#include "windows_device_notifications.inc"
std::optional<usb::Target> WindowsServices::usb_target(const std::u16string& path){
    refresh_device_configuration(false);const auto found=device_configuration_->usb_targets.find(folded(path));
    if(found==device_configuration_->usb_targets.end())return std::nullopt;
    const auto node=std::find_if(device_configuration_->nodes.begin(),device_configuration_->nodes.end(),[&](const auto& item){return folded(item.second.interface_path)==found->first;});
    if(node==device_configuration_->nodes.end() || !node->second.present)return std::nullopt;
    return found->second;
}
std::optional<std::uint32_t> WindowsServices::invoke_device_configuration(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const bool wide=name.ends_with('W');
    const auto finish=[&](std::uint64_t result)->std::optional<std::uint32_t>{arg.return_to(state,result);return std::nullopt;};
    const auto read=[&](std::uint64_t pointer){return pointer?(wide?read_utf16(memory,GuestAddress{pointer},32768):utf8_to_utf16(read_ansi(memory,pointer,32768))):std::u16string{};};
    if(!runtime_.network.external_io)throw HostCallFault("原生顯示裝置盤點不支援 deterministic compare");
    if(name=="CM_Register_Notification" || name=="CM_Unregister_Notification")return invoke_device_notification(state,memory,service);
    try{
        if(name.starts_with("SetupGetInfDriverStoreLocation")){
            const auto failed=[&](std::uint32_t error){last_error_=error;return finish(0);};
            if(!arg(0) || arg(1) || arg(2) || (!arg(3) && arg(4)))return failed(87);
            // GetNativeSystemInfo 對兩種客體 ABI 都回報 AMD64；WOW64 程序同樣查詢原生驅動套件。
            const auto path=driver_store::stored_inf(filesystem_,read(arg(0)),false);const auto ansi=utf16_to_utf8(path);
            const auto length=(wide?path.size():ansi.size())+1;if(length>260)return failed(206);
            if(arg(5))memory.store(GuestAddress{arg(5)},32,length);if(!arg(3) || arg(4)<length)return failed(122);
            auto bytes=wide?utf16_bytes(path):std::vector<std::uint8_t>(ansi.begin(),ansi.end());if(!wide)bytes.push_back(0);memory.write(GuestAddress{arg(3)},bytes);return finish(1);
        }
#include "windows_driver_packages.inc"
        bool enumerate_hid=static_cast<bool>(raw_input_) || name.starts_with("SetupDiClass") || name.starts_with("SetupDiGetClassDescription") || name.starts_with("SetupDiBuildClassInfoList") || name.starts_with("SetupDiOpenClassRegKey");
        if(name.starts_with("SetupDiGetClassDevs")){
            const auto flags=static_cast<std::uint32_t>(arg(3));if(flags&~31U || (!(flags&4U) && !arg(0))){last_error_=87;return finish(x86_?UINT32_MAX:UINT64_MAX);}
            bool enumerate_bluetooth=false,low_energy=false;
            if(arg(0)){std::array<std::uint8_t,16> guid{};memory.read(GuestAddress{arg(0)},guid);enumerate_hid=enumerate_hid || guid==hid_interface_guid || guid==hid_class_guid;enumerate_bluetooth=bluetooth_class(guid);low_energy=enumerate_bluetooth && guid!=bluetooth::windows::radio_interface;}
            else if(arg(3)&4U)enumerate_hid=true;
            if(enumerate_bluetooth || ((flags&4U) && bluetooth_)){
                const auto error=enumerate_bluetooth_devices(memory,low_energy);if(!error)return std::nullopt;
                if(*error && !(flags&4U)){last_error_=*error;return finish(x86_?UINT32_MAX:UINT64_MAX);}
            }
        }
        if(name.starts_with("CM_Locate_DevNode") && folded(read(arg(1))).starts_with(u"hid\\"))enumerate_hid=true;
        refresh_device_configuration(enumerate_hid);
        const auto& nodes=device_configuration_->nodes;
        if(name.starts_with("SetupDiClass") || name.starts_with("SetupDiGetClassDescription") || name.starts_with("SetupDiBuildClassInfoList") || name.starts_with("SetupDiOpenClassRegKey")){
#include "windows_setup_classes.inc"
        }

        // SetupAPI 的集合只引用同一組 CM devnode；軟體 key 由共用裝置模型產生。
        if(name.starts_with("SetupDi") || name=="DiUninstallDevice"){
#include "windows_setupapi.inc"
        }
        if(name.starts_with("CM_Locate_DevNode")){
            if(!arg(0))return finish(3);if(arg(2)&~3U)return finish(4);const auto id=folded(read(arg(1)));
            const auto node=id.empty()?nodes.find(1):std::find_if(nodes.begin(),nodes.end(),[&](const auto& entry){return !entry.second.pending_set && folded(entry.second.id)==id;});
            if(node==nodes.end() || (!node->second.present && !(arg(2)&1U)))return finish(13);memory.store(GuestAddress{arg(0)},32,node->first);return finish(0);
        }
        if(name.starts_with("CM_Get_Device_ID_List")){
            const bool size=name.find("_Size")!=name.npos;const auto flags=static_cast<std::uint32_t>(arg(size?2:3));
            if(flags&~0x303U || ((flags&3U)==3) || ((flags&3U) && (flags&0x200U)))return finish(4);
            const auto filter=folded(read(arg(size?1:0)));if((flags&(3U|0x200U)) && filter.empty())return finish(3);
            std::u16string list;
            for(const auto& [handle,node]:nodes){
                (void)handle;if(node.pending_set)continue;if((flags&0x100) && !node.present)continue;if((flags&1) && folded(node.enumerator)!=filter)continue;if((flags&2) && folded(node.service)!=filter)continue;if((flags&0x200) && folded(node.class_guid)!=filter)continue;
                list+=node.id;list.push_back(0);
            }
            list.push_back(0);if(list.size()==1)list.push_back(0);const auto encoded=utf16_to_utf8(list);const auto length=wide?list.size():encoded.size();
            if(size){if(!arg(0))return finish(3);memory.store(GuestAddress{arg(0)},32,length);return finish(0);}
            if(!arg(1))return finish(3);if(arg(2)<length)return finish(26);
            if(wide)memory.write(GuestAddress{arg(1)},std::span(reinterpret_cast<const std::uint8_t*>(list.data()),list.size()*2));else memory.write(GuestAddress{arg(1)},std::span(reinterpret_cast<const std::uint8_t*>(encoded.data()),encoded.size()));return finish(0);
        }
        if(name.starts_with("CM_Get_Device_Interface_List")){
            // CM_GET_DEVICE_INTERFACE_LIST_PRESENT／ALL_DEVICES；可依裝置執行個體篩選，結果為多字串的介面路徑。
            const bool size=name.find("_Size")!=name.npos;const auto guid_at=arg(size?1:0),device_at=arg(size?2:1);const auto flags=static_cast<std::uint32_t>(arg(size?3:4));
            if(flags&~1U)return finish(11);if(!guid_at)return finish(3);std::array<std::uint8_t,16> guid{};memory.read(GuestAddress{guid_at},guid);
            const auto device=device_at?folded(read(device_at)):std::u16string{};std::u16string list;bool device_found=device.empty();
            for(const auto& [handle,node]:nodes){
                (void)handle;if(node.pending_set)continue;if(!device.empty()){if(folded(node.id)!=device)continue;device_found=true;}
                if(!(flags&1U) && !node.present)continue;if(node.interface_path.empty() || !supports_interface(node,guid))continue;
                list+=node.interface_path;list.push_back(0);
            }
            if(!device_found)return finish(13);
            list.push_back(0);const auto encoded=utf16_to_utf8(list);const auto length=wide?list.size():encoded.size();
            if(size){if(!arg(0))return finish(3);memory.store(GuestAddress{arg(0)},32,length);return finish(0);}
            if(!arg(2))return finish(3);if(static_cast<std::uint32_t>(arg(3))<length)return finish(26);
            if(wide)memory.write(GuestAddress{arg(2)},std::span(reinterpret_cast<const std::uint8_t*>(list.data()),list.size()*2));else memory.write(GuestAddress{arg(2)},std::span(reinterpret_cast<const std::uint8_t*>(encoded.data()),encoded.size()));return finish(0);
        }
        const auto node_at=name=="CM_Get_DevNode_Status"?arg(2):name=="CM_Get_Device_ID_Size" || name=="CM_Get_Child" || name=="CM_Get_Parent" || name=="CM_Get_Sibling"?arg(1):arg(0);
        const auto found=nodes.find(static_cast<std::uint32_t>(node_at));if(found==nodes.end())return finish(13);const auto& node=found->second;
        if(name=="CM_Get_Child" || name=="CM_Get_Parent" || name=="CM_Get_Sibling"){
            if(!arg(0))return finish(3);if(arg(2))return finish(4);std::uint32_t next=0;
            if(name=="CM_Get_Parent")next=node.parent;
            else for(const auto& [handle,candidate]:nodes)if(!candidate.pending_set && (name=="CM_Get_Child"?candidate.parent==found->first:handle>found->first && node.parent && candidate.parent==node.parent)){next=handle;break;}
            if(!next)return finish(13);memory.store(GuestAddress{arg(0)},32,next);return finish(0);
        }
        if(name=="CM_Enable_DevNode" || name=="CM_Disable_DevNode"){
            const bool enable=name=="CM_Enable_DevNode";const auto flags=static_cast<std::uint32_t>(arg(1));
            if(enable?flags!=0:(flags&~15U)!=0)return finish(4);
            initialize_tokens();const auto& token=runtime_.token_objects.at(effective_token_id());
            const bool admin=token.integrity>=0x3000U && std::any_of(token.groups.begin(),token.groups.end(),[](const auto& group){return group.sid==security::account_sid(*security::find_account("Administrators")) && (group.attributes&0x14U)==4U;});
            if(!admin)return finish(51);if(!node.present)return finish(13);
            // 原生橋接裝置沒有宣告 DN_DISABLEABLE；不可僅修改 devnode 狀態冒充停用硬體。
            if(!enable)return finish(40);
            return finish((node.status&8U) && !node.problem?0U:48U);
        }
        if(name=="CM_Get_DevNode_Status"){
            if(!arg(0) || !arg(1))return finish(3);if(arg(3))return finish(4);memory.check(GuestAddress{arg(0)},4,Access::Write);memory.check(GuestAddress{arg(1)},4,Access::Write);
            memory.store(GuestAddress{arg(0)},32,node.status);memory.store(GuestAddress{arg(1)},32,node.problem);return finish(0);
        }
        if(name=="CM_Get_Device_ID_Size"){if(!arg(0))return finish(3);if(arg(2))return finish(4);memory.store(GuestAddress{arg(0)},32,node.id.size());return finish(0);}
        if(name=="CM_Get_Device_IDA" || name=="CM_Get_Device_IDW"){
            if(!arg(1))return finish(3);if(arg(3))return finish(4);const auto ansi=utf16_to_utf8(node.id);if(arg(2)<=(wide?node.id.size():ansi.size()))return finish(26);
            auto bytes=wide?utf16_bytes(node.id):std::vector<std::uint8_t>(ansi.begin(),ansi.end());if(!wide)bytes.push_back(0);memory.write(GuestAddress{arg(1)},bytes);return finish(0);
        }
        if(name.starts_with("CM_Get_DevNode_Registry_Property")){
            if(!arg(4))return finish(3);if(arg(5))return finish(4);const auto property=node.properties.find(static_cast<std::uint32_t>(arg(1)));
            if(arg(1)<1 || arg(1)>37)return finish(53);if(property==node.properties.end())return finish(37);
            const auto bytes=property_bytes(property->second.second,wide);const auto capacity=memory.load(GuestAddress{arg(4)},32);memory.check(GuestAddress{arg(4)},4,Access::Write);if(arg(2))memory.check(GuestAddress{arg(2)},4,Access::Write);
            if(arg(3) && capacity>=bytes.size())memory.check(GuestAddress{arg(3)},bytes.size(),Access::Write);
            memory.store(GuestAddress{arg(4)},32,bytes.size());if(arg(2))memory.store(GuestAddress{arg(2)},32,property->second.second.type);
            if(!arg(3) || capacity<bytes.size())return finish(26);memory.write(GuestAddress{arg(3)},bytes);return finish(0);
        }
        if(name=="CM_Get_DevNode_PropertyW"){
            if(arg(5))return finish(4);if(!arg(1) || !arg(2) || !arg(4))return finish(3);
            const auto capacity=memory.load(GuestAddress{arg(4)},32);if(!arg(3) && capacity)return finish(3);
            const auto key=read_device_property_key(memory,arg(1));if(!valid_device_property_key(key))return finish(53);
            const auto property=device_property(node,key,nodes);if(!property)return finish(37);
            memory.check(GuestAddress{arg(2)},4,Access::Write);memory.check(GuestAddress{arg(4)},4,Access::Write);
            const bool fits=arg(3) && capacity>=property->bytes.size();if(fits)memory.check(GuestAddress{arg(3)},property->bytes.size(),Access::Write);
            memory.store(GuestAddress{arg(2)},32,property->type);memory.store(GuestAddress{arg(4)},32,property->bytes.size());
            if(!fits)return finish(26);memory.write(GuestAddress{arg(3)},property->bytes);return finish(0);
        }
        if(name=="CM_Open_DevNode_Key"){
            if(!arg(4))return finish(3);if(arg(3)>1)return finish(31);if(arg(5)&~0x301U || (arg(5)&0x300U)==0x300U)return finish(4);
            if(arg(2) || (arg(5)&0x300U))return finish(52);if(arg(1)&~0x20119U)return finish(51);
            if(runtime_.registry.handles.size()>=4096)return finish(2);memory.check(GuestAddress{arg(4)},x86_?4:8,Access::Write);
            auto& registry=runtime_.registry;RegistryTransaction transaction(registry,registry_file_,&registry_store_cache_);if(transaction.error)return finish(51);
            // 裝置鍵通常已存在；沿用快取避免每次都重新解析整個登錄檔，只有實際寫入才使快取失效。
            bool committed=false;
            const auto path=folded(std::u16string(1,2)+((arg(5)&1)?u"\\system\\currentcontrolset\\control\\class\\"+node.class_guid+u"\\0000":u"\\system\\currentcontrolset\\enum\\"+node.id));
            if((arg(5)&1) && node.class_guid.empty())return finish(46);
            const auto missing_software=(arg(5)&1) && std::any_of(node.software.begin(),node.software.end(),[&](const auto& item){
                const auto key=registry.keys.find(path);return key==registry.keys.end() || !key->second.contains(item.first);});
            if(registry.keys.contains(path) && missing_software){
                const auto before=registry;auto& values=registry.keys[path];
                for(const auto& [value_name,data]:node.software)if(values.emplace(value_name,data).second){registry.bytes+=data.bytes.size();++registry.values;}
                if(const auto error=transaction.commit();error){registry=before;return finish(51);}
                committed=true;
            }
            if(!registry.keys.contains(path)){
                std::uint64_t added=0;for(const auto& [property,data]:node.properties){(void)property;added+=data.second.bytes.size();}
                if(registry.bytes+added>output_limit || registry.values+node.properties.size()>4096 || registry.keys.size()+12>4096)return finish(2);
                const auto before=registry;
                for(std::size_t end=1;;){registry.keys.try_emplace(path.substr(0,end));end=path.find(u'\\',end+1);if(end==path.npos)break;}
                auto& values=registry.keys[path];for(const auto& [property,data]:node.properties){(void)property;values.emplace(folded(data.first),data.second);}
                registry.bytes+=added;registry.values+=node.properties.size();
                if(arg(5)&1)for(const auto& [value_name,data]:node.software)if(values.emplace(value_name,data).second){registry.bytes+=data.bytes.size();++registry.values;}
                if(const auto error=transaction.commit();error){registry=before;return finish(51);}
                committed=true;
            }
            if(!committed)transaction.unchanged();
            const auto handle=registry.next_handle++;registry.handles.emplace(handle,RegistryHandle{path,static_cast<std::uint32_t>(arg(1)&~0x100U)});memory.store(GuestAddress{arg(4)},x86_?32:64,handle);return finish(0);
        }
        throw HostCallFault("尚未實作裝置組態服務："+std::string(name));
    }catch(const driver_store::Error& error){last_error_=error.code;return finish(0);}
    catch(const std::invalid_argument&){last_error_=13;return finish(0);}
    catch(const MemoryFault&){if(name.starts_with("Setup") || name.starts_with("Di")){last_error_=87;return finish(name.starts_with("SetupDiGetClassDevs") || name.starts_with("SetupDiCreateDeviceInfoList") || name=="SetupDiOpenDevRegKey" || name=="SetupDiOpenDeviceInterfaceRegKey"?(x86_?UINT32_MAX:UINT64_MAX):0);}return finish(3);}
    catch(const std::bad_alloc&){if(name.starts_with("Setup") || name.starts_with("Di")){last_error_=8;return finish(name.starts_with("SetupDiGetClassDevs") || name.starts_with("SetupDiCreateDeviceInfoList") || name=="SetupDiOpenDevRegKey" || name=="SetupDiOpenDeviceInterfaceRegKey"?(x86_?UINT32_MAX:UINT64_MAX):0);}return finish(2);}
}
}
