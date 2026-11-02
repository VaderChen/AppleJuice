#pragma once
#include "x64arm/wlan.hpp"
#include "x64arm/private_key_store.hpp"
#include "x64arm/readonly_files.hpp"
#include "x64arm/security_access.hpp"

namespace x64arm::wlan {
struct Error {std::uint32_t code,reason{};};
inline void require(bool value,std::uint32_t code=87,std::uint32_t reason=0){if(!value)throw Error{code,reason};}
inline void check(std::uint32_t code){if(code)throw Error{code};}
struct Profile {
    std::u16string name;
    std::string xml,key,key_type;
    Connection connection;
    bool automatic{},hidden{},key_protected{};
    std::uint32_t flags{};
    std::uint64_t order{};
    Bytes descriptor;
};
Profile parse_profile(std::string_view);
std::string profile_xml(const Profile&,const PrivateKeyStoreContext&,bool plaintext,bool renew=false);
std::string profile_key(const Profile&,const PrivateKeyStoreContext&);
std::string rename_profile_xml(const Profile&,std::u16string_view,bool automatic);
inline constexpr std::uint32_t read_access=0x20001,execute_access=0x20021,write_access=0x70023;
// 模擬 WLAN 服務的儲存身分只用於固定命名空間；每個操作仍先依原客體權杖檢查 profile DACL。
class Profiles {
    ReadOnlyFiles files_;
    VirtualToken caller_;
    std::string sid_,interface_;
    std::string key(std::u16string_view)const;
    std::optional<Profile> load(std::u16string_view,bool machine);
public:
    Profiles(const ReadOnlyFiles&,const VirtualToken&,std::string sid,std::string interface);
    PrivateKeyStoreContext context(){return {files_,sid_};}
    std::vector<Profile> list();
    Profile get(std::u16string_view,std::uint32_t access);
    std::uint32_t access(const Profile&,std::uint32_t desired)const;
    bool administrator()const;
    void save(Profile,bool overwrite,std::optional<std::string> security);
    void erase(std::u16string_view);
};
}
