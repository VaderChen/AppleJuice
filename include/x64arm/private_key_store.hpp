#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>
namespace x64arm {
class ReadOnlyFiles;
struct HostFile;
struct PrivateKeyStoreContext {ReadOnlyFiles& files;std::string user_sid;};
enum class PrivateKeyAccess { Application, LogonUser };
// 僅使用此客體的 C: 映射及虛擬 SID；不讀寫宿主鑰匙圈或家目錄。
std::uint32_t private_key_store_read(const PrivateKeyStoreContext&,std::uint32_t,const std::string&,bool,std::vector<std::uint8_t>&);
std::uint32_t private_key_store_write(const PrivateKeyStoreContext&,std::uint32_t,const std::string&,bool,std::span<const std::uint8_t>,bool create,PrivateKeyAccess access=PrivateKeyAccess::Application);
std::uint32_t private_key_store_modify(const PrivateKeyStoreContext&,std::uint32_t,const std::string&,bool,const std::function<std::uint32_t(std::vector<std::uint8_t>&)>&);
std::uint32_t private_key_store_delete(const PrivateKeyStoreContext&,std::uint32_t,const std::string&,bool);
std::uint32_t private_key_store_list(const PrivateKeyStoreContext&,std::uint32_t,bool,std::vector<std::string>&);
std::uint32_t private_key_store_security(const PrivateKeyStoreContext&,std::uint32_t,const std::string&,bool,std::uint32_t,const std::function<std::uint32_t(const HostFile&)>&);
}
