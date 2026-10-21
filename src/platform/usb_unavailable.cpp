#include "x64arm/usb.hpp"
namespace x64arm::usb {
std::vector<Device> devices(){return {};}
std::unique_ptr<Session> open(std::uint64_t,std::span<const unsigned>,std::uint32_t& error){error=50;return {};}
}
