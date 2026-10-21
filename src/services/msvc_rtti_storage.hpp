#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace x64arm::msvc_rtti {
struct Base {
    std::string name;
    std::uint32_t children{};
    std::int32_t member{},vbptr{-1},vbtable{};
    std::uint32_t flags{64};
};
struct Shape {
    std::vector<Base> bases;
    std::uint32_t flags{},offset{},constructor_displacement{};
};
// 所有 RTTI reference 在 x64 是同一個 metadata image 的 RVA，在 x86 是絕對位址。
// vtable 的 -1 slot 由使用者連到 locator；metadata 不包含任何宿主指標。
class Storage {
public:
    explicit Storage(unsigned pointer_bytes):p_(pointer_bytes){bytes.resize(64);}
    std::vector<std::uint8_t> bytes;
    std::map<std::string,std::uint32_t,std::less<>> types;
    std::uint32_t add(const Shape& shape){
        if(shape.bases.empty() || shape.bases.front().children+1!=shape.bases.size())throw std::invalid_argument("RTTI hierarchy 沒有完整 root");
        const auto hierarchy=allocate(16),array=allocate(shape.bases.size()*4),locator=allocate(p_==8?24:20);
        hierarchies_[shape.bases.front().name]=hierarchy;
        put(hierarchy+4,shape.flags);put(hierarchy+8,shape.bases.size());reference(hierarchy+12,array);
        for(std::size_t i=0;i<shape.bases.size();++i){const auto& base=shape.bases[i];if(base.children>=shape.bases.size()-i)throw std::invalid_argument("RTTI base 子樹越界");
            const auto descriptor=allocate(28);reference(array+i*4,descriptor);reference(descriptor,type(base.name));put(descriptor+4,base.children);
            put(descriptor+8,static_cast<std::uint32_t>(base.member));put(descriptor+12,static_cast<std::uint32_t>(base.vbptr));put(descriptor+16,static_cast<std::uint32_t>(base.vbtable));put(descriptor+20,base.flags);
            // 目前所有 STL 路徑都是 public；各 base 自己的子樹提供 attribute 查詢。
            const auto own=allocate(16),own_array=allocate((base.children+1)*4);put(own+4,base.children?shape.flags:0);put(own+8,base.children+1);reference(own+12,own_array);reference(descriptor+24,own);
            own_references_.emplace_back(descriptor+24,base.name);
            own_arrays_.push_back({own_array,array+static_cast<std::uint32_t>(i)*4,base.children+1});
        }
        put(locator,p_==8?1:0);put(locator+4,shape.offset);put(locator+8,shape.constructor_displacement);reference(locator+12,type(shape.bases.front().name));reference(locator+16,hierarchy);if(p_==8)reference(locator+20,locator);
        return locator;
    }
    void relocate(std::uint64_t image){
        // 子樹沿用相同 descriptors；attrs 不因 complete-object 路徑改成 private。
        for(const auto& item:own_arrays_)for(unsigned i=0;i<item[2];++i)reference(item[0]+i*4,word(item[1]+i*4));
        for(const auto& [cell,type]:own_references_)if(const auto found=hierarchies_.find(type);found!=hierarchies_.end())put(cell,found->second);
        if(p_==4)for(const auto at:references_){const auto value=image+word(at);if(value>UINT32_MAX)throw std::overflow_error("Win32 RTTI metadata 超過位址範圍");put(at,value);}
    }
private:
    unsigned p_;std::vector<std::uint32_t> references_;std::vector<std::array<std::uint32_t,3>> own_arrays_;
    std::map<std::string,std::uint32_t,std::less<>> hierarchies_;std::vector<std::pair<std::uint32_t,std::string>> own_references_;
    std::uint32_t allocate(std::size_t size){while(bytes.size()%4)bytes.push_back(0);if(bytes.size()>UINT32_MAX || size>UINT32_MAX-bytes.size())throw std::bad_alloc{};const auto offset=static_cast<std::uint32_t>(bytes.size());bytes.resize(bytes.size()+size);return offset;}
    void put(std::size_t at,std::uint64_t value){for(unsigned i=0;i<4;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(8*i));}
    std::uint32_t word(std::size_t at)const{std::uint32_t value=0;for(unsigned i=0;i<4;++i)value|=std::uint32_t{bytes[at+i]}<<(8*i);return value;}
    void reference(std::size_t at,std::uint32_t value){
        if(at>UINT32_MAX || at>bytes.size() || bytes.size()-at<4)throw std::invalid_argument("RTTI reference 超出 metadata 範圍");
        put(at,value);references_.push_back(static_cast<std::uint32_t>(at));
    }
    std::uint32_t type(const std::string& name){
        if(const auto found=types.find(name);found!=types.end())return found->second;while(bytes.size()%p_)bytes.push_back(0);
        const auto offset=allocate(2*p_+name.size()+1);for(std::size_t i=0;i<name.size();++i)bytes[offset+2*p_+i]=static_cast<std::uint8_t>(name[i]);types.emplace(name,offset);return offset;
    }
};
}
