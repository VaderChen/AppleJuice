/* 客體 lowering 與宿主共用的格式能力契約；只宣告已具備實際資源路徑的能力。 */
#pragma once
#define D9_INVALID 0x8876086cU
#define D9_UNAVAILABLE 0x8876086aU
static inline unsigned d9_raw_depth(unsigned format){
    return format==0x36314644U || format==0x34324644U || format==0x5a544e49U;
}
/* 驅動的「硬體陰影貼圖」：D16、D24X8、D24S8 可建立為 DEPTHSTENCIL 紋理；取樣時以紋理座標 z 與深度比較
 * （LESS_EQUAL，線性過濾即 PCF），與 INTZ／DF16／DF24 讀原始深度不同。 */
static inline unsigned d9_shadow_depth(unsigned format){return format==75 || format==77 || format==80;}
static inline unsigned d9_depth_format(unsigned format){
    switch(format){
    case 71:case 77:return 40;
    case 75:return 20;
    case 80:case 0x36314644U:return 55;
    case 0x34324644U:case 0x5a544e49U:return 45;
    default:return 0;
    }
}
static inline unsigned d9_color(unsigned f){return f==21 || f==22;}
static inline unsigned d9_packed_color(unsigned f){return f==23 || f==25 || f==26;}
static inline unsigned d9_render_color(unsigned f){return d9_color(f) || d9_packed_color(f) || f==32 || f==35 || f==36 || (f>=111 && f<=116);}
static inline unsigned d9_native_format(unsigned format){
    switch(format){
    case 116:return 2;case 113:return 10;case 36:return 11;case 115:return 16;
    case 35:return 24;case 32:return 28;case 112:return 34;case 34:return 35;
    case 114:return 41;case 51:return 49;case 111:return 54;case 81:return 56;
    case 50:return 61;case 28:return 65;case 0x31545844U:return 71;
    case 0x33545844U:return 74;case 0x35545844U:return 77;
    case 23:return 85;case 25:return 86;case 21:return 87;case 22:return 88;
    case 26:return 115;default:return d9_raw_depth(format) || d9_shadow_depth(format)?d9_depth_format(format):0;
    }
}
static inline unsigned d9_srgb(unsigned f){return f==21 || f==22 || f==32 || f==23 || f==25 || f==26 || f==50 || f==51 || f==0x31545844U || f==0x33545844U || f==0x35545844U;}
static inline unsigned d9_device(unsigned adapter,unsigned device){return !adapter && (device==1 || device==4);}
/* 16 位元後緩衝（R5G6B5、X1R5G5B5、A1R5G5B5）：Windows 8 之後 16 位元顯示模式由系統模擬，視窗模式由桌面合成轉換。 */
static inline unsigned d9_back16(unsigned f){return f==23 || f==24 || f==25;}
/* 顯示模式格式：X8R8G8B8 與模擬的 16 位元模式（R5G6B5、X1R5G5B5）。 */
static inline unsigned d9_display(unsigned f){return f==22 || f==23 || f==24;}
static inline unsigned d9_check_format(unsigned adapter,unsigned device,unsigned display,unsigned usage,unsigned type,unsigned format){
    if(!d9_device(adapter,device))return D9_INVALID;
    if(!d9_display(display) || (type!=1 && type!=3 && type!=4 && type!=5))return D9_UNAVAILABLE;
    if(usage&2){
        if(usage&~0x120002U)return D9_UNAVAILABLE;
        if(type==1)return usage==2 && d9_depth_format(format)?0:D9_UNAVAILABLE;
        return (type==3 || type==5) && (d9_raw_depth(format) || d9_shadow_depth(format))?0:D9_UNAVAILABLE;
    }
    if((usage&~0x3f0201U) || d9_raw_depth(format) || d9_shadow_depth(format) || !d9_native_format(format))return D9_UNAVAILABLE;
    if((usage&1) && !d9_render_color(format))return D9_UNAVAILABLE;
    if((usage&0x10000U) && !d9_srgb(format))return D9_UNAVAILABLE;
    if((usage&0x40000U) && (!d9_render_color(format) || !d9_srgb(format) || d9_packed_color(format)))return D9_UNAVAILABLE;
    if(type==1 && ((usage&~0xc0001U) || !d9_render_color(format)))return D9_UNAVAILABLE;
    if(type==4 && usage!=0 && usage!=0x200)return D9_UNAVAILABLE;
    return 0;
}
static inline unsigned d9_check_type(unsigned adapter,unsigned device,unsigned display,unsigned back,unsigned windowed){
    if(!d9_device(adapter,device))return D9_INVALID;
    /* 視窗模式：32 位元桌面接受 32 位元與 16 位元後緩衝；全螢幕：後緩衝與顯示模式同為 32 位元或同一種 16 位元格式（A1R5G5B5 可配 X1R5G5B5 模式）。 */
    if(windowed)return display==22 && (d9_color(back) || d9_back16(back) || !back)?0:D9_UNAVAILABLE;
    if(display==22)return d9_color(back)?0:D9_UNAVAILABLE;
    if(display==23)return back==23?0:D9_UNAVAILABLE;
    if(display==24)return back==24 || back==25?0:D9_UNAVAILABLE;
    return D9_UNAVAILABLE;
}
static inline unsigned d9_check_depth(unsigned adapter,unsigned device,unsigned display,unsigned color,unsigned depth){
    if(!d9_device(adapter,device))return D9_INVALID;
    return d9_display(display) && (d9_render_color(color) || d9_back16(color)) && d9_depth_format(depth)?0:D9_UNAVAILABLE;
}
static inline unsigned d9_check_conversion(unsigned adapter,unsigned device,unsigned source,unsigned destination){
    if(!d9_device(adapter,device))return D9_INVALID;
    return d9_color(source) && source==destination?0:D9_UNAVAILABLE;
}
