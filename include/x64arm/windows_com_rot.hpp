#pragma once
#include "x64arm/windows_com_bind.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace x64arm::rot {
using Bytes=std::vector<std::uint8_t>;
struct Error{std::uint32_t code;};
struct Scope{
    std::u16string sid,station;std::uint32_t session{};
    bool operator==(const Scope&)const=default;
};
struct Entry{
    std::uint32_t cookie{},flags{},native_pid{},pid{};std::uint64_t sequence{},thread{},changed{};
    std::array<std::uint8_t,16> owner{};Scope scope;Bytes comparison,object,moniker;
    bool operator==(const Entry&)const=default;
};
class SharedRot;
}
namespace x64arm {
struct ComRotEnumeration {
    std::uint32_t references{1},pins{};std::vector<ComBindReference> monikers;std::size_t cursor{};
    bool operator==(const ComRotEnumeration&)const=default;
};
struct ComRotOperation {
    enum class Kind {Register,IsRunning,GetObject,GetTime,Enumerate,Snapshot,Clone,Revoke} kind{};
    unsigned step{},resume_step{};std::uint32_t flags{},result{},transfer_result{},requested{},fetched{},cookie{};
    std::uint64_t scratch{},buffer{},object{},moniker{},reduced{},context{},rot_data{},stream{},delivered{},output{},count_output{},enumerator{},created_enumerator{},changed{};
    std::size_t candidate{},cursor{};rot::Bytes comparison,object_wire,moniker_wire;
    std::vector<rot::Entry> snapshot;std::vector<std::uint64_t> delivered_objects;
    bool duplicate{},object_marshaled{},moniker_marshaled{},stream_marshaled{},published{},candidate_owned{},object_held{},release_wire{},transfer_failed{},proxy_transfer{};
    bool operator==(const ComRotOperation&)const=default;
};
struct ComRotBridge {
    std::map<std::uint64_t,std::uint32_t> objects;
    std::map<std::uint64_t,ComRotEnumeration> enumerators;
    std::map<std::uint64_t,ComRotOperation> operations;
    std::uint64_t table{},enum_table{},next{1};
    bool operator==(const ComRotBridge&)const=default;
};
}
