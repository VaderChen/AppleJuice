#include "x64arm/windows_services.hpp"
#include <algorithm>
#include <functional>

namespace x64arm {
std::optional<std::uint32_t> WindowsServices::invoke_ui_helpers(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);std::uint64_t result=0;
    if(service.symbol=="IsWindowUnicode"){
        const auto window=gui_.windows.find(arg(0));
        if(arg(0)==gui_.desktop)result=1;
        else if(window==gui_.windows.end())last_error_=1400;
        else result=window->second.unicode;
    }else{
        const auto dialog=arg(0),control=arg(1)==dialog?0:arg(1);const bool previous=static_cast<std::uint32_t>(arg(2))!=0;
        if(!gui_.windows.contains(dialog) || (control && !gui_.windows.contains(control)))last_error_=1400;
        else if(!control && previous && service.symbol!="GetNextDlgGroupItem")result=0;
        else {
            // z_order 是兄弟的實際順序；可見且啟用的 CONTROLPARENT 才開放其子階層。
            // 保留所有節點的位置，讓沒有 TABSTOP 或暫時停用的起點仍可找相鄰控制項。
            std::map<std::uint64_t,std::vector<std::uint64_t>> children;
            std::set<std::uint64_t> ordered;
            for(auto handle:gui_.z_order)if(const auto w=gui_.windows.find(handle);w!=gui_.windows.end() && ordered.insert(handle).second)children[w->second.parent].push_back(handle);
            for(const auto& [handle,window]:gui_.windows)if(!ordered.contains(handle))children[window.parent].push_back(handle);
            if(service.symbol=="GetNextDlgGroupItem"){
                auto ancestor=control;std::size_t depth=0;
                while(ancestor && ancestor!=dialog && depth++<gui_.windows.size()){const auto w=gui_.windows.find(ancestor);if(w==gui_.windows.end())break;ancestor=w->second.parent;}
                if(control && ancestor!=dialog){last_error_=87;arg.return_to(state,0);return std::nullopt;}
                auto parent=control?gui_.windows.at(control).parent:dialog;
                for(std::size_t n=0;parent!=dialog && n<gui_.windows.size();++n){const auto p=gui_.windows.find(parent);if(p==gui_.windows.end() || !(p->second.ex_style&0x10000U))break;parent=p->second.parent;}
                struct GroupItem {std::uint64_t handle;bool eligible,group;};std::vector<GroupItem> items;std::set<std::uint64_t> visited{parent};
                std::function<void(std::uint64_t,bool)> collect=[&](std::uint64_t p,bool admitted){const auto siblings=children.find(p);if(siblings==children.end())return;
                    for(auto h:siblings->second){if(!visited.insert(h).second)continue;const auto& w=gui_.windows.at(h);const bool enabled=admitted && w.visible && w.enabled && !(w.style&0x08000000U),container=(w.ex_style&0x10000U)!=0;
                        items.push_back({h,enabled && !container,(w.style&0x20000U)!=0});if(container)collect(h,enabled);}}
                ;collect(parent,true);
                if(!items.empty()){
                    const auto at=std::find_if(items.begin(),items.end(),[&](const auto& item){return item.handle==control;});
                    if(control && at==items.end()){last_error_=87;arg.return_to(state,0);return std::nullopt;}
                    const auto index=control?static_cast<std::size_t>(at-items.begin()):previous?0:items.size()-1;
                    std::size_t begin=0,end=items.size();
                    if(control){for(std::size_t i=0;i<=index;++i)if(items[i].group)begin=i;
                        for(std::size_t i=index+1;i<items.size();++i)if(items[i].group){end=i;break;}}
                    const auto count=end-begin;
                    for(std::size_t step=1;step<=count;++step){const auto offset=index-begin;
                        const auto i=begin+(previous?(offset+count-step)%count:(offset+step)%count);
                        if(items[i].eligible){result=items[i].handle;break;}}
                    if(!result)result=control;
                }
                arg.return_to(state,result);return std::nullopt;
            }
            struct Item {std::uint64_t handle;bool eligible;};std::vector<Item> items;std::set<std::uint64_t> visited{dialog};
            std::function<void(std::uint64_t,bool)> collect=[&](std::uint64_t parent,bool admitted){
                const auto siblings=children.find(parent);if(siblings==children.end())return;
                for(auto handle:siblings->second){
                    if(!visited.insert(handle).second)continue;const auto& window=gui_.windows.at(handle);
                    const bool enabled=admitted && window.visible && window.enabled && !(window.style&0x08000000U);
                    const bool container=(window.ex_style&0x10000U)!=0;
                    items.push_back({handle,enabled && !container && (window.style&0x10000U)!=0});
                    collect(handle,enabled && container);
                }
            };
            collect(dialog,true);
            const auto start=std::find_if(items.begin(),items.end(),[&](const auto& item){return item.handle==control;});
            if(control && start==items.end())last_error_=87;
            else if(!items.empty()){
                const auto count=items.size();auto index=control?static_cast<std::size_t>(start-items.begin()):count-1;
                std::vector<std::uint64_t> ancestors;std::set<std::uint64_t> path;
                for(auto handle=control;handle && handle!=dialog && path.insert(handle).second;){
                    handle=gui_.windows.at(handle).parent;if(handle && handle!=dialog)ancestors.push_back(handle);
                }
                if(previous){
                    // 從一般／停用容器內起跳的反向搜尋，以最高層無法導覽的祖先為起點。
                    for(auto handle:ancestors){const auto& window=gui_.windows.at(handle);
                        if(!(window.ex_style&0x10000U) || !window.visible || !window.enabled || (window.style&0x08000000U))index=static_cast<std::size_t>(std::find_if(items.begin(),items.end(),[&](const auto& item){return item.handle==handle;})-items.begin());}
                    for(std::size_t step=1;step<=count;++step){const auto at=(index+count-step)%count;if(items[at].eligible){result=items[at].handle;break;}}
                }else if(control){
                    // 正向搜尋先走起點所在的兄弟；繞回根節點後，仍遵守正常容器准入條件。
                    std::set<std::uint64_t> local,seen;
                    std::function<void(std::uint64_t)> admit=[&](std::uint64_t parent){
                        const auto siblings=children.find(parent);if(siblings==children.end())return;
                        for(auto handle:siblings->second){if(!seen.insert(handle).second)continue;const auto& window=gui_.windows.at(handle);
                            if(!window.visible || !window.enabled || (window.style&0x08000000U))continue;
                            if(window.ex_style&0x10000U)admit(handle);else if(window.style&0x10000U)local.insert(handle);}
                    };
                    admit(gui_.windows.at(control).parent);for(auto handle:ancestors)admit(gui_.windows.at(handle).parent);
                    for(auto at=index+1;at<count;++at)if(items[at].eligible || local.contains(items[at].handle)){result=items[at].handle;break;}
                    if(!result)for(std::size_t at=0;at<=index;++at)if(items[at].eligible){result=items[at].handle;break;}
                }else for(const auto& item:items)if(item.eligible){result=item.handle;break;}
                if(!result)result=control;
            }else result=control;
        }
    }
    arg.return_to(state,result);return std::nullopt;
}
}
