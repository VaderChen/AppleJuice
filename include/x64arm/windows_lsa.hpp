#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>
namespace x64arm {
struct LsaSession {
    std::uint64_t id{},time{};
    std::uint32_t type{},session{};
    std::u16string user,domain,full_name;
    std::vector<std::uint8_t> sid;
    bool operator==(const LsaSession&)const=default;
};
struct LsaState {
    // 已註冊的連線保留登入程序名稱；一般連線沒有名稱。
    std::map<std::uint64_t,std::optional<std::string>> connections;
    std::set<std::uint64_t> buffers;
    std::map<std::uint64_t,LsaSession> sessions;
    bool operator==(const LsaState&)const=default;
};
}
