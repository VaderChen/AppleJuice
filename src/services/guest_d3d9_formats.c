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
/* 同一常駐映像沿用既有 PE 載入、COM 轉發與 AOT 產碼。 */
#undef EXPORT
#undef CALL
#include "guest_d3d9_constants.c"
