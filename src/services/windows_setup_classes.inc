// 類別盤點、名稱／GUID 查詢及 registry handle 共用實際 devnode 與安裝類別資料。
const bool open=name.starts_with("SetupDiOpenClassRegKey"),build=name.starts_with("SetupDiBuildClassInfoList");
const bool names=name.starts_with("SetupDiClassGuidsFromName"),extended=name.find("Ex")!=name.npos;
const auto failed_class=[&](std::uint32_t error){last_error_=error;return finish(open?(x86_?UINT32_MAX:UINT64_MAX):0);};
const auto number=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
if(extended){
    const auto machine_index=open?3U:4U,reserved_index=open?4U:5U;
    if(arg(reserved_index))return failed_class(87);
    if(arg(machine_index) && !read(arg(machine_index)).empty())return failed_class(50);
}
const auto flags=open?(extended?number(2):1U):build?number(0):0U;
if((open && flags!=1 && flags!=2) || (build && (flags&~3U)))return failed_class(1004);
auto& registry=runtime_.registry;RegistryTransaction transaction(registry,registry_file_);
if(transaction.error)return failed_class(transaction.error);
const std::u16string root=std::u16string(1,2)+u"\\system\\currentcontrolset\\control\\class";
const std::u16string interface_root=std::u16string(1,2)+u"\\system\\currentcontrolset\\control\\deviceclasses";
const auto text_value=[](const RegistryValue& value){
    std::u16string result;if(value.type!=1 || value.bytes.size()%2)return result;
    for(std::size_t i=0;i<value.bytes.size();i+=2){const auto c=static_cast<char16_t>(value.bytes[i]|(std::uint16_t{value.bytes[i+1]}<<8));if(!c)break;result.push_back(c);}return result;
};
// 虛擬裝置的安裝類別 metadata 由同一節點產生；保留原先 registry 的有效欄位。
const auto before=registry;bool changed=false;
for(const auto& [handle,node]:nodes){
    (void)handle;const auto property=node.properties.find(8);if(property==node.properties.end() || node.class_guid.empty())continue;
    const auto class_name=text_value(property->second.second);if(class_name.empty())continue;
    const auto path=root+u"\\"+folded(node.class_guid);
    const auto add=[&](const std::u16string& target,std::u16string key,std::u16string_view value){
        auto& entries=registry.keys[target];if(entries.contains(key))return;
        RegistryValue data;data.type=1;data.bytes=utf16_bytes(value);data.display_name=key;
        registry.bytes+=data.bytes.size();++registry.values;entries.emplace(std::move(key),std::move(data));changed=true;
    };
    add(path,u"class",class_name);add(path,u"",class_name);
    if(!node.interface_path.empty())add(interface_root+u"\\"+guid_name(node.interface_guid),u"",class_name);
}
if(changed){
    if(registry.bytes>output_limit || registry.values>4096 || registry.keys.size()>4096){registry=before;return failed_class(8);}
    for(const auto& base:{root,interface_root})for(std::size_t end=1;;){registry.keys.try_emplace(base.substr(0,end));end=base.find(u'\\',end+1);if(end==base.npos){registry.keys.try_emplace(base);break;}}
    if(const auto error=transaction.commit();error){registry=before;return failed_class(error);}
}
if(open){
    const auto access=number(1);if((access&0x300U)==0x300U || (access&~0x20f033fU))return failed_class(87);
    const auto& base=flags==1?root:interface_root;std::u16string path=base;
    if(arg(0)){WindowsGuid guid{};memory.read(GuestAddress{arg(0)},guid);path+=u"\\"+folded(windows_guid_text(guid));}
    if(!registry.keys.contains(path))return failed_class(2);
    if(registry.handles.size()>=4096)return failed_class(8);
    const auto token=registry.next_handle++;const auto granted=access&0x02000000U?0xf003fU:access&~0x300U;
    registry.handles.emplace(token,RegistryHandle{path,granted,registry_view(access,x86_),path});return finish(token);
}
struct ClassInfo {WindowsGuid guid;std::u16string name,description;bool no_install{},no_display{},no_use{};};
std::vector<ClassInfo> classes;const auto prefix=root+u"\\";
for(const auto& [path,entries]:registry.keys){
    if(!path.starts_with(prefix))continue;const auto tail=std::u16string_view(path).substr(prefix.size());
    const auto guid=parse_windows_guid(tail,true);if(!guid)continue;
    const auto label=entries.find(u"class");if(label==entries.end())continue;
    const auto class_name=text_value(label->second);if(class_name.empty())continue;
    const auto description=entries.find(u"");classes.push_back({*guid,class_name,description==entries.end()?class_name:text_value(description->second),entries.contains(u"noinstallclass"),entries.contains(u"nodisplayclass"),entries.contains(u"nouseclass")});
}
if(names || build){
    if(!arg(3) || (!arg(1) && number(2)))return failed_class(87);
    if(names && !arg(0))return failed_class(87);const auto requested=names?folded(read(arg(0))):std::u16string{};
    std::vector<WindowsGuid> selected;
    for(const auto& item:classes)if(names?folded(item.name)==requested:!item.no_use && (!(flags&1U) || !item.no_install) && (!(flags&2U) || !item.no_display))selected.push_back(item.guid);
    memory.check(GuestAddress{arg(3)},4,Access::Write);
    if(arg(1) && number(2)>=selected.size())memory.check(GuestAddress{arg(1)},selected.size()*16,Access::Write);
    memory.store(GuestAddress{arg(3)},32,selected.size());
    if(number(2)<selected.size())return failed_class(122);
    for(std::size_t i=0;i<selected.size();++i)memory.write(GuestAddress{arg(1)+i*16},selected[i]);return finish(1);
}
if(!arg(0) || (!arg(1) && number(2)))return failed_class(87);
WindowsGuid guid{};memory.read(GuestAddress{arg(0)},guid);
const auto item=std::find_if(classes.begin(),classes.end(),[&](const auto& entry){return entry.guid==guid;});if(item==classes.end())return failed_class(2);
const auto& value=name.starts_with("SetupDiClassNameFromGuid")?item->name:item->description;
const auto ansi=utf16_to_utf8(value);const auto count=(wide?value.size():ansi.size())+1;
if(arg(3))memory.check(GuestAddress{arg(3)},4,Access::Write);
const bool fits=arg(1) && number(2)>=count;if(fits)memory.check(GuestAddress{arg(1)},wide?count*2:count,Access::Write);
if(arg(3))memory.store(GuestAddress{arg(3)},32,count);if(!fits)return failed_class(122);
if(wide)memory.write(GuestAddress{arg(1)},utf16_bytes(value));else {std::vector<std::uint8_t> bytes(ansi.begin(),ansi.end());bytes.push_back(0);memory.write(GuestAddress{arg(1)},bytes);}return finish(1);
