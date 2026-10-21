#include "x64arm/windows_icon.hpp"
#import <AppKit/AppKit.h>
#import <ImageIO/ImageIO.h>

namespace x64arm {
namespace {
std::optional<GuiIcon> pixels(CGImageRef image,std::uint32_t width,std::uint32_t height){
    if(!image || !width || !height || width>4096 || height>4096 || std::uint64_t{width}*height>16U*1024U*1024U)return {};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(width)*height*4);
    const auto color=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    const auto context=CGBitmapContextCreate(bytes.data(),width,height,8,static_cast<std::size_t>(width)*4,color,static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast)|static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Big));CGColorSpaceRelease(color);
    if(!context)return {};CGContextSetInterpolationQuality(context,kCGInterpolationHigh);CGContextDrawImage(context,CGRectMake(0,0,width,height),image);CGContextRelease(context);
    GuiIcon icon;icon.width=width;icon.height=height;icon.alpha=true;icon.rgba.resize(static_cast<std::size_t>(width)*height);
    for(std::size_t i=0;i<icon.rgba.size();++i){const auto alpha=bytes[i*4+3];std::uint32_t pixel=std::uint32_t{alpha}<<24;
        for(unsigned c=0;c<3;++c){const auto value=alpha?std::min(255U,(std::uint32_t{bytes[i*4+c]}*255U+alpha/2U)/alpha):0U;pixel|=value<<(c*8);}icon.rgba[i]=pixel;}
    finish_icon(icon);return icon;
}
NSString* symbol(std::uint32_t id){
    switch(id){
        case 0:return @"doc";case 1:return @"doc.text";case 2:case 82:return @"app";
        case 3:case 75:case 76:return @"folder";case 4:return @"folder.fill";case 57:return @"folder.badge.plus";
        case 5:case 6:case 7:case 8:case 12:case 58:case 140:return @"externaldrive";
        case 9:case 10:case 51:return @"externaldrive.connected.to.line.below";
        case 13:case 104:return @"globe";case 15:return @"server.rack";case 17:return @"network";
        case 16:case 50:case 54:return @"printer";case 52:case 53:return @"faxmachine";
        case 22:return @"magnifyingglass";case 23:return @"questionmark.circle";
        case 28:return @"square.and.arrow.up";case 29:return @"arrowshape.turn.up.right.fill";case 30:return @"clock";
        case 31:return @"trash";case 32:return @"trash.fill";case 47:return @"lock.fill";
        case 49:case 55:return @"square.stack.3d.up";
        case 71:return @"music.note";case 72:return @"photo";case 73:return @"film";case 74:return @"doc.on.doc";
        case 77:return @"shield.lefthalf.filled";case 78:return @"exclamationmark.triangle.fill";
        case 79:return @"info.circle.fill";case 80:return @"xmark.octagon.fill";case 81:return @"key.fill";
        case 83:return @"pencil";case 84:return @"xmark";case 94:return @"desktopcomputer";case 95:return @"laptopcomputer";
        case 96:return @"person.2.fill";case 97:case 98:return @"sdcard";case 99:return @"iphone";
        case 100:return @"camera";case 101:return @"video";case 102:return @"ipod";
        case 103:return @"point.3.connected.trianglepath.dotted";case 105:return @"doc.zipper";case 106:return @"gearshape";
        default:return @"opticaldisc";
    }
}
}
std::optional<GuiIcon> native_png_icon(std::span<const std::uint8_t> bytes){
    @autoreleasepool {
        if(bytes.size()<24)return {};const auto data=CFDataCreate(kCFAllocatorDefault,bytes.data(),static_cast<CFIndex>(bytes.size()));if(!data)return {};
        const auto source=CGImageSourceCreateWithData(data,nullptr);CFRelease(data);if(!source)return {};
        const auto properties=CGImageSourceCopyPropertiesAtIndex(source,0,nullptr);
        std::int64_t width=0,height=0;if(properties){const auto w=static_cast<CFNumberRef>(CFDictionaryGetValue(properties,kCGImagePropertyPixelWidth)),h=static_cast<CFNumberRef>(CFDictionaryGetValue(properties,kCGImagePropertyPixelHeight));if(w)CFNumberGetValue(w,kCFNumberSInt64Type,&width);if(h)CFNumberGetValue(h,kCFNumberSInt64Type,&height);CFRelease(properties);}
        if(width<=0 || height<=0 || width>4096 || height>4096 || width*height>16*1024*1024){CFRelease(source);return {};}
        const auto image=CGImageSourceCreateImageAtIndex(source,0,nullptr);CFRelease(source);auto result=pixels(image,static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height));if(image)CGImageRelease(image);return result;
    }
}
std::optional<GuiIcon> native_stock_icon(std::uint32_t id,std::uint32_t size){
    @autoreleasepool {
        if(!stock_icon_valid(id) || !size || size>256)return {};
        NSImage* image=nil;
        if(id==3 || id==4 || id==57 || id==75 || id==76)image=[[NSImage imageNamed:NSImageNameFolder] copy];
        else if(id==94 || id==95 || id==8)image=[[NSImage imageNamed:NSImageNameComputer] copy];
        else if(id==13 || id==17 || id==104)image=[[NSImage imageNamed:NSImageNameNetwork] copy];
        else if(id==2 || id==82)image=[[NSImage imageNamed:NSImageNameApplicationIcon] copy];
        if(!image)image=[[NSImage imageWithSystemSymbolName:symbol(id) accessibilityDescription:nil] copy];
        if(!image)return {};image.size=NSMakeSize(size,size);
        NSBitmapImageRep* rep=[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:size pixelsHigh:size bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSCalibratedRGBColorSpace bytesPerRow:size*4 bitsPerPixel:32];
        if(!rep)return {};[NSGraphicsContext saveGraphicsState];[NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithBitmapImageRep:rep]];
        [image drawInRect:NSMakeRect(0,0,size,size) fromRect:NSZeroRect operation:NSCompositingOperationCopy fraction:1 respectFlipped:NO hints:nil];
        [NSGraphicsContext restoreGraphicsState];auto result=pixels(rep.CGImage,size,size);if(result)result->system=0x20000U+id;return result;
    }
}
std::optional<GuiIcon> native_system_cursor(std::uint32_t id){
    @autoreleasepool {
        NSCursor* cursor=nil;
        switch(id){
            case 32512:case 32514:case 32516:case 32650:cursor=NSCursor.arrowCursor;break;
            case 32513:cursor=NSCursor.IBeamCursor;break;case 32515:cursor=NSCursor.crosshairCursor;break;
            case 32640:case 32642:case 32643:cursor=NSCursor.resizeLeftRightCursor;break;
            case 32641:case 32644:cursor=NSCursor.resizeUpDownCursor;break;
            case 32645:cursor=NSCursor.closedHandCursor;break;case 32648:cursor=NSCursor.operationNotAllowedCursor;break;
            case 32649:cursor=NSCursor.pointingHandCursor;break;case 32651:cursor=NSCursor.contextualMenuCursor;break;
            default:return {};
        }
        NSRect rect={NSZeroPoint,cursor.image.size};const auto cg=[cursor.image CGImageForProposedRect:&rect context:nil hints:nil];
        auto icon=pixels(cg,static_cast<std::uint32_t>(rect.size.width),static_cast<std::uint32_t>(rect.size.height));
        if(!icon)return {};icon->is_icon=false;icon->system=id;icon->hotspot_x=static_cast<std::uint32_t>(cursor.hotSpot.x);icon->hotspot_y=static_cast<std::uint32_t>(cursor.hotSpot.y);finish_icon(*icon);return icon;
    }
}
}
