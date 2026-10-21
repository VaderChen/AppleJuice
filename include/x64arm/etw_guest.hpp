#pragma once
#include "x64arm/etw_state.hpp"
#include "x64arm/memory.hpp"

namespace x64arm::etw::guest {
struct Fault {std::uint32_t code;};
struct Layout {
    unsigned unit,properties,property_file,property_name,logfile,logfile_header,buffer_callback,
        buffer_size,filled,lost,event_callback,kernel,context,current_event,event_trace,event_record;
};
constexpr Layout layout(bool x86){
    return x86?Layout{4,120,108,112,416,112,384,388,392,396,400,404,408,24,88,104}:
               Layout{8,120,112,116,448,120,400,408,412,416,424,432,440,32,88,112};
}
Descriptor descriptor(const GuestMemory&,std::uint64_t);
// EVENT_DATA_DESCRIPTOR／MOF_FIELD 的 DataPtr 固定 64-bit，與 guest pointer 寬度無關。
void data(const GuestMemory&,Record&,std::uint32_t count,std::uint64_t descriptors,bool descriptor_type);
Record classic(const GuestMemory&,std::uint64_t header,bool x86);
Bytes logfile_header(const Header&,bool x86,std::uint64_t logger_name,std::uint64_t file_name);
std::size_t event_size(const Record&,bool x86,bool event_record);
Bytes event(const Record&,bool x86,bool event_record,std::uint64_t arena,std::uint64_t timestamp,std::uint64_t context);
}
