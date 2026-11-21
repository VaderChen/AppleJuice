#pragma once
#include <string_view>

namespace x64arm {
struct D3dxShaderEntry {
    bool preprocess{},file{},resource{},wide{},d3d10{},valid{},has_status{};
    bool effect{},effect_pool{};
    unsigned macros_arg{},pump_arg{},code_arg{};

    explicit D3dxShaderEntry(std::string_view symbol){
        d3d10=symbol.starts_with("D3DX10");
        if(!d3d10 && !symbol.starts_with("D3DX11"))return;
        symbol.remove_prefix(6);
        preprocess=symbol.starts_with("PreprocessShaderFrom");
        effect_pool=d3d10 && symbol.starts_with("CreateEffectPoolFrom");
        effect=effect_pool || (d3d10 && symbol.starts_with("CreateEffectFrom"));
        valid=preprocess || effect || symbol.starts_with("CompileFrom");
        if(!valid)return;
        file=symbol.find("FromFile")!=symbol.npos;
        resource=symbol.find("FromResource")!=symbol.npos;
        wide=symbol.ends_with('W');
        macros_arg=file?1U:3U;
        pump_arg=macros_arg+(preprocess?2U:6U);
        if(effect && !effect_pool)++pump_arg;
        code_arg=pump_arg+1;
        // D3DX10 前處理只有 Memory 入口帶末端 HRESULT，D3DX11 的五種入口都有。
        has_status=!preprocess || !d3d10 || (!file && !resource);
    }
};
}
