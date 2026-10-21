#include "fixed_metal_library.hpp"
#include "x64arm/fixed_metal_data.hpp"
namespace x64arm {
bool fixed_metal_offline_available() noexcept{return fixed_metal_data::offline;}
id<MTLLibrary> fixed_metal_library(id<MTLDevice> device,NSError** error,bool force_source){
    if(fixed_metal_data::offline && !force_source){
        // metallib 隨引擎內嵌，CLI／Launcher 移動或重新封裝均不依賴建置路徑。
        auto data=dispatch_data_create(fixed_metal_data::bytes,sizeof(fixed_metal_data::bytes),nullptr,DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        return [device newLibraryWithData:data error:error];
    }
    return [device newLibraryWithSource:[NSString stringWithUTF8String:fixed_metal_data::source] options:nil error:error];
}
}
