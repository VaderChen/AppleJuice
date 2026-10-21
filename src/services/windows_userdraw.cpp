#include "x64arm/windows_userdraw.hpp"
#include <algorithm>
#include <cmath>
namespace x64arm {
std::uint32_t gui_system_color(unsigned index){
    static constexpr std::array<std::uint32_t,31> colors{0xc8c8c8,0,0xd1b499,0xdbcdbf,0xf0f0f0,0xffffff,0x646464,0,0,0xffffff,0xb4b4b4,0xfcf7f4,0xababab,0xd77800,0xffffff,0xf0f0f0,0xa0a0a0,0x6d6d6d,0,0,0xffffff,0x696969,0xe3e3e3,0,0xe1ffff,0,0xcc6600,0xead1b9,0xf2e4d7,0xff9933,0xf0f0f0};
    return index<colors.size()?colors[index]:0;
}
namespace {
void fill(std::vector<GuiDraw>& out,int left,int top,int right,int bottom,std::uint32_t color){if(right<=left || bottom<=top)return;GuiDraw d;d.kind=GuiDraw::Kind::Fill;d.x=left;d.y=top;d.right=right;d.bottom=bottom;d.color=color;out.push_back(d);}
void line(std::vector<GuiDraw>& out,int x,int y,int right,int bottom,std::uint32_t color,int width=1){GuiDraw d;d.kind=GuiDraw::Kind::Line;d.x=x;d.y=y;d.right=right;d.bottom=bottom;d.color=color;d.pen_width=width;out.push_back(d);}
void ellipse(std::vector<GuiDraw>& out,const GuiRect& r,std::uint32_t border,std::uint32_t color){GuiDraw d;d.kind=GuiDraw::Kind::Ellipse;d.x=r[0];d.y=r[1];d.right=r[2];d.bottom=r[3];d.color=border;d.filled=true;d.fill_color=color;out.push_back(d);}
void frame(std::vector<GuiDraw>& out,GuiRect& r,std::uint32_t light,std::uint32_t dark,unsigned sides=15){
    if(r[0]>=r[2] || r[1]>=r[3])return;
    if(sides&2)fill(out,r[0],r[1],r[2],r[1]+1,light);
    if(sides&8)fill(out,r[0],r[3]-1,r[2],r[3],dark);
    if(sides&1)fill(out,r[0],r[1]+((sides&2)?1:0),r[0]+1,r[3]-((sides&8)?1:0),light);
    if(sides&4)fill(out,r[2]-1,r[1]+((sides&2)?1:0),r[2],r[3]-((sides&8)?1:0),dark);
    if(sides&1)++r[0];if(sides&2)++r[1];if(sides&4)--r[2];if(sides&8)--r[3];
}
void arrow(std::vector<GuiDraw>& out,const GuiRect& r,unsigned direction,std::uint32_t color){
    const int cx=r[0]+(r[2]-r[0])/2,cy=r[1]+(r[3]-r[1])/2,half=std::max(1,std::min(r[2]-r[0],r[3]-r[1])/4);
    for(int i=0;i<half;++i){const auto breadth=2*i+1;if(direction<2){const auto y=cy+(direction==0?i-half/2:half/2-i);fill(out,cx-i,y,cx-i+breadth,y+1,color);}else{const auto x=cx+(direction==2?i-half/2:half/2-i);fill(out,x,cy-i,x+1,cy-i+breadth,color);}}
}
void checkmark(std::vector<GuiDraw>& out,const GuiRect& r,std::uint32_t color){const int w=r[2]-r[0],h=r[3]-r[1],width=std::max(1,std::min(w,h)/8);line(out,r[0]+w/5,r[1]+h/2,r[0]+2*w/5,r[1]+3*h/4,color,width);line(out,r[0]+2*w/5,r[1]+3*h/4,r[0]+4*w/5,r[1]+h/4,color,width);}
}
std::uint32_t gui_draw_edge(GuiRect& rect,unsigned edge,unsigned flags,std::vector<GuiDraw>& out){
    if((edge&~15U) || (flags&~0xf81fU))return 87;if((edge&3)==3 || (edge&12)==12)return 87;
    if(flags&0x10)return 50; // 對角邊框需要不同的 BF_MIDDLE 區域，不能退化為矩形。
    auto r=rect;const bool mono=flags&0x8000,flat=flags&0x4000,soft=flags&0x1000;
    for(unsigned layer=0;layer<2;++layer){const auto part=(edge>>(layer*2))&3;if(!part)continue;const bool raised=part==1;auto light=gui_system_color(layer?22:20),dark=gui_system_color(layer?16:21);if(soft && !layer)light=gui_system_color(22);if(mono)light=dark=gui_system_color(6);else if(flat)light=dark=gui_system_color(16);frame(out,r,raised?light:dark,raised?dark:light,flags&15);}
    if(flags&0x800)fill(out,r[0],r[1],r[2],r[3],gui_system_color(mono?5:15));if(flags&0x2000)rect=r;return 0;
}
std::uint32_t gui_frame_control(GuiRect& rect,unsigned type,unsigned flags,std::vector<GuiDraw>& out){
    if(type<1 || type>5 || (flags&~0xff1fU))return 87;if(type==5)return 50;const auto shape=flags&31;const bool valid=type==1?shape<=4:type==2?(shape<=4 || shape==8 || shape==16):type==3?(shape<=3 || shape==5 || shape==8 || shape==16):type==4?(shape<=2 || shape==4 || shape==8 || shape==16):(shape==0 || shape==1);
    if(!valid)return 87;const bool transparent=flags&0x800;if(transparent && !(type==2 && (shape==8 || shape==16)))return 50;
    auto r=rect;if(r[0]>=r[2] || r[1]>=r[3])return 0;const auto width=r[2]-r[0],height=r[3]-r[1];const bool mono=(flags&0x8000) || type==2 || (type==4 && shape!=16),flat=flags&0x4000,pushed=flags&0x200,checked=flags&0x400,inactive=flags&0x100,hot=flags&0x1000;
    const auto face=mono?0xffffffU:gui_system_color(15),ink=mono?0U:gui_system_color(inactive?17:18);
    if(width<5 || height<5){if(!transparent)fill(out,r[0],r[1],r[2],r[3],face);return 0;}
    if(type==2){if(!transparent)fill(out,r[0],r[1],r[2],r[3],shape>=8?gui_system_color(4):0xffffff);if(shape==8 || shape==16)arrow(out,r,shape==8?0:1,gui_system_color(inactive?17:7));else if(shape==1 || shape==3)checkmark(out,r,0);else if(shape==2){const int cx=r[0]+width/2,cy=r[1]+height/2,radius=std::max(1,std::min(width,height)/6);ellipse(out,{cx-radius,cy-radius,cx+radius+1,cy+radius+1},0,0);}else arrow(out,r,shape==4?2:3,0);return 0;}
    if(type==4 && shape!=16){
        const auto side=std::min(width,height);r[0]+=(width-side)/2;r[1]+=(height-side)/2;r[2]=r[0]+side;r[3]=r[1]+side;fill(out,rect[0],rect[1],rect[2],rect[3],0xffffff);
        if(shape==1 || shape==2 || shape==4){ellipse(out,r,0,shape==2?0:0xffffff);if(checked && shape!=2){const auto inset=std::max(2,side/3);ellipse(out,{r[0]+inset,r[1]+inset,r[2]-inset,r[3]-inset},0,0);}}
        else {frame(out,r,0,0);if(checked){if(shape==8)fill(out,r[0]+2,r[1]+2,r[2]-2,r[3]-2,0);else checkmark(out,r,0);}}return 0;
    }
    if(type==3 && (shape==8 || shape==16)){fill(out,r[0],r[1],r[2],r[3],face);const auto extent=std::min(width,height);for(int delta=3;delta<extent;delta+=4){const int bottom=r[3]-2;const int x=shape==8?r[2]-2:r[0]+1;const int dx=shape==8?-delta:delta;line(out,x+dx,bottom,x,bottom-delta,mono?0:gui_system_color(20));line(out,x+dx,bottom-1,x,bottom-delta-1,mono?0:gui_system_color(16));}return 0;}
    fill(out,r[0],r[1],r[2],r[3],face);const auto light=mono?0:gui_system_color(20),dark=mono?0:gui_system_color(21);
    frame(out,r,flat || mono?ink:pushed?dark:light,flat || mono?ink:pushed?light:dark);if(!flat && !mono)frame(out,r,pushed?gui_system_color(16):gui_system_color(22),pushed?gui_system_color(22):gui_system_color(16));
    if(hot && !mono){auto border=rect;frame(out,border,gui_system_color(26),gui_system_color(26));}
    if(type==4){if(checked){for(int y=r[1];y<r[3];++y)for(int x=r[0]+((r[0]+y)&1);x<r[2];x+=2)fill(out,x,y,x+1,y+1,mono?0:gui_system_color(20));}if(flags&0x2000)rect=r;return 0;}
    if(pushed){++r[0];++r[1];++r[2];++r[3];}
    if(type==3){arrow(out,r,shape==5?1:shape,ink);return 0;}
    const auto inset=std::max(2,std::min(width,height)/4);GuiRect glyph{rect[0]+inset,rect[1]+inset,rect[2]-inset,rect[3]-inset};if(pushed)for(auto& v:glyph)++v;const auto thickness=std::max(1,std::min(width,height)/10);
    if(shape==0){line(out,glyph[0],glyph[1],glyph[2],glyph[3],ink,thickness);line(out,glyph[2]-1,glyph[1],glyph[0]-1,glyph[3],ink,thickness);}
    else if(shape==1)fill(out,glyph[0],glyph[3]-thickness,glyph[2],glyph[3],ink);
    else if(shape==2 || shape==3){if(shape==3){auto back=glyph;back[0]+=thickness+1;back[3]-=thickness+1;frame(out,back,ink,ink);glyph[1]+=thickness+1;glyph[2]-=thickness+1;fill(out,glyph[0],glyph[1],glyph[2],glyph[3],face);}fill(out,glyph[0],glyph[1],glyph[2],glyph[1]+thickness,ink);frame(out,glyph,ink,ink);}
    else {constexpr std::array<unsigned,7> question{14,17,1,2,4,0,4};const auto cell=std::max(1,std::min((glyph[2]-glyph[0])/5,(glyph[3]-glyph[1])/7));const auto x=glyph[0]+((glyph[2]-glyph[0])-cell*5)/2,y=glyph[1]+((glyph[3]-glyph[1])-cell*7)/2;for(unsigned row=0;row<7;++row)for(unsigned col=0;col<5;++col)if(question[row]&(16U>>col))fill(out,x+static_cast<int>(col)*cell,y+static_cast<int>(row)*cell,x+static_cast<int>(col+1)*cell,y+static_cast<int>(row+1)*cell,ink);}
    return 0;
}
}
