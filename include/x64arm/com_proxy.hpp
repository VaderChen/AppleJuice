#pragma once
#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace x64arm::com_proxy {
enum class Kind : std::uint8_t { Unknown,DataObject,EnumFormats,EnumStatData,AdviseSink,SequentialStream,Stream,Storage,EnumStatStg,Moniker,EnumMoniker,BindContext,RunningObjectTable,EnumString };
enum class Status { Ok,Missing,Invalid,Limit,Disconnected,WrongApartment,Busy };
struct Key {
    std::uint64_t owner{},apartment{},original{};
    Kind kind{Kind::Unknown};
    auto operator<=>(const Key&) const = default;
};
struct Origin {
    std::uint64_t owner{},object{};
    bool operator==(const Origin&) const = default;
};
struct Entry {
    Key key{};
    std::uint32_t references{1},pins{};
    bool connected{true},origin_owned{true};
    bool operator==(const Entry&) const = default;
};
struct Retirement {
    Origin origin{};
    bool held{true},started{};
    bool operator==(const Retirement&) const = default;
};
struct Disposal {
    std::uint64_t header{},retirement{};
    bool operator==(const Disposal&) const = default;
};
struct Result {
    Status status{Status::Ok};
    std::uint64_t address{},retirement{};
    std::uint32_t references{};
    std::optional<Disposal> disposal{};
    bool operator==(const Result&) const = default;
};
struct Disconnect {
    std::vector<Origin> releases;
    bool operator==(const Disconnect&) const = default;
};

// 每個 apartment／介面快取持有來源的一份實際引用；client AddRef／Release 在本地計數。
// 已開始的方法以 pin 延後最後一份來源引用的釋放，允許 server 回呼 client 後重入 Release。
// Retirement 在來源執行緒真正開始 Release 前仍保有引用，讓斷線清理能辨認尚未派送的工作。
class Registry {
public:
    explicit Registry(std::size_t limit=4096):limit_(limit){}
    const Entry* get(std::uint64_t address) const;
    std::uint64_t find(const Key&) const;
    Result adopt(const Key&,std::uint64_t header);
    Result add_ref(std::uint64_t address);
    Result release(std::uint64_t address);
    Result pin(std::uint64_t address,std::uint64_t apartment);
    Result unpin(std::uint64_t address);
    Result retire_owned(Origin);
    std::optional<Origin> start_retirement(std::uint64_t id);
    bool finish_retirement(std::uint64_t id);
    Disconnect disconnect(std::uint64_t owner);
    const std::map<std::uint64_t,Entry>& entries() const {return entries_;}
    const std::map<std::uint64_t,Retirement>& retirements() const {return retirements_;}
    bool valid() const;
    bool operator==(const Registry&) const = default;
private:
    std::size_t limit_{};
    std::uint64_t next_{1};
    std::map<std::uint64_t,Entry> entries_;
    std::map<Key,std::uint64_t> keys_;
    std::map<std::uint64_t,Retirement> retirements_;
    Result dispose(std::map<std::uint64_t,Entry>::iterator);
};
}
