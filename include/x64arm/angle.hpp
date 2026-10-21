#pragma once
#include "x64arm/windows_abi.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
namespace x64arm {
// EGL／GLES 共用客體 ABI 描述；Metal 與 D3D11 路徑不重新實作 GLES 語義。
enum class AngleType {Void,U32,I32,U8,Float,U64,I64,Size,Attrib,Handle,Pointer,String,Proc};
struct AngleParameter {AngleType type,element;std::string_view name,length;bool input;};
struct AngleCommand {std::string_view name,family;AngleType result;std::vector<AngleParameter> parameters;};
std::span<const AngleCommand> angle_commands();
std::span<const WindowsServiceDescriptor> angle_service_descriptors();
bool angle_module(std::string_view);
struct AngleWindow {void* view{};unsigned width{},height{};};
class NativeAngle {
public:
    virtual ~NativeAngle()=default;
    virtual bool supports(std::string_view)const=0;
    virtual std::uint64_t invoke(GuestState&,GuestMemory&,const WindowsCall&,const AngleCommand&,std::uint64_t,
        const std::function<std::uint64_t(std::span<const std::uint8_t>)>&,
        const std::function<void(std::uint64_t)>&,
        const std::function<std::uint64_t(std::string_view)>&,
        const std::function<AngleWindow(std::uint64_t)>&)=0;
    virtual std::string diagnostics()const=0;
};
std::unique_ptr<NativeAngle> make_native_angle();
}
