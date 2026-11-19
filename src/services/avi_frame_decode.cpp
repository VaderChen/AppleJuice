#include "avi_container.hpp"
#include "texture_load.hpp"

namespace x64arm::avi {
bool decode_frame_image(View data,unsigned width,unsigned height,Bytes& rgba){
    try{
        auto image=texture_image::decode(data);
        if(image.desc.width!=width || image.desc.height!=height || image.desc.format!=28 || image.pixels.empty())return false;
        rgba=std::move(image.pixels.front());return true;
    }catch(const texture_image::Error&){return false;}
}
}
