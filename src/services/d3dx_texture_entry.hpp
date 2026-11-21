#pragma once
#include "x64arm/windows_graphics.hpp"
#include <string_view>

namespace x64arm {
struct D3dxTextureEntry {
    bool info{},view{},file{},resource{},valid{};
    unsigned source_arg{},load_arg{},pump_arg{},out_arg{},status_arg{};

    explicit D3dxTextureEntry(std::string_view symbol){
        if(!symbol.starts_with("D3DX10") && !symbol.starts_with("D3DX11"))return;
        symbol.remove_prefix(6);
        info=symbol.starts_with("GetImageInfoFrom");
        view=symbol.starts_with("CreateShaderResourceViewFrom");
        valid=info || view || symbol.starts_with("CreateTextureFrom");
        file=symbol.find("FromFile")!=symbol.npos;
        resource=symbol.find("FromResource")!=symbol.npos;
        source_arg=info?0U:1U;
        load_arg=source_arg+(file?1U:2U);
        pump_arg=load_arg+(info?0U:1U);
        out_arg=pump_arg+1;
        status_arg=out_arg+1;
    }
};

struct D3dxPreparedTexture {
    GraphicsResourceDesc desc;
    unsigned first{},source_levels{},usage{},access{};
};
}
