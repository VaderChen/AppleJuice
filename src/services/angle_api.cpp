#include "x64arm/angle.hpp"
namespace x64arm {namespace {
#include "angle_api_generated.inc"
}
std::span<const AngleCommand> angle_commands(){return commands;}
bool angle_module(std::string_view name){return name=="libegl.dll" || name=="libglesv2.dll";}
std::span<const WindowsServiceDescriptor> angle_service_descriptors(){
    static const auto descriptors=[] {std::vector<WindowsServiceDescriptor> result;
        for(const auto& c:commands){std::uint32_t wide=0;for(unsigned i=0;i<c.parameters.size();++i)if(c.parameters[i].type==AngleType::U64 || c.parameters[i].type==AngleType::I64)wide|=1U<<i;
            result.push_back({c.family,c.name,static_cast<unsigned>(c.parameters.size()),wide});}return result;}();
    return descriptors;
}
}
