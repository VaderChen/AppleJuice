#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace x64arm::authentication {
struct Identity {
    std::uint32_t uid{},gid{};
    std::vector<std::uint32_t> groups;
    std::u16string name,full_name;
};
struct Result {
    std::uint32_t status{},substatus{};
    std::optional<Identity> identity;
};
class Backend {
public:
    virtual ~Backend()=default;
    virtual Result verify(std::u16string_view user,std::u16string_view domain,std::u16string_view password)=0;
};
std::shared_ptr<Backend> native_backend();
// 只清除相容層自己的暫存；呼叫者的客體密碼由呼叫者管理。
struct Secret {
    std::u16string value;
    Secret()=default;
    Secret(const Secret&)=delete;
    Secret& operator=(const Secret&)=delete;
    ~Secret(){volatile char16_t* p=value.data();for(std::size_t i=0;i<value.size();++i)p[i]=0;}
};
struct ByteSecret {
    std::string value;
    ~ByteSecret(){volatile char* p=value.data();for(std::size_t i=0;i<value.size();++i)p[i]=0;}
};
std::uint32_t winerror(std::uint32_t status);
}
