#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct addrinfo;
namespace x64arm {class SharedWake;}
namespace x64arm::socket_database {
enum class Kind {HostName,HostAddress,ProtocolName,ProtocolNumber,ServiceName,ServicePort,AddressInfo,NameInfo};
struct Query {
    Kind kind{};
    std::string name,protocol;
    std::vector<std::uint8_t> address;
    std::uint32_t number{},family{2};
    bool has_protocol{};
    // 現代 resolver 只持有宿主輸入快照，不能引用客體位址。
    bool has_name{},has_service{};
    int flags{},socket_type{},protocol_number{};
    unsigned service_capacity{};
};
struct Result {
    Kind kind{};
    std::string name,protocol;
    std::vector<std::string> aliases;
    std::vector<std::vector<std::uint8_t>> addresses;
    std::uint32_t number{},family{2},error{};
    std::shared_ptr<addrinfo> address_info;
};
struct Job {
    std::atomic_bool ready{},cancelled{};
    Result result;
    std::shared_ptr<SharedWake> wake;
};
Result lookup(const Query&);
std::shared_ptr<Job> submit(Query,std::shared_ptr<SharedWake> wake={});
std::vector<std::uint8_t> pack(const Result&,std::uint64_t base,bool x86);
}
