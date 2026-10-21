#pragma once
#include "x64arm/com_marshal.hpp"
#include "x64arm/com_exporter.hpp"
#include <string>
namespace x64arm {
struct ComMarshalObject {
    std::uint64_t outer{};std::uint32_t references{1},pins{};bool standard{};
    bool operator==(const ComMarshalObject&)const=default;
};
struct ComMarshalOperation {
    enum class Kind { Forward,Foreground,Marshal,Unmarshal,ReleaseData,Disconnect,PublicMarshal,PublicUnmarshal,PublicRelease,InterThread,GetAndRelease } kind{Kind::Forward};
    unsigned step{};std::string symbol;
    std::uint64_t marshal{},object{},stream{},output{},scratch{},reference{},token{},result{},owner{},temporary{},interface{},delivered{},context_pointer{};
    std::uint32_t error{},flags{},context{};
    bool awaiting{},held{},stream_owned{},temporary_owned{},interface_owned{},delivered_owned{},packet_owned{},factory_owned{};
    std::uint64_t canonical{},generation{};bool canonical_owned{},remote{},exported{};
    std::uint64_t export_operation{};
    std::optional<com_marshal::RemoteWire> remote_wire;
    WindowsGuid iid{};
    bool operator==(const ComMarshalOperation&)const=default;
};
struct ComMarshalBridge {
    com_marshal::Registry packets;
    com_exporter::Registry exports;
    std::map<std::pair<std::uint64_t,std::uint64_t>,std::uint64_t> object_generations;
    std::uint64_t next_object_generation{1};
    std::map<std::uint64_t,ComMarshalObject> objects;
    std::map<std::uint64_t,ComMarshalOperation> operations;
    std::uint64_t unknown_table{},marshal_table{},standard_table{},next{1};
    bool operator==(const ComMarshalBridge&)const=default;
};
}
