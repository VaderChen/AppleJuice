#pragma once
#include <cstdint>
#include <memory>

namespace x64arm {
struct D3D12State;
class NativeOpenGlContext;
struct GraphicsResource;
// 呼叫端持有共用圖形鎖，成功建立包裝後另持有對應 COM 參照。
std::shared_ptr<NativeOpenGlContext> on12_backend(D3D12State*,std::uint64_t device,std::uint32_t& level);
bool on12_direct_queue(D3D12State*,std::uint64_t device,std::uint64_t queue);
std::shared_ptr<GraphicsResource> on12_resource(D3D12State*,std::uint64_t device,std::uint64_t resource);
}
