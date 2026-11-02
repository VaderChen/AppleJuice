#pragma once
#include <cstdint>
#include <memory>

namespace x64arm {
struct VulkanWindow {void* view{};std::uint32_t width{},height{};};
// 持有視窗呈現層的生命週期；Vulkan surface 必須先銷毀，再釋放這個擁有者。
class NativeVulkanSurface {
public:
    virtual ~NativeVulkanSurface()=default;
    virtual void* layer()const=0;
    virtual void resize(std::uint32_t,std::uint32_t)=0;
};
std::shared_ptr<NativeVulkanSurface> make_native_vulkan_surface(const VulkanWindow&);
}
