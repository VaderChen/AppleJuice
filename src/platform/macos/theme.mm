#include "x64arm/windows_theme.hpp"
#import <AppKit/AppKit.h>
#include <algorithm>
#include <cmath>
#include <cstring>

@interface MacWinThemeView : NSView
@end
@implementation MacWinThemeView
- (BOOL)isFlipped {return YES;}
@end
namespace x64arm {
namespace {
enum class Kind {Button,Check,Radio,Group,Edit,Caret,Drop,Combo,Arrow,ScrollSlot,ScrollThumb,SliderTrack,SliderThumb,Progress,Tab,Header,Tip,Glyph,ListItem};
struct Part {Kind kind;NSCell* __strong cell{};NSView* __strong view{};NSSize size{};bool disabled{},pressed{},hot{},focused{},fixed{},transparent{},vertical{};unsigned direction{};};
std::optional<Part> part_for(std::u16string_view cls,int part,int state) {
    if(part<0 || state<0)return {};if(!part)part=1;if(!state)state=1;
    Part item{Kind::Button};const auto group=[&](unsigned maximum){return static_cast<unsigned>(state)<=maximum;};
    item.disabled=state==4;item.pressed=state==3;item.hot=state==2;item.transparent=true;
    if(cls==u"BUTTON") {
        if(part==1 || part==5 || part==6){if(!group(part==6?6:5))return {};item.kind=Kind::Button;item.focused=state>=5;}
        else if(part==2 || part==3){if(!group(part==2?8:12))return {};item.kind=part==2?Kind::Radio:Kind::Check;item.disabled=(state-1)%4==3;item.hot=(state-1)%4==1;item.pressed=(state-1)%4==2;item.fixed=item.transparent=true;}
        else if(part==4){if(!group(2))return {};item.kind=Kind::Group;item.disabled=state==2;item.transparent=true;}
        else if(part==7){if(!group(5))return {};item.kind=Kind::Glyph;item.direction=1;item.fixed=item.transparent=true;}
        else return {};
    }else if(cls==u"EDIT") {
        if(part<1 || part>9 || !group(part==1?8:part==2?1:6))return {};item.kind=part==2?Kind::Caret:Kind::Edit;item.disabled=state==4;item.focused=state==(part==1?5:3);item.pressed=false;item.transparent=part==2;
    }else if(cls==u"COMBOBOX") {
        if(part<1 || part>8 || !group(4))return {};item.kind=part==1 || part==6 || part==7?Kind::Drop:Kind::Combo;item.focused=part==3 || part==4?state==3:false;
    }else if(cls==u"SPIN") {
        if(part<1 || part>4 || !group(4))return {};item.kind=Kind::Arrow;item.direction=part==1?0U:part==2?2U:part==3?1U:3U;item.fixed=true;
    }else if(cls==u"SCROLLBAR") {
        if(part<1 || part>10)return {};
        if(part==1){if(!group(20))return {};item.kind=Kind::Arrow;constexpr unsigned directions[]{0,2,3,1};item.direction=directions[state>=17?state-17:(state-1)/4];item.disabled=state<17 && (state-1)%4==3;item.hot=state>=17 || (state-1)%4==1;item.pressed=state<17 && (state-1)%4==2;item.fixed=true;}
        else {if(!group(5))return {};item.kind=part==2 || part==3 || part==8 || part==9?Kind::ScrollThumb:part==10?Kind::Group:Kind::ScrollSlot;item.transparent=true;}
    }else if(cls==u"TRACKBAR") {
        if(part<1 || part>8 || !group(part<=2?1:5))return {};item.kind=part<=2?Kind::SliderTrack:Kind::SliderThumb;item.vertical=part==2 || part>=6;item.disabled=state==5;item.focused=state==4;item.transparent=true;item.fixed=item.kind==Kind::SliderThumb;
    }else if(cls==u"PROGRESS") {
        if(part<1 || part>4 || !group(part<=2?1:4))return {};item.kind=Kind::Progress;item.disabled=false;item.transparent=true;item.vertical=part==2 || part==4;
    }else if(cls==u"TAB") {
        if(part<1 || part>11 || !group(part>=9?1:5))return {};item.kind=part>=9?Kind::Group:Kind::Tab;item.focused=state==3 || state==5;
    }else if(cls==u"HEADER") {
        if(part<1 || part>7 || !group(part==4?2:12))return {};item.kind=part==4?Kind::Glyph:Kind::Header;item.direction=state==1?0U:2U;item.disabled=false;item.fixed=part==4;item.transparent=part==4;
    }else if(cls==u"TOOLTIP") {
        if(part!=1 && part!=2)return {};if(!group(2))return {};item.kind=Kind::Tip;item.disabled=false;
    }else if(cls==u"TREEVIEW") {
        if((part==2 || part==4) && group(2)){item.kind=Kind::Glyph;item.direction=state==1?1U:2U;item.fixed=item.transparent=true;item.disabled=false;}
        else if(part==1 && group(6)){item.kind=Kind::ListItem;item.focused=state==3 || state==5 || state==6;item.transparent=!(item.focused || item.hot);}
        else return {};
    }else if(cls==u"LISTVIEW") {
        if(part!=1 || !group(6))return {};item.kind=Kind::ListItem;item.focused=state==3 || state==5 || state==6;item.transparent=!(item.focused || item.hot);
    }else return {};
    if(item.kind==Kind::Button || item.kind==Kind::Check || item.kind==Kind::Radio || item.kind==Kind::Arrow || item.kind==Kind::Drop || item.kind==Kind::Header || item.kind==Kind::Tab) {
        auto* cell=[[NSButtonCell alloc] initTextCell:@""];cell.font=[NSFont systemFontOfSize:13];cell.enabled=!item.disabled;cell.highlighted=item.pressed;
        cell.buttonType=item.kind==Kind::Check?NSButtonTypeSwitch:item.kind==Kind::Radio?NSButtonTypeRadio:NSButtonTypeMomentaryPushIn;
        cell.bezelStyle=item.kind==Kind::Header?NSBezelStyleSmallSquare:item.kind==Kind::Tab?NSBezelStyleRoundRect:item.kind==Kind::Arrow || item.kind==Kind::Drop?NSBezelStyleTexturedRounded:NSBezelStyleRounded;
        if(item.kind==Kind::Check || item.kind==Kind::Radio)cell.state=state>=9?NSControlStateValueMixed:state>=5?NSControlStateValueOn:NSControlStateValueOff;
        if(item.kind==Kind::Arrow || item.kind==Kind::Drop){NSString* const symbols[]{@"chevron.up",@"chevron.right",@"chevron.down",@"chevron.left"};cell.image=[NSImage imageWithSystemSymbolName:symbols[item.kind==Kind::Drop?2:item.direction] accessibilityDescription:nil];cell.imagePosition=NSImageOnly;}
        item.cell=cell;item.size=cell.cellSize;
        if(item.kind==Kind::Header || item.kind==Kind::Tab)item.size=NSMakeSize(std::max<CGFloat>(36,item.size.width),std::max<CGFloat>(20,item.size.height));
    }else if(item.kind==Kind::Edit || item.kind==Kind::Combo) {
        auto* cell=[[NSTextFieldCell alloc] initTextCell:@""];cell.font=[NSFont systemFontOfSize:13];cell.bezeled=YES;cell.bezelStyle=NSTextFieldRoundedBezel;cell.enabled=!item.disabled;cell.drawsBackground=cls!=u"COMBOBOX" || part!=3;cell.backgroundColor=[NSColor textBackgroundColor];item.cell=cell;item.size=cell.cellSize;
    }else if(item.kind==Kind::SliderTrack || item.kind==Kind::SliderThumb) {
        auto* cell=[[NSSliderCell alloc] init];cell.minValue=0;cell.maxValue=1;cell.doubleValue=.5;cell.enabled=!item.disabled;cell.vertical=item.vertical;item.cell=cell;item.size=item.vertical?NSMakeSize(20,item.fixed?20:40):NSMakeSize(item.fixed?20:40,20);
    }else if(item.kind==Kind::ScrollSlot || item.kind==Kind::ScrollThumb) {
        const bool vertical=part==3 || part==5 || part==7 || part==9;const auto width=[NSScroller scrollerWidthForControlSize:NSControlSizeRegular scrollerStyle:NSScrollerStyleLegacy];
        auto* scroller=[[NSScroller alloc] initWithFrame:NSMakeRect(0,0,vertical?width:64,vertical?64:width)];scroller.scrollerStyle=NSScrollerStyleLegacy;scroller.enabled=!item.disabled;scroller.knobProportion=.5;scroller.doubleValue=.5;item.view=scroller;item.size=scroller.frame.size;
    }else if(item.kind==Kind::Progress){auto* bar=[[NSProgressIndicator alloc] initWithFrame:NSMakeRect(0,0,80,16)];bar.style=NSProgressIndicatorStyleBar;bar.indeterminate=NO;bar.minValue=0;bar.maxValue=1;bar.doubleValue=part==1 || part==2?0:1;item.view=bar;item.size=item.vertical?NSMakeSize(16,80):bar.frame.size;}
    else if(item.kind==Kind::Group || item.kind==Kind::Tip){auto* box=[[NSBox alloc] initWithFrame:NSMakeRect(0,0,24,20)];box.boxType=NSBoxCustom;box.cornerRadius=4;box.borderWidth=1;box.fillColor=item.kind==Kind::Tip?[NSColor windowBackgroundColor]:[NSColor clearColor];box.borderColor=[NSColor separatorColor];item.view=box;item.size=box.frame.size;}
    else if(item.kind==Kind::Caret)item.size=NSMakeSize(1,20);
    else if(item.kind==Kind::Glyph)item.size=NSMakeSize(14,14);
    else item.size=NSMakeSize(24,20);
    item.size.width=std::max<CGFloat>(1,item.size.width);item.size.height=std::max<CGFloat>(1,item.size.height);return item;
}
std::uint32_t color(NSColor* value){auto* c=[value colorUsingColorSpace:[NSColorSpace deviceRGBColorSpace]];return static_cast<std::uint32_t>(std::lround(c.redComponent*255))|(static_cast<std::uint32_t>(std::lround(c.greenComponent*255))<<8)|(static_cast<std::uint32_t>(std::lround(c.blueComponent*255))<<16);}
GuiThemeMetrics metrics(const Part& part,std::uint32_t dpi,std::array<std::int32_t,2> size) {
    const CGFloat scale=static_cast<CGFloat>(dpi)/96;GuiThemeMetrics result;
    result.intrinsic={static_cast<std::int32_t>(std::ceil(part.size.width*scale)),static_cast<std::int32_t>(std::ceil(part.size.height*scale))};result.minimum=result.intrinsic;
    if(size[0]<=0 || size[1]<=0)size=result.intrinsic;
    const NSRect bounds=NSMakeRect(0,0,size[0]/scale,size[1]/scale);
    const auto content=part.cell?[part.cell drawingRectForBounds:bounds]:part.kind==Kind::Group || part.kind==Kind::Tip?NSInsetRect(bounds,3,3):bounds;
    result.content={static_cast<std::int32_t>(std::ceil(NSMinX(content)*scale)),static_cast<std::int32_t>(std::ceil(NSMinY(content)*scale)),static_cast<std::int32_t>(std::floor(NSMaxX(content)*scale)),static_cast<std::int32_t>(std::floor(NSMaxY(content)*scale))};
    result.text_color=color(part.disabled?[NSColor disabledControlTextColor]:[NSColor controlTextColor]);result.background_color=color([NSColor windowBackgroundColor]);result.transparent=part.transparent;result.fixed_size=part.fixed;return result;
}
}
std::optional<GuiThemeMetrics> native_theme_metrics(std::u16string_view cls,std::int32_t part,std::int32_t state,std::uint32_t dpi,std::array<std::int32_t,2> size) {
    @autoreleasepool {if(!dpi || dpi>768)return {};auto item=part_for(cls,part,state);if(!item)return {};__block GuiThemeMetrics result;[[NSAppearance appearanceNamed:NSAppearanceNameAqua] performAsCurrentDrawingAppearance:^{result=metrics(*item,dpi,size);}];return result;}
}
std::optional<GuiBitmap> native_theme_background(std::u16string_view cls,std::int32_t part,std::int32_t state,std::uint32_t dpi,std::array<std::int32_t,2> size) {
    @autoreleasepool {
        if(!dpi || dpi>768 || size[0]<=0 || size[1]<=0 || size[0]>4096 || size[1]>4096 || static_cast<std::uint64_t>(size[0])*static_cast<std::uint32_t>(size[1])>4194304)return {};
        auto item=part_for(cls,part,state);if(!item)return {};const CGFloat scale=static_cast<CGFloat>(dpi)/96;
        // AppKit 的 bitmap context 與 Metal AlphaBlend 共用預乘 alpha。
        auto* rep=[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:size[0] pixelsHigh:size[1] bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bitmapFormat:0 bytesPerRow:size[0]*4 bitsPerPixel:32];if(!rep)return {};
        auto* context=[NSGraphicsContext graphicsContextWithBitmapImageRep:rep];if(!context)return {};
        std::memset(rep.bitmapData,0,static_cast<std::size_t>(rep.bytesPerRow)*static_cast<std::size_t>(size[1]));[NSGraphicsContext saveGraphicsState];[NSGraphicsContext setCurrentContext:context];
        auto* transform=[NSAffineTransform transform];[transform translateXBy:0 yBy:size[1]];[transform scaleXBy:scale yBy:-scale];[transform concat];
        const NSRect bounds=NSMakeRect(0,0,size[0]/scale,size[1]/scale);NSRectClip(bounds);auto* view=[[MacWinThemeView alloc] initWithFrame:bounds];view.appearance=[NSAppearance appearanceNamed:NSAppearanceNameAqua];
        [view.appearance performAsCurrentDrawingAppearance:^{
            if(item->cell){if(item->kind==Kind::SliderTrack)[(NSSliderCell*)item->cell drawBarInside:bounds flipped:YES];else if(item->kind==Kind::SliderThumb)[(NSSliderCell*)item->cell drawKnob:bounds];else [item->cell drawWithFrame:bounds inView:view];}
            else if(item->view){item->view.appearance=view.appearance;item->view.frame=item->vertical?NSMakeRect(0,0,bounds.size.height,bounds.size.width):bounds;
                if(item->kind==Kind::ScrollSlot)[(NSScroller*)item->view drawKnobSlotInRect:bounds highlight:item->pressed];else if(item->kind==Kind::ScrollThumb)[(NSScroller*)item->view drawKnob];
                else {auto* cached=[item->view bitmapImageRepForCachingDisplayInRect:item->view.bounds];[item->view cacheDisplayInRect:item->view.bounds toBitmapImageRep:cached];auto* image=[[NSImage alloc] initWithSize:item->view.bounds.size];[image addRepresentation:cached];
                    [NSGraphicsContext saveGraphicsState];if(item->vertical){auto* rotation=[NSAffineTransform transform];[rotation translateXBy:0 yBy:bounds.size.height];[rotation rotateByDegrees:-90];[rotation concat];}[image drawInRect:item->view.bounds fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1 respectFlipped:YES hints:nil];[NSGraphicsContext restoreGraphicsState];}}
            else if(item->kind==Kind::Glyph){NSString* const symbols[]{@"chevron.up",@"chevron.right",@"chevron.down",@"chevron.left"};auto* image=[NSImage imageWithSystemSymbolName:symbols[item->direction] accessibilityDescription:nil];const auto width=std::min(item->size.width,bounds.size.width),height=std::min(item->size.height,bounds.size.height);const auto frame=NSMakeRect(0,(bounds.size.height-height)/2,width,height);[image drawInRect:frame fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:item->disabled?.45:1 respectFlipped:YES hints:nil];}
            else if(item->kind==Kind::Caret){[[NSColor textColor] setFill];NSRectFill(NSMakeRect(0,0,std::min<CGFloat>(1,bounds.size.width),bounds.size.height));}
            else if(item->kind==Kind::ListItem){if(item->focused || item->hot){[(item->focused?[NSColor selectedContentBackgroundColor]:[NSColor unemphasizedSelectedContentBackgroundColor]) setFill];NSRectFill(bounds);}}
            if(item->hot || item->focused){[[NSColor controlAccentColor] setStroke];auto* ring=[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(bounds,.5,.5) xRadius:4 yRadius:4];ring.lineWidth=1;[ring stroke];}
        }];
        [NSGraphicsContext restoreGraphicsState];GuiBitmap result{static_cast<std::uint32_t>(size[0]),static_cast<std::uint32_t>(size[1]),{}};result.pixels.resize(static_cast<std::size_t>(size[0])*static_cast<std::size_t>(size[1]));
        for(std::int32_t y=0;y<size[1];++y)for(std::int32_t x=0;x<size[0];++x){const auto offset=static_cast<std::size_t>(y)*static_cast<std::size_t>(rep.bytesPerRow)+static_cast<std::size_t>(x)*4;const auto* bytes=rep.bitmapData+offset;
            result.pixels[static_cast<std::size_t>(y)*static_cast<std::size_t>(size[0])+static_cast<std::size_t>(x)]=static_cast<std::uint32_t>(bytes[0])|(static_cast<std::uint32_t>(bytes[1])<<8)|(static_cast<std::uint32_t>(bytes[2])<<16)|(static_cast<std::uint32_t>(bytes[3])<<24);}
        result.raw_alpha=true;return result;
    }
}
}
