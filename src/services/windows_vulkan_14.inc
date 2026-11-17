// Vulkan 1.4 與更新範本的手寫命令：
// - vkMapMemory2／vkUnmapMemory2 沿用 vkMapMemory 的整段對映（同一記憶體只對映一次，保留到 vkFreeMemory）；不支援 VK_MEMORY_MAP_PLACED_BIT_EXT。
// - vkCopyMemoryToImage／vkCopyImageToMemory：pHostPointer 依影像格式（vkCreateImage 記錄）換算客體緩衝區大小，複製到宿主緩衝區後呼叫宿主；
//   VK_HOST_IMAGE_COPY_MEMCPY 以 VkSubresourceHostMemcpySize 取得大小。
// - 更新範本：建立時依項目算出 pData 長度；VkDescriptorImageInfo／BufferInfo／BufferView 在 x86 與 x64 客體的版面與宿主相同，直接整段複製。
namespace {
// 讀取客體資料到宿主緩衝區；讀不到時回傳 false。
bool guest_read(VulkanCall& c,std::uint64_t guest,std::uint8_t* host,std::size_t size){
    try{c.memory().read(GuestAddress{guest},std::span<std::uint8_t>(host,size));return true;}catch(const MemoryFault&){return false;}
}
// 每個區塊（未壓縮格式為 1×1）的位元組數與寬高；深度／模板格式依要求的 aspect 回傳該平面的大小。
struct FormatBlock {std::uint32_t bytes{},width{1},height{1};};
std::optional<FormatBlock> vulkan_format_block(VkFormat format,VkImageAspectFlags aspect){
    const auto f=static_cast<std::uint32_t>(format);
    const auto in=[&](std::uint32_t low,std::uint32_t high){return f>=low && f<=high;};
    if(f==1)return FormatBlock{1};if(in(2,8))return FormatBlock{2};if(in(9,15))return FormatBlock{1};if(in(16,22))return FormatBlock{2};
    if(in(23,36))return FormatBlock{3};if(in(37,69))return FormatBlock{4};if(in(70,76))return FormatBlock{2};if(in(77,83))return FormatBlock{4};
    if(in(84,90))return FormatBlock{6};if(in(91,97))return FormatBlock{8};if(in(98,100))return FormatBlock{4};if(in(101,103))return FormatBlock{8};
    if(in(104,106))return FormatBlock{12};if(in(107,109))return FormatBlock{16};if(in(110,112))return FormatBlock{8};if(in(113,115))return FormatBlock{16};
    if(in(116,118))return FormatBlock{24};if(in(119,121))return FormatBlock{32};if(in(122,123))return FormatBlock{4};
    if(f==124)return FormatBlock{2};if(f==125 || f==126)return FormatBlock{4};if(f==127)return FormatBlock{1};
    if(in(128,130)){if(aspect&VK_IMAGE_ASPECT_STENCIL_BIT)return FormatBlock{1};return FormatBlock{f==128?2U:4U};}
    if(in(131,134) || in(139,140))return FormatBlock{8,4,4};if(in(135,138) || in(141,146))return FormatBlock{16,4,4};
    if(in(147,150) || in(153,154))return FormatBlock{8,4,4};if(in(151,152) || in(155,156))return FormatBlock{16,4,4};
    static constexpr std::array<std::pair<std::uint32_t,std::uint32_t>,14> astc{{{4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},{8,8},{10,5},{10,6},{10,8},{10,10},{12,10},{12,12}}};
    if(in(157,184)){const auto& [w,h]=astc[(f-157)/2];return FormatBlock{16,w,h};}
    if(in(1000066000,1000066013)){const auto& [w,h]=astc[f-1000066000];return FormatBlock{16,w,h};} // ASTC HDR
    if(f==1000340000 || f==1000340001 || f==1000470000)return FormatBlock{2}; // A4R4G4B4、A4B4G4R4、A1B5G5R5
    if(f==1000470001)return FormatBlock{1};                                   // A8_UNORM
    if(f==1000464000)return FormatBlock{4};                                   // R16G16_SFIXED5
    return std::nullopt;
}
}
// 整段對映共用：回傳客體位址（0 為失敗，result 為宿主結果）。
std::uint64_t vulkan_map_whole(VulkanCall& c,VkDevice device_handle,std::uint64_t memory,VkResult& result){
    auto& vulkan=c.vulkan();const auto device=reinterpret_cast<std::uintptr_t>(device_handle);result=VK_SUCCESS;
    if(const auto found=vulkan.mapped.find(memory);found!=vulkan.mapped.end())return found->second.guest;
    const auto allocation=vulkan.allocations.find(memory);if(allocation==vulkan.allocations.end()){result=VK_ERROR_MEMORY_MAP_FAILED;return 0;}
    void* host=nullptr;result=vulkan.fn.vkMapMemory(device_handle,reinterpret_cast<VkDeviceMemory>(static_cast<std::uintptr_t>(memory)),0,VK_WHOLE_SIZE,0,&host);
    if(result!=VK_SUCCESS)return 0;
    const auto size=static_cast<std::size_t>((allocation->second+host_page-1)&~static_cast<VkDeviceSize>(host_page-1));
    const auto guest=reinterpret_cast<std::uintptr_t>(host)%host_page==0?c.host().map(host,size):0;
    if(!guest){vulkan.fn.vkUnmapMemory(device_handle,reinterpret_cast<VkDeviceMemory>(static_cast<std::uintptr_t>(memory)));result=VK_ERROR_MEMORY_MAP_FAILED;return 0;}
    vulkan.mapped[memory]={device,guest,size};return guest;
}
std::optional<std::uint32_t> custom_vkMapMemory2(VulkanCall& c) {
    // (device, VkMemoryMapInfo*, ppData)
    const auto* info=static_cast<const VkMemoryMapInfo*>(c.in_struct(1,c.struct_id("VkMemoryMapInfo"),1));const auto out=c.arg(2);
    if(!info || !out || (info->flags&VK_MEMORY_MAP_PLACED_BIT_EXT))return failure(c,VK_ERROR_MEMORY_MAP_FAILED);
    VkResult result{};const auto guest=vulkan_map_whole(c,c.handle<VkDevice>(0),reinterpret_cast<std::uintptr_t>(info->memory),result);
    if(!guest)return failure(c,result);c.store_pointer(out,guest+info->offset);return c.done(VK_SUCCESS);
}
// 與 vkUnmapMemory 相同保留對映（VK_MEMORY_UNMAP_RESERVE_BIT_EXT 也一樣）；只核對記憶體確實已對映。
std::optional<std::uint32_t> custom_vkUnmapMemory2(VulkanCall& c) {
    const auto* info=static_cast<const VkMemoryUnmapInfo*>(c.in_struct(1,c.struct_id("VkMemoryUnmapInfo"),1));
    if(!info)return failure(c,VK_ERROR_MEMORY_MAP_FAILED);return c.done(VK_SUCCESS);
}
std::optional<std::uint32_t> custom_vkCreateImage(VulkanCall& c) {
    auto& vulkan=c.vulkan();const auto* info=static_cast<const VkImageCreateInfo*>(c.in_struct(1,c.struct_id("VkImageCreateInfo"),1));
    if(!info || !c.arg(3))return failure(c,VK_ERROR_INITIALIZATION_FAILED);
    VkImage image{};const auto result=vulkan.fn.vkCreateImage(c.handle<VkDevice>(0),info,nullptr,&image);
    if(result==VK_SUCCESS){vulkan.images[reinterpret_cast<std::uintptr_t>(image)]={info->format,info->imageType,info->arrayLayers};c.store64(c.arg(3),reinterpret_cast<std::uintptr_t>(image));}
    c.finish();return failure(c,result);
}
std::optional<std::uint32_t> custom_vkDestroyImage(VulkanCall& c) {
    auto& vulkan=c.vulkan();vulkan.images.erase(c.arg(1));vulkan.fn.vkDestroyImage(c.handle<VkDevice>(0),c.handle<VkImage>(1),nullptr);return c.done(0);
}
namespace {
// 主機影像複製一個區域的客體資料大小。
std::optional<std::size_t> host_copy_size(VulkanCall& c,VkDevice device,VkImage image,VkHostImageCopyFlags flags,const VkImageSubresourceLayers& subresource,
                                          std::uint32_t row_length,std::uint32_t image_height,const VkExtent3D& extent){
    auto& vulkan=c.vulkan();const auto found=vulkan.images.find(reinterpret_cast<std::uintptr_t>(image));if(found==vulkan.images.end())return std::nullopt;
    const auto layers=subresource.layerCount==VK_REMAINING_ARRAY_LAYERS?found->second.layers-std::min(found->second.layers,subresource.baseArrayLayer):subresource.layerCount;
    if(flags&VK_HOST_IMAGE_COPY_MEMCPY_BIT){
        if(!vulkan.fn.vkGetImageSubresourceLayout2)return std::nullopt;
        VkSubresourceHostMemcpySize memcpy_size{};memcpy_size.sType=VK_STRUCTURE_TYPE_SUBRESOURCE_HOST_MEMCPY_SIZE;
        VkSubresourceLayout2 layout{};layout.sType=VK_STRUCTURE_TYPE_SUBRESOURCE_LAYOUT_2;layout.pNext=&memcpy_size;
        VkImageSubresource2 request{};request.sType=VK_STRUCTURE_TYPE_IMAGE_SUBRESOURCE_2;request.imageSubresource={subresource.aspectMask,subresource.mipLevel,subresource.baseArrayLayer};
        vulkan.fn.vkGetImageSubresourceLayout2(device,image,&request,&layout);return static_cast<std::size_t>(memcpy_size.size)*std::max(1U,layers);
    }
    const auto block=vulkan_format_block(found->second.format,subresource.aspectMask);if(!block)return std::nullopt;
    const auto blocks=[](std::uint64_t value,std::uint32_t unit){return (value+unit-1)/unit;};
    const auto row=blocks(row_length?row_length:extent.width,block->width),rows=blocks(image_height?image_height:extent.height,block->height);
    const auto width=blocks(extent.width,block->width),height=blocks(extent.height,block->height);
    const std::uint64_t slices=std::uint64_t{std::max(1U,extent.depth)}*std::max(1U,layers);
    if(!width || !height)return 0;
    const auto size=((slices-1)*rows+(height-1))*row*block->bytes+width*block->bytes;if(size>(1ULL<<32))return std::nullopt;
    return static_cast<std::size_t>(size);
}
}
std::optional<std::uint32_t> custom_vkCopyMemoryToImage(VulkanCall& c) {
    // (device, VkCopyMemoryToImageInfo*)：各區域的 pHostPointer 先複製客體資料到宿主緩衝區。
    auto* info=static_cast<VkCopyMemoryToImageInfo*>(c.in_struct(1,c.struct_id("VkCopyMemoryToImageInfo"),1));if(!info)return failure(c,VK_ERROR_INITIALIZATION_FAILED);
    const auto device=c.handle<VkDevice>(0);auto* regions=const_cast<VkMemoryToImageCopy*>(info->pRegions);
    for(std::uint32_t i=0;i<info->regionCount;++i){
        const auto guest=reinterpret_cast<std::uintptr_t>(regions[i].pHostPointer);
        const auto size=host_copy_size(c,device,info->dstImage,info->flags,regions[i].imageSubresource,regions[i].memoryRowLength,regions[i].memoryImageHeight,regions[i].imageExtent);
        if(!size || !guest)return failure(c,VK_ERROR_FORMAT_NOT_SUPPORTED);
        auto* host=c.allocate(*size);if(*size && !guest_read(c,guest,host,*size))return failure(c,VK_ERROR_MEMORY_MAP_FAILED);regions[i].pHostPointer=host;
    }
    const auto result=c.fn().vkCopyMemoryToImage(device,info);c.finish();return failure(c,result);
}
std::optional<std::uint32_t> custom_vkCopyImageToMemory(VulkanCall& c) {
    // (device, VkCopyImageToMemoryInfo*)：宿主寫入暫存緩衝區後寫回各區域的客體 pHostPointer。
    auto* info=static_cast<VkCopyImageToMemoryInfo*>(c.in_struct(1,c.struct_id("VkCopyImageToMemoryInfo"),1));if(!info)return failure(c,VK_ERROR_INITIALIZATION_FAILED);
    const auto device=c.handle<VkDevice>(0);auto* regions=const_cast<VkImageToMemoryCopy*>(info->pRegions);
    std::vector<std::tuple<std::uint64_t,std::uint8_t*,std::size_t>> outputs;
    for(std::uint32_t i=0;i<info->regionCount;++i){
        const auto guest=reinterpret_cast<std::uintptr_t>(regions[i].pHostPointer);
        const auto size=host_copy_size(c,device,info->srcImage,info->flags,regions[i].imageSubresource,regions[i].memoryRowLength,regions[i].memoryImageHeight,regions[i].imageExtent);
        if(!size || !guest)return failure(c,VK_ERROR_FORMAT_NOT_SUPPORTED);
        auto* host=c.allocate(*size);regions[i].pHostPointer=host;outputs.emplace_back(guest,host,*size);
    }
    const auto result=c.fn().vkCopyImageToMemory(device,info);
    if(result==VK_SUCCESS)for(const auto& [guest,host,size]:outputs)if(size)c.memory().write(GuestAddress{guest},std::span<const std::uint8_t>(host,size));
    c.finish();return failure(c,result);
}
std::optional<std::uint32_t> custom_vkCreateDescriptorUpdateTemplate(VulkanCall& c) {
    // 依項目算出 pData 需要的長度：影像類 VkDescriptorImageInfo、緩衝類 VkDescriptorBufferInfo、texel 緩衝 VkBufferView、
    // 加速結構 handle、inline uniform block 為 descriptorCount 個位元組。
    auto& vulkan=c.vulkan();const auto* info=static_cast<const VkDescriptorUpdateTemplateCreateInfo*>(c.in_struct(1,c.struct_id("VkDescriptorUpdateTemplateCreateInfo"),1));
    if(!info || !c.arg(3))return failure(c,VK_ERROR_INITIALIZATION_FAILED);
    std::size_t size=0;
    for(std::uint32_t i=0;i<info->descriptorUpdateEntryCount;++i){
        const auto& entry=info->pDescriptorUpdateEntries[i];if(!entry.descriptorCount)continue;std::size_t element=sizeof(VkDescriptorImageInfo);
        switch(entry.descriptorType){
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:element=sizeof(VkBufferView);break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:element=sizeof(VkDescriptorBufferInfo);break;
        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:element=sizeof(VkAccelerationStructureKHR);break;
        case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK:size=std::max<std::size_t>(size,entry.offset+entry.descriptorCount);continue;
        default:break;
        }
        size=std::max<std::size_t>(size,entry.offset+std::size_t{entry.descriptorCount-1}*entry.stride+element);
    }
    if(size>(64U<<20))return failure(c,VK_ERROR_OUT_OF_HOST_MEMORY);
    VkDescriptorUpdateTemplate created{};const auto result=vulkan.fn.vkCreateDescriptorUpdateTemplate(c.handle<VkDevice>(0),info,nullptr,&created);
    if(result==VK_SUCCESS){vulkan.templates[reinterpret_cast<std::uintptr_t>(created)]=size;c.store64(c.arg(3),reinterpret_cast<std::uintptr_t>(created));}
    c.finish();return failure(c,result);
}
std::optional<std::uint32_t> custom_vkDestroyDescriptorUpdateTemplate(VulkanCall& c) {
    auto& vulkan=c.vulkan();vulkan.templates.erase(c.arg(1));vulkan.fn.vkDestroyDescriptorUpdateTemplate(c.handle<VkDevice>(0),c.handle<VkDescriptorUpdateTemplate>(1),nullptr);return c.done(0);
}
namespace {
// 範本資料：依建立時算出的長度把客體 pData 複製到宿主；未知的範本回傳 nullptr。
const void* template_data(VulkanCall& c,std::uint64_t template_handle,std::uint64_t guest){
    const auto found=c.vulkan().templates.find(template_handle);if(found==c.vulkan().templates.end() || !guest)return nullptr;
    auto* host=c.allocate(found->second);if(found->second && !guest_read(c,guest,host,found->second))return nullptr;return host;
}
}
std::optional<std::uint32_t> custom_vkUpdateDescriptorSetWithTemplate(VulkanCall& c) {
    // (device, descriptorSet, descriptorUpdateTemplate, pData)
    const auto* data=template_data(c,c.arg(2),c.arg(3));if(!data)return c.done(0);
    c.fn().vkUpdateDescriptorSetWithTemplate(c.handle<VkDevice>(0),c.handle<VkDescriptorSet>(1),c.handle<VkDescriptorUpdateTemplate>(2),data);return c.done(0);
}
std::optional<std::uint32_t> custom_vkCmdPushDescriptorSetWithTemplate(VulkanCall& c) {
    // (commandBuffer, descriptorUpdateTemplate, layout, set, pData)
    const auto* data=template_data(c,c.arg(1),c.arg(4));if(!data)return c.done(0);
    c.fn().vkCmdPushDescriptorSetWithTemplate(c.handle<VkCommandBuffer>(0),c.handle<VkDescriptorUpdateTemplate>(1),c.handle<VkPipelineLayout>(2),c.u32(3),data);return c.done(0);
}
std::optional<std::uint32_t> custom_vkCmdPushDescriptorSetWithTemplate2(VulkanCall& c) {
    // (commandBuffer, VkPushDescriptorSetWithTemplateInfo*)：pData 依範本長度換成宿主複本。
    auto* info=static_cast<VkPushDescriptorSetWithTemplateInfo*>(c.in_struct(1,c.struct_id("VkPushDescriptorSetWithTemplateInfo"),1));if(!info)return c.done(0);
    const auto* data=template_data(c,reinterpret_cast<std::uintptr_t>(info->descriptorUpdateTemplate),reinterpret_cast<std::uintptr_t>(info->pData));if(!data)return c.done(0);
    info->pData=data;c.fn().vkCmdPushDescriptorSetWithTemplate2(c.handle<VkCommandBuffer>(0),info);c.finish();return c.done(0);
}
