// OLE 屬性集串流格式（MS-OLEPS）。屬性值保留序列化資料，包含字典與未知型別。
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace x64arm::property_set {
using Bytes = std::vector<std::uint8_t>;
using Guid = std::array<std::uint8_t, 16>;
struct Section {
    Guid format{};
    std::map<std::uint32_t, Bytes> properties;
    bool operator==(const Section&) const = default;
};
struct Stream {
    std::uint16_t version{};
    std::uint32_t system{0x00020006};
    Guid clsid{};
    std::vector<Section> sections;
    bool operator==(const Stream&) const = default;
};
struct Invalid : std::runtime_error {
    Invalid() : std::runtime_error("屬性集串流格式錯誤") {}
};
inline std::uint32_t read(std::span<const std::uint8_t> data, size_t offset, unsigned width = 4) {
    if (offset > data.size() || width > data.size() - offset) throw Invalid{};
    std::uint32_t value = 0;
    for (unsigned i = 0; i < width; ++i) value |= std::uint32_t(data[offset+i]) << (8*i);
    return value;
}
inline std::optional<unsigned> scalar_width(std::uint32_t type) {
    switch(type) {
    case 0:case 1:return 0;
    case 16:case 17:return 1;
    case 2:case 11:case 18:return 2;
    case 3:case 4:case 10:case 19:case 22:case 23:return 4;
    case 5:case 6:case 7:case 20:case 21:case 64:return 8;
    default:return std::nullopt;
    }
}
inline Stream decode(std::span<const std::uint8_t> data) {
    if (read(data, 0, 2) != 0xfffe) throw Invalid{};
    Stream result;
    result.version = static_cast<std::uint16_t>(read(data, 2, 2));
    result.system = read(data, 4);
    const auto count = read(data, 24);
    if (result.version > 1 || !count || count > 2 || data.size() < 28+20*count) throw Invalid{};
    std::copy_n(data.begin()+8, 16, result.clsid.begin());
    std::vector<std::pair<size_t,size_t>> ranges;
    for (unsigned i=0; i<count; ++i) {
        const size_t descriptor = 28+20*i;
        const size_t start = read(data, descriptor+16);
        if (start < 28+20*count || start%4 || start > data.size()) throw Invalid{};
        const size_t size = read(data, start);
        if (size < 8 || size%4 || size > data.size()-start) throw Invalid{};
        for (auto [begin,end] : ranges) if (start < end && begin < start+size) throw Invalid{};
        ranges.emplace_back(start,start+size);
        const auto section = data.subspan(start,size);
        const auto entries = read(section,4);
        if (entries > (size-8)/8) throw Invalid{};
        Section decoded;
        std::copy_n(data.data()+descriptor, 16, decoded.format.begin());
        std::vector<std::pair<size_t,std::uint32_t>> offsets;
        for (unsigned j=0; j<entries; ++j) {
            const auto id = read(section,8+8ULL*j);
            const auto offset = read(section,12+8ULL*j);
            if (offset%4 || offset < 8+8ULL*entries || offset >= size) throw Invalid{};
            offsets.emplace_back(offset,id);
        }
        std::sort(offsets.begin(),offsets.end());
        for (size_t j=0; j<offsets.size(); ++j) {
            const auto [offset,id] = offsets[j];
            const auto end = j+1<offsets.size() ? offsets[j+1].first : size;
            if (offset == end || !decoded.properties.try_emplace(id,section.data()+offset,section.data()+end).second)
                throw Invalid{};
        }
        result.sections.push_back(std::move(decoded));
    }
    return result;
}

inline Bytes encode(const Stream& stream) {
    if (stream.version > 1 || stream.sections.empty() || stream.sections.size() > 2) throw Invalid{};
    Bytes data(28+20*stream.sections.size());
    const auto put = [&](size_t at, size_t value, unsigned width=4) {
        if (value > UINT32_MAX) throw Invalid{};
        for (unsigned i=0; i<width; ++i) data[at+i] = static_cast<std::uint8_t>(value >> (8*i));
    };
    put(0,0xfffe,2);
    put(2,stream.version,2);
    put(4,stream.system);
    std::copy(stream.clsid.begin(),stream.clsid.end(),data.begin()+8);
    put(24,stream.sections.size());
    for (size_t i=0; i<stream.sections.size(); ++i) {
        const auto& section = stream.sections[i];
        const auto start = data.size();
        const auto descriptor = 28+20*i;
        std::copy(section.format.begin(),section.format.end(),data.data()+descriptor);
        put(descriptor+16,start);
        const auto entries = section.properties.size();
        if (start > UINT32_MAX-8 || entries > (UINT32_MAX-start-8)/8) throw Invalid{};
        data.resize(start+8+8*entries);
        put(start+4,entries);
        size_t slot = start+8;
        for (const auto& [id,value] : section.properties) {
            if (value.empty() || value.size()%4 || value.size() > UINT32_MAX-data.size()) throw Invalid{};
            put(slot,id);
            put(slot+4,data.size()-start);
            data.insert(data.end(),value.begin(),value.end());
            slot += 8;
        }
        put(start,data.size()-start);
    }
    return data;
}
}
