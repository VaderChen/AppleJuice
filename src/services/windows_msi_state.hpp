#pragma once
#include "x64arm/windows_services.hpp"
#include "x64arm/msi_query.hpp"
#include "x64arm/unicode.hpp"

namespace x64arm {
namespace msi_api {
inline std::u16string input(const GuestMemory& memory,std::uint64_t address,bool wide){return address?(wide?read_utf16(memory,GuestAddress{address},1048576):utf8_to_utf16(read_ansi(memory,address))):std::u16string{};}
inline std::uint32_t output(GuestMemory& memory,std::u16string_view text,bool wide,std::uint64_t buffer,std::uint64_t size){
    if(!size)return buffer?87U:0U;memory.check(GuestAddress{size},4,Access::Write);const auto capacity=memory.load(GuestAddress{size},32);
    const auto bytes=wide?std::string{}:utf16_to_utf8(text);const auto length=wide?text.size():bytes.size();
    if(length>UINT32_MAX)throw msi::Error{8,"MSI 字串超過資源上限"};
    if(buffer && capacity){const auto copied=std::min<std::uint64_t>(capacity-1,length);const auto count=static_cast<std::size_t>(copied);
        if(wide)memory.write(GuestAddress{buffer},utf16_bytes(text.substr(0,count)));
        else {std::vector<std::uint8_t> data(bytes.begin(),bytes.begin()+static_cast<std::ptrdiff_t>(count));data.push_back(0);memory.write(GuestAddress{buffer},data);}}
    memory.store(GuestAddress{size},32,length);return buffer && length && capacity<=length?234U:0U;
}
}
struct MsiState {
    enum class Type {Record,Database,View};
    struct Object {
        Type type{Type::Record};std::uint64_t owner{},document{};
        std::vector<msi::Cell> fields;std::map<std::uint32_t,std::size_t> positions;
        msi::Query query;msi::Result result;std::size_t cursor{};bool executed{};
        bool operator==(const Object&)const=default;
    };
    struct Document {
        msi::Database data;std::vector<std::uint8_t> committed;
        std::u16string path;std::uint64_t file{},busy{};std::uint32_t mode{};bool failed{};
        bool operator==(const Document&)const=default;
    };
    enum class Kind {Open,Stream,Commit,Product,Component};
    enum class Phase {Start,Opened,Size,Read,Destination,SeekBody,WriteBody,FlushBody,SeekHeader,WriteHeader,Flush,
                      RestoreSeek,RestoreWrite,RestoreEndSeek,RestoreEnd,RestoreFlush,RegistryStart,RegistryOpen,RegistrySize,RegistryRead,RegistryClose,ComponentAttributes,ComponentKey,ComponentValue};
    struct Location {std::uint64_t root{};std::u16string path;bool installed{},machine{};bool operator==(const Location&)const=default;};
    struct Operation {
        Kind kind{Kind::Open};Phase phase{Phase::Start};std::uint64_t document{},file{},scratch{},buffer{},output{},size{},owner{};
        std::uint32_t mode{},record{},field{},saved_error{},error{},requested{},buffer_size{65536};
        std::size_t offset{},header_size{};bool owned{},modified{},patch{},wide{},published{},restoring{},restore_failed{},found_product{},found_installed{},known_property{},installed_property{},component_source{};
        std::u16string path,destination,property,value,product;std::vector<std::uint8_t> bytes,previous;msi::Database initial;
        std::vector<Location> locations;std::size_t location{};std::uint64_t registry_key{};std::uint32_t registry_type{},success{};
        bool operator==(const Operation&)const=default;
    };
    std::map<std::uint32_t,Object> objects;std::map<std::uint64_t,Document> documents;std::map<std::uint64_t,Operation> operations;
    std::uint32_t next_handle{1};std::uint64_t next_document{1},next_operation{1};
    bool operator==(const MsiState&)const=default;
    std::uint32_t add(Object value){if(objects.size()>=65536 || !next_handle)throw msi::Error{8,"MSI handle 超過資源上限"};const auto id=next_handle++;objects.emplace(id,std::move(value));return id;}
    Object& get(std::uint32_t handle,Type type){auto it=objects.find(handle);if(it==objects.end() || it->second.type!=type)throw msi::Error{6,"MSI handle 無效"};return it->second;}
};
}
