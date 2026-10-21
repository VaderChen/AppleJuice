#include <source_location>
#include "x64arm/windows_services.hpp"
#include "graphics_frame_test_control.hpp"
#include "x64arm/d3dx_font_raster.hpp"
#include "x64arm/d3dx_sprite_sort.hpp"
#include "x64arm/gpu_memory.hpp"
#include "x64arm/graphics_index_range.hpp"
#include "d3d9_interfaces.hpp"
#include "d3d9_perf.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_paths.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <functional>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <cctype>
#include <cstdlib>
#include <zlib.h>
#include "d3dx9_xfile.hpp"
#include "d3dx9_xwriter.hpp"
#include "d3dx9_gutter.hpp"
#include "d3dx9_skin.hpp"
#include "d3dx9_prt_compression.hpp"
#include "d3dx9_prt_engine.hpp"
#include "d3dx9_animation.hpp"
#include "d3dx9_animation_codec.hpp"
#include "x64arm/x87_fp.hpp"
extern "C" {
#include "softfloat.h"
}
#include "d3dx9_xdocument.hpp"
#include "d3dx9_constants.hpp"
#include "d3dx9_effect.hpp"
#include "d3d9_shader.hpp"
#include "d3d9_formats.hpp"
#include "d3dx_mesh.hpp"
#include "d3dx_atlas.hpp"
#include "d3dx_sh.hpp"
#include "native_progress_job.hpp"
#include "texture_load.hpp"
#include "x64arm/native_font_data.hpp"
#include "x64arm/windows_fonts.hpp"
#include "x64arm/raster_surface.hpp"
#include "hlsl_include.hpp"

