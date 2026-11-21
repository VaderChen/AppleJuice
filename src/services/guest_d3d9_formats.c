/* Windows COM ABI 的純格式查詢；resident AOT 直接執行同一份能力契約。 */
#include "d3d9_format_contract.h"
#if defined(__i386__)
#define CALL __attribute__((stdcall))
#else
#define CALL
#endif
#define EXPORT __declspec(dllexport) unsigned CALL
EXPORT CheckDeviceFormat(void* self,unsigned adapter,unsigned device,unsigned display,unsigned usage,unsigned type,unsigned format){
    (void)self;return d9_check_format(adapter,device,display,usage,type,format);
}
EXPORT CheckDeviceType(void* self,unsigned adapter,unsigned device,unsigned display,unsigned back,unsigned windowed){
    (void)self;return d9_check_type(adapter,device,display,back,windowed);
}
EXPORT CheckDepthStencilMatch(void* self,unsigned adapter,unsigned device,unsigned display,unsigned color,unsigned depth){
    (void)self;return d9_check_depth(adapter,device,display,color,depth);
}
EXPORT CheckDeviceFormatConversion(void* self,unsigned adapter,unsigned device,unsigned source,unsigned destination){
    (void)self;return d9_check_conversion(adapter,device,source,destination);
}
#include "d3dx_half.h"

/* 客體位址可未對齊，陣列重疊時仍逐項先讀後寫。 */
struct __attribute__((packed,may_alias)) D3dxHalfBits { uint16_t value; };
struct __attribute__((packed,may_alias)) D3dxFloatBits { uint32_t value; };
__declspec(dllexport) void *CALL D3DXFloat32To16Array(void *out,const void *in,unsigned count)
{
    volatile struct D3dxHalfBits *dest=out;
    const volatile struct D3dxFloatBits *src=in;
    for(unsigned i=0;i<count;++i)dest[i].value=d3dx_narrow_half(src[i].value);
    return out;
}
__declspec(dllexport) void *CALL D3DXFloat16To32Array(void *out,const void *in,unsigned count)
{
    volatile struct D3dxFloatBits *dest=out;
    const volatile struct D3dxHalfBits *src=in;
    for(unsigned i=0;i<count;++i)dest[i].value=d3dx_expand_half(src[i].value);
    return out;
}
#include "d3dx10_version.h"
#define D3DX10_VERSION_ENTRY(version) \
    EXPORT D3DX10CheckVersion##version(unsigned d3d,unsigned d3dx){return d3dx10_check_version(d3d,d3dx,version);}
D3DX10_SDK_VERSIONS(D3DX10_VERSION_ENTRY)
#undef D3DX10_VERSION_ENTRY
#include "guest_sh_light.c"
/* 同一常駐映像沿用既有 PE 載入、COM 轉發與 AOT 產碼。 */
#undef EXPORT
#undef CALL
#include "guest_d3d9_constants.c"
