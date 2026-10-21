#include "x64arm/registry.hpp"
#include <algorithm>
#include <array>
#include <string_view>
#include <set>

namespace x64arm {
namespace {
bool under(std::u16string_view path,std::u16string_view root){return path==root || (path.starts_with(root) && path.size()>root.size() && path[root.size()]==u'\\');}
std::u16string fold(std::u16string value){for(auto& c:value)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);return value;}
}
RegistryView registry_view(std::uint32_t flags,bool x86){return flags&0x100U?RegistryView::Native64:flags&0x200U?RegistryView::Wow32:x86?RegistryView::Wow32:RegistryView::Native64;}
std::u16string registry_view_path(std::u16string path,RegistryView view){
    auto lower=fold(path);std::size_t classes=0;
    constexpr std::u16string_view machine=u"\x0002\\software",machine_classes=u"\x0002\\software\\classes",user_classes=u"\x0001\\software\\classes";
    const std::u16string classes_alias=std::u16string(machine)+u"\\wow6432node\\classes";
    if(under(lower,classes_alias)){path=std::u16string(machine_classes)+u"\\wow6432node"+path.substr(classes_alias.size());lower=fold(path);}
    if(under(lower,machine_classes))classes=machine_classes.size();
    else if(under(lower,user_classes))classes=user_classes.size();
    else if(!lower.empty() && lower[0]==0)classes=1;
    else if(lower.starts_with(u"\x0003\\")){
        const auto sid=lower.find(u'\\',2);if(sid!=lower.npos){const auto root=lower.substr(0,sid)+u"\\software\\classes";if(under(lower,root))classes=root.size();}
    }
    if(classes){
        // 現代 Windows 的 Classes 根共用；只有列出的 COM／媒體子樹轉向。
        const auto tail=std::u16string_view(lower).substr(classes);
        if(under(tail,u"\\wow6432node")){
            const auto remaining=tail.substr(12);if(under(remaining,u"\\appid") || under(remaining,u"\\protocols") || under(remaining,u"\\typelib"))path.erase(classes,12);
            return path;
        }
        constexpr std::array redirected{u"\\clsid",u"\\directshow",u"\\interface",u"\\media type",u"\\mediafoundation"};
        if(view==RegistryView::Wow32)for(const auto root:redirected)if(under(tail,root)){path.insert(classes,u"\\wow6432node");break;}
        return path;
    }
    if(view!=RegistryView::Wow32 || !under(lower,machine))return path;
    const auto tail=std::u16string_view(lower).substr(machine.size());
    // 保留官方的直接實體路徑相容入口，避免 Wow6432Node 重複轉向。
    if(under(tail,u"\\wow6432node"))return path;
    constexpr std::array shared{
        u"\\clients",u"\\policies",u"\\registeredapplications",u"\\microsoft\\com3",
        u"\\microsoft\\cryptography\\calais\\current",u"\\microsoft\\cryptography\\calais\\readers",u"\\microsoft\\cryptography\\services",
        u"\\microsoft\\ctf\\systemshared",u"\\microsoft\\ctf\\tip",u"\\microsoft\\dfs",u"\\microsoft\\driver signing",
        u"\\microsoft\\enterprisecertificates",u"\\microsoft\\eventsystem",u"\\microsoft\\msmq",u"\\microsoft\\non-driver signing",
        u"\\microsoft\\notepad\\defaultfonts",u"\\microsoft\\ole",u"\\microsoft\\ras",u"\\microsoft\\rpc",
        u"\\microsoft\\software\\microsoft\\shared tools\\msinfo",u"\\microsoft\\systemcertificates",u"\\microsoft\\termservlicensing",u"\\microsoft\\transactionserver",
        u"\\microsoft\\windows\\currentversion\\app paths",u"\\microsoft\\windows\\currentversion\\control panel\\cursors\\schemes",
        u"\\microsoft\\windows\\currentversion\\explorer\\autoplayhandlers",u"\\microsoft\\windows\\currentversion\\explorer\\driveicons",u"\\microsoft\\windows\\currentversion\\explorer\\kindmap",
        u"\\microsoft\\windows\\currentversion\\group policy",u"\\microsoft\\windows\\currentversion\\policies",u"\\microsoft\\windows\\currentversion\\previewhandlers",u"\\microsoft\\windows\\currentversion\\setup",u"\\microsoft\\windows\\currentversion\\telephony\\locations",
        u"\\microsoft\\windows nt\\currentversion\\console",u"\\microsoft\\windows nt\\currentversion\\fontdpi",u"\\microsoft\\windows nt\\currentversion\\fontlink",u"\\microsoft\\windows nt\\currentversion\\fontmapper",u"\\microsoft\\windows nt\\currentversion\\fonts",u"\\microsoft\\windows nt\\currentversion\\fontsubstitutes",u"\\microsoft\\windows nt\\currentversion\\gre_initialize",
        u"\\microsoft\\windows nt\\currentversion\\image file execution options",u"\\microsoft\\windows nt\\currentversion\\language pack",u"\\microsoft\\windows nt\\currentversion\\networkcards",u"\\microsoft\\windows nt\\currentversion\\perflib",u"\\microsoft\\windows nt\\currentversion\\ports",u"\\microsoft\\windows nt\\currentversion\\print",u"\\microsoft\\windows nt\\currentversion\\profilelist",u"\\microsoft\\windows nt\\currentversion\\time zones"};
    for(const auto root:shared)if(under(tail,root))return path;
    path.insert(machine.size(),u"\\wow6432node");return path;
}
void initialize_registry_views(RegistryState& state){
    // 視圖根存在於系統命名空間；空的新視圖不複製另一視圖的使用者值。
    for(const auto root:{u"\x0002\\software",u"\x0002\\software\\classes",u"\x0001\\software\\classes"})if(state.keys.contains(root))state.keys.try_emplace(std::u16string(root)+u"\\wow6432node");
}
std::vector<std::u16string> registry_subkeys(const RegistryState& state,const RegistryHandle& handle){
    const auto& logical=registry_logical_path(handle);std::set<std::u16string_view> candidates;
    const auto redirected=logical+u"\\wow6432node";
    const std::array<std::u16string_view,3> bases{handle.path,logical,redirected};
    for(auto base=bases.begin();base!=bases.end();++base){
        if(std::find(bases.begin(),base,*base)!=base)continue;
        auto prefix=std::u16string(*base);prefix+=u'\\';
        for(auto it=state.keys.lower_bound(prefix);it!=state.keys.end() && it->first.starts_with(prefix);){
            const auto tail=std::u16string_view(it->first).substr(prefix.size());const auto slash=tail.find(u'\\');
            candidates.insert(tail.substr(0,slash));
            if(slash==tail.npos){++it;continue;}
            // 只略過同一子鍵的後代；"a["、"a]" 等相鄰名稱仍須保留。
            auto next=it->first.substr(0,prefix.size()+slash+1);next.back()=u'\\'+1;
            it=state.keys.lower_bound(next);
        }
    }
    std::vector<std::u16string> result;result.reserve(candidates.size());
    for(const auto& child:candidates){
        if(handle.view==RegistryView::Wow32 && child==u"wow6432node" && logical.find(u"\\wow6432node")==logical.npos)continue;
        auto path=logical+u"\\";path+=child;
        auto physical=registry_view_path(std::move(path),handle.view);if(state.keys.contains(physical))result.push_back(std::move(physical));
    }
    return result;
}
}
