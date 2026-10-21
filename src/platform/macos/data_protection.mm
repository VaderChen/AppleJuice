#include "x64arm/data_protection.hpp"
#include "application.hpp"
#import <AppKit/AppKit.h>
namespace x64arm {
std::uint32_t native_data_protection_prompt(void* parent,std::u16string_view text,bool protect,bool password,std::u16string& output){
    if(![NSThread isMainThread])return 50;
    @autoreleasepool {
        prepare_native_application();NSAlert* alert=[[NSAlert alloc] init];alert.messageText=protect?@"保護資料":@"還原受保護資料";
        NSString* prompt=[[NSString alloc] initWithCharacters:reinterpret_cast<const unichar*>(text.data()) length:text.size()];alert.informativeText=prompt.length?prompt:@"請確認此程式的資料保護操作。";
        [alert addButtonWithTitle:@"確認"];[alert addButtonWithTitle:@"取消"];NSSecureTextField* field=nil;
        if(password){field=[[NSSecureTextField alloc] initWithFrame:NSMakeRect(0,0,320,24)];field.placeholderString=@"此資料的保護密碼";alert.accessoryView=field;[alert.window setInitialFirstResponder:field];}
        NSView* view=parent?(__bridge NSView*)parent:nil;NSWindow* owner=view.window;
        __block NSModalResponse response=NSModalResponseCancel;
        if(owner && owner.visible){[alert beginSheetModalForWindow:owner completionHandler:^(NSModalResponse value){response=value;[NSApp stopModalWithCode:value];}];response=[NSApp runModalForWindow:alert.window];}
        else response=[alert runModal];
        if(response!=NSAlertFirstButtonReturn){field.stringValue=@"";return 1223;}
        if(password){NSString* value=field.stringValue;if(value.length>32768){field.stringValue=@"";return 87;}output.resize(value.length);if(value.length)[value getCharacters:reinterpret_cast<unichar*>(output.data()) range:NSMakeRange(0,value.length)];field.stringValue=@"";}
        else output.clear();return 0;
    }
}
}