namespace x64arm {
namespace {
using namespace d3d9_abi;
constexpr std::uint32_t invalid=0x8876086cU, unavailable=0x8876086aU, out_of_memory=0x8007000eU, no_interface=0x80004002U;
struct Failure {std::uint32_t value;std::source_location where=std::source_location::current();};
void require(bool value,std::uint32_t error=invalid,const std::source_location where=std::source_location::current()){if(!value)throw Failure{error,where};}
using Matrix=std::array<float,16>;
using Vector=std::array<float,4>;
Matrix identity(){Matrix value{};value[0]=value[5]=value[10]=value[15]=1;return value;}
Vector transform(Vector value,const Matrix& matrix){Vector output{};for(unsigned c=0;c<4;++c)for(unsigned r=0;r<4;++r)output[c]+=value[r]*matrix[r*4+c];return output;}
std::uint32_t bits(float value){return std::bit_cast<std::uint32_t>(value);}
float real(std::uint32_t value){return std::bit_cast<float>(value);}
Vector color(std::uint32_t value){return {float((value>>16)&255)/255,float((value>>8)&255)/255,float(value&255)/255,float(value>>24)/255};}
std::uint32_t word(std::span<const std::uint8_t> data,std::size_t at){std::uint32_t value{};std::memcpy(&value,data.data()+at,4);return value;}
void put(std::span<std::uint8_t> data,std::size_t at,std::uint32_t value){std::memcpy(data.data()+at,&value,4);}
bool color_format(std::uint32_t value){return value==21 || value==22;}
bool render_color_format(unsigned value){return color_format(value) || value==32 || value==35 || value==36 || (value>=111 && value<=116);}
unsigned depth_format(unsigned value){switch(value){case 71:case 77:return 40;case 75:return 20;case 80:return 55;default:return 0;}}

std::span<const WindowsServiceDescriptor> methods(Kind kind,bool ex=false){if(ex){if(kind==Kind::Factory)return factoryExMethods;if(kind==Kind::Device)return deviceExMethods;if(kind==Kind::SwapChain)return swapChainExMethods;}switch(kind){case Kind::PRTEngine:return prtEngineMethods;case Kind::RenderToEnvMap:return renderEnvMethods;case Kind::PRTComp:return prtCompMethods;case Kind::AnimSet:return animationSetMethods;case Kind::CompressedSet:return compressedSetMethods;case Kind::AnimController:return animationControllerMethods;case Kind::Skin:return skinMethods;case Kind::Gutter:return gutterMethods;case Kind::XFile:return fileMethods;case Kind::XEnum:return fileEnumMethods;case Kind::XData:return fileDataMethods;case Kind::RenderToSurface:return renderToSurfaceMethods;case Kind::Effect:return effectMethods;case Kind::EffectPool:return effectPoolMethods;case Kind::StateBlock:return stateBlockMethods;case Kind::Sprite:return spriteMethods;case Kind::Font:return fontMethods;case Kind::ConstantTable:return constantTableMethods;case Kind::VertexShader:return vertexShaderMethods;case Kind::PixelShader:return pixelShaderMethods;case Kind::VertexDeclaration:return vertexDeclarationMethods;case Kind::PRT:return prtMethods;case Kind::Mesh:return meshMethods;case Kind::Blob:return blobMethods;case Kind::Factory:return factoryMethods;case Kind::Device:return deviceMethods;case Kind::Surface:return surfaceMethods;case Kind::Texture:return textureMethods;case Kind::CubeTexture:return cubeTextureMethods;case Kind::VolumeTexture:return volumeTextureMethods;case Kind::Volume:return volumeMethods;case Kind::VertexBuffer:return vertexBufferMethods;case Kind::IndexBuffer:return indexBufferMethods;case Kind::SwapChain:return swapChainMethods;}return {};}
Guid iid(Kind kind){switch(kind){
case Kind::AnimSet:return {0x3a,0x8e,0x4e,0xfa,0x86,0x97,0x7d,0x40,0x8b,0x4c,0x59,0x95,0x89,0x37,0x64,0xaf};
case Kind::CompressedSet:return {0x0d,0x48,0xc2,0x6c,0x08,0x38,0x39,0x47,0x9f,0x88,0xde,0x49,0xfa,0xcd,0x8d,0x4c};
case Kind::RenderToEnvMap:return {0x4b,0x1b,0x3f,0x31,0xb0,0xc7,0xa2,0x4f,0x9d,0x9d,0x8d,0x38,0x0b,0x64,0x38,0x5e};case Kind::PRTEngine:return {0x78,0x42,0x3a,0x68,0x5f,0xcd,0x24,0x4d,0x90,0xad,0xc4,0xe1,0xb6,0x85,0x5d,0x53};case Kind::PRTComp:return {0x65,0xd4,0x58,0xa7,0x8d,0xfe,0xad,0x45,0x9c,0xf0,0xd0,0x1e,0x56,0x26,0x6a,0x07};case Kind::AnimController:return {0xec,0x48,0x89,0xac,0x6d,0xf8,0xe2,0x43,0x96,0xde,0x31,0xfc,0x35,0xf9,0x6d,0x9e};case Kind::Skin:return {0x40,0xa5,0xea,0x11,0xa6,0xf9,0x49,0x4d,0xae,0x6a,0xe1,0x92,0x21,0xf7,0x0c,0xc4};case Kind::Gutter:return {0xec,0x01,0x8f,0x83,0x29,0x97,0x27,0x45,0xaa,0xdb,0xdf,0x70,0xad,0xe7,0xfe,0xa9};case Kind::XFile:case Kind::XEnum:case Kind::XData:return {static_cast<std::uint8_t>(kind==Kind::XFile?0xf9:kind==Kind::XEnum?0xfc:0xfd),0x8c,0xf0,0xce,0x4f,0x7b,0x29,0x44,0x96,0x24,0x2a,0x69,0x0a,0x93,0x32,0x01};case Kind::RenderToSurface:return {0x46,0xf3,0x85,0x69,0x3d,0x2c,0xb3,0x43,0xbe,0x8b,0xda,0xae,0x8a,0x03,0xd8,0x94};case Kind::Effect:return {0xb3,0xb4,0xce,0xf6,0x4c,0x4e,0xdd,0x40,0xb8,0x83,0x8d,0x8d,0xe5,0xea,0x0c,0xd5};case Kind::EffectPool:return {0x04,0xab,0x37,0x95,0x50,0x32,0x2e,0x41,0x82,0x13,0xfc,0xd2,0xf8,0x67,0x79,0x33};case Kind::StateBlock:return {0xe5,0x4f,0x7c,0xb0,0x0d,0x31,0xa8,0x4b,0xa2,0x3c,0x4f,0x0f,0x20,0x6f,0x21,0x8b};case Kind::Sprite:return {0x2d,0x76,0x0b,0xba,0x28,0x7d,0xec,0x43,0xb9,0xdc,0x2f,0x84,0x44,0x3b,0x06,0x14};case Kind::Font:return {0x70,0xbb,0x9d,0xd7,0x21,0x5f,0x36,0x4d,0xbb,0xc2,0xff,0x52,0x5c,0x21,0x3c,0xdc};case Kind::Factory:return iidFactory;case Kind::Device:return iidDevice;case Kind::Surface:return iidSurface;case Kind::Texture:return iidTexture;case Kind::CubeTexture:return iidCubeTexture;case Kind::VolumeTexture:return iidVolumeTexture;case Kind::Volume:return iidVolume;case Kind::VertexBuffer:return iidVertexBuffer;case Kind::IndexBuffer:return iidIndexBuffer;case Kind::SwapChain:return iidSwapChain;case Kind::VertexShader:return {0x7e,0x55,0xc5,0xef,0x65,0x62,0x13,0x46,0x8a,0x94,0x43,0x85,0x78,0x89,0xeb,0x36};case Kind::PixelShader:return {0xdc,0xdb,0x3b,0x6d,0x2,0x5b,0x15,0x44,0xb8,0x52,0xce,0x5e,0x8b,0xcc,0xb2,0x89};case Kind::VertexDeclaration:return {0x9c,0xc5,0x13,0xdd,0xfa,0x36,0x98,0x40,0xa8,0xfb,0xc7,0xed,0x39,0xdc,0x85,0x46};case Kind::ConstantTable:case Kind::Mesh:case Kind::Blob:case Kind::PRT:return {};}return {};}
bool texture_kind(Kind kind){return kind==Kind::Texture || kind==Kind::CubeTexture || kind==Kind::VolumeTexture;}
bool resource(Kind kind){return kind==Kind::Surface || kind==Kind::Volume || texture_kind(kind) || kind==Kind::VertexBuffer || kind==Kind::IndexBuffer || kind==Kind::VertexShader || kind==Kind::PixelShader || kind==Kind::VertexDeclaration;}
bool resource_interface(Kind kind){return kind==Kind::Surface || texture_kind(kind) || kind==Kind::VertexBuffer || kind==Kind::IndexBuffer;}
constexpr Guid resource_iid{0x5d,0xc0,0xee,0x05,0x7d,0x8f,0x62,0x43,0xb9,0x99,0xd1,0xba,0xf3,0x57,0xc7,0x04};
constexpr Guid base_texture_iid{0x7e,0xa8,0x0c,0x58,0x3c,0x1d,0x54,0x4d,0x99,0x1d,0xb7,0xd3,0xe3,0xc2,0x98,0xce};
bool supports(Kind kind,const Guid& requested,bool ex=false){if(ex && ((kind==Kind::Factory && requested==iidFactoryEx) || (kind==Kind::Device && requested==iidDeviceEx) || (kind==Kind::SwapChain && requested==iidSwapChainEx)))return true;return requested==unknown || requested==iid(kind) || (resource_interface(kind) && requested==resource_iid) || (texture_kind(kind) && requested==base_texture_iid);}
struct SpriteItem {std::uint64_t texture{};std::array<Vector,6> positions{};std::array<std::array<float,2>,6> uv{};std::uint32_t color{};bool retained{};};
struct D3DLight {std::uint32_t type{3};Vector diffuse{1,1,1,0},specular{},ambient{};std::array<float,3> position{},direction{0,0,1};float range{},falloff{},attenuation0{},attenuation1{},attenuation2{},theta{},phi{};};
static_assert(sizeof(D3DLight)==104);
unsigned texture_slot(unsigned slot){require(slot<16 || (slot>=257 && slot<=260));return slot<16?slot:slot-241;}
struct D3DStream {std::uint64_t buffer{};unsigned offset{},stride{};};
struct Object {
    std::shared_ptr<d3dx_animation::Set> animation_set;
    std::shared_ptr<d3dx_animation::Controller> animation_controller;
    std::shared_ptr<d3dx_skin::Info> skin;
    std::shared_ptr<d3dx_gutter::Map> gutter;
    std::array<std::uint64_t,4> render_targets{};std::uint64_t depth_target{},depth_back{};
    std::shared_ptr<d3d9_xfile::Templates> x_templates;
    std::shared_ptr<d3d9_xfile::Document> x_document;unsigned x_index{};
    GraphicsView native_view;unsigned multisample{};bool lockable{};
    std::shared_ptr<GraphicsResource> cached_resource,cached_fixed_resource;
    std::vector<std::vector<std::uint8_t>> cached_pixels,cached_fixed_pixels;
    std::array<std::uint64_t,8> helper_refs{};
    Kind kind{};std::uint64_t root{},parent{},back{},window{},focus{},mapped{},draws{},frames{},texture{},vertex_buffer{},index_buffer{},chain{},creator{};
    std::uint32_t device_type{1},references{1},width{},height{},depth{1},format{22},pool{2},flags{},lock_flags{},fvf{},priority{},bound{},usage{},stream_offset{},stream_stride{},lock_count{},latency{3};
    bool ex{},implicit{},scene{},auto_depth{},lock_writable{},software_vertex{};
    std::array<std::uint64_t,19> extra_textures{};
    std::array<D3DStream,15> extra_streams{};
    std::array<unsigned,16> stream_frequency=[](){std::array<unsigned,16> v;v.fill(1);return v;}();
    std::array<std::uint64_t,2> shaders{};std::uint64_t vertex_declaration{};
    std::vector<std::uint64_t> subresources;unsigned levels{1},lod{};
    std::map<Guid,std::pair<std::vector<std::uint8_t>,std::uint64_t>> private_data;
    std::shared_ptr<fx9::Runtime> effect;std::shared_ptr<fx9::Pool> effect_pool;
    std::shared_ptr<Object> saved,recording;
    std::set<std::pair<unsigned,unsigned>> state_mask;
    std::vector<SpriteItem> sprites;Matrix sprite_transform=identity(),sprite_world=identity(),sprite_view=identity();bool sprite_rh{};
    std::uint64_t font_handle{},font_dc{};std::map<std::string,std::uint64_t> font_textures;std::map<unsigned,std::array<int,4>> font_bounds;
    D3dxFontDrawCache font_draw_cache;
    std::shared_ptr<d3dx_prt::Engine> prt_engine;
    std::shared_ptr<d3dx_prt::Compressed> compressed_prt;
    std::shared_ptr<d3dx9::Constants> constant_table;
    std::shared_ptr<GraphicsShader> shader;
    std::map<std::string,std::shared_ptr<GraphicsShader>> fixed_shaders;
    std::array<std::array<std::vector<std::uint8_t>,3>,2> constants;
    std::shared_ptr<GpuMemoryReservation> storage;
    std::shared_ptr<NativeOpenGlContext> native;
    std::vector<std::uint8_t> presentation,pixels;
    std::vector<std::array<std::uint32_t,5>> attributes;
    std::array<std::uint32_t,6> viewport{};
    std::array<std::int32_t,4> scissor{};
    std::array<float,17> material{1,1,1,1,1,1,1,1};
    std::map<unsigned,D3DLight> lights;std::map<unsigned,bool> light_enabled;
    std::map<std::uint32_t,std::uint32_t> sampler=[](){std::map<std::uint32_t,std::uint32_t> result;for(unsigned slot=0;slot<20;++slot)for(auto [key,value]:std::map<unsigned,unsigned>{{1,1},{2,1},{3,1},{5,1},{6,1},{7,0},{10,1}})result[slot*32+key]=value;return result;}(),stage{{1,4},{2,2},{3,1},{4,2},{5,2},{6,1},{28,1},{33,1},{36,1},{65,1},{68,1},{97,1},{100,1},{129,1},{132,1},{161,1},{164,1},{193,1},{196,1},{225,1},{228,1}};
    std::map<std::uint32_t,Matrix> transforms{{2,identity()},{3,identity()},{256,identity()}};
    std::map<std::uint32_t,std::uint32_t> render{{48,0},{60,0xffffffff},{128,0},{129,0},{130,0},{131,0},{132,0},{133,0},{134,0},{135,0},{136,1},{152,0},{176,0},{7,0},{8,3},{9,2},{14,1},{15,0},{24,0},{25,8},{26,0},{19,2},{20,1},{22,3},{23,4},{27,0},{28,0},{29,0},{52,0},{53,1},{54,1},{55,1},{56,8},{57,0},{58,0xffffffff},{59,0xffffffff},{185,0},{186,1},{187,1},{188,1},{189,8},{137,1},{139,0},{141,1},{142,1},{143,0},{145,1},{146,2},{147,0},{148,0},{151,0},{167,0},{168,15},{171,1},{174,0},{206,0}};
};
std::uint64_t call(Object& device,GlOperation op,std::initializer_list<std::uint64_t> args={},std::span<const std::uint8_t> input={},std::span<std::uint8_t> output={}){
    return device.native->execute(op,std::span(args.begin(),args.size()),input,output);
}
#include "d3d9_lighting.hpp"
void toggle(Object& device,std::uint32_t capability,bool enabled){call(device,enabled?GlOperation::Enable:GlOperation::Disable,{capability});}
void check_gpu(Object& device){require(!call(device,GlOperation::GetError),unavailable);}
void caps(GuestMemory& memory,std::uint64_t address){
    // SM3 共用 bytecode→Metal、16 組頂點串流、像素與頂點紋理階段。
    std::array<std::uint32_t,76> values{};
    values[0]=1;values[3]=0x20000000;values[5]=0x80000001U;values[7]=0x90400;values[8]=0x70;values[9]=0x1000000;
    values[15]=0x14805;values[16]=values[17]=0x07000700;values[19]=0x15;values[22]=values[23]=16384;values[25]=2048;values[26]=16384;values[27]=16;values[28]=bits(1.e10F);values[35]=1;values[36]=15;values[37]=values[38]=1;
    values[10]=255;values[11]=values[12]=0x7ff;values[14]=8;values[34]=0x1ff;
    values[39]=0x18;values[40]=8;values[42]=4;values[43]=255;values[44]=bits(1);values[45]=349525;values[46]=1048575;values[47]=16;values[48]=256;values[58]=1;values[60]=4;
    values[49]=0xfffe0300U;values[50]=256;values[51]=0xffff0300U;values[52]=bits(8);values[53]=0x41;values[62]=1;values[63]=24;values[64]=32;values[65]=4;
    values[66]=0x1f;values[67]=24;values[68]=32;values[69]=4;values[70]=512;values[71]=0x03000300;values[72]=65535;values[73]=65535;values[74]=512;values[75]=512;
    memory.write(GuestAddress{address},std::span(reinterpret_cast<const std::uint8_t*>(values.data()),sizeof(values)));
}
}
struct D3DXAtlasOperation {
    WindowsServiceDescriptor service{};std::uint64_t id{},thread{},stack{},callback{},context{};
    std::shared_ptr<Object> mesh;
    std::vector<std::uint8_t> vertices,attributes,declaration;
    unsigned stride{},flags{},texture_index{};
    std::shared_ptr<d3dx_atlas::Result> output;
    std::unique_ptr<NativeProgressJob> worker;
};
struct D3DXHierarchyOperation {
    struct Frame {const d3d9_xfile::Frame* source{};unsigned parent{UINT32_MAX};std::uint64_t address{},last_child{},last_mesh{};};
    WindowsServiceDescriptor service{};std::uint64_t id{},thread{},stack{},pending_output{},last_root{};
    std::shared_ptr<d3d9_xfile::Hierarchy> source;std::vector<Frame> frames;std::vector<std::pair<unsigned,unsigned>> meshes;
    std::vector<std::uint64_t> temporaries,owned,containers;std::vector<GuestCallback> cleanup;
    unsigned phase{},index{},callback_result{},failure{};bool awaiting{},returned{};
};
struct D3DXFillOperation {
    WindowsServiceDescriptor service{};std::uint64_t id{},thread{},stack{},temporary{};
    unsigned subresource{},index{},batch{};bool returned{};std::vector<std::uint8_t> pixels;
};
struct D3DXPRTCompOperation {
    WindowsServiceDescriptor service{};std::uint64_t id{},thread{},stack{},callback{},context{};
    std::shared_ptr<d3dx_prt::Compressed> output;std::unique_ptr<NativeProgressJob> worker;
};
struct D3DXPRTEngineOperation {
    WindowsServiceDescriptor service{};std::uint64_t id{},thread{},stack{},callback{},context{};
    std::shared_ptr<Object> target,total;std::shared_ptr<std::vector<float>> output;std::unique_ptr<NativeProgressJob> worker;
};
struct D3D9State {std::map<std::uint64_t,D3DXPRTEngineOperation> prt_engine_operations;std::map<std::uint64_t,D3DXPRTCompOperation> prt_comp_operations;std::map<std::uint64_t,D3DXFillOperation> fill_operations;std::map<std::uint64_t,D3DXHierarchyOperation> hierarchy_operations;std::map<std::uint64_t,D3DXAtlasOperation> atlas_operations;std::uint64_t next_atlas_operation{};std::map<std::uint64_t,std::shared_ptr<Object>> objects;std::map<std::pair<Kind,bool>,std::uint64_t> tables;std::vector<std::string> completed;D3D9Perf perf;};
void WindowsServices::discard_d3dx_progress(GuestState& state,GuestMemory& memory,std::uint64_t key){if(d3d9_){if(const auto found=d3d9_->fill_operations.find(key);found!=d3d9_->fill_operations.end() && found->second.temporary)(void)free_heap(memory,found->second.temporary,state);d3d9_->prt_engine_operations.erase(key);d3d9_->prt_comp_operations.erase(key);d3d9_->fill_operations.erase(key);d3d9_->atlas_operations.erase(key);d3d9_->hierarchy_operations.erase(key);}}
std::optional<std::uint32_t> WindowsServices::resume_d3dx_progress(GuestState& state,GuestMemory& memory,std::uint64_t key,std::uint32_t result){
    if(d3d9_ && d3d9_->prt_engine_operations.contains(key)){auto& operation=d3d9_->prt_engine_operations.at(key);if(operation.thread!=threads_.current)throw HostCallFault("PRT 模擬回呼執行緒不符");operation.worker->reply(result);const auto service=operation.service;return invoke_d3d9(state,memory,service);}
    if(d3d9_ && d3d9_->prt_comp_operations.contains(key)){auto& operation=d3d9_->prt_comp_operations.at(key);if(operation.thread!=threads_.current)throw HostCallFault("PRT 壓縮回呼執行緒不符");operation.worker->reply(result);const auto service=operation.service;return invoke_d3d9(state,memory,service);}
    if(d3d9_ && d3d9_->fill_operations.contains(key)){auto& operation=d3d9_->fill_operations.at(key);if(operation.thread!=threads_.current)throw HostCallFault("D3DX 貼圖回呼執行緒不符");operation.returned=true;const auto service=operation.service;return invoke_d3d9(state,memory,service);}
    if(d3d9_ && d3d9_->hierarchy_operations.contains(key)){auto& operation=d3d9_->hierarchy_operations.at(key);if(operation.thread!=threads_.current)throw HostCallFault("D3DX 階層回呼執行緒不符");operation.callback_result=result;operation.returned=true;const auto service=operation.service;return invoke_d3d9(state,memory,service);}
    if(!d3d9_ || !d3d9_->atlas_operations.contains(key))throw HostCallFault("D3DX 進度回呼缺少運算狀態");
    auto& operation=d3d9_->atlas_operations.at(key);if(operation.thread!=threads_.current)throw HostCallFault("D3DX 進度回呼執行緒不符");
    operation.worker->reply(result);const auto service=operation.service;return invoke_d3d9(state,memory,service);
}
std::string WindowsServices::d3d9_diagnostics()const{
    if(!d3d9_)return {};std::ostringstream out;for(const auto& line:d3d9_->completed)out<<line;
    for(const auto& [address,value]:d3d9_->objects)if(value->kind==Kind::Device)out<<"d3d9_backend=Metal device="<<address<<" draws="<<value->draws<<" frames="<<value->frames<<'\n';
    out<<"d3d9_live_interfaces="<<d3d9_->objects.size()<<'\n';
    if(std::getenv("X64ARM_TRACE_GRAPHICS_ERRORS"))for(const auto& [address,value]:d3d9_->objects)out<<"d3d9_live_object="<<address<<" kind="<<static_cast<unsigned>(value->kind)<<" references="<<value->references<<" bound="<<value->bound<<" bytes="<<value->pixels.size()<<'\n';
    return out.str();
}
std::span<const WindowsServiceDescriptor> d3d9_service_descriptors(){
    static const auto table=[](){std::vector<WindowsServiceDescriptor> value{{"d3d9.dll","Direct3DCreate9",1},{"d3d9.dll","Direct3DCreate9Ex",2}};
        for(const auto entry:{WindowsServiceDescriptor{"d3d9.dll","D3DPERF_BeginEvent",2},{"d3d9.dll","D3DPERF_EndEvent",0},{"d3d9.dll","D3DPERF_GetStatus",0},{"d3d9.dll","D3DPERF_SetMarker",2},{"d3d9.dll","D3DPERF_SetRegion",2},{"d3d9.dll","D3DPERF_SetOptions",1},{"d3d9.dll","D3DPERF_QueryRepeatFrame",0}})value.push_back(entry);
        for(auto family:{"d3dx9_42.dll","d3dx9_43.dll"}){value.push_back({family,"D3DXCreatePRTEngine",5});value.push_back({family,"D3DXConcatenateMeshes",8});value.push_back({family,"D3DXCreateRenderToEnvMap",7});value.push_back({family,"D3DXCreateRenderToSurface",7});value.push_back({family,"D3DXFileCreate",1});value.push_back({family,"D3DXCreateSkinInfo",4});value.push_back({family,"D3DXCreateSkinInfoFVF",4});value.push_back({family,"D3DXCreateTextureGutterHelper",5});}
        for(auto family:{"d3dx9_42.dll","d3dx9_43.dll"})for(auto entry:{WindowsServiceDescriptor{family,"D3DXCreateMesh",6},{family,"D3DXCreateMeshFVF",6},{family,"D3DXGetDeclLength",1},{family,"D3DXGetFVFVertexSize",1},{family,"D3DXComputeNormals",2},{family,"D3DXComputeTangentFrameEx",16},{family,"D3DXTessellateNPatches",6}})value.push_back(entry);
        for(auto family:{"d3dx9_42.dll","d3dx9_43.dll"})for(auto entry:{WindowsServiceDescriptor{family,"D3DXCreatePRTCompBuffer",7},{family,"D3DXLoadPRTCompBufferFromFileA",2},{family,"D3DXLoadPRTCompBufferFromFileW",2},{family,"D3DXSavePRTCompBufferToFileA",2},{family,"D3DXSavePRTCompBufferToFileW",2}})value.push_back(entry);
        for(auto family:{"d3dx9_42.dll","d3dx9_43.dll"})for(auto entry:{WindowsServiceDescriptor{family,"D3DXLoadPRTBufferFromFileW",2},{family,"D3DXLoadPRTBufferFromFileA",2},{family,"D3DXCreatePRTBuffer",4},{family,"D3DXCreatePRTBufferTex",5},{family,"D3DXSavePRTBufferToFileW",2},{family,"D3DXSavePRTBufferToFileA",2}})value.push_back(entry);
        for(auto family:{"d3dx9_42.dll","d3dx9_43.dll"})for(auto entry:{WindowsServiceDescriptor{family,"D3DXGetDeclVertexSize",2},{family,"D3DXCreateBuffer",2},{family,"D3DXIntersect",10},{family,"D3DXIntersectSubset",11},{family,"D3DXCreatePolygon",5}})value.push_back(entry);
        for(auto family:{"d3dx9_42.dll","d3dx9_43.dll"})for(auto entry:{WindowsServiceDescriptor{family,"D3DXConvertMeshSubsetToSingleStrip",5},{family,"D3DXConvertMeshSubsetToStrips",7}})value.push_back(entry);
        for(auto family:{"d3dx9_42.dll","d3dx9_43.dll"})for(auto entry:{WindowsServiceDescriptor{family,"D3DXAssembleShaderFromFileW",6},{family,"D3DXAssembleShaderFromFileA",6},{family,"D3DXAssembleShader",7}})value.push_back(entry);
        for(auto family:{"d3dx9_42.dll","d3dx9_43.dll"})for(auto entry:{WindowsServiceDescriptor{family,"D3DXCompileShaderFromFileW",9},{family,"D3DXCompileShaderFromFileA",9},{family,"D3DXCompileShader",10}})value.push_back(entry);
        for(auto family:{"d3dx9_42.dll","d3dx9_43.dll"})for(auto entry:{WindowsServiceDescriptor{family,"D3DXGetShaderInputSemantics",3},{family,"D3DXGetShaderOutputSemantics",3},{family,"D3DXGetShaderSize",1},{family,"D3DXGetShaderVersion",1}})value.push_back(entry);
        for(auto family:{"d3dx9_42.dll","d3dx9_43.dll"})for(auto entry:{WindowsServiceDescriptor{family,"D3DXGetImageInfoFromFileW",2},{family,"D3DXGetImageInfoFromFileA",2},{family,"D3DXGetImageInfoFromFileInMemory",3}})value.push_back(entry);
        for(auto family:{"d3dx9_42.dll","d3dx9_43.dll"})for(auto entry:{WindowsServiceDescriptor{family,"D3DXValidMesh",3},{family,"D3DXCleanMesh",6}})value.push_back(entry);
        for(auto family:{"d3dx9_42.dll","d3dx9_43.dll"})for(auto entry:{WindowsServiceDescriptor{family,"D3DXFillTexture",3},{family,"D3DXFillCubeTexture",3},{family,"D3DXSHProjectCubeMap",5}})value.push_back(entry);
        for(auto family:{"d3dx9_42.dll","d3dx9_43.dll"})for(auto entry:{WindowsServiceDescriptor{family,"D3DXComputeNormalMap",6},{family,"D3DXCreateTexture",8},{family,"D3DXCreateTextureFromFileW",3},{family,"D3DXCreateTextureFromFileA",3},{family,"D3DXCreateTextureFromFileExW",14},{family,"D3DXCreateTextureFromFileExA",14},{family,"D3DXCreateTextureFromFileInMemory",4},{family,"D3DXCreateTextureFromFileInMemoryEx",15},{family,"D3DXCreateTextureFromResourceW",4},{family,"D3DXCreateTextureFromResourceA",4},{family,"D3DXCreateTextureFromResourceExW",15},{family,"D3DXCreateTextureFromResourceExA",15}})value.push_back(entry);
        for(auto family:{"d3dx9_42.dll","d3dx9_43.dll"})for(auto entry:{WindowsServiceDescriptor{family,"D3DXCreateCubeTexture",7},{family,"D3DXCreateCubeTextureFromFileW",3},{family,"D3DXCreateCubeTextureFromFileA",3},{family,"D3DXCreateCubeTextureFromFileExW",13},{family,"D3DXCreateCubeTextureFromFileExA",13},{family,"D3DXCreateCubeTextureFromFileInMemory",4},{family,"D3DXCreateCubeTextureFromFileInMemoryEx",14},{family,"D3DXCreateCubeTextureFromResourceW",4},{family,"D3DXCreateCubeTextureFromResourceA",4},{family,"D3DXCreateCubeTextureFromResourceExW",14},{family,"D3DXCreateCubeTextureFromResourceExA",14}})value.push_back(entry);
        for(auto family:{"d3dx9_42.dll","d3dx9_43.dll"})for(auto entry:{WindowsServiceDescriptor{family,"D3DXCreateTextA",8},{family,"D3DXCreateTextW",8}})value.push_back(entry);
        for(auto family:{"d3dx9_42.dll","d3dx9_43.dll"})for(auto entry:{WindowsServiceDescriptor{family,"D3DXCreateSprite",2},{family,"D3DXCreateFontA",12},{family,"D3DXCreateFontW",12},{family,"D3DXCreateFontIndirectA",3},{family,"D3DXCreateFontIndirectW",3}})value.push_back(entry);
        for(auto family:{"d3dx9_42.dll","d3dx9_43.dll"})for(auto entry:{WindowsServiceDescriptor{family,"D3DXCreateEffectPool",1},{family,"D3DXCreateEffect",9},{family,"D3DXCreateEffectFromFileA",8},{family,"D3DXCreateEffectFromFileW",8}})value.push_back(entry);
        for(auto family:{"d3dx9_42.dll","d3dx9_43.dll"})for(auto entry:{WindowsServiceDescriptor{family,"D3DXSaveMeshToXA",7},{family,"D3DXSaveMeshToXW",7},{family,"D3DXSaveTextureToFileA",4},{family,"D3DXSaveTextureToFileW",4},{family,"D3DXSaveTextureToFileInMemory",4}})value.push_back(entry);
        for(auto family:{"d3dx9_42.dll","d3dx9_43.dll"})for(auto entry:{WindowsServiceDescriptor{family,"D3DXUVAtlasCreate",19},{family,"D3DXComputeIMTFromPerVertexSignal",8},{family,"D3DXComputeIMTFromPerTexelSignal",11},{family,"D3DXComputeIMTFromTexture",7}})value.push_back(entry);
        for(auto family:{"d3dx9_42.dll","d3dx9_43.dll"})for(auto entry:{WindowsServiceDescriptor{family,"D3DXFrameFind",2},{family,"D3DXFrameDestroy",2},{family,"D3DXFrameAppendChild",2},{family,"D3DXFrameRegisterNamedMatrices",2},{family,"D3DXFrameNumNamedMatrices",1},{family,"D3DXFrameCalculateBoundingSphere",3}})value.push_back(entry);
        for(auto family:{"d3dx9_42.dll","d3dx9_43.dll"})for(auto entry:{WindowsServiceDescriptor{family,"D3DXCreateKeyframedAnimationSet",7,2},{family,"D3DXCreateCompressedAnimationSet",7,2},{family,"D3DXCreateAnimationController",5}})value.push_back(entry);
        for(auto family:{"d3dx9_42.dll","d3dx9_43.dll"})for(auto entry:{WindowsServiceDescriptor{family,"D3DXLoadMeshHierarchyFromXA",7},{family,"D3DXLoadMeshHierarchyFromXW",7},{family,"D3DXLoadMeshHierarchyFromXInMemory",8}})value.push_back(entry);
        for(auto family:{"d3dx9_42.dll","d3dx9_43.dll"})for(auto entry:{WindowsServiceDescriptor{family,"D3DXCreateVolumeTexture",9},{family,"D3DXCreateVolumeTextureFromFileW",3},{family,"D3DXCreateVolumeTextureFromFileA",3},{family,"D3DXCreateVolumeTextureFromFileExW",15},{family,"D3DXCreateVolumeTextureFromFileExA",15},{family,"D3DXCreateVolumeTextureFromFileInMemory",4},{family,"D3DXCreateVolumeTextureFromFileInMemoryEx",16},{family,"D3DXCreateVolumeTextureFromResourceW",4},{family,"D3DXCreateVolumeTextureFromResourceA",4},{family,"D3DXCreateVolumeTextureFromResourceExW",16},{family,"D3DXCreateVolumeTextureFromResourceExA",16}})value.push_back(entry);
        for(auto kind:{Kind::VolumeTexture,Kind::Volume,Kind::PRTEngine,Kind::RenderToEnvMap,Kind::PRTComp,Kind::AnimSet,Kind::CompressedSet,Kind::AnimController,Kind::Skin,Kind::Gutter,Kind::XFile,Kind::XEnum,Kind::XData,Kind::RenderToSurface,Kind::Factory,Kind::Device,Kind::Surface,Kind::Texture,Kind::CubeTexture,Kind::VertexBuffer,Kind::IndexBuffer,Kind::SwapChain,Kind::Mesh,Kind::Blob,Kind::PRT,Kind::VertexShader,Kind::PixelShader,Kind::VertexDeclaration,Kind::ConstantTable,Kind::Sprite,Kind::Font,Kind::StateBlock,Kind::Effect,Kind::EffectPool})for(auto entry:methods(kind))value.push_back(entry);
        for(auto family:{"d3dx9_42.dll","d3dx9_43.dll"})for(auto entry:{WindowsServiceDescriptor{family,"D3DXLoadMeshFromXW",8},{family,"D3DXLoadMeshFromXA",8},{family,"D3DXLoadMeshFromXInMemory",9}})value.push_back(entry);
        for(auto kind:{Kind::Factory,Kind::Device,Kind::SwapChain})for(auto entry:methods(kind,true))value.push_back(entry);return value;}();return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_d3d9(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(!runtime_.network.external_io)throw HostCallFault("Direct3D 9 Metal 裝置不支援 deterministic compare");
    if(!d3d9_)d3d9_=std::make_shared<D3D9State>();auto& bridge=*d3d9_;
    const WindowsCall arg(state,memory,service);auto symbol=service.symbol;const unsigned ps=x86_?4:8;const auto pb=static_cast<std::uint8_t>(ps*8);
    std::vector<GuestCallback> private_callbacks;
    const auto private_ref=[&](std::uint64_t pointer,unsigned slot){const auto table=memory.load(GuestAddress{pointer},pb);private_callbacks.push_back({memory.load(GuestAddress{table+slot*ps},pb),{pointer}});};
    const auto u=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    if(symbol.starts_with("D3DPERF_")){
        std::uint32_t result=0;
        if(symbol=="D3DPERF_GetStatus")result=bridge.perf.active()?1:0;
        else if(symbol=="D3DPERF_SetOptions")bridge.perf.options(u(0));
        else if(symbol=="D3DPERF_EndEvent")result=bridge.perf.end(threads_.current);
        else if(symbol=="D3DPERF_QueryRepeatFrame")result=0; // 記錄器沒有提出 frame replay 請求。
        else if(bridge.perf.active()){
            const auto name=arg(1)?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(1)},32768)):std::string{};
            if(symbol=="D3DPERF_BeginEvent")result=bridge.perf.begin(threads_.current,u(0),name);
            else bridge.perf.marker(threads_.current,u(0),name,symbol=="D3DPERF_SetRegion");
        }else if(symbol=="D3DPERF_BeginEvent")result=UINT32_MAX;
        arg.return_to(state,result);return std::nullopt;
    }
    const auto return_scalar=[&](double value,bool single){
        if(x86_){const auto top=((state.x87_status>>11)+7U)&7U;const auto converted=f64_to_extF80({std::bit_cast<std::uint64_t>(value)});state.x87[top]={converted.signif,converted.signExp};state.x87_status=static_cast<std::uint16_t>((state.x87_status&~0x3800U)|(top<<11));state.x87_tag=static_cast<std::uint16_t>((state.x87_tag&~(3U<<(top*2)))|(x87_tag(state.x87[top])<<(top*2)));}
        else {const std::uint64_t raw=single?std::bit_cast<std::uint32_t>(static_cast<float>(value)):std::bit_cast<std::uint64_t>(value);for(unsigned i=0;i<(single?4U:8U);++i)state.xmm[0][i]=static_cast<std::uint8_t>(raw>>(8*i));}
    };
    const auto read=[&](std::uint64_t address,std::size_t size){std::vector<std::uint8_t> data(size);memory.read(GuestAddress{address},data);return data;};
    const auto shader_code=[&](std::uint64_t address){require(address);auto code=read(address,4);const auto version=word(code,0);const bool pixel=(version>>16)==0xffff;require(pixel || (version>>16)==0xfffe);
                    // SM2/3 指令內帶 operand 長度；SM1 由公開 token 規則掃描至 END，完整合法性由編譯器檢查。
                    bool ended=false;for(std::size_t at=1;at<1048576;){const auto token=static_cast<std::uint32_t>(memory.load(GuestAddress{address+at*4},32));unsigned length=1;if((token&0xffff)==0xffff){auto end=read(address+at*4,4);code.insert(code.end(),end.begin(),end.end());ended=true;break;}if((token&0xffff)==0xfffe)length+=((token>>16)&0x7fff);else if(((version>>8)&255)>=2)length+=(token>>24)&15;
                        else{switch(token&0xffff){case 0:case 0xfffd:break;case 1:case 6:case 7:case 14:case 15:case 16:case 19:case 78:case 79:length=3;break;case 4:case 18:case 80:case 88:length=5;break;case 31:length=3;break;case 81:length=6;break;case 65:case 87:length=2;break;case 64:case 66:length=(pixel && (version&255)>=4)?3:2;break;case 76:case 89:length=4;break;default:require(((token&0xffff)>=2 && (token&0xffff)<=24) || ((token&0xffff)>=67 && (token&0xffff)<=86));length=4;if((token&0xffff)>=67)length=3;break;}}
                        require(at+length<=1048576);auto instruction=read(address+at*4,std::size_t(length)*4);code.insert(code.end(),instruction.begin(),instruction.end());at+=length;}
                    require(ended);return code;};
    const auto store_pointer=[&](std::uint64_t address,std::uint64_t pointer){require(address!=0);memory.store(GuestAddress{address},pb,pointer);};
    const auto output=[&](unsigned i){store_pointer(arg(i),0);};
    const auto get=[&](std::uint64_t address,Kind kind){const auto it=bridge.objects.find(address);require(it!=bridge.objects.end() && it->second->kind==kind);return it->second;};
    const auto get_texture=[&](std::uint64_t address){const auto it=bridge.objects.find(address);require(it!=bridge.objects.end() && texture_kind(it->second->kind));return it->second;};
    std::function<std::uint32_t(std::uint64_t)> release;
    std::function<void(const std::shared_ptr<Object>&)> destroy;
    const auto hold=[&](const std::shared_ptr<Object>& value){require(value->references<UINT32_MAX);
        if((value->implicit || resource(value->kind)) && !value->references){auto parent=bridge.objects.at(value->parent);require(parent->references<UINT32_MAX);++parent->references;}return ++value->references;};
    const auto drop_binding=[&](std::uint64_t& binding){if(!binding)return;auto entry=bridge.objects.at(binding);binding=0;require(entry->bound!=0);--entry->bound;if(!entry->references && !entry->bound)destroy(entry);};
    const auto bind_target=[&](std::uint64_t& binding,std::uint64_t address){
        if(binding==address)return;
        if(address){auto surface=get(address,Kind::Surface);++surface->bound;auto owner=bridge.objects.at(surface->parent);if(texture_kind(owner->kind))++owner->bound;}
        const auto previous=binding;binding=address;if(!previous)return;
        auto surface=get(previous,Kind::Surface);require(surface->bound!=0);--surface->bound;auto owner=bridge.objects.at(surface->parent);
        if(texture_kind(owner->kind)){require(owner->bound!=0);if(!--owner->bound && !owner->references)destroy(owner);}
        else if(!surface->implicit && !surface->bound && !surface->references)destroy(surface);
    };
    const auto unbind_targets=[&](Object& device){for(auto& target:device.render_targets)bind_target(target,0);bind_target(device.depth_target,0);};
    const auto unbind=[&](Object& device){for(auto& shader:device.shaders)drop_binding(shader);drop_binding(device.vertex_declaration);drop_binding(device.texture);for(auto& texture:device.extra_textures)drop_binding(texture);drop_binding(device.vertex_buffer);drop_binding(device.index_buffer);for(auto& stream:device.extra_streams){drop_binding(stream.buffer);stream.offset=stream.stride=0;}device.stream_frequency.fill(1);device.stream_offset=device.stream_stride=0;};
    const auto saved_bindings=[](const Object& value){std::array<std::uint64_t,40> pointers{value.shaders[0],value.shaders[1],value.vertex_declaration,value.texture,value.vertex_buffer,value.index_buffer};std::copy(value.extra_textures.begin(),value.extra_textures.end(),pointers.begin()+6);for(unsigned i=0;i<15;++i)pointers[25+i]=value.extra_streams[i].buffer;return pointers;};
    destroy=[&](const std::shared_ptr<Object>& value){
        if(value->animation_set){auto& set=*value->animation_set;if(set.guest_name)(void)free_heap(memory,set.guest_name,state);for(const auto& animation:set.animations)if(animation.guest_name)(void)free_heap(memory,animation.guest_name,state);}
        if(value->animation_controller){for(auto pointer:value->animation_controller->sets)release(pointer);for(const auto& track:value->animation_controller->tracks)if(track.set)release(track.set);}
        if(value->skin)for(auto& bone:value->skin->bones){if(bone.guest_name)(void)free_heap(memory,bone.guest_name,state);if(bone.guest_offset)(void)free_heap(memory,bone.guest_offset,state);}
        const auto release_effect_value=[&](const std::shared_ptr<fx9::Value>& item){if(item.use_count()==1)for(auto pointer:item->interfaces)if(pointer)release(pointer);};
        if(value->effect){auto& fx=*value->effect;for(const auto& [key,item]:fx.recorded_values){(void)key;release_effect_value(item);}for(const auto& update:fx.recording_updates)if(update.texture)release(update.texture);for(const auto& [key,updates]:fx.blocks){(void)key;for(const auto& update:updates)if(update.texture)release(update.texture);}for(const auto& item:fx.values)release_effect_value(item);fx.values.clear();for(const auto& [key,pointer]:fx.shaders){(void)key;release(pointer);}for(auto pointer:fx.allocations)(void)free_heap(memory,pointer,state);if(fx.manager)private_ref(fx.manager,2);if(fx.pool)release(fx.pool);}
        if(value->effect_pool)for(const auto& [key,item]:value->effect_pool->values){(void)key;release_effect_value(item);}

        for(const auto& [key,data]:value->private_data){(void)key;if(data.second)private_ref(data.second,2);}
        if(value->saved){for(auto pointer:saved_bindings(*value->saved))if(pointer)release(pointer);value->saved.reset();}
        for(auto pointer:value->helper_refs)if(pointer)release(pointer);
        for(const auto& item:value->sprites)if(item.retained)release(item.texture);
        for(const auto& [key,pointer]:value->font_textures){(void)key;release(pointer);}
        if(value->font_dc){auto it=gui_.contexts.find(value->font_dc);if(it!=gui_.contexts.end()){if(it->second.bitmap)gui_.bitmaps.erase(it->second.bitmap);gui_.contexts.erase(it);}gui_.fonts.erase(value->font_handle);}
        if(value->kind==Kind::Mesh){if(value->vertex_buffer)release(value->vertex_buffer);if(value->index_buffer)release(value->index_buffer);}
        if(value->kind==Kind::Device){unbind_targets(*value);if(value->recording)unbind(*value->recording);unbind(*value);if(bridge.completed.size()<64)bridge.completed.push_back("d3d9_backend=Metal draws="+std::to_string(value->draws)+" frames="+std::to_string(value->frames)+"\n");}
        if(value->chain){auto chain=get(value->chain,Kind::SwapChain);require(!chain->references);bridge.objects.erase(chain->root);(void)free_heap(memory,chain->root,state);}
        if(!value->subresources.empty()){for(auto address:value->subresources){auto surface=get(address,value->kind==Kind::VolumeTexture?Kind::Volume:Kind::Surface);require(!surface->references);if(surface->mapped)(void)free_heap(memory,surface->mapped,state);bridge.objects.erase(address);(void)free_heap(memory,address,state);}}
        else if(value->back){const auto back=get(value->back,Kind::Surface);require(!back->references);if(back->mapped)(void)free_heap(memory,back->mapped,state);bridge.objects.erase(back->root);(void)free_heap(memory,back->root,state);}
        if(value->depth_back){auto depth=get(value->depth_back,Kind::Surface);require(!depth->references);bridge.objects.erase(depth->root);(void)free_heap(memory,depth->root,state);}
        if(value->mapped)(void)free_heap(memory,value->mapped,state);
        bridge.objects.erase(value->root);(void)free_heap(memory,value->root,state);
    };
    release=[&](std::uint64_t address)->std::uint32_t{
        const auto value=bridge.objects.at(address);require(value->references!=0);const auto remaining=--value->references;if(remaining)return remaining;
        if(!value->implicit && !value->bound)destroy(value);
        if(value->parent)release(value->parent);return 0;
    };
    const auto create=[&](Kind kind,std::uint64_t parent=0,bool implicit=false,bool ex=false){
        require(bridge.objects.size()<2048,out_of_memory);
        const auto key=std::pair{kind,ex};if(!bridge.tables.contains(key)){auto entries=methods(kind,ex);const auto address=runtime_allocate(memory,{},entries.size()*ps);
            for(std::size_t i=0;i<entries.size();++i)memory.store(GuestAddress{address+i*ps},pb,resolve(entries[i].family,entries[i].symbol).value);bridge.tables[key]=address;}
        auto value=std::make_shared<Object>();value->root=allocate_heap(memory,ps);require(value->root!=0,out_of_memory);value->kind=kind;value->ex=ex;value->implicit=implicit;value->references=implicit?0:1;
        try{bridge.objects.emplace(value->root,value);memory.store(GuestAddress{value->root},pb,bridge.tables.at(key));}
        catch(...){bridge.objects.erase(value->root);(void)free_heap(memory,value->root,state);throw;}
        value->parent=parent;if(parent && !implicit)hold(bridge.objects.at(parent));return value;
    };
