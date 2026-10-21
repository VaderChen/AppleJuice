#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <utility>

// GL 共用物件名稱空間（wglShareLists、share context）：紋理、display list、buffer、shader 與 program
// 在共用群組內以同一份表格存放。名稱 0 與後端保留的 UINT32_MAX 為各 context 自己的預設物件，
// 依 context 序號放在 GL 名稱範圍之外，不與共用名稱互相覆蓋。
namespace x64arm {
inline std::uint64_t next_gl_name_owner(){static std::uint64_t serial{};return ++serial;}
template<class Value,bool PerContextDefaults=false> class GlSharedNames {
    using Map=std::map<std::uint64_t,Value>;
    std::shared_ptr<Map> map_=std::make_shared<Map>();
    std::uint64_t owner_=next_gl_name_owner();
    bool local(std::uint64_t name)const{return PerContextDefaults && (name==0 || name==UINT32_MAX);}
    std::uint64_t key(std::uint64_t name)const{return local(name)?(owner_<<32)|name:name;}
    bool owned(std::uint64_t key)const{return PerContextDefaults && (key>>32)==owner_;}
public:
    using iterator=typename Map::iterator;
    GlSharedNames()=default;GlSharedNames(const GlSharedNames&)=delete;GlSharedNames& operator=(const GlSharedNames&)=delete;
    ~GlSharedNames(){if(PerContextDefaults && map_.use_count()>1)for(auto it=map_->begin();it!=map_->end();)it=owned(it->first)?map_->erase(it):std::next(it);}
    Value& operator[](std::uint64_t name){return (*map_)[key(name)];}
    Value& at(std::uint64_t name){return map_->at(key(name));}
    iterator find(std::uint64_t name){return map_->find(key(name));}
    iterator lower_bound(std::uint64_t name){return map_->lower_bound(key(name));}
    iterator begin(){return map_->begin();}
    iterator end(){return map_->end();}
    bool contains(std::uint64_t name)const{return map_->contains(key(name));}
    bool empty()const{return map_->empty();}
    std::size_t size()const{return map_->size();}
    std::size_t erase(std::uint64_t name){return map_->erase(key(name));}
    iterator erase(iterator it){return map_->erase(it);}
    template<class... Args> auto try_emplace(std::uint64_t name,Args&&... args){return map_->try_emplace(key(name),std::forward<Args>(args)...);}
    template<class... Args> auto emplace(std::uint64_t name,Args&&... args){return map_->emplace(key(name),std::forward<Args>(args)...);}
    template<class V> auto insert_or_assign(std::uint64_t name,V&& value){return map_->insert_or_assign(key(name),std::forward<V>(value));}
    // 群組內只剩本 context 的預設物件時，才可加入其他群組。
    bool unused()const{for(const auto& [name,value]:*map_)if(!owned(name))return false;return true;}
    bool same_group(const GlSharedNames& other)const{return map_==other.map_;}
    // 帶著自己的預設物件加入 source 的群組。
    void join(const GlSharedNames& source){
        if(map_==source.map_)return;
        if(PerContextDefaults)for(auto& [name,value]:*map_)if(owned(name))source.map_->insert_or_assign(name,std::move(value));
        map_=source.map_;
    }
};
// 共用群組的名稱配置器；複製語義與 unsigned 相同，但遞增會反映到群組內所有 context。
class GlSharedCounter {
    std::shared_ptr<unsigned> value_;
public:
    GlSharedCounter(unsigned initial=1):value_(std::make_shared<unsigned>(initial)){}
    GlSharedCounter(const GlSharedCounter&)=delete;GlSharedCounter& operator=(const GlSharedCounter&)=delete;
    operator unsigned()const{return *value_;}
    GlSharedCounter& operator=(unsigned value){*value_=value;return *this;}
    unsigned operator++(int){return (*value_)++;}
    GlSharedCounter& operator++(){++*value_;return *this;}
    void join(const GlSharedCounter& source){if(*source.value_<*value_)*source.value_=*value_;value_=source.value_;}
};
}
