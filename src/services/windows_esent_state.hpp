#pragma once
#include "x64arm/windows_services.hpp"
#include "x64arm/esent_database.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <set>

namespace x64arm {
namespace ese_api {
struct Error {std::int32_t code;};
inline void require(bool value,std::int32_t code=-1003){if(!value)throw Error{code};}
inline std::int32_t format_error(const esent::Error& error){
    switch(error.failure){
    case esent::Failure::InvalidHeader:return -1028;
    case esent::Failure::Checksum:return -1018;
    case esent::Failure::Limit:return -1011;
    case esent::Failure::Unsupported:return -1001;
    case esent::Failure::MissingTable:return -1305;
    case esent::Failure::MissingColumn:return -1507;
    case esent::Failure::Corrupt:return -1206;
    }return -1206;
}
inline std::u16string input(const GuestMemory& memory,std::uint64_t address,bool wide){
    return address?(wide?read_utf16(memory,GuestAddress{address},32768):utf8_to_utf16(read_ansi(memory,address))):std::u16string{};
}
inline std::u16string folded(std::u16string text){for(auto& c:text)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);return text;}
inline void writable(GuestMemory& memory,std::uint64_t address,std::size_t size){require(address!=0);memory.check(GuestAddress{address},size,Access::Write);}
inline std::int32_t text_output(GuestMemory& memory,std::u16string_view text,bool wide,std::uint64_t address,std::uint32_t capacity){
    require(address!=0);
    const auto narrow=wide?std::string{}:utf16_to_utf8(text);const auto unit=wide?2U:1U;
    const auto length=wide?text.size():narrow.size();const auto count=static_cast<std::size_t>(capacity/unit);
    if(count){const auto copied=std::min(length,count-1);if(wide)memory.write(GuestAddress{address},utf16_bytes(text.substr(0,copied)));
        else {std::vector<std::uint8_t> data(narrow.begin(),narrow.begin()+static_cast<std::ptrdiff_t>(copied));data.push_back(0);memory.write(GuestAddress{address},data);}}
    return count>length?0:-1038;
}
}

// 所有控制代碼皆屬客體程序；資料檔由 Windows 檔案服務持有，沒有宿主路徑旁路。
struct EsentState {
    struct Parameter {std::uint64_t number{};std::u16string text;bool operator==(const Parameter&)const=default;};
    using Parameters=std::map<std::uint32_t,Parameter>;
    struct Instance {std::u16string name,display;bool initialized{};Parameters parameters;bool operator==(const Instance&)const=default;};
    struct Open {std::uint32_t references{},flags{};bool operator==(const Open&)const=default;};
    struct Session {std::uint64_t instance{},busy{};std::map<std::uint32_t,Open> databases;bool operator==(const Session&)const=default;};
    struct Document {
        std::uint64_t instance{},file{};std::u16string path,key;std::uint32_t flags{},maximum_pages{};esent::Database data;
        bool operator==(const Document&)const=default;
    };
    struct Cursor {
        std::uint64_t instance{},session{};std::uint32_t database{},flags{};std::string table;
        std::vector<esent::Row> rows;std::int64_t position{};bool operator==(const Cursor&)const=default;
    };
    enum class Phase {Start,Opened,Size,Read};
    struct Operation {
        Phase phase{Phase::Start};std::uint64_t instance{},session{},file{},scratch{},buffer{};
        std::u16string path,key;std::uint32_t flags{},maximum_pages{},saved_error{},requested{};
        std::size_t offset{};esent::Binary bytes;bool operator==(const Operation&)const=default;
    };
    Parameters defaults{{0,{0,u".\\"}},{1,{0,u".\\"}},{2,{0,u".\\"}},{3,{0,u"edb"}},
                        {5,{16,{}}},{6,{300,{}}},{8,{1024,{}}},{34,{0,u"on"}},{102,{0,{}}}};
    std::map<std::uint64_t,Instance> instances;std::map<std::uint64_t,Session> sessions;
    std::map<std::uint32_t,Document> documents;std::map<std::uint64_t,Cursor> cursors;
    std::map<std::uint64_t,Operation> operations;
    std::uint64_t next_handle{1},next_operation{1},legacy_instance{};std::uint32_t next_database{1},page_size{4096},maximum_instances{16};bool multi_instance{};
    bool operator==(const EsentState&)const=default;
    std::uint64_t handle(){ese_api::require(next_handle<UINT32_MAX,-1011);return next_handle++;}
    Instance& instance(std::uint64_t id){if(!id && !multi_instance)id=legacy_instance;auto it=instances.find(id);ese_api::require(it!=instances.end(),-1115);return it->second;}
    Session& session(std::uint64_t id){auto it=sessions.find(id);ese_api::require(it!=sessions.end(),-1104);ese_api::require(instance(it->second.instance).initialized,-1029);ese_api::require(!it->second.busy,-1910);return it->second;}
    Document& document(std::uint64_t session_id,std::uint32_t id){auto& s=session(session_id);auto it=documents.find(id);ese_api::require(it!=documents.end() && it->second.instance==s.instance,-1010);ese_api::require(s.databases.contains(id),-1203);return it->second;}
    Cursor& cursor(std::uint64_t session_id,std::uint64_t id){(void)session(session_id);auto it=cursors.find(id);ese_api::require(it!=cursors.end() && it->second.session==session_id,-1310);return it->second;}
    void end_session(std::uint64_t id){std::erase_if(cursors,[&](const auto& entry){return entry.second.session==id;});sessions.erase(id);}
    std::uint64_t add_session(std::uint64_t instance_id){
        auto& inst=instance(instance_id);ese_api::require(inst.initialized,-1029);
        const auto count=std::count_if(sessions.begin(),sessions.end(),[&](const auto& entry){return entry.second.instance==instance_id;});
        ese_api::require(static_cast<std::uint64_t>(count)<inst.parameters.at(5).number,-1101);
        const auto id=handle();Session s;s.instance=instance_id;sessions.emplace(id,std::move(s));return id;
    }
    std::uint64_t add_cursor(Cursor value){
        const auto& params=instance(value.instance).parameters;std::set<std::pair<std::uint32_t,std::string>> tables;std::size_t count=0;
        for(const auto& [id,c]:cursors){(void)id;if(c.instance==value.instance){++count;tables.emplace(c.database,c.table);}}
        ese_api::require(count<params.at(8).number,-1013);tables.emplace(value.database,value.table);
        ese_api::require(tables.size()<=params.at(6).number,-1311);
        const auto id=handle();cursors.emplace(id,std::move(value));return id;
    }
};
}
