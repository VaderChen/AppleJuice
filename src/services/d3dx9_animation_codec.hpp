#pragma once
namespace x64arm::d3dx_animation {
// 壓縮資料是此引擎 D3DX API 產生及讀回的不透明格式；不宣稱可讀取 Microsoft 私有壓縮格式。
inline std::vector<std::uint8_t> compress_set(const Set& set){
    std::vector<std::uint8_t> plain;const auto number=[&](unsigned value){for(unsigned c=0;c<4;++c)plain.push_back(static_cast<std::uint8_t>(value>>(c*8)));};
    number(static_cast<unsigned>(set.animations.size()));for(const auto& animation:set.animations){number(static_cast<unsigned>(animation.name.size()));plain.insert(plain.end(),animation.name.begin(),animation.name.end());for(const auto& keys:animation.keys){number(static_cast<unsigned>(keys.size()));for(const auto& key:keys){number(std::bit_cast<unsigned>(key.time));for(auto c:key.value)number(std::bit_cast<unsigned>(c));}}}
    if(plain.size()>128U*1024U*1024U)throw std::bad_alloc{};std::vector<std::uint8_t> data{'X','6','4','A','N','I','M','1'};for(unsigned c=0;c<4;++c)data.push_back(static_cast<std::uint8_t>(plain.size()>>(c*8)));uLongf size=compressBound(plain.size());data.resize(12+size);if(compress2(data.data()+12,&size,plain.data(),plain.size(),Z_BEST_COMPRESSION)!=Z_OK)throw std::bad_alloc{};data.resize(12+size);return data;
}
inline std::vector<Animation> decompress_set(std::span<const std::uint8_t> source){
    const auto check=[](bool valid){if(!valid)throw std::invalid_argument("動畫壓縮資料無效或不支援此格式");};check(source.size()>=12 && std::memcmp(source.data(),"X64ANIM1",8)==0);unsigned size=0;std::memcpy(&size,source.data()+8,4);check(size<=128U*1024U*1024U);std::vector<std::uint8_t> plain(size);uLongf actual=size;check(uncompress(plain.data(),&actual,source.data()+12,source.size()-12)==Z_OK && actual==size);std::size_t cursor=0;
    const auto number=[&](){check(cursor<=plain.size() && plain.size()-cursor>=4);unsigned value=0;std::memcpy(&value,plain.data()+cursor,4);cursor+=4;return value;};
    const auto count=number();check(count<=65536);std::vector<Animation> result(count);
    for(auto& animation:result){const auto length=number();check(length<=plain.size()-cursor);animation.name.assign(reinterpret_cast<const char*>(plain.data()+cursor),length);cursor+=length;for(auto& keys:animation.keys){const auto n=number();check(n<=1048576 && n<=(plain.size()-cursor)/20);keys.resize(n);float previous=-1;for(auto& key:keys){key.time=std::bit_cast<float>(number());check(std::isfinite(key.time) && key.time>=previous);previous=key.time;for(auto& c:key.value){c=std::bit_cast<float>(number());check(std::isfinite(c));}}}}check(cursor==plain.size());return result;
}
}
