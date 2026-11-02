#include "x64arm/native_vulkan.hpp"
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>
#include <algorithm>
#include <stdexcept>
#include <unordered_map>

namespace x64arm {
namespace {
std::unordered_map<void*,std::weak_ptr<NativeVulkanSurface>> window_layers;
class MetalVulkanSurface final:public NativeVulkanSurface {
    NSView* __strong view_{};
    CALayer* __strong previous_{};
    CAMetalLayer* __strong layer_{};
    BOOL wanted_{},managed_{};
public:
    explicit MetalVulkanSurface(const VulkanWindow& window){@autoreleasepool{
        if(window.view && !NSThread.isMainThread)throw std::runtime_error("Vulkan 視窗層必須在主執行緒建立");
        layer_=[CAMetalLayer layer];layer_.device=MTLCreateSystemDefaultDevice();
        if(!layer_.device)throw std::runtime_error("Vulkan 視窗層沒有可用 Metal 裝置");
        layer_.pixelFormat=MTLPixelFormatBGRA8Unorm;layer_.framebufferOnly=NO;
        view_=(__bridge NSView*)window.view;
        if(view_){
            previous_=view_.layer;wanted_=view_.wantsLayer;
            if([view_ respondsToSelector:NSSelectorFromString(@"setOpenGLManaged:")]){
                managed_=[[view_ valueForKey:@"openGLManaged"] boolValue];[view_ setValue:@YES forKey:@"openGLManaged"];
            }
            view_.wantsLayer=YES;view_.layer=layer_;
        }
        resize(window.width,window.height);
    }}
    ~MetalVulkanSurface()override{@autoreleasepool{
        if(view_ && view_.layer==layer_){
            view_.layer=previous_;view_.wantsLayer=wanted_;
            if([view_ respondsToSelector:NSSelectorFromString(@"setOpenGLManaged:")])[view_ setValue:@(managed_) forKey:@"openGLManaged"];
            [view_ setNeedsDisplay:YES];
        }
        if(view_){const auto key=(__bridge void*)view_;const auto found=window_layers.find(key);if(found!=window_layers.end() && found->second.expired())window_layers.erase(found);}
    }}
    void* layer()const override{return (__bridge void*)layer_;}
    void resize(std::uint32_t width,std::uint32_t height)override{@autoreleasepool{
        const auto size=CGSizeMake(std::max(1U,width),std::max(1U,height));
        if(CGSizeEqualToSize(layer_.drawableSize,size) && (!view_ || CGRectEqualToRect(layer_.frame,view_.bounds)))return;
        [CATransaction begin];[CATransaction setDisableActions:YES];
        if(view_)layer_.frame=view_.bounds;
        layer_.drawableSize=size;
        [CATransaction commit];
    }}
};
}
std::shared_ptr<NativeVulkanSurface> make_native_vulkan_surface(const VulkanWindow& window){
    if(window.view)if(const auto found=window_layers.find(window.view);found!=window_layers.end())if(auto shared=found->second.lock()){shared->resize(window.width,window.height);return shared;}
    auto result=std::make_shared<MetalVulkanSurface>(window);if(window.view)window_layers[window.view]=result;return result;
}
}
