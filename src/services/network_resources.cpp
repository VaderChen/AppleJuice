#include "x64arm/network_resources.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace x64arm::network_resources {
std::u16string canonical(std::u16string value){for(auto& c:value){if(c==u'/')c=u'\\';else if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);}while(value.size()>2 && value.back()==u'\\')value.pop_back();return value;}
std::u16string Path::remote()const{auto result=u"\\\\"+server;if(!share.empty())result+=u"\\"+share;return result;}
std::optional<Path> parse_path(std::u16string_view input){
    if(input.empty() || input.size()>32767 || input.find(char16_t{})!=input.npos)return {};
    std::u16string value(input);std::replace(value.begin(),value.end(),u'/',u'\\');
    if(value.starts_with(u"\\\\?\\UNC\\") || value.starts_with(u"\\\\?\\unc\\"))value=u"\\\\"+value.substr(8);
    if(!value.starts_with(u"\\\\") || value.size()<3)return {};
    const auto server_end=value.find(u'\\',2);const auto server=value.substr(2,server_end==value.npos?value.npos:server_end-2);
    if(server.empty() || server==u"." || server==u".." || server.find_first_of(u"@?#%")!=server.npos)return {};
    Path path;path.server=server;if(server_end==value.npos || server_end+1==value.size())return path;
    const auto share_end=value.find(u'\\',server_end+1);path.share=value.substr(server_end+1,share_end==value.npos?value.npos:share_end-server_end-1);
    if(path.share.empty() || path.share==u"." || path.share==u".." || path.share.find_first_of(u":@?#%")!=path.share.npos)return {};
    if(share_end!=value.npos)path.system=value.substr(share_end);
    for(std::size_t at=0;at<path.system.size();){const auto start=at+1;auto end=path.system.find(u'\\',start);if(end==path.system.npos)end=path.system.size();const auto part=std::u16string_view(path.system).substr(start,end-start);if(part==u".." || part.find(u':')!=part.npos)return {};at=end;}
    return path;
}
std::size_t packed_size(std::span<const PackedResource> values,unsigned unit){
    if(unit!=4 && unit!=8)throw std::invalid_argument("NETRESOURCE 指標寬度無效");
    if(values.size()>UINT32_MAX/(16+4*unit))throw std::length_error("NETRESOURCE 輸出過大");std::size_t total=values.size()*(16+4*unit);
    for(const auto& value:values){if(value.strings.size()!=4)throw std::invalid_argument("NETRESOURCE 字串欄位無效");for(const auto& bytes:value.strings){if(bytes.size()>UINT32_MAX-total)throw std::length_error("NETRESOURCE 輸出過大");total+=bytes.size();}}
    return total;
}
std::vector<std::uint8_t> pack(std::span<const PackedResource> values,unsigned unit,std::uint64_t address){
    const auto length=packed_size(values,unit);if(address>UINT64_MAX-length || (unit==4 && address+length>UINT32_MAX))throw std::length_error("NETRESOURCE 地址溢位");
    std::vector<std::uint8_t> bytes(length);const auto put=[&](std::size_t offset,std::uint64_t value,unsigned size){for(unsigned n=0;n<size;++n)bytes[offset+n]=static_cast<std::uint8_t>(value>>(n*8));};
    auto tail=values.size()*(16+4*unit);
    for(std::size_t i=0;i<values.size();++i){const auto offset=i*(16+4*unit);const auto& row=values[i];put(offset,row.value.scope,4);put(offset+4,row.value.type,4);put(offset+8,row.value.display,4);put(offset+12,row.value.usage,4);
        for(unsigned s=0;s<4;++s)if(!row.strings[s].empty()){put(offset+16+s*unit,address+tail,unit);std::copy(row.strings[s].begin(),row.strings[s].end(),bytes.begin()+static_cast<std::ptrdiff_t>(tail));tail+=row.strings[s].size();}
    }
    return bytes;
}
}
