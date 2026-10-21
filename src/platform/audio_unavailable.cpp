#include "x64arm/audio.hpp"
#include <stdexcept>
namespace x64arm::audio {
bool input_available(){return false;}
std::unique_ptr<Input> open_input(std::shared_ptr<Buffer>){throw std::runtime_error("錄音後端需要 macOS Core Audio");}
bool output_available(){return false;}
std::unique_ptr<Output> open_output(std::shared_ptr<Buffer>){throw std::runtime_error("Core Audio requires macOS");}
std::unique_ptr<Output> open_output(std::shared_ptr<Buffer>,std::string_view){throw std::runtime_error("Core Audio requires macOS");}
std::vector<DeviceInfo> devices(){return {};}
}
