#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace x64arm::etw {
using Bytes = std::vector<std::uint8_t>;
using Guid = std::array<std::uint8_t,16>;
enum class Failure {InvalidFormat,Unsupported,TooLarge,InvalidParameter};
class Error final : public std::runtime_error {
public:
    Error(Failure value,const char* message):std::runtime_error(message),failure(value){}
    Failure failure;
};
struct Limits {
    std::size_t file_bytes{512U*1024U*1024U},records{262144},buffers{65536};
    std::uint32_t buffer_bytes{1024U*1024U};
};
struct Descriptor {
    std::uint16_t id{},task{};std::uint8_t version{},channel{},level{},opcode{};
    std::uint64_t keyword{};
    bool operator==(const Descriptor&)const=default;
};
struct Extension {
    std::uint16_t type{},flags{};Bytes data;
    bool operator==(const Extension&)const=default;
};
// System／Compact／PerfInfo 的 group/version 與額外 header 保留原值；不捏造 GUID。
enum class Kind {Modern,Classic,System,Compact,PerfInfo};
struct Record {
    Kind kind{Kind::Modern};bool x86{};
    std::uint16_t flags{},property{},version{};
    std::uint8_t group{};std::uint32_t thread{},process{};
    std::uint64_t timestamp{},processor_time{};
    Guid provider{},activity{};Descriptor descriptor;
    std::uint16_t processor{},logger{};
    Bytes extra_header,data;std::vector<Extension> extensions;
    bool operator==(const Record&)const=default;
};
struct Header {
    bool x86{};std::uint32_t buffer_size{65536},version{0x0000000a},build{19045},processors{1};
    std::uint32_t timer_resolution{156250},maximum_file_size{},mode{1},buffers_written{},start_buffers{1};
    std::uint32_t pointer_size{8},events_lost{},cpu_mhz{},clock{2},buffers_lost{};
    std::uint64_t end_time{},boot_time{},frequency{10000000},start_time{},start_timestamp{};
    std::array<std::uint8_t,172> timezone{};
    std::u16string session_name,file_name;
    bool operator==(const Header&)const=default;
};
struct Buffer {
    std::uint32_t size{},saved{},current{},filled{},state{};
    std::uint16_t flags{},type{},processor{},logger{};
    std::uint64_t timestamp{},sequence{};
    std::vector<Record> records;
};
struct File {Header header;std::vector<Buffer> buffers;};
// 序列化為真正未壓縮 ETL：WMI buffer + System logfile header + 原始事件。
// 檔案順序／buffer 邊界保留於 parse；事件排序交給 ProcessTrace 消費流程。
File parse(std::span<const std::uint8_t>,const Limits& = {});
Bytes serialize(const Header&,std::span<const Record>,const Limits& = {});
Record parse_record(std::span<const std::uint8_t>,std::uint16_t processor=0,std::uint16_t logger=0);
Bytes serialize_record(const Record&);
std::uint64_t filetime(const Header&,std::uint64_t raw_timestamp);
}
