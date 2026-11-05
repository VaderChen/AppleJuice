#pragma once
#include "x64arm/windows_graphics.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// 僅供明確啟用的單幀診斷；一般執行不讀取控制檔、不回讀 GPU。
// 控制檔：遞增請求編號、第一筆 draw（從零起算）、最多擷取幾筆。
namespace x64arm {
struct D3D9FrameCapture {
    struct Asset {std::uint64_t hash{};std::vector<std::uint8_t> bytes;std::string name;};
    static constexpr std::size_t budget=128U*1024U*1024U;
    std::filesystem::path directory;
    std::vector<Asset> assets;
    std::uint64_t request{},frame{},draw{},first{},count{};
    std::size_t written{};
    bool armed{};

    static bool enabled(){static const bool value=[] {const auto* p=std::getenv("X64ARM_D3D9_CAPTURE_REQUEST");return p && *p;}();return value;}
    void presented(std::uint64_t device,std::uint64_t frames) noexcept {
        armed=false;assets.clear();written=0;draw=0;
        try {
            const auto* root=std::getenv("X64ARM_D3D9_CAPTURE_DIR");if(!root || !*root)return;
            std::ifstream input(std::getenv("X64ARM_D3D9_CAPTURE_REQUEST"));
            std::uint64_t serial{},begin{},length{};
            if(!(input>>serial>>begin>>length) || !serial || serial==request || !length || length>256 || begin>65536)return;
            std::string extra;if(input>>extra)return;
            directory=std::filesystem::path(root)/("capture-"+std::to_string(serial)+"-device-"+std::to_string(device)+"-frame-"+std::to_string(frames+1));
            std::filesystem::create_directories(directory);
            request=serial;frame=frames+1;first=begin;count=length;armed=true;
            std::ofstream out(directory/"request.txt");out<<"request="<<request<<" frame="<<frame<<" first="<<first<<" count="<<count<<" budget="<<budget<<'\n';
        }catch(const std::exception& error){std::fprintf(stderr,"[d3d9 capture] %s\n",error.what());}
    }
    bool select(){if(!armed)return false;const auto index=draw++;return index>=first && index-first<count;}
    std::string save(std::span<const std::uint8_t> bytes){
        std::uint64_t hash=14695981039346656037ULL;for(const auto byte:bytes){hash^=byte;hash*=1099511628211ULL;}
        for(const auto& asset:assets)if(asset.hash==hash && std::equal(bytes.begin(),bytes.end(),asset.bytes.begin(),asset.bytes.end()))return asset.name;
        if(bytes.size()>budget-written)return "omitted-budget";
        const auto name="asset-"+std::to_string(assets.size())+".bin";
        std::ofstream out(directory/name,std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
        if(!out)throw std::runtime_error("單幀擷取寫入失敗");
        assets.push_back({hash,{bytes.begin(),bytes.end()},name});written+=bytes.size();return name;
    }
    template<class Context> void view(std::ostream& out,const std::string& label,Context& native,const GraphicsView& view){
        if(!view.resource)return;const auto& d=view.resource->desc;
        out<<label<<" dimension="<<d.dimension<<" format="<<d.format<<" width="<<d.width<<" height="<<d.height<<" depth="<<d.depth
           <<" levels="<<d.levels<<" layers="<<d.layers<<" samples="<<d.samples<<" view_format="<<view.format<<" level="<<view.level
           <<" slice="<<view.slice<<" flags="<<view.flags<<" data=";
        // 先限制單次回讀大小；診斷失敗不改變遊戲的繪圖結果。
        const auto estimate=d.dimension==1?std::uint64_t{d.width}:std::uint64_t{d.width}*d.height*d.depth*16U;
        if(estimate>budget){out<<"omitted-resource-size\n";return;}
        try {const auto bytes=native.graphics_read(view);out<<save(bytes)<<'\n';}
        catch(const std::exception& error){out<<"read-error "<<error.what()<<'\n';}
    }
};
}
