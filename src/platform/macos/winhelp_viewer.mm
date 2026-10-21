#include "winhelp_viewer.hpp"
#include <algorithm>
#include <stdexcept>
@interface X64ArmWinHelpViewer ()
@property(nonatomic,strong) NSTextView* textView;
@property(nonatomic) NSUInteger currentPage;
@property(nonatomic,copy) NSString* lastError;
@property(nonatomic,strong) NSScrollView* scroll;
@property(nonatomic,strong) NSButton* backButton;
@property(nonatomic,strong) NSSearchField* searchField;
@property(nonatomic,copy) NSArray<NSDictionary*>* searchResults;
@property(nonatomic,strong) NSPanel* searchPanel;
@property(nonatomic,strong) NSTableView* resultTable;
@property(nonatomic,strong) NSTextField* resultStatus;
@property(nonatomic,strong) NSPopover* popup;
@property(nonatomic) NSUInteger popupPage;
- (BOOL)followLink:(id)value anchorView:(NSTextView*)view index:(NSUInteger)index;
- (BOOL)displayPage:(NSUInteger)page remember:(BOOL)remember;
@end
@implementation X64ArmWinHelpViewer
- (instancetype)initWithDocument:(std::shared_ptr<const x64arm::winhelp::Document>)document codepage:(std::uint32_t)codepage {
    if(![NSThread isMainThread] || !document)throw std::invalid_argument("WinHelp 視窗須在主執行緒建立有效文件");
    auto window=[[NSWindow alloc] initWithContentRect:NSMakeRect(100,100,720,560) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable|NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];window.releasedWhenClosed=NO;
    self=[super initWithWindow:window];if(!self)return nil;document_=std::move(document);codepage_=codepage;self.currentPage=NSNotFound;self.contentsPage=NSNotFound;self.lastError=@"";self.popupPage=NSNotFound;
    NSView* root=window.contentView;
    auto bar=[[NSView alloc] initWithFrame:NSMakeRect(0,516,720,44)];bar.autoresizingMask=NSViewWidthSizable|NSViewMinYMargin;[root addSubview:bar];
    self.backButton=[NSButton buttonWithTitle:@"返回" target:self action:@selector(backAction:)];self.backButton.frame=NSMakeRect(12,8,80,28);self.backButton.enabled=NO;[bar addSubview:self.backButton];
    auto contents=[NSButton buttonWithTitle:@"目錄" target:self action:@selector(contentsAction:)];contents.frame=NSMakeRect(100,8,80,28);[bar addSubview:contents];
    self.searchField=[[NSSearchField alloc] initWithFrame:NSMakeRect(400,10,300,26)];self.searchField.autoresizingMask=NSViewMinXMargin;self.searchField.placeholderString=@"搜尋關鍵字";self.searchField.target=self;self.searchField.action=@selector(searchAction:);[bar addSubview:self.searchField];self.searchResults=@[];
    self.searchPanel=[[NSPanel alloc] initWithContentRect:NSMakeRect(160,160,560,360) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];self.searchPanel.releasedWhenClosed=NO;self.searchPanel.title=@"關鍵字搜尋";
    self.resultStatus=[NSTextField labelWithString:@""];self.resultStatus.frame=NSMakeRect(12,328,536,24);self.resultStatus.autoresizingMask=NSViewWidthSizable|NSViewMinYMargin;[self.searchPanel.contentView addSubview:self.resultStatus];
    auto results=[[NSScrollView alloc] initWithFrame:NSMakeRect(0,0,560,320)];results.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;results.hasVerticalScroller=YES;self.resultTable=[[NSTableView alloc] initWithFrame:results.bounds];
    for(NSString* key in @[@"keyword",@"title"]){auto column=[[NSTableColumn alloc] initWithIdentifier:key];column.title=[key isEqualToString:@"keyword"]?@"關鍵字":@"主題";column.width=[key isEqualToString:@"keyword"]?210:320;[self.resultTable addTableColumn:column];}
    self.resultTable.delegate=self;self.resultTable.dataSource=self;self.resultTable.target=self;self.resultTable.doubleAction=@selector(resultAction:);results.documentView=self.resultTable;[self.searchPanel.contentView addSubview:results];
    self.scroll=[[NSScrollView alloc] initWithFrame:NSMakeRect(0,0,720,516)];self.scroll.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;self.scroll.hasVerticalScroller=YES;self.scroll.autohidesScrollers=YES;
    self.textView=[[NSTextView alloc] initWithFrame:NSMakeRect(0,0,720,516)];self.textView.editable=NO;self.textView.selectable=YES;self.textView.richText=YES;self.textView.delegate=self;self.textView.backgroundColor=NSColor.whiteColor;self.textView.textContainerInset=NSMakeSize(20,20);self.textView.verticallyResizable=YES;self.textView.horizontallyResizable=NO;self.textView.autoresizingMask=NSViewWidthSizable;self.textView.textContainer.widthTracksTextView=YES;
    self.textView.linkTextAttributes=@{NSForegroundColorAttributeName:[NSColor colorWithSRGBRed:0 green:0.45 blue:0 alpha:1],NSUnderlineStyleAttributeName:@(NSUnderlineStyleSingle)};
    self.scroll.documentView=self.textView;[root addSubview:self.scroll];return self;
}
- (BOOL)displayPage:(NSUInteger)page remember:(BOOL)remember {
    if(![NSThread isMainThread])return NO;
    try{
        auto text=x64arm::winhelp::attributed_page(*document_,page,codepage_);
        if(remember && self.currentPage!=NSNotFound && self.currentPage!=page)history_.push_back(self.currentPage);
        [self.popup close];self.currentPage=page;[self.textView.textStorage setAttributedString:text];
        self.window.title=text.length?[text.string componentsSeparatedByString:@"\n"].firstObject:@"說明";
        [self.textView.layoutManager ensureLayoutForTextContainer:self.textView.textContainer];const auto used=[self.textView.layoutManager usedRectForTextContainer:self.textView.textContainer];
        [self.textView setFrameSize:NSMakeSize(self.scroll.contentSize.width,std::max(self.scroll.contentSize.height,used.size.height+40))];
        [self.textView scrollRangeToVisible:NSMakeRange(0,0)];self.backButton.enabled=!history_.empty();self.lastError=@"";return YES;
    }catch(const std::exception& error){self.lastError=[NSString stringWithUTF8String:error.what()];return NO;}
}
- (BOOL)showPage:(NSUInteger)page {if(![self displayPage:page remember:YES])return NO;[self showWindow:nil];return YES;}
- (BOOL)goContents {if(self.contentsPage!=NSNotFound)return [self displayPage:self.contentsPage remember:YES];const auto target=document_->contents();return target?[self displayPage:target->page remember:YES]:NO;}
- (BOOL)goBack {if(history_.empty())return NO;const auto page=history_.back();if(![self displayPage:page remember:NO])return NO;history_.pop_back();self.backButton.enabled=!history_.empty();return YES;}
- (BOOL)followLink:(id)value {return [self followLink:value anchorView:self.textView index:0];}
- (BOOL)followLink:(id)value anchorView:(NSTextView*)view index:(NSUInteger)index {
    if(![value isKindOfClass:NSDictionary.class] || ![value[@"opcode"] isKindOfClass:NSNumber.class] || ![value[@"target"] isKindOfClass:NSNumber.class]){self.lastError=@"無效的說明連結";return NO;}
    const auto opcode=[value[@"opcode"] unsignedIntValue],target=[value[@"target"] unsignedIntValue];std::optional<x64arm::winhelp::DocumentLocation> location;
    if(opcode==0xe0 || opcode==0xe1)location=document_->locate(target);else if(opcode==0xe2 || opcode==0xe3 || opcode==0xe6 || opcode==0xe7)location=document_->hashed_context(target);else{self.lastError=@"此連結類型尚未接上視窗導覽";return NO;}
    if(!location){self.lastError=@"找不到說明主題";return NO;}
    // 非標頭的精確字元捲動仍需原生 range 對照，不假裝已抵達指定位置。
    if(!location->heading){self.lastError=@"主題內字元位置導覽尚未接上";return NO;}
    if(opcode==0xe0 || opcode==0xe2 || opcode==0xe6){NSRect anchor=view.visibleRect;
        if(index<view.string.length){const auto glyphs=[view.layoutManager glyphRangeForCharacterRange:NSMakeRange(index,1) actualCharacterRange:nullptr];anchor=[view.layoutManager boundingRectForGlyphRange:glyphs inTextContainer:view.textContainer];anchor.origin.x+=view.textContainerOrigin.x;anchor.origin.y+=view.textContainerOrigin.y;}
        return [self showPopupPage:location->page anchorView:view rect:anchor];
    }
    return [self displayPage:location->page remember:YES];
}
- (BOOL)showPopupPage:(NSUInteger)page anchorView:(NSView*)anchor rect:(NSRect)rect {
    if(![NSThread isMainThread] || !anchor.window.visible){self.lastError=@"彈出說明缺少可見的來源視窗";return NO;}
    try{
        auto text=x64arm::winhelp::attributed_page(*document_,page,codepage_);
        auto scroll=[[NSScrollView alloc] initWithFrame:NSMakeRect(0,0,480,320)];scroll.hasVerticalScroller=YES;scroll.autohidesScrollers=YES;
        auto view=[[NSTextView alloc] initWithFrame:scroll.bounds];view.editable=NO;view.selectable=YES;view.delegate=self;view.backgroundColor=NSColor.whiteColor;view.textContainerInset=NSMakeSize(12,12);view.verticallyResizable=YES;view.horizontallyResizable=NO;view.autoresizingMask=NSViewWidthSizable;view.textContainer.widthTracksTextView=YES;view.linkTextAttributes=self.textView.linkTextAttributes;[view.textStorage setAttributedString:text];scroll.documentView=view;
        [view.layoutManager ensureLayoutForTextContainer:view.textContainer];const auto height=[view.layoutManager usedRectForTextContainer:view.textContainer].size.height+24;[view setFrameSize:NSMakeSize(480,std::max(320.0,height))];
        auto controller=[[NSViewController alloc] init];controller.view=scroll;
        if(!self.popup){self.popup=[[NSPopover alloc] init];self.popup.behavior=NSPopoverBehaviorTransient;self.popup.animates=NO;}
        const BOOL shown=self.popup.shown;self.popup.contentViewController=controller;self.popup.contentSize=NSMakeSize(480,std::clamp(height,80.0,320.0));
        if(!shown)[self.popup showRelativeToRect:rect ofView:anchor preferredEdge:NSMaxYEdge];
        if(!self.popup.shown){self.lastError=@"無法顯示彈出說明";return NO;}self.popupPage=page;self.lastError=@"";return YES;
    }catch(const std::exception& error){self.lastError=[NSString stringWithUTF8String:error.what()];return NO;}
}
- (NSUInteger)searchKeyword:(NSString*)query partial:(BOOL)partial {
    if(![NSThread isMainThread] || !query)return 0;
    NSMutableArray* found=[NSMutableArray array];const auto encoding=CFStringConvertEncodingToNSStringEncoding(CFStringConvertWindowsCodepageToEncoding(codepage_));BOOL limited=NO;
    for(const auto& [key,targets]:document_->keywords()){
        NSString* keyword=[[NSString alloc] initWithBytes:key.data() length:key.size() encoding:encoding];if(!keyword)continue;
        const bool match=partial?(query.length==0 || [keyword rangeOfString:query options:NSCaseInsensitiveSearch|NSAnchoredSearch].location!=NSNotFound):[keyword compare:query options:NSCaseInsensitiveSearch]==NSOrderedSame;
        if(!match)continue;
        for(const auto offset:targets){if(found.count==10000){limited=YES;break;}const auto target=document_->locate(offset);NSString* title=@"尚未支援的主題或巨集";
            if(target){const auto& name=document_->pages().at(target->page).header.title;title=[[NSString alloc] initWithBytes:name.data() length:name.size() encoding:encoding]?:@"無法解碼的標題";}
            [found addObject:@{@"keyword":keyword,@"title":title,@"offset":@(offset)}];
        }if(limited)break;
    }
    self.searchResults=found;[self.resultTable reloadData];self.resultStatus.stringValue=limited?@"結果超過 10,000 筆，請縮小關鍵字範圍。":found.count?[NSString stringWithFormat:@"%lu 筆結果；按兩下開啟主題。",static_cast<unsigned long>(found.count)]:@"找不到符合的關鍵字。";return found.count;
}
- (BOOL)openSearchResult:(NSUInteger)row {
    if(row>=self.searchResults.count){self.lastError=@"無效的搜尋結果";return NO;}
    const auto target=document_->locate([self.searchResults[row][@"offset"] unsignedIntValue]);
    if(!target || !target->heading){self.lastError=@"此搜尋結果尚未接上導覽";return NO;}
    if(![self displayPage:target->page remember:YES])return NO;[self.searchPanel orderOut:nil];[self showWindow:nil];return YES;
}
- (NSInteger)numberOfRowsInTableView:(NSTableView*)table {(void)table;return static_cast<NSInteger>(self.searchResults.count);}
- (NSView*)tableView:(NSTableView*)table viewForTableColumn:(NSTableColumn*)column row:(NSInteger)row {
    (void)table;if(row<0 || static_cast<NSUInteger>(row)>=self.searchResults.count)return nil;auto text=[NSTextField labelWithString:self.searchResults[static_cast<NSUInteger>(row)][column.identifier]?:@""];text.lineBreakMode=NSLineBreakByTruncatingTail;return text;
}
- (void)searchAction:(id)sender {(void)sender;[self searchKeyword:self.searchField.stringValue partial:YES];[self.searchPanel makeKeyAndOrderFront:nil];}
- (void)resultAction:(id)sender {(void)sender;if(self.resultTable.clickedRow>=0)[self openSearchResult:static_cast<NSUInteger>(self.resultTable.clickedRow)];}
- (void)close {[self.popup close];[self.searchPanel close];[super close];}
- (BOOL)textView:(NSTextView*)view clickedOnLink:(id)link atIndex:(NSUInteger)index {[self followLink:link anchorView:view index:index];return YES;}
- (void)backAction:(id)sender {(void)sender;[self goBack];}
- (void)contentsAction:(id)sender {(void)sender;[self goContents];}
@end
