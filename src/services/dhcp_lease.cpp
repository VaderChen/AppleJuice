#include "x64arm/dhcp_lease.hpp"
#include <algorithm>
#include <map>
#include <mutex>
#include <stdexcept>

namespace x64arm::dhcp {
namespace {
class Controller final:public Backend {
    std::shared_ptr<Platform> platform_;
    unsigned polls_;
    std::mutex mutex_;
    std::map<std::string,std::vector<Service>> released_;
    Result wait_for(std::string_view name,const std::vector<Service>& previous,bool renew,bool changed){
        for(unsigned n=0;n<polls_;++n){
            const auto current=platform_->snapshot(name);
            if(current.error)return {current.error,changed};
            if(!current.interface_exists)return {87,changed};
            bool done=true;
            for(const auto& old:previous){
                const auto item=std::find_if(current.services.begin(),current.services.end(),[&](const auto& s){return s.id==old.id;});
                if(renew){
                    if(item==current.services.end() || !item->dhcp || !item->configured ||
                       item->obtained<=old.obtained || item->expires<=item->obtained || item->addresses.empty() ||
                       !std::any_of(item->addresses.begin(),item->addresses.end(),[&](const auto& ip){return std::find(current.addresses.begin(),current.addresses.end(),ip)!=current.addresses.end();}))done=false;
                }else{
                    if(item!=current.services.end() && (item->configured || item->obtained>0))done=false;
                    for(const auto& ip:old.addresses)if(std::find(current.addresses.begin(),current.addresses.end(),ip)!=current.addresses.end())done=false;
                }
            }
            if(done)return {0,changed};
            if(n+1<polls_)platform_->wait();
        }
        return {1460,changed};
    }
public:
    Controller(std::shared_ptr<Platform> p,unsigned polls):platform_(std::move(p)),polls_(polls){if(!platform_ || !polls_)throw std::invalid_argument("DHCP 後端參數無效");}
    ~Controller() override {
        // 引擎正常退出時恢復自己移除的設定；restore 不覆蓋外部的新設定。
        for(const auto& [name,services]:released_){(void)name;for(const auto& s:services)try{(void)platform_->restore(s);}catch(...) {}}
    }
    Result change(std::string_view name,bool renew) override {
        std::lock_guard lock(mutex_);
        if(name.empty() || name.size()>127 || name.find('\0')!=name.npos)return {87,false};
        auto current=platform_->snapshot(name);
        if(current.error)return {current.error,false};
        if(!current.interface_exists)return {87,false};
        std::vector<Service> services;
        for(const auto& s:current.services)if(s.dhcp)services.push_back(s);
        if(services.empty())return {87,false};
        const std::string key(name);
        if(!renew){
            std::vector<Service> configured;for(const auto& s:services)if(s.configured)configured.push_back(s);
            if(configured.empty()){
                const auto entry=released_.find(key);
                return wait_for(name,entry==released_.end()?services:entry->second,false,false);
            }
            // 先配置復原記錄，避免宿主修改成功後因記憶體不足遺失設定。
            auto [entry,inserted]=released_.try_emplace(key);
            auto saved=entry->second;auto next=saved;
            for(const auto& s:configured){const auto old=std::find_if(next.begin(),next.end(),[&](const auto& p){return p.id==s.id;});if(old==next.end())next.push_back(s);else *old=s;}
            entry->second=std::move(next);
            const auto failure=platform_->remove(configured);
            if(failure){if(inserted)released_.erase(entry);else entry->second=std::move(saved);return {failure,false};}
            return wait_for(name,entry->second,false,true);
        }
        // 使用釋放時的租約作基準，不能把舊的 State 快照誤當成續租成功。
        if(const auto entry=released_.find(key);entry!=released_.end()){
            for(auto& s:services){
                const auto old=std::find_if(entry->second.begin(),entry->second.end(),[&](const auto& p){return p.id==s.id;});
                if(old!=entry->second.end()){
                    s.obtained=std::max(s.obtained,old->obtained);
                    if(!s.configured){s.settings=old->settings;s.preferences=old->preferences;}
                }
            }
        }
        bool changed=false;
        for(const auto& s:services){
            const auto error=s.configured?platform_->refresh(s):platform_->restore(s);
            if(error)return {error,changed};
            changed=true;
        }
        auto result=wait_for(name,services,true,changed);
        if(!result.error)released_.erase(key);
        return result;
    }
};
}
std::shared_ptr<Backend> backend(std::shared_ptr<Platform> p,unsigned polls){return std::make_shared<Controller>(std::move(p),polls);}
std::shared_ptr<Backend> native_backend(){return backend(native_platform());}
}
