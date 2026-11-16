#pragma once
#import <AppKit/AppKit.h>
#include <cstdint>
// Windows OCR_*／IDC_* 系統游標對應的 AppKit 游標；AppKit 沒有的形狀以 SF Symbols 建立（熱點置中）。
// gui.mm 設定原生游標與 icons.mm 產生游標點陣圖共用這份對應。
inline NSCursor* x64arm_system_cursor(std::uint32_t id){
    NSString* symbol=nil;
    switch(id){
    case 32512:case 32641:return NSCursor.arrowCursor;             // IDC_ARROW、IDC_ICON（已淘汰，同箭頭）
    case 32513:return NSCursor.IBeamCursor;
    case 32515:return NSCursor.crosshairCursor;
    case 32644:return NSCursor.resizeLeftRightCursor;               // IDC_SIZEWE
    case 32645:return NSCursor.resizeUpDownCursor;                  // IDC_SIZENS
    case 32648:return NSCursor.operationNotAllowedCursor;           // IDC_NO
    case 32649:return NSCursor.pointingHandCursor;                  // IDC_HAND
    case 32514:case 32650:symbol=@"hourglass";break;                // IDC_WAIT、IDC_APPSTARTING
    case 32516:symbol=@"arrow.up";break;                            // IDC_UPARROW
    case 32640:case 32646:symbol=@"arrow.up.and.down.and.arrow.left.and.right";break; // IDC_SIZE、IDC_SIZEALL
    case 32642:symbol=@"arrow.up.left.and.arrow.down.right";break;  // IDC_SIZENWSE
    case 32643:symbol=@"arrow.up.right.and.arrow.down.left";break;  // IDC_SIZENESW
    case 32651:symbol=@"questionmark";break;                        // IDC_HELP
    case 32671:symbol=@"mappin";break;                              // IDC_PIN
    case 32672:symbol=@"person";break;                              // IDC_PERSON
    default:return nil;
    }
    NSImage* image=[[NSImage imageWithSystemSymbolName:symbol accessibilityDescription:nil] copy];if(!image)return nil;
    image.size=NSMakeSize(24,24);return [[NSCursor alloc] initWithImage:image hotSpot:NSMakePoint(12,12)];
}