#include "d3d9_state_block.inc"
    const auto make_texture=[&](const std::shared_ptr<Object>& device,unsigned width,unsigned height,unsigned levels,unsigned usage,unsigned format,unsigned pool,bool cube=false,unsigned depth=1,bool volume=false){
        require(!cube || (width==height && !volume));require(depth && (!volume || (depth<=2048 && width<=2048 && height<=2048)));require(width && height && width<=16384 && height<=16384 && pool<=3 && (!device->ex || pool!=1));const auto native=native_format(format);const auto maximum=static_cast<unsigned>(std::bit_width(std::max({width,height,depth})));if(!levels)levels=maximum;require(levels<=maximum);require(usage==0 || ((usage==0x200 || (usage==1 && render_color_format(format))) && pool==0),unavailable);
        require(!volume || usage!=1);auto texture=create(volume?Kind::VolumeTexture:cube?Kind::CubeTexture:Kind::Texture,device->root);texture->depth=depth;texture->width=width;texture->height=height;texture->levels=levels;texture->format=format;texture->pool=pool;texture->usage=usage;
        try{if(usage==1){GraphicsResourceDesc desc;desc.dimension=3;desc.width=width;desc.height=height;desc.levels=levels;desc.layers=cube?6:1;desc.format=native;desc.bind=0x28;desc.misc=cube?4:0;texture->native_view={device->native->graphics_resource(desc),native,0,levels,0,desc.layers};texture->native_view.dimension=cube?9:4;}
            texture->subresources.reserve(levels*(cube?6U:1U));for(unsigned face=0;face<(cube?6U:1U);++face)for(unsigned level=0;level<levels;++level){auto surface=create(volume?Kind::Volume:Kind::Surface,texture->root,true);surface->depth=std::max(1U,depth>>level);texture->subresources.push_back(surface->root);if(!face && !level)texture->back=surface->root;surface->width=std::max(1U,width>>level);surface->height=std::max(1U,height>>level);surface->format=format;surface->pool=pool;surface->usage=usage;if(texture->native_view.resource)surface->native_view={texture->native_view.resource,native,level,1,face,1};const auto pitch=graphics_format_pitch(native,surface->width,surface->height);const auto size=std::size_t(pitch[0])*pitch[1]*surface->depth;surface->storage=reserve_gpu_memory(size);surface->pixels.resize(size);}}
        catch(...){release(texture->root);throw;}return texture;
    };
    const auto surface_device=[&](const Object& surface){auto owner=bridge.objects.at(surface.parent);if(texture_kind(owner->kind))owner=bridge.objects.at(owner->parent);require(owner->kind==Kind::Device);return owner;};
    const auto read_surface=[&](Object& surface){if(surface.native_view.resource){require(!surface.multisample);surface.pixels=surface_device(surface)->native->graphics_read(surface.native_view);}};
    const auto write_surface=[&](Object& surface){if(surface.native_view.resource){const auto pitch=graphics_format_pitch(surface.native_view.format,surface.width,surface.height);surface_device(surface)->native->graphics_write(surface.native_view,surface.pixels,pitch[0],pitch[0]*pitch[1]);}};
    const auto target_size=[&](const Object& device){auto target=get(device.render_targets[0],Kind::Surface);return std::array<unsigned,2>{target->width,target->height};};
    const auto fixed_targets=[&](Object& device){auto color=get(device.render_targets[0],Kind::Surface);GraphicsView depth;if(device.depth_target)depth=get(device.depth_target,Kind::Surface)->native_view;device.native->graphics_fixed_targets(color->native_view,depth);};
    static const bool resource_cache=[] {const auto* value=std::getenv("X64ARM_D3D9_RESOURCE_CACHE");return !value || std::string_view(value)!="0";}();
    // 快取不可變內容快照；任何 CPU 寫入都以完整內容比較偵測，舊 GPU 資源留給在途命令。
    const auto texture_resource=[&](Object& device,Object& texture,bool fixed=false){
        if(texture.native_view.resource)return texture.native_view.resource;
        const bool cube=texture.kind==Kind::CubeTexture,volume=texture.kind==Kind::VolumeTexture;
        GraphicsResourceDesc d;d.dimension=volume?4:3;d.depth=texture.depth;d.width=texture.width;d.height=texture.height;d.format=fixed || (volume && graphics_format_bc_family(sampled_format(texture.format)))?28:sampled_format(texture.format);d.bind=8;d.levels=texture.levels;d.layers=cube?6:1;d.misc=cube?4:0;
        auto& cached=fixed?texture.cached_fixed_resource:texture.cached_resource;auto& pixels=fixed?texture.cached_fixed_pixels:texture.cached_pixels;
        bool same=resource_cache && cached && pixels.size()==texture.subresources.size();
        for(std::size_t i=0;i<texture.subresources.size();++i){const auto surface=get(texture.subresources[i],volume?Kind::Volume:Kind::Surface);require(!surface->mapped);if(same && pixels[i]!=surface->pixels)same=false;}
        if(same)return cached;
        auto native=device.native->graphics_resource(d);std::vector<std::vector<std::uint8_t>> snapshots;
        if(resource_cache)snapshots.reserve(texture.subresources.size());
        for(unsigned face=0;face<d.layers;++face)for(unsigned level=0;level<d.levels;++level){const auto surface=get(texture.subresources.at(face*d.levels+level),volume?Kind::Volume:Kind::Surface);const auto pitch=graphics_format_pitch(d.format,surface->width,surface->height);auto bytes=sampled_pixels(surface->pixels,texture.format);if(d.format!=sampled_format(texture.format))bytes=texture_image::transform(bytes,sampled_format(texture.format),{surface->width,surface->height,surface->depth},{0,0,0,surface->width,surface->height,surface->depth},d.format,{surface->width,surface->height,surface->depth},1);device.native->graphics_write(GraphicsView{native,d.format,level,1,face},bytes,pitch[0],pitch[0]*pitch[1]);if(resource_cache)snapshots.push_back(surface->pixels);}
        if(resource_cache){cached=native;pixels=std::move(snapshots);}return native;
    };
    const auto upload=[&](Object& device,Object& texture){
        require(texture.kind!=Kind::VolumeTexture,unavailable);call(device,GlOperation::BindTexture,{0xde1,1});call(device,GlOperation::TexParameteri,{0xde1,0x813d,texture.levels-1});
        device.native->graphics_fixed_texture({});
        if(resource_cache && (!texture.native_view.resource || texture.native_view.format==28 || texture.native_view.format==87)){auto native=texture_resource(device,texture,true);GraphicsView view{native,native->desc.format,0,texture.levels};view.dimension=4;if(device.native->graphics_fixed_texture(view))return;}
        for(unsigned level=0;level<texture.levels;++level){
            const auto surface=get(texture.subresources.at(level),Kind::Surface);require(!surface->mapped);read_surface(*surface);auto rgba=sampled_pixels(surface->pixels,texture.format);
            rgba=texture_image::transform(rgba,sampled_format(texture.format),{surface->width,surface->height,1},{0,0,0,surface->width,surface->height,1},28,{surface->width,surface->height,1},1);
            call(device,GlOperation::TexImage2D,{0xde1,level,0x1908,surface->width,surface->height,0,0x1908,0x1401},rgba);check_gpu(device);
        }
    };
    const auto surface_desc=[&](const Object& surface,std::uint64_t address){require(address!=0);const std::array<std::uint32_t,8> desc{surface.format,1,surface.usage,surface.pool,surface.multisample,0,surface.width,surface.height};memory.write(GuestAddress{address},std::span(reinterpret_cast<const std::uint8_t*>(desc.data()),sizeof(desc)));};
    const auto volume_desc=[&](const Object& volume,std::uint64_t address){require(address);const std::array<std::uint32_t,7> desc{volume.format,2,volume.usage,volume.pool,volume.width,volume.height,volume.depth};memory.write(GuestAddress{address},std::span(reinterpret_cast<const std::uint8_t*>(desc.data()),sizeof(desc)));};
    const auto volume_box=[&](const Object& volume,std::uint64_t address){std::array<unsigned,6> box{0,0,volume.width,volume.height,0,volume.depth};if(address){auto data=read(address,24);for(unsigned i=0;i<6;++i)box[i]=word(data,i*4);}require(box[0]<box[2] && box[1]<box[3] && box[4]<box[5] && box[2]<=volume.width && box[3]<=volume.height && box[5]<=volume.depth);return box;};
    const auto volume_lock=[&](Object& volume,std::uint64_t address,std::uint64_t area,unsigned flags){
        require(address && !volume.mapped && (volume.pool!=0 || volume.usage==0x200));require(!(flags&~0xa810U));require(!(flags&0x2000U) || (volume.usage==0x200 && !area));require(!(flags&0x10U) || !(flags&0x2000U));
        memory.check(GuestAddress{address},8+pb/8,Access::Write);const auto box=volume_box(volume,area);const auto format=native_format(volume.format),block=graphics_format_bc_family(format)?4U:1U;require(!(box[0]%block) && !(box[1]%block) && (!(box[2]%block) || box[2]==volume.width) && (!(box[3]%block) || box[3]==volume.height));
        const auto pitch=graphics_format_pitch(format,volume.width,volume.height),unit=graphics_format_pitch(format,block,block);const auto slice=std::size_t(pitch[0])*pitch[1];volume.mapped=allocate_heap(memory,volume.pixels.size());require(volume.mapped,out_of_memory);volume.lock_flags=flags;memory.write(GuestAddress{volume.mapped},volume.pixels);memory.store(GuestAddress{address},32,pitch[0]);memory.store(GuestAddress{address+4},32,slice);store_pointer(address+8,volume.mapped+std::size_t(box[4])*slice+std::size_t(box[1]/block)*pitch[0]+std::size_t(box[0]/block)*unit[0]);
    };
    const auto surface_lock=[&](Object& surface,std::uint64_t address,std::uint64_t area,std::uint32_t flags){
        const auto parent=bridge.objects.at(surface.parent);const bool texture=texture_kind(parent->kind),dynamic=texture && surface.usage==0x200;
        require(address && !surface.mapped && !(flags&~0xe810U) && (!surface.implicit || texture || surface.lockable));require(!surface.native_view.resource || surface.lockable);
        require(!texture || surface.pool!=0 || dynamic);require(!(flags&0x2000) || (dynamic && !area && !(flags&0x10)));
        memory.check(GuestAddress{address},2*ps,Access::Write);
        std::array<std::int32_t,4> rect{0,0,static_cast<std::int32_t>(surface.width),static_cast<std::int32_t>(surface.height)};
        if(area){auto bytes=read(area,16);std::memcpy(rect.data(),bytes.data(),16);}
        require(rect[0]>=0 && rect[1]>=0 && rect[2]>rect[0] && rect[3]>rect[1] && static_cast<std::uint32_t>(rect[2])<=surface.width && static_cast<std::uint32_t>(rect[3])<=surface.height);
        const auto format=native_format(surface.format),block=graphics_format_bc_family(format)?4U:1U;const auto pitch=graphics_format_pitch(format,surface.width,surface.height),unit_pitch=graphics_format_pitch(format,block,block);require(!(static_cast<unsigned>(rect[0])%block) && !(static_cast<unsigned>(rect[1])%block) && (!(static_cast<unsigned>(rect[2])%block) || static_cast<unsigned>(rect[2])==surface.width) && (!(static_cast<unsigned>(rect[3])%block) || static_cast<unsigned>(rect[3])==surface.height));
        read_surface(surface);surface.mapped=allocate_heap(memory,surface.pixels.size());require(surface.mapped!=0,out_of_memory);surface.lock_flags=flags;memory.write(GuestAddress{surface.mapped},surface.pixels);
        memory.store(GuestAddress{address},32,pitch[0]);store_pointer(address+ps,surface.mapped+std::uint64_t{static_cast<unsigned>(rect[1])/block}*pitch[0]+std::uint64_t{static_cast<unsigned>(rect[0])/block}*unit_pitch[0]);
    };
    const auto surface_unlock=[&](Object& surface){require(surface.mapped!=0);if(!(surface.lock_flags&0x10)){surface.pixels=read(surface.mapped,surface.pixels.size());write_surface(surface);}(void)free_heap(memory,surface.mapped,state);surface.mapped=0;};
    const auto display=[&](std::uint64_t address){const std::array<std::uint32_t,4> mode{1440,900,60,22};require(address!=0);memory.write(GuestAddress{address},std::span(reinterpret_cast<const std::uint8_t*>(mode.data()),sizeof(mode)));};
    const auto display_ex=[&](std::uint64_t mode,std::uint64_t rotation){
        require(mode || rotation);if(mode){require(memory.load(GuestAddress{mode},32)==24);const std::array<std::uint32_t,6> data{24,1440,900,60,22,1};memory.write(GuestAddress{mode},std::span(reinterpret_cast<const std::uint8_t*>(data.data()),24));}
        if(rotation)memory.store(GuestAddress{rotation},32,1);
    };
    const auto present=[&](Object& device,std::uint64_t source,std::uint64_t destination,std::uint64_t window,std::uint64_t dirty,std::uint32_t flags){
        require(!device.scene && !source && !destination && (!window || window==device.window) && !dirty && !flags);require(gui_.windows.contains(device.window));
        if(device.device_type==4){++device.frames;return;}
        device.native->bind();auto back=get(device.back,Kind::Surface);if(back->multisample)device.native->graphics_resolve(device.native->graphics_drawable(false),back->native_view,back->native_view.format);require(device.native->swap(),unavailable);++device.frames;
        if(const auto* directory=std::getenv("X64ARM_D3D9_CAPTURE_DIR");directory && *directory){bool capture=device.frames==1;if(const auto* frames=std::getenv("X64ARM_D3D9_CAPTURE_FRAMES")){std::istringstream input(frames);std::string token;while(std::getline(input,token,',')){char* end=nullptr;const auto frame=std::strtoull(token.c_str(),&end,10);if(end && !*end && frame==device.frames)capture=true;}}if(capture){if(std::getenv("X64ARM_D3D9_CAPTURE_SURFACES"))for(const auto& [id,surface]:bridge.objects)if(surface->kind==Kind::Surface && (surface->usage&1) && !surface->multisample && surface->native_view.resource && surface_device(*surface)->root==device.root){const auto bytes=device.native->graphics_read(surface->native_view);std::ofstream out(std::filesystem::path(directory)/("surface-"+std::to_string(id)+"-frame-"+std::to_string(device.frames)+"-format-"+std::to_string(surface->native_view.format)+"-"+std::to_string(surface->width)+"x"+std::to_string(surface->height)+".bin"),std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));}(void)device.native->capture(std::filesystem::path(directory)/("device-"+std::to_string(device.root)+(device.frames==1?std::string{}:"-frame-"+std::to_string(device.frames))+".png"));}}
        graphics_close_after_present(device.window,device.frames,[&](std::uint64_t target){enqueue_gui_message({target,0,0,0x10});return true;});
    };
    const auto setup=[&](Object& device,std::uint64_t address,bool preserve=false){
        require(address!=0);const unsigned pointer_offset=x86_?28:32,tail=pointer_offset+ps;auto pp=read(address,x86_?56:64);const auto n=[&](unsigned at){return word(pp,at);};
        const auto window=memory.load(GuestAddress{address+pointer_offset},pb);const auto target=window?window:device.focus;
        // 無呈現視窗但明確指定尺寸的裝置仍可執行離屏資源／軟體網格處理。
        require((target && gui_.windows.contains(target)) || (!target && n(0) && n(4) && n(tail)==1));
        const auto width=n(0)?n(0):gui_.windows.at(target).width,height=n(4)?n(4):gui_.windows.at(target).height,format=n(8)?n(8):22;
        require(width && height && width<=16384 && height<=16384 && std::uint64_t{width}*height<=64*1024*1024);
        require(color_format(format) && n(12)<=1 && (!n(16) || (n(16)>=2 && n(16)<=16 && n(24)==1)) && !n(20) && (n(24)==1 || n(24)==3) && n(tail+0)==1 && n(tail+4)<=1);
        // DISCARD_DEPTHSTENCIL 允許 Present 後丟棄深度；保留內容仍符合其未定義內容契約。
        require(!(n(tail+12)&~2U) && !n(tail+16)); // 尚未提供 lockable backbuffer 或全螢幕刷新率。
        require(!n(tail+4) || depth_format(n(tail+8)),unavailable);
        require(n(tail+20)==0 || n(tail+20)==1 || n(tail+20)==0x80000000U,unavailable);
        auto native=make_metal_opengl(native_gui_ && target && device.device_type!=4?native_gui_->graphics_view(target):nullptr,width,height);require(bool(native),unavailable);
        require(native->swap_interval(n(tail+20)==0x80000000U?0:1),unavailable);
        unbind_targets(device);
        if(!preserve){unbind(device);for(auto& stage:device.constants)for(auto& values:stage)values.clear();device.sampler=Object{}.sampler;device.stage={{1,4},{2,2},{3,1},{4,2},{5,2},{6,1},{28,1},{33,1},{36,1},{65,1},{68,1},{97,1},{100,1},{129,1},{132,1},{161,1},{164,1},{193,1},{196,1},{225,1},{228,1}};}
        device.native=std::move(native);device.window=target;device.width=width;device.height=height;device.format=format;device.auto_depth=n(tail+4)!=0;
        if(!preserve){device.render=Object{}.render;device.render[7]=device.auto_depth?1U:0U;
        device.transforms={{2,identity()},{3,identity()},{256,identity()}};device.fvf=0;
        device.viewport={0,0,width,height,bits(0),bits(1)};device.scissor={0,0,static_cast<std::int32_t>(width),static_cast<std::int32_t>(height)};}
        put(pp,0,width);put(pp,4,height);put(pp,8,format);put(pp,12,1);device.presentation=pp;memory.write(GuestAddress{address},pp);
        auto back=get(device.back,Kind::Surface);back->width=width;back->height=height;back->format=format;back->pool=0;back->usage=1;back->multisample=n(16);back->native_view=device.native->graphics_drawable(false);if(back->multisample){GraphicsResourceDesc desc;desc.dimension=3;desc.width=width;desc.height=height;desc.format=87;desc.samples=back->multisample;desc.bind=0x20;back->native_view={device.native->graphics_resource(desc),desc.format};}bind_target(device.render_targets[0],back->root);
        if(device.auto_depth){if(!device.depth_back)device.depth_back=create(Kind::Surface,device.root,true)->root;auto depth=get(device.depth_back,Kind::Surface);depth->width=width;depth->height=height;depth->format=n(tail+8);depth->pool=0;depth->usage=2;depth->multisample=n(16);GraphicsResourceDesc desc;desc.dimension=3;desc.width=width;desc.height=height;desc.format=depth_format(depth->format);desc.samples=n(16)?n(16):1;desc.bind=0x40;depth->native_view={device.native->graphics_resource(desc),desc.format};device.native->graphics_clear(depth->native_view,{1,0,0,0},depth->format==75?3:1);bind_target(device.depth_target,depth->root);}
        fixed_targets(device);
    };
    std::uint64_t result=0;
    try {
const auto d3dx_path=[&](std::string_view path){return utf16_to_utf8(normalized_windows_path(utf8_to_utf16(path),runtime_.current_directory));};
#include "d3dx9_mesh_handlers.inc"
        if(mesh_handled){}
        else if(symbol=="Direct3DCreate9" || symbol=="Direct3DCreate9Ex"){
            const bool ex=symbol=="Direct3DCreate9Ex";if(ex)output(1);require(u(0)==32,unavailable);{auto native=make_metal_opengl(nullptr,1,1);require(bool(native),unavailable);auto factory=create(Kind::Factory,0,false,ex);factory->native=std::move(native);if(ex)store_pointer(arg(1),factory->root);else result=factory->root;}
        }else {
            const auto dot=symbol.find('.');require(dot!=std::string_view::npos);const auto prefix=symbol.substr(0,dot);symbol.remove_prefix(dot+1);
            const auto kind=prefix.starts_with("Factory")?Kind::Factory:prefix.starts_with("Device")?Kind::Device:prefix.starts_with("SwapChain")?Kind::SwapChain:prefix=="StateBlock"?Kind::StateBlock:prefix=="VertexShader"?Kind::VertexShader:prefix=="PixelShader"?Kind::PixelShader:prefix=="VertexDeclaration"?Kind::VertexDeclaration:prefix=="Texture"?Kind::Texture:prefix=="CubeTexture"?Kind::CubeTexture:prefix=="VolumeTexture"?Kind::VolumeTexture:prefix=="Volume"?Kind::Volume:prefix=="VertexBuffer"?Kind::VertexBuffer:prefix=="IndexBuffer"?Kind::IndexBuffer:Kind::Surface;auto value=get(arg(0),kind);const bool recording=kind==Kind::Device && value->recording && recordable(symbol);auto& object=recording?*value->recording:*value;
            if(symbol=="QueryInterface"){
                output(2);Guid requested{};memory.read(GuestAddress{arg(1)},requested);if(!supports(kind,requested,object.ex))result=no_interface;
                else {hold(value);store_pointer(arg(2),object.root);}
            }else if(symbol=="AddRef")result=hold(value);
            else if(symbol=="Release")result=release(object.root);
            else if(kind==Kind::StateBlock){
                if(symbol=="GetDevice"){output(1);hold(get(object.parent,Kind::Device));store_pointer(arg(1),object.parent);}
                else if(symbol=="Capture")capture_state(value,*get(object.parent,Kind::Device));
                else if(symbol=="Apply"){auto device=get(object.parent,Kind::Device);require(!device->recording);apply_state(*device,*object.saved,object.state_mask);}
                else require(false);
            }
            else if((resource_interface(kind) || kind==Kind::Volume) && (symbol=="SetPrivateData" || symbol=="GetPrivateData" || symbol=="FreePrivateData")){
                Guid key{};require(arg(1));memory.read(GuestAddress{arg(1)},key);auto it=object.private_data.find(key);
                if(symbol=="SetPrivateData"){
                    require(arg(2) && u(3) && u(3)<=64*1024*1024 && !(u(4)&~1U));std::pair<std::vector<std::uint8_t>,std::uint64_t> data;
                    if(u(4)){require(u(3)==ps);data.second=arg(2);data.first.resize(ps);std::memcpy(data.first.data(),&data.second,ps);private_ref(data.second,1);}else data.first=read(arg(2),u(3));
                    if(it!=object.private_data.end() && it->second.second)private_ref(it->second.second,2);object.private_data[key]=std::move(data);
                }else {require(it!=object.private_data.end(),0x88760866U);
                    if(symbol=="FreePrivateData"){if(it->second.second)private_ref(it->second.second,2);object.private_data.erase(it);}
                    else {require(arg(3));const auto capacity=memory.load(GuestAddress{arg(3)},32);memory.store(GuestAddress{arg(3)},32,it->second.first.size());require(arg(2) && capacity>=it->second.first.size(),0x88760867U);memory.write(GuestAddress{arg(2)},it->second.first);if(it->second.second)private_ref(it->second.second,1);}
                }
            }
            else if(kind==Kind::Factory){
                if(symbol=="GetAdapterModeCountEx" || symbol=="EnumAdapterModesEx"){
                    require(!u(1) && arg(2));auto filter=read(arg(2),12);const bool valid=word(filter,0)==12 && word(filter,4)==22 && word(filter,8)<=1;
                    if(symbol=="GetAdapterModeCountEx")result=valid?1:0;else{require(valid && !u(3));display_ex(arg(4),0);}
                }else if(symbol=="GetAdapterDisplayModeEx"){require(!u(1));display_ex(arg(2),arg(3));}
                else if(symbol=="GetAdapterLUID"){require(!u(1) && arg(2));memory.store(GuestAddress{arg(2)},64,0x583634414d544c01ULL);}
                else if(symbol=="GetAdapterCount")result=1;
                else if(symbol=="GetAdapterModeCount")result=u(1)==0 && u(2)==22?1:0;
                else if(symbol=="GetAdapterMonitor")result=u(1)==0?1:0;
                else if(symbol=="GetAdapterIdentifier"){
                    require(!u(1) && !(u(2)&~2U) && arg(3));std::vector<std::uint8_t> data(x86_?1100:1104);
                    const auto text=[&](unsigned offset,unsigned size,std::string source){source.resize(std::min<std::size_t>(source.size(),size-1));std::copy(source.begin(),source.end(),data.begin()+offset);};
                    text(0,512,"x64arm Metal");text(512,512,object.native->info().renderer);text(1024,32,"\\\\.\\DISPLAY1");put(data,1064,0x106b);std::copy(iidFactory.begin(),iidFactory.end(),data.begin()+1080);memory.write(GuestAddress{arg(3)},data);
                }else if(symbol=="GetAdapterDisplayMode"){require(!u(1));display(arg(2));}
                else if(symbol=="EnumAdapterModes"){require(!u(1) && u(2)==22 && !u(3));display(arg(4));}
                else if(symbol=="GetDeviceCaps"){require(!u(1) && (u(2)==1 || u(2)==4) && arg(3));caps(memory,arg(3));memory.store(GuestAddress{arg(3)},32,u(2));}
                else if(symbol=="CheckDeviceType")require(!u(1) && (u(2)==1 || u(2)==4) && u(3)==22 && (color_format(u(4)) || (!u(4) && u(5))) && u(5),unavailable);
                else if(symbol=="CheckDeviceFormat"){
                    require(!u(1) && (u(2)==1 || u(2)==4) && u(3)==22,unavailable);const auto usage=u(4),type=u(5),format=u(6);require(type==1 || type==3 || type==4 || type==5,unavailable);
                    if(usage&2)require(type==1 && usage==2 && depth_format(format),unavailable);
                    else {require(!(usage&~0x1a0201U) && (!(usage&1) || render_color_format(format)),unavailable);(void)native_format(format);if(type==1)require(!(usage&~0x80001U) && render_color_format(format),unavailable);if(type==4)require(usage==0 || usage==0x200,unavailable);}
                }else if(symbol=="CheckDepthStencilMatch")require(!u(1) && (u(2)==1 || u(2)==4) && u(3)==22 && render_color_format(u(4)) && depth_format(u(5)),unavailable);
                else if(symbol=="CheckDeviceMultiSampleType"){
                    if(arg(6))memory.store(GuestAddress{arg(6)},32,0);require(!u(1) && (u(2)==1 || u(2)==4) && u(4) && (!u(5) || (u(5)>=2 && u(5)<=16)),unavailable);const auto format=depth_format(u(3))?depth_format(u(3)):render_color_format(u(3))?native_format(u(3)):0;require(format && object.native->graphics_samples_supported(format,u(5)?u(5):1),unavailable);if(arg(6))memory.store(GuestAddress{arg(6)},32,1);
                }else if(symbol=="CheckDeviceFormatConversion")require(!u(1) && (u(2)==1 || u(2)==4) && color_format(u(3)) && u(3)==u(4),unavailable);
                else if(symbol=="CreateDevice" || symbol=="CreateDeviceEx"){
                    const auto out=symbol=="CreateDeviceEx"?7U:6U;output(out);if(out==7)require(!arg(6));const auto processing=u(4)&0xe0U;require(!u(1) && (u(2)==1 || u(2)==4) && (processing==0x20 || processing==0x40 || processing==0x80) && !(u(4)&~0x8e6U),unavailable);require(!arg(3) || gui_.windows.contains(arg(3)));
                    auto device=create(Kind::Device,object.root,false,object.ex);device->creator=threads_.current;device->focus=arg(3);device->flags=u(4);device->device_type=u(2);device->software_vertex=processing==0x20;
                    try{device->chain=create(Kind::SwapChain,device->root,true,object.ex)->root;device->back=create(Kind::Surface,device->root,true)->root;setup(*device,arg(5));}
                    catch(...){release(device->root);throw;}
                    store_pointer(arg(out),device->root);
                }else if(symbol=="RegisterSoftwareDevice")result=unavailable;
                else throw HostCallFault("尚未實作 Direct3D 9 factory 方法："+std::string(symbol));
            }else if(kind==Kind::Surface || kind==Kind::Volume){
                if(symbol=="GetDevice"){output(1);auto parent=bridge.objects.at(object.parent);if(texture_kind(parent->kind))parent=get(parent->parent,Kind::Device);hold(parent);store_pointer(arg(1),parent->root);}
                else if(symbol=="GetType")result=1;
                else if(symbol=="SetPriority" || symbol=="GetPriority"){
                    auto parent=bridge.objects.at(object.parent);if(texture_kind(parent->kind) && parent->pool==1){result=parent->priority;if(symbol=="SetPriority")parent->priority=u(1);}
                }
                else if(symbol=="GetDesc"){if(kind==Kind::Volume)volume_desc(object,arg(1));else surface_desc(object,arg(1));}
                else if(symbol=="LockBox")volume_lock(object,arg(1),arg(2),u(3));
                else if(symbol=="UnlockBox")surface_unlock(object);
                else if(symbol=="LockRect")surface_lock(object,arg(1),arg(2),u(3));
                else if(symbol=="UnlockRect")surface_unlock(object);
                else if(symbol=="GetContainer"){
                    output(2);auto parent=bridge.objects.at(object.parent);Guid requested{};memory.read(GuestAddress{arg(1)},requested);
                    if(parent->kind==Kind::Device && object.root==parent->back)parent=get(parent->chain,Kind::SwapChain);
                    if(!supports(parent->kind,requested,parent->ex))result=no_interface;
                    else{hold(parent);store_pointer(arg(2),parent->root);}
                }
                else throw HostCallFault("尚未實作 Direct3D 9 surface 方法："+std::string(symbol));
            }else if(kind==Kind::SwapChain){
                auto device=get(object.parent,Kind::Device);
                if(symbol=="Present")present(*device,arg(1),arg(2),arg(3),arg(4),u(5));
                else if(symbol=="GetBackBuffer"){output(3);require(!u(1) && !u(2));auto back=get(device->back,Kind::Surface);hold(back);store_pointer(arg(3),back->root);}
                else if(symbol=="GetDevice"){output(1);hold(device);store_pointer(arg(1),device->root);}
                else if(symbol=="GetDisplayMode")display(arg(1));
                else if(symbol=="GetDisplayModeEx")display_ex(arg(1),arg(2));
                else if(symbol=="GetPresentParameters"){require(arg(1)!=0);memory.write(GuestAddress{arg(1)},device->presentation);}
                else if(symbol=="GetLastPresentCount"){require(arg(1)!=0);memory.store(GuestAddress{arg(1)},32,static_cast<std::uint32_t>(device->frames));}
                else throw HostCallFault("尚未實作 Direct3D 9 swap chain 方法："+std::string(symbol));
            }else if(kind==Kind::VertexBuffer || kind==Kind::IndexBuffer){
                if(symbol=="GetDevice"){output(1);auto parent=get(object.parent,Kind::Device);hold(parent);store_pointer(arg(1),parent->root);}
                else if(symbol=="GetType")result=kind==Kind::VertexBuffer?6:7;
                else if(symbol=="GetDesc"){
                    require(arg(1)!=0);const std::array<std::uint32_t,6> desc{object.format,kind==Kind::VertexBuffer?6U:7U,object.usage,object.pool,static_cast<std::uint32_t>(object.pixels.size()),object.fvf};
                    memory.write(GuestAddress{arg(1)},std::span(reinterpret_cast<const std::uint8_t*>(desc.data()),kind==Kind::VertexBuffer?24:20));
                }else if(symbol=="SetPriority" || symbol=="GetPriority"){result=object.priority;if(symbol=="SetPriority" && object.pool==1)object.priority=u(1);}
                else if(symbol=="Lock"){
                    output(3);const auto offset=u(1),size=u(2)?u(2):static_cast<std::uint32_t>(object.pixels.size())-offset,flags=u(4);
                    require(offset<object.pixels.size() && size && std::uint64_t{offset}+size<=object.pixels.size() && !(flags&~0xb810U));
                    require(!(flags&0x3000) || (object.usage&0x200));require(!(flags&0x10) || !(object.usage&8));require(!(flags&0x10) || !(flags&0x3000));require(!(flags&0x2000) || !object.mapped);
                    if(!object.mapped){object.mapped=allocate_heap(memory,object.pixels.size());require(object.mapped!=0,out_of_memory);memory.write(GuestAddress{object.mapped},object.pixels);object.lock_writable=false;}
                    require(object.lock_count<65536);++object.lock_count;object.lock_writable|=!(flags&0x10);store_pointer(arg(3),object.mapped+offset);
                }else if(symbol=="Unlock"){
                    require(object.lock_count!=0);if(!--object.lock_count){if(object.lock_writable)object.pixels=read(object.mapped,object.pixels.size());(void)free_heap(memory,object.mapped,state);object.mapped=0;}
                }else if(symbol=="PreLoad"){
                    // 軟體 vertex processing 的 buffer 常駐於受額度限制的 backing；draw 時再提交 Metal。
                }else throw HostCallFault("尚未實作 Direct3D 9 buffer 方法："+std::string(symbol));
            }else if(texture_kind(kind)){
                const bool cube=kind==Kind::CubeTexture,volume=kind==Kind::VolumeTexture;
                const auto level_surface=[&](unsigned level,unsigned face=0){require(level<object.levels && face<(cube?6U:1U));return get(object.subresources.at(face*object.levels+level),volume?Kind::Volume:Kind::Surface);};
                if(symbol=="GetDevice"){output(1);auto parent=get(object.parent,Kind::Device);hold(parent);store_pointer(arg(1),parent->root);}
                else if(symbol=="GetType")result=volume?4:cube?5:3;
                else if(symbol=="GetLevelCount")result=object.levels;
                else if(symbol=="GetAutoGenFilterType"){
                    // CreateTexture 家族尚不接受 AUTOGEN；普通 mipmap 的查詢契約為 NONE。
                    require(!(object.usage&0x400),unavailable);result=0;
                }else if(symbol=="SetAutoGenFilterType" || symbol=="GenerateMipSubLevels"){
                    // 官方契約：非 AUTOGEN 紋理不受這兩個方法影響，保留應用程式寫入的各層內容。
                    require(!(object.usage&0x400),unavailable);
                }
                else if(symbol=="GetLOD")result=object.pool==1?object.lod:0;
                else if(symbol=="SetLOD"){result=object.pool==1?object.lod:0;if(object.pool==1)object.lod=std::min(u(1),object.levels-1);}
                else if(symbol=="GetPriority")result=object.priority;
                else if(symbol=="SetPriority"){result=object.priority;if(object.pool==1)object.priority=u(1);}
                else if(symbol=="GetLevelDesc"){if(volume)volume_desc(*level_surface(u(1)),arg(2));else surface_desc(*level_surface(u(1)),arg(2));}
                else if(symbol=="GetVolumeLevel"){output(2);auto level=level_surface(u(1));hold(level);store_pointer(arg(2),level->root);}
                else if(symbol=="LockBox")volume_lock(*level_surface(u(1)),arg(2),arg(3),u(4));
                else if(symbol=="UnlockBox")surface_unlock(*level_surface(u(1)));
                else if(symbol=="AddDirtyBox"){(void)volume_box(*level_surface(0),arg(1));}
                else if(symbol=="GetSurfaceLevel"){output(2);auto surface=level_surface(u(1));hold(surface);store_pointer(arg(2),surface->root);}
                else if(symbol=="GetCubeMapSurface"){output(3);auto surface=level_surface(u(2),u(1));hold(surface);store_pointer(arg(3),surface->root);}
                else if(symbol=="LockRect")surface_lock(*level_surface(u(cube?2:1),cube?u(1):0),arg(cube?3:2),arg(cube?4:3),u(cube?5:4));
                else if(symbol=="UnlockRect")surface_unlock(*level_surface(u(cube?2:1),cube?u(1):0));
                else if(symbol=="PreLoad"){if(object.pool<=1){auto device=get(object.parent,Kind::Device);if(volume)(void)texture_resource(*device,object);else if(!cube)upload(*device,object);}}
                else if(symbol=="AddDirtyRect"){
                    // 尚未做局部上傳最佳化；每次同步完整 level 0，包含所有 dirty 區域。
                    require(!cube || u(1)<6);if(arg(cube?2:1)){auto rect=read(arg(cube?2:1),16);require(word(rect,0)<word(rect,8) && word(rect,4)<word(rect,12) && word(rect,8)<=object.width && word(rect,12)<=object.height);}
                }else throw HostCallFault("尚未實作 Direct3D 9 texture 方法："+std::string(symbol));
            }else {
                if(symbol=="CreateStateBlock"){output(2);require(!object.recording && u(1)>=1 && u(1)<=3);auto block=create(Kind::StateBlock,object.root);block->state_mask=state_mask(object,u(1));capture_state(block,object);store_pointer(arg(2),block->root);}
                else if(symbol=="BeginStateBlock"){require(!object.recording);auto record=std::make_shared<Object>(object);record->saved.reset();record->state_mask.clear();for(auto pointer:saved_bindings(*record))if(pointer)++bridge.objects.at(pointer)->bound;object.recording=std::move(record);}
                else if(symbol=="EndStateBlock"){output(1);require(bool(object.recording));auto block=create(Kind::StateBlock,object.root);block->state_mask=object.recording->state_mask;capture_state(block,*object.recording);unbind(*object.recording);object.recording.reset();store_pointer(arg(1),block->root);}
                else if(symbol=="TestCooperativeLevel")require(gui_.windows.contains(object.window));
                else if(symbol=="GetAvailableTextureMem"){const auto stats=gpu_memory_stats();result=std::min<std::uint64_t>(UINT32_MAX,stats.limit-stats.used);}
                else if(symbol=="GetDirect3D"){output(1);auto factory=get(object.parent,Kind::Factory);hold(factory);store_pointer(arg(1),factory->root);}
                else if(symbol=="GetDeviceCaps"){require(arg(1)!=0);caps(memory,arg(1));memory.store(GuestAddress{arg(1)},32,object.device_type);}
                else if(symbol=="GetDisplayMode"){require(!u(1));display(arg(2));}
                else if(symbol=="GetCreationParameters"){
                    require(arg(1)!=0);memory.check(GuestAddress{arg(1)},12+ps,Access::Write);memory.store(GuestAddress{arg(1)},32,0);memory.store(GuestAddress{arg(1)+4},32,object.device_type);store_pointer(arg(1)+8,object.focus);memory.store(GuestAddress{arg(1)+8+ps},32,object.flags);
                }else if(symbol=="GetNumberOfSwapChains")result=1;
                else if(symbol=="GetSwapChain"){output(2);require(!u(1));auto chain=get(object.chain,Kind::SwapChain);hold(chain);store_pointer(arg(2),chain->root);}
                else if(symbol=="Reset" || symbol=="ResetEx"){
                    const bool preserve=symbol=="ResetEx";require(!object.scene && object.creator==threads_.current);if(preserve)require(!arg(2));
                    if(!preserve){require(!get(object.back,Kind::Surface)->references && !get(object.chain,Kind::SwapChain)->references && (!object.depth_back || !get(object.depth_back,Kind::Surface)->references));for(const auto& [address,entry]:bridge.objects){(void)address;require(!resource(entry->kind) || entry->parent!=object.root || entry->pool!=0 || !entry->references);}}
                    setup(object,arg(1),preserve);for(unsigned i=0;i<4;++i)memory.store(GuestAddress{arg(1)+4*i},32,0);
                }else if(symbol=="Present" || symbol=="PresentEx")present(object,arg(1),arg(2),arg(3),arg(4),symbol=="PresentEx"?u(5):0);
                else if(symbol=="GetDisplayModeEx"){require(!u(1));display_ex(arg(2),arg(3));}
                else if(symbol=="CheckDeviceState"){require(gui_.windows.contains(arg(1)));result=gui_.windows.at(arg(1)).visible?0:0x08760878U;}
                else if(symbol=="SetMaximumFrameLatency"){require(u(1)<=20);object.latency=u(1)?u(1):3;} // 同步 Present 目前至多一個待完成 frame，符合此上限。
                else if(symbol=="GetMaximumFrameLatency"){require(arg(1)!=0);memory.store(GuestAddress{arg(1)},32,object.latency);}
                else if(symbol=="GetBackBuffer" || symbol=="GetRenderTarget" || symbol=="GetDepthStencilSurface"){
                    const bool backbuffer=symbol=="GetBackBuffer",depth=symbol=="GetDepthStencilSurface";const unsigned dest=backbuffer?4:depth?1:2;output(dest);require(depth || (backbuffer?(!u(1) && !u(2) && !u(3)):u(1)<4));const auto address=depth?object.depth_target:backbuffer?object.back:object.render_targets[u(1)];require(address!=0,0x88760866U);auto surface=get(address,Kind::Surface);hold(surface);store_pointer(arg(dest),address);
                }else if(symbol=="SetRenderTarget" || symbol=="SetDepthStencilSurface"){
                    const bool depth=symbol=="SetDepthStencilSurface";const auto address=arg(depth?1:2);require(depth || (u(1)<4 && (u(1) || address)));if(address){auto surface=get(address,Kind::Surface);require(surface_device(*surface)->root==object.root && (surface->usage&(depth?2U:1U)) && surface->native_view.resource && !surface->mapped);}
                    bind_target(depth?object.depth_target:object.render_targets[u(1)],address);if(!depth && address){auto surface=get(address,Kind::Surface);object.viewport={0,0,surface->width,surface->height,bits(0),bits(1)};}
                }else if(symbol=="CreateRenderTarget" || symbol=="CreateDepthStencilSurface"){
                    output(7);const bool depth=symbol=="CreateDepthStencilSurface";const auto format=depth?depth_format(u(3)):render_color_format(u(3))?native_format(u(3)):0;require(u(1) && u(2) && u(1)<=16384 && u(2)<=16384 && format && !u(5) && u(6)<=1 && !arg(8));require(u(4)==0 || (u(4)>=2 && u(4)<=16));require(depth || !u(6) || !u(4));const auto samples=u(4)?u(4):1;require(object.native->graphics_samples_supported(format,samples),unavailable);
                    auto surface=create(Kind::Surface,object.root);try{surface->width=u(1);surface->height=u(2);surface->format=u(3);surface->usage=depth?2:1;surface->pool=0;surface->multisample=u(4);surface->lockable=!depth && u(6);GraphicsResourceDesc desc;desc.dimension=3;desc.width=u(1);desc.height=u(2);desc.format=format;desc.samples=samples;desc.bind=depth?0x40:0x20;surface->native_view={object.native->graphics_resource(desc),format};}catch(...){release(surface->root);throw;}store_pointer(arg(7),surface->root);
                }
                else if(symbol=="CreateVertexBuffer" || symbol=="CreateIndexBuffer"){
                    output(5);const bool vertex=symbol=="CreateVertexBuffer";require(u(1) && u(1)<=64*1024*1024 && !(u(2)&~0x218U) && u(4)<=2 && (!object.ex || u(4)!=1) && !arg(6));
                    require(!(u(2)&0x200) || u(4)==0);if(!vertex)require((u(3)==101 && !(u(1)%2)) || (u(3)==102 && !(u(1)%4)));
                    auto buffer=create(vertex?Kind::VertexBuffer:Kind::IndexBuffer,object.root);try{buffer->pool=u(4);buffer->usage=u(2);buffer->format=vertex?100:u(3);buffer->fvf=vertex?u(3):0;buffer->storage=reserve_gpu_memory(u(1));buffer->pixels.resize(u(1));}catch(...){release(buffer->root);throw;}store_pointer(arg(5),buffer->root);
                }else if(symbol=="SetStreamSource" || symbol=="SetIndices"){
                    const bool vertex=symbol=="SetStreamSource";if(vertex)require(u(1)<16);const auto slot=vertex?u(1):0;const auto address=arg(vertex?2:1);std::shared_ptr<Object> buffer;
                    if(address){buffer=get(address,vertex?Kind::VertexBuffer:Kind::IndexBuffer);require(buffer->parent==object.root);if(vertex)require(u(3)<buffer->pixels.size() && u(4)<=256);}
                    auto& binding=vertex?(slot?object.extra_streams[slot-1].buffer:object.vertex_buffer):object.index_buffer;if(address!=binding){if(buffer)++buffer->bound;drop_binding(binding);binding=address;}
                    if(vertex){if(slot){object.extra_streams[slot-1].offset=address?u(3):0;object.extra_streams[slot-1].stride=address?u(4):0;}else{object.stream_offset=address?u(3):0;object.stream_stride=address?u(4):0;}}
                }else if(symbol=="GetStreamSource" || symbol=="GetIndices"){
                    const bool vertex=symbol=="GetStreamSource";const auto dest=vertex?2U:1U;output(dest);if(vertex)require(u(1)<16 && arg(3) && arg(4));const auto slot=vertex?u(1):0;
                    if(vertex){memory.store(GuestAddress{arg(3)},32,slot?object.extra_streams[slot-1].offset:object.stream_offset);memory.store(GuestAddress{arg(4)},32,slot?object.extra_streams[slot-1].stride:object.stream_stride);}
                    const auto binding=vertex?(slot?object.extra_streams[slot-1].buffer:object.vertex_buffer):object.index_buffer;if(binding){auto buffer=bridge.objects.at(binding);hold(buffer);store_pointer(arg(dest),binding);}
                }else if(symbol=="SetStreamSourceFreq"){
                    const auto slot=u(1),frequency=u(2),mode=frequency&0xc0000000U;require(slot<16 && (frequency&0x3fffffffU) && mode!=0xc0000000U && (mode!=0x40000000U || slot==0) && (mode!=0x80000000U || slot!=0));object.stream_frequency[slot]=frequency;
                }else if(symbol=="GetStreamSourceFreq"){require(u(1)<16 && arg(2));memory.store(GuestAddress{arg(2)},32,object.stream_frequency[u(1)]);}
                else if(symbol=="CreateTexture"){
                    output(7);require(!arg(8));auto texture=make_texture(get(object.root,Kind::Device),u(1),u(2),u(3),u(4),u(5),u(6));store_pointer(arg(7),texture->root);
                }else if(symbol=="CreateVolumeTexture"){output(8);require(!arg(9));auto texture=make_texture(get(object.root,Kind::Device),u(1),u(2),u(4),u(5),u(6),u(7),false,u(3),true);store_pointer(arg(8),texture->root);
                }else if(symbol=="CreateCubeTexture"){output(6);require(!arg(7));auto texture=make_texture(get(object.root,Kind::Device),u(1),u(1),u(2),u(3),u(4),u(5),true);store_pointer(arg(6),texture->root);
                }else if(symbol=="SetTexture"){
                    const auto slot=texture_slot(u(1));auto& binding=slot?object.extra_textures[slot-1]:object.texture;std::shared_ptr<Object> texture;if(arg(2)){texture=get_texture(arg(2));require(texture->parent==object.root && texture->pool<=1);}
                    if(arg(2)!=binding){if(texture)++texture->bound;drop_binding(binding);binding=arg(2);}
                    if(!u(1) && !binding){const std::uint32_t name=1;call(object,GlOperation::DeleteTextures,{1},std::span(reinterpret_cast<const std::uint8_t*>(&name),4));check_gpu(object);}
                }else if(symbol=="GetTexture"){output(2);const auto slot=texture_slot(u(1));const auto binding=slot?object.extra_textures[slot-1]:object.texture;if(binding){auto texture=get_texture(binding);hold(texture);store_pointer(arg(2),texture->root);}}
                else if(symbol=="UpdateTexture"){
                    auto source=get_texture(arg(1)),target=get_texture(arg(2));require(source->kind==target->kind);
                    require(source->parent==object.root && target->parent==object.root && source->pool==2 && target->pool==0 && source->format==target->format);
                    unsigned first=0;while(first<source->levels && (std::max(1U,source->width>>first)!=target->width || std::max(1U,source->height>>first)!=target->height || std::max(1U,source->depth>>first)!=target->depth))++first;require(first<source->levels && target->levels<=source->levels-first);
                    for(unsigned face=0;face<(source->kind==Kind::CubeTexture?6U:1U);++face)for(unsigned level=0;level<target->levels;++level)require(!get(source->subresources.at(face*source->levels+first+level),source->kind==Kind::VolumeTexture?Kind::Volume:Kind::Surface)->mapped && !get(target->subresources.at(face*target->levels+level),source->kind==Kind::VolumeTexture?Kind::Volume:Kind::Surface)->mapped);
                    for(unsigned face=0;face<(source->kind==Kind::CubeTexture?6U:1U);++face)for(unsigned level=0;level<target->levels;++level){auto surface=get(target->subresources.at(face*target->levels+level),source->kind==Kind::VolumeTexture?Kind::Volume:Kind::Surface);surface->pixels=get(source->subresources.at(face*source->levels+first+level),source->kind==Kind::VolumeTexture?Kind::Volume:Kind::Surface)->pixels;write_surface(*surface);}
                }else if(symbol=="EvictManagedResources"){
                    const std::uint32_t name=1;call(object,GlOperation::DeleteTextures,{1},std::span(reinterpret_cast<const std::uint8_t*>(&name),4));check_gpu(object);
                }else if(symbol=="SetSamplerState" || symbol=="GetSamplerState" || symbol=="SetTextureStageState" || symbol=="GetTextureStageState"){
                    const bool sampler=symbol.find("Sampler")!=std::string_view::npos,write=symbol.starts_with("Set");const auto slot=sampler?texture_slot(u(1)):u(1);require(sampler || slot<8);auto& values=sampler?object.sampler:object.stage;const auto state_key=u(2)+slot*32;require(values.contains(state_key));
                    if(!write){require(arg(3)!=0);memory.store(GuestAddress{arg(3)},32,values.at(state_key));}
                    else{const auto key=u(2),v=u(3);bool valid=false;if(sampler){if(key==1 || key==2 || key==3)valid=v==1 || v==3;else if(key==7)valid=v<=2;else if(key==10)valid=v>=1 && v<=16;else valid=v>=1 && v<=3;}
                        else if(u(1))valid=(key==1 || key==4) && v==1;else if(key==1)valid=v>=1 && v<=4;else if(key==4)valid=v>=1 && v<=4;else if(key==28)valid=v==1;else valid=(v&15)<=3 && !(v&~0x3fU);
                        require(valid,unavailable);values[state_key]=v;}
                }else if(symbol=="CreateOffscreenPlainSurface"){
                    output(5);require(u(1) && u(2) && u(1)<=16384 && u(2)<=16384 && std::uint64_t{u(1)}*u(2)<=64*1024*1024 && render_color_format(u(3)) && (u(4)==0 || u(4)==2 || u(4)==3) && !arg(6));
                    auto surface=create(Kind::Surface,object.root);try{surface->width=u(1);surface->height=u(2);surface->format=u(3);surface->pool=u(4);const auto pitch=graphics_format_pitch(native_format(u(3)),u(1),u(2));const auto size=std::size_t(pitch[0])*pitch[1];surface->storage=reserve_gpu_memory(size);surface->pixels.resize(size);}catch(...){release(surface->root);throw;}store_pointer(arg(5),surface->root);

#include "d3d9_surface_transfer.inc"
                }else if(symbol=="GetRenderTargetData"){
                    auto source=get(arg(1),Kind::Surface),target=get(arg(2),Kind::Surface);auto owner=bridge.objects.at(target->parent);if(owner->kind==Kind::Texture)owner=get(owner->parent,Kind::Device);require(surface_device(*source)->root==object.root && (source->usage&1) && !source->multisample && owner->root==object.root && target->pool==2 && !target->mapped && source->width==target->width && source->height==target->height && source->format==target->format);target->pixels=object.native->graphics_read(source->native_view);if(source->format==22)for(std::size_t i=3;i<target->pixels.size();i+=4)target->pixels[i]=255;
                }else if(symbol=="BeginScene"){require(!object.scene);object.scene=true;}
                else if(symbol=="EndScene"){require(object.scene);object.scene=false;call(object,GlOperation::Flush);check_gpu(object);}
                else if(symbol=="Clear"){
                    require(u(3) && !(u(3)&~7U) && (!(u(3)&6) || object.depth_target));if(u(3)&4)require(get(object.depth_target,Kind::Surface)->format==75);fixed_targets(object);const auto size=target_size(object);const auto z=real(u(5));require(std::isfinite(z) && z>=0 && z<=1);require(!u(1) || (arg(2) && u(1)<=65536));
                    std::vector<std::array<std::int32_t,4>> rects;const auto& v=object.viewport;
                    const std::array<std::int32_t,4> viewport{static_cast<std::int32_t>(v[0]),static_cast<std::int32_t>(v[1]),static_cast<std::int32_t>(v[0]+v[2]),static_cast<std::int32_t>(v[1]+v[3])};
                    if(u(1)){auto bytes=read(arg(2),std::size_t{u(1)}*16);rects.resize(u(1));std::memcpy(rects.data(),bytes.data(),bytes.size());for(const auto& r:rects)require(r[2]>=r[0] && r[3]>=r[1]);}else rects.push_back(viewport);
                    auto rgba=color(u(4));call(object,GlOperation::ClearColor,{bits(rgba[0]),bits(rgba[1]),bits(rgba[2]),bits(rgba[3])});call(object,GlOperation::ClearDepth,{std::bit_cast<std::uint64_t>(static_cast<double>(z))});call(object,GlOperation::DepthMask,{1});call(object,GlOperation::ClearStencil,{u(6)});toggle(object,0xc11,true);
                    for(auto r:rects){r[0]=std::max(r[0],viewport[0]);r[1]=std::max(r[1],viewport[1]);r[2]=std::min(r[2],viewport[2]);r[3]=std::min(r[3],viewport[3]);if(r[2]<=r[0] || r[3]<=r[1])continue;
                        call(object,GlOperation::Scissor,{static_cast<unsigned>(r[0]),size[1]-static_cast<unsigned>(r[3]),static_cast<unsigned>(r[2]-r[0]),static_cast<unsigned>(r[3]-r[1])});call(object,GlOperation::Clear,{((u(3)&1)?0x4000U:0U)|((u(3)&2)?0x100U:0U)|((u(3)&4)?0x400U:0U)});if(u(3)&1)for(unsigned slot=1;slot<4;++slot)if(object.render_targets[slot]){auto target=get(object.render_targets[slot],Kind::Surface);object.native->graphics_fixed_targets(target->native_view,{});call(object,GlOperation::Clear,{0x4000});}fixed_targets(object);}
                    check_gpu(object);
                }else if(symbol=="SetViewport" || symbol=="GetViewport"){
                    require(arg(1)!=0);if(symbol=="GetViewport")memory.write(GuestAddress{arg(1)},std::span(reinterpret_cast<const std::uint8_t*>(object.viewport.data()),24));
                    else{auto data=read(arg(1),24);std::array<std::uint32_t,6> v{};std::memcpy(v.data(),data.data(),24);require(v[2] && v[3] && std::uint64_t{v[0]}+v[2]<=target_size(object)[0] && std::uint64_t{v[1]}+v[3]<=target_size(object)[1] && std::isfinite(real(v[4])) && std::isfinite(real(v[5])) && real(v[4])>=0 && real(v[5])<=1 && real(v[4])<=real(v[5]));object.viewport=v;}
                }else if(symbol=="SetScissorRect" || symbol=="GetScissorRect"){
                    require(arg(1)!=0);if(symbol=="GetScissorRect")memory.write(GuestAddress{arg(1)},std::span(reinterpret_cast<const std::uint8_t*>(object.scissor.data()),16));
                    else{auto data=read(arg(1),16);std::array<std::int32_t,4> r{};std::memcpy(r.data(),data.data(),16);require(r[0]>=0 && r[1]>=0 && r[2]>r[0] && r[3]>r[1] && static_cast<unsigned>(r[2])<=target_size(object)[0] && static_cast<unsigned>(r[3])<=target_size(object)[1]);object.scissor=r;}
                }else if(symbol=="SetTransform" || symbol=="GetTransform" || symbol=="MultiplyTransform"){
                    require(arg(2) && (u(1)==2 || u(1)==3 || (u(1)>=16 && u(1)<24) || (u(1)>=256 && u(1)<512)));auto& matrix=object.transforms.try_emplace(u(1),identity()).first->second;
                    if(symbol=="GetTransform")memory.write(GuestAddress{arg(2)},std::span(reinterpret_cast<const std::uint8_t*>(matrix.data()),64));
                    else{auto data=read(arg(2),64);Matrix m{};std::memcpy(m.data(),data.data(),64);for(auto element:m)require(std::isfinite(element));
                        if(symbol=="SetTransform")matrix=m;else{Matrix next{};for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)for(unsigned k=0;k<4;++k)next[r*4+c]+=m[r*4+k]*matrix[k*4+c];matrix=next;}}
#include "d3d9_lighting_handlers.inc"
                }else if(symbol=="SetRenderState" || symbol=="GetRenderState"){
                    // DITHERENABLE 影響 16-bit 表面；目前 32-bit target 保留可查詢／state block 還原的值，不改變色彩。
                    require(object.render.contains(u(1)));if(symbol=="GetRenderState"){require(arg(2)!=0);memory.store(GuestAddress{arg(2)},32,object.render.at(u(1)));}
                    else{const auto key=u(1),v=u(2);bool valid=false;switch(key){case 7:valid=v<=1;break;case 8:valid=v==3;break;case 9:valid=v==1 || v==2;break;case 14:case 15:case 26:case 27:case 174:case 52:case 185:valid=v<=1;break;case 19:case 20:valid=v>=1 && v<=11;break;case 22:valid=v>=1 && v<=3;break;case 23:case 25:case 53:case 54:case 55:case 56:case 186:case 187:case 188:case 189:valid=v>=1 && v<=8;break;case 24:valid=v<=255;break;case 28:case 48:case 128:case 129:case 130:case 131:case 132:case 133:case 134:case 135:case 152:case 176:case 206:valid=v==0;break;case 151:valid=v<=3 || v==256;break;case 167:case 29:case 137:case 141:case 142:case 143:valid=v<=1;break;case 60:case 139:case 57:case 58:case 59:valid=true;break;case 145:case 146:case 147:case 148:valid=v<=2;break;case 168:valid=v==15;break;case 136:case 171:valid=v==1;break;}require(valid,unavailable);object.render[key]=v;}
                }else if(symbol=="SetFVF"){
                    if(u(1)){auto data=fvf_decl(u(1));auto decl=create(Kind::VertexDeclaration,object.root);decl->pixels=std::move(data);++decl->bound;drop_binding(object.vertex_declaration);object.vertex_declaration=decl->root;release(decl->root);object.fvf=u(1);}
                }else if(symbol=="GetFVF"){require(arg(1)!=0);memory.store(GuestAddress{arg(1)},32,object.fvf);}
                else if(symbol=="SetSoftwareVertexProcessing"){require((object.flags&0x80) || object.software_vertex==(u(1)!=0));object.software_vertex=u(1)!=0;}
                else if(symbol=="GetSoftwareVertexProcessing")result=object.software_vertex;
                else if(symbol=="CreateVertexShader" || symbol=="CreatePixelShader"){
                    output(2);require(arg(1));auto code=shader_code(arg(1));const auto version=word(code,0);const bool pixel=symbol=="CreatePixelShader";require((version>>16)==(pixel?0xffffU:0xfffeU));
                    auto native=object.native->graphics_shader(pixel?1:0,code);auto shader=create(pixel?Kind::PixelShader:Kind::VertexShader,object.root);shader->pixels=std::move(code);shader->shader=std::move(native);store_pointer(arg(2),shader->root);
                }else if(symbol=="CreateVertexDeclaration"){
                    output(2);require(arg(1));std::vector<std::uint8_t> data;for(unsigned i=0;i<65;++i){auto e=read(arg(1)+i*8,8);data.insert(data.end(),e.begin(),e.end());if(e[0]==255 && e[1]==0 && e[4]==17)break;require(i<64 && e[0]<16 && !e[1] && e[4]<17 && e[5]<=6);}auto vertex_decl=create(Kind::VertexDeclaration,object.root);vertex_decl->pixels=std::move(data);store_pointer(arg(2),vertex_decl->root);
                }else if(symbol=="SetVertexShader" || symbol=="SetPixelShader" || symbol=="SetVertexDeclaration"){
                    const bool vertex_decl=symbol=="SetVertexDeclaration",pixel=symbol=="SetPixelShader";auto& bound=vertex_decl?object.vertex_declaration:object.shaders[pixel?1:0];if(arg(1)!=bound){std::shared_ptr<Object> shader;if(arg(1)){shader=get(arg(1),vertex_decl?Kind::VertexDeclaration:pixel?Kind::PixelShader:Kind::VertexShader);require(shader->parent==object.root);++shader->bound;}drop_binding(bound);bound=arg(1);}if(vertex_decl)object.fvf=0;
                }else if(symbol=="GetVertexShader" || symbol=="GetPixelShader" || symbol=="GetVertexDeclaration"){
                    output(1);const auto pointer=symbol=="GetVertexDeclaration"?object.vertex_declaration:object.shaders[symbol=="GetPixelShader"?1:0];if(pointer){hold(bridge.objects.at(pointer));store_pointer(arg(1),pointer);}
                }else if(symbol.starts_with("SetVertexShaderConstant") || symbol.starts_with("SetPixelShaderConstant") || symbol.starts_with("GetVertexShaderConstant") || symbol.starts_with("GetPixelShaderConstant")){
                    const bool pixel=symbol.find("Pixel")!=symbol.npos,write=symbol.starts_with("Set");const auto register_set=symbol.back()=='F'?0U:symbol.back()=='I'?1U:2U;const unsigned limit=register_set?16U:pixel?224U:256U,stride=register_set==2?4U:16U;require(u(1)<=limit && u(3)<=limit-u(1));auto& values=object.constants[pixel?1:0][register_set];if(values.empty())values.resize(limit*stride);if(u(3)){const auto offset=u(1)*stride,size=u(3)*stride;if(write){auto data=read(arg(2),size);std::copy(data.begin(),data.end(),values.begin()+offset);if(std::getenv("X64ARM_TRACE_D3D9_CONSTANTS")){std::fprintf(stderr,"[d3d9 constants] %s set=%u first=%u count=%u",pixel?"PS":"VS",register_set,u(1),u(3));for(unsigned at=0;at<size;at+=4)std::fprintf(stderr," %08x",word(data,at));std::fprintf(stderr,"\n");}}else memory.write(GuestAddress{arg(2)},std::span(values).subspan(offset,size));}
                }
                else if(symbol=="DrawPrimitiveUP" || symbol=="DrawIndexedPrimitiveUP" || symbol=="DrawPrimitive" || symbol=="DrawIndexedPrimitive"){
                    const bool up=symbol.ends_with("UP"),indexed=symbol.find("Indexed")!=std::string_view::npos,rhw=(object.fvf&0xe)==4;
                    const auto type=u(1),primitives=up?u(indexed?4:2):u(indexed?6:3),stride=up?u(indexed?8:4):object.stream_stride;
                    if(object.device_type==4){
                        require(object.scene && type>=1 && type<=6 && (!indexed || type!=1) && primitives<=349525);
                        if(up){drop_binding(object.vertex_buffer);object.stream_offset=object.stream_stride=0;if(indexed)drop_binding(object.index_buffer);}
                        ++object.draws;arg.return_to(state,0);return std::nullopt;
                    }
                    if(object.shaders[0] || object.shaders[1]){
#include "d3d9_programmable_draw.inc"
                        arg.return_to(state,0);return std::nullopt;
                    }
                    require(!object.shaders[1],unavailable);
                    const auto position_format=object.fvf&0x400e;const bool blended_fvf=position_format>=6 && position_format<=14;
                    require(object.scene && object.fvf && type>=1 && type<=6 && (!indexed || type!=1) && (position_format==2 || blended_fvf || rhw));require(primitives<=349525);
                    const auto clear_up=[&](){if(up){drop_binding(object.vertex_buffer);object.stream_offset=object.stream_stride=0;if(indexed)drop_binding(object.index_buffer);}};
                    if(!primitives){clear_up();arg.return_to(state,0);return std::nullopt;}
                    const std::uint32_t count=type==1?primitives:type==2?primitives*2:type==3?primitives+1:type==4?primitives*3:primitives+2;
                    const unsigned betas=blended_fvf?(position_format-4)/2:0,normal_offset=(rhw?16U:12U)+betas*4;const bool vertex_blending=!rhw && object.render.at(151)!=0,blend_indexed=vertex_blending && object.render.at(167)!=0;const unsigned blend_weights=object.render.at(151)==256?0:object.render.at(151);
                    const bool fvf_indices=(object.fvf&0x9000)!=0;require(!vertex_blending || (blend_weights<=3 && betas>=blend_weights+(blend_indexed?1U:0U) && (!blend_indexed || fvf_indices)));
                    const auto drawing_world=vertex_blending?identity():object.transforms.at(256);
                    const auto color_offset=normal_offset+((object.fvf&0x10)?12U:0U)+((object.fvf&0x20)?4U:0U),specular_offset=color_offset+((object.fvf&0x40)?4U:0U),tex_offset=specular_offset+((object.fvf&0x80)?4U:0U);const auto vertex_size=tex_offset+((object.fvf&0x100)?8U:0U);require(stride>=vertex_size && stride<=256);
                    const auto min=indexed?u(up?2:3):up?0:u(2),vertices=indexed?u(up?3:4):count;require(std::uint64_t{min}+vertices<=1048576 && vertices);
                    const std::int64_t base=!up && indexed?static_cast<std::int32_t>(u(2)):0;require(base+min>=0);
                    std::vector<std::uint8_t> bytes;const auto extent=std::size_t{vertices-1}*stride+vertex_size;
                    if(up){const auto address=arg(indexed?7:3);require(address && address<=UINT64_MAX-std::uint64_t{min}*stride);bytes=read(address+std::uint64_t{min}*stride,extent);}
                    else{auto buffer=get(object.vertex_buffer,Kind::VertexBuffer);const auto at=object.stream_offset+static_cast<std::uint64_t>(base+min)*stride;
                        require(!buffer->mapped && at<=buffer->pixels.size() && extent<=buffer->pixels.size()-at);bytes.assign(buffer->pixels.begin()+static_cast<std::ptrdiff_t>(at),buffer->pixels.begin()+static_cast<std::ptrdiff_t>(at+extent));}
                    std::vector<std::uint32_t> indices(count);
                    if(indexed){std::vector<std::uint8_t> data;unsigned size=0;
                        if(up){require(arg(5) && (u(6)==101 || u(6)==102));size=u(6)==101?2:4;data=read(arg(5),std::size_t{count}*size);}
                        else{auto buffer=get(object.index_buffer,Kind::IndexBuffer);size=buffer->format==101?2:4;const auto at=std::uint64_t{u(5)}*size,extent_index=std::uint64_t{count}*size;
                            require(!buffer->mapped && at<=buffer->pixels.size() && extent_index<=buffer->pixels.size()-at);data.assign(buffer->pixels.begin()+static_cast<std::ptrdiff_t>(at),buffer->pixels.begin()+static_cast<std::ptrdiff_t>(at+extent_index));}
                        for(unsigned i=0;i<count;++i){std::uint32_t index=0;std::memcpy(&index,data.data()+i*size,size);require(index>=min && index-min<vertices);indices[i]=index;}}
                    else for(unsigned i=0;i<count;++i)indices[i]=min+i;
                    const bool gpu_vertices=!rhw && object.native->fixed_vertex_supported();
                    std::vector<FixedGraphicsVertex> packed;packed.reserve(gpu_vertices?std::min(count,vertices):count);const auto& v=object.viewport;
                    std::vector<std::uint32_t> slots(gpu_vertices?vertices:0,UINT32_MAX),packed_indices;if(gpu_vertices)packed_indices.reserve(count);
                    for(auto index:indices){
                        if(gpu_vertices && slots[index-min]!=UINT32_MAX){packed_indices.push_back(slots[index-min]);continue;}
                        if(gpu_vertices){slots[index-min]=static_cast<std::uint32_t>(packed.size());packed_indices.push_back(slots[index-min]);}
                        const auto at=std::size_t{index-min}*stride;Vector position{real(word(bytes,at)),real(word(bytes,at+4)),real(word(bytes,at+8)),1}; // 頂點浮點值交由 GPU 裁切；退化圖元不應使整次 API 呼叫失敗。
                        Vector vertex_color=(object.fvf&0x40)?color(word(bytes,at+color_offset)):Vector{1,1,1,1},secondary{};
                        const auto vertex_specular=(object.fvf&0x80)?color(word(bytes,at+specular_offset)):Vector{};
                        Vector normal{};if(object.fvf&0x10)for(unsigned component=0;component<3;++component)normal[component]=real(word(bytes,at+normal_offset+component*4));
#include "d3d9_vertex_blend.inc"
                        if(!rhw && object.render.at(137)){auto lit=lit_colors(object,position,normal,vertex_color,vertex_specular,(object.fvf&0x40)!=0,(object.fvf&0x80)!=0,vertex_blending);vertex_color=lit.first;secondary=lit.second;}
                        else if(object.render.at(29))secondary=vertex_specular;
                        require(gpu_vertices || !object.render.at(29),unavailable);
                        if(rhw){const auto reciprocal=real(word(bytes,at+12));require(std::isfinite(reciprocal) && reciprocal>0);const auto w=1/reciprocal;position={(2*(position[0]-static_cast<float>(v[0])+0.5F)/static_cast<float>(v[2])-1)*w,(1-2*(position[1]-static_cast<float>(v[1])+0.5F)/static_cast<float>(v[3]))*w,(2*position[2]-1)*w,w};}
                        else if(!gpu_vertices){position=transform(transform(transform(position,drawing_world),object.transforms.at(2)),object.transforms.at(3));position[0]+=position[3]/static_cast<float>(v[2]);position[1]-=position[3]/static_cast<float>(v[3]);position[2]=2*position[2]-position[3];}
                        Vector texcoord{0,0,0,1};if(object.fvf&0x100){texcoord[0]=real(word(bytes,at+tex_offset));texcoord[1]=real(word(bytes,at+tex_offset+4));}
                        packed.push_back({position,vertex_color,texcoord,secondary});
                    }
                    auto drawing_type=type;
                    if(object.render.at(9)==1 && type!=1){
                        // 平面著色使用 D3D 圖元的第一頂點色；展開 strip/fan 保留個別圖元顏色。
                        std::vector<FixedGraphicsVertex> flat;
                        const auto vertex=[&](unsigned n)->const FixedGraphicsVertex&{return packed[gpu_vertices?packed_indices[n]:n];};
                        const auto emit=[&](std::initializer_list<unsigned> group,unsigned first){const auto& provoking=vertex(first);for(auto n:group){auto value=vertex(n);value.color=provoking.color;value.secondary=provoking.secondary;flat.push_back(value);}};
                        if(type==2){flat.reserve(count);for(unsigned n=0;n+1<count;n+=2)emit({n,n+1},n);}
                        else if(type==3){flat.reserve(std::size_t(count-1)*2);for(unsigned n=0;n+1<count;++n)emit({n,n+1},n);drawing_type=2;}
                        else if(type==4){flat.reserve(count);for(unsigned n=0;n+2<count;n+=3)emit({n,n+1,n+2},n);}
                        else if(type==5){flat.reserve(std::size_t(count-2)*3);for(unsigned n=0;n+2<count;++n)emit({n+(n&1U),n+1-(n&1U),n+2},n);drawing_type=4;}
                        else {flat.reserve(std::size_t(count-2)*3);for(unsigned n=1;n+1<count;++n)emit({n,n+1,0},n);drawing_type=4;}
                        packed=std::move(flat);packed_indices.clear();
                    }
                    fixed_targets(object);const auto target_dimensions=target_size(object);
                    const bool two_sided=object.render.at(185) && type>=4;object.native->graphics_fixed_stencil({object.render.at(52),object.render.at(57),object.render.at(58),object.render.at(59),object.render.at(56),object.render.at(53),object.render.at(54),object.render.at(55),object.render.at(two_sided?189:56),object.render.at(two_sided?186:53),object.render.at(two_sided?187:54),object.render.at(two_sided?188:55)});
                    call(object,GlOperation::DepthRange,{std::bit_cast<std::uint64_t>(rhw?0.:static_cast<double>(real(v[4]))),std::bit_cast<std::uint64_t>(rhw?1.:static_cast<double>(real(v[5])))});call(object,GlOperation::Viewport,{v[0],target_dimensions[1]-v[1]-v[3],v[2],v[3]});toggle(object,0xb71,object.render.at(7));call(object,GlOperation::DepthFunc,{0x1ff+object.render.at(23)});call(object,GlOperation::DepthMask,{object.render.at(14)});
                    toggle(object,0xb60,object.render.at(28));toggle(object,0xbc0,object.render.at(15));call(object,GlOperation::AlphaFunc,{0x1ff+object.render.at(25),bits(static_cast<float>(object.render.at(24))/255)});
                    toggle(object,0xbe2,object.render.at(27));const std::array<unsigned,11> blend{0,1,0x300,0x301,0x302,0x303,0x304,0x305,0x306,0x307,0x308};call(object,GlOperation::BlendFunc,{blend[object.render.at(19)-1],blend[object.render.at(20)-1]});
                    toggle(object,0xb44,object.render.at(22)!=1);call(object,GlOperation::FrontFace,{0x900});call(object,GlOperation::CullFace,{object.render.at(22)==2?0x404U:0x405U});
                    toggle(object,0xc11,object.render.at(174));const auto& r=object.scissor;call(object,GlOperation::Scissor,{static_cast<unsigned>(r[0]),target_dimensions[1]-static_cast<unsigned>(r[3]),static_cast<unsigned>(r[2]-r[0]),static_cast<unsigned>(r[3]-r[1])});
                    const bool textured=object.texture && object.stage.at(1)!=1;toggle(object,0xde1,textured);
                    const auto factor=color(object.render.at(60));call(object,GlOperation::TexEnvfv,{0x2300,0x2201},std::span(reinterpret_cast<const std::uint8_t*>(factor.data()),16));
                    const auto uses_texture=[&](unsigned op,unsigned first,unsigned second){return ((op==2 || op==4) && (first&15U)==2) || ((op==3 || op==4) && (second&15U)==2);};
                    const bool combine=object.stage.at(1)!=1 && (textured || !uses_texture(object.stage.at(1),object.stage.at(2),object.stage.at(3)));
                    call(object,GlOperation::TextureCombine,{object.stage.at(1)==1?2U:object.stage.at(1),object.stage.at(4)==1?3U:object.stage.at(4),0x80000000U|object.stage.at(2)|(object.stage.at(3)<<6)|(object.stage.at(5)<<12)|((object.stage.at(4)==1?1U:object.stage.at(6))<<18),combine?1U:0U});
                    if(textured){upload(object,*get(object.texture,Kind::Texture));
                        call(object,GlOperation::TexParameteri,{0xde1,0x2802,object.sampler.at(1)==1?0x2901U:0x812fU});call(object,GlOperation::TexParameteri,{0xde1,0x2803,object.sampler.at(2)==1?0x2901U:0x812fU});
                        call(object,GlOperation::TexParameteri,{0xde1,0x84fe,(object.sampler.at(5)==3 || object.sampler.at(6)==3)?object.sampler.at(10):1U});
                        const auto mip=object.sampler.at(7),linear=object.sampler.at(6)>=2?1U:0U;call(object,GlOperation::TexParameteri,{0xde1,0x2801,mip?0x2700U+(mip==2?2U:0U)+linear:0x2600U+linear});call(object,GlOperation::TexParameteri,{0xde1,0x2800,object.sampler.at(5)==1?0x2600U:0x2601U});check_gpu(object);}
                    if(gpu_vertices)object.native->graphics_fixed_draw({packed,indexed?std::span<const std::uint32_t>(packed_indices):std::span<const std::uint32_t>{},{drawing_world,object.transforms.at(2),object.transforms.at(3)},{static_cast<float>(v[2]),static_cast<float>(v[3])},drawing_type});
                    else {
                    const std::array<unsigned,6> modes{0,1,3,4,5,6};call(object,GlOperation::Begin,{modes[drawing_type-1]});
                    for(const auto& entry:packed){call(object,GlOperation::TexCoord4fv,{},std::span(reinterpret_cast<const std::uint8_t*>(entry.texcoord.data()),16));call(object,GlOperation::Color4fv,{},std::span(reinterpret_cast<const std::uint8_t*>(entry.color.data()),16));call(object,GlOperation::Vertex4fv,{},std::span(reinterpret_cast<const std::uint8_t*>(entry.position.data()),16));}
                    call(object,GlOperation::End);}
                    check_gpu(object);++object.draws;clear_up();
                }else throw HostCallFault("尚未實作 Direct3D 9 device 方法："+std::string(symbol));
                if(recording)record_state(object,symbol,u(1),u(2),u(3));
            }
        }
    }catch(const Failure& error){result=(service.symbol=="Direct3DCreate9" || service.symbol=="FactoryEx.GetAdapterModeCountEx")?0:error.value;if(std::getenv("X64ARM_TRACE_GRAPHICS_ERRORS"))std::fprintf(stderr,"[d3d9] %s HRESULT=%08x (%s:%u)\n",std::string(service.symbol).c_str(),error.value,error.where.file_name(),error.where.line());}
    catch(const texture_image::Error& error){result=error.hresult;if(std::getenv("X64ARM_TRACE_GRAPHICS_ERRORS"))std::fprintf(stderr,"[d3d9] %s: %s\n",std::string(service.symbol).c_str(),error.what());}
    catch(const std::bad_alloc&){result=service.symbol=="Direct3DCreate9"?0:out_of_memory;}
    catch(const std::invalid_argument& error){result=invalid;if(std::getenv("X64ARM_TRACE_GRAPHICS_ERRORS"))std::fprintf(stderr,"[d3d9] %s: %s\n",std::string(service.symbol).c_str(),error.what());}
    for(auto it=bridge.prt_engine_operations.begin();it!=bridge.prt_engine_operations.end();)if(it->second.thread==threads_.current && it->second.stack==state.reg(Gpr::Rsp) && it->second.service.symbol==service.symbol)it=bridge.prt_engine_operations.erase(it);else ++it;
    for(auto it=bridge.prt_comp_operations.begin();it!=bridge.prt_comp_operations.end();)if(it->second.thread==threads_.current && it->second.stack==state.reg(Gpr::Rsp) && it->second.service.symbol==service.symbol)it=bridge.prt_comp_operations.erase(it);else ++it;
    for(auto it=bridge.fill_operations.begin();it!=bridge.fill_operations.end();)if(it->second.thread==threads_.current && it->second.stack==state.reg(Gpr::Rsp) && it->second.service.symbol==service.symbol){if(it->second.temporary)(void)free_heap(memory,it->second.temporary,state);it=bridge.fill_operations.erase(it);}else ++it;
    for(auto it=bridge.hierarchy_operations.begin();it!=bridge.hierarchy_operations.end();)if(it->second.thread==threads_.current && it->second.stack==state.reg(Gpr::Rsp) && it->second.service.symbol==service.symbol){for(auto pointer:it->second.owned)release(pointer);for(auto pointer:it->second.temporaries)(void)free_heap(memory,pointer,state);it=bridge.hierarchy_operations.erase(it);}else ++it;
    for(auto it=bridge.atlas_operations.begin();it!=bridge.atlas_operations.end();)if(it->second.thread==threads_.current && it->second.stack==state.reg(Gpr::Rsp) && it->second.service.symbol==service.symbol)it=bridge.atlas_operations.erase(it);else ++it;
    if(!private_callbacks.empty()){start_callbacks(state,memory,std::move(private_callbacks),Continuation::Return,result);return std::nullopt;}
    arg.return_to(state,result);return std::nullopt;
}
}
