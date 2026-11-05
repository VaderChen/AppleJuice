/* 客體 lowering 與宿主共用的格式能力契約；只宣告已具備實際資源路徑的能力。 */
#pragma once
#define D9_INVALID 0x8876086cU
#define D9_UNAVAILABLE 0x8876086aU
static inline unsigned d9_raw_depth(unsigned format){
    return format==0x36314644U || format==0x34324644U || format==0x5a544e49U;
}
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
    case 26:return 115;default:return d9_raw_depth(format)?d9_depth_format(format):0;
    }
}
static inline unsigned d9_srgb(unsigned f){return f==21 || f==22 || f==32 || f==23 || f==25 || f==26 || f==50 || f==51 || f==0x31545844U || f==0x33545844U || f==0x35545844U;}
static inline unsigned d9_device(unsigned adapter,unsigned device){return !adapter && (device==1 || device==4);}
static inline unsigned d9_check_format(unsigned adapter,unsigned device,unsigned display,unsigned usage,unsigned type,unsigned format){
    if(!d9_device(adapter,device))return D9_INVALID;
    if(display!=22 || (type!=1 && type!=3 && type!=4 && type!=5))return D9_UNAVAILABLE;
    if(usage&2){
        if(usage&~0x120002U)return D9_UNAVAILABLE;
        if(type==1)return usage==2 && d9_depth_format(format)?0:D9_UNAVAILABLE;
        return (type==3 || type==5) && d9_raw_depth(format)?0:D9_UNAVAILABLE;
    }
    if((usage&~0x3f0201U) || d9_raw_depth(format) || !d9_native_format(format))return D9_UNAVAILABLE;
    if((usage&1) && !d9_render_color(format))return D9_UNAVAILABLE;
    if((usage&0x10000U) && !d9_srgb(format))return D9_UNAVAILABLE;
    if((usage&0x40000U) && (!d9_render_color(format) || !d9_srgb(format) || d9_packed_color(format)))return D9_UNAVAILABLE;
    if(type==1 && ((usage&~0xc0001U) || !d9_render_color(format)))return D9_UNAVAILABLE;
    if(type==4 && usage!=0 && usage!=0x200)return D9_UNAVAILABLE;
    return 0;
}
static inline unsigned d9_check_type(unsigned adapter,unsigned device,unsigned display,unsigned back,unsigned windowed){
    if(!d9_device(adapter,device))return D9_INVALID;
    return display==22 && (d9_color(back) || (!back && windowed))?0:D9_UNAVAILABLE;
}
static inline unsigned d9_check_depth(unsigned adapter,unsigned device,unsigned display,unsigned color,unsigned depth){
    if(!d9_device(adapter,device))return D9_INVALID;
    return display==22 && d9_render_color(color) && d9_depth_format(depth)?0:D9_UNAVAILABLE;
}
static inline unsigned d9_check_conversion(unsigned adapter,unsigned device,unsigned source,unsigned destination){
    if(!d9_device(adapter,device))return D9_INVALID;
    return d9_color(source) && source==destination?0:D9_UNAVAILABLE;
}
