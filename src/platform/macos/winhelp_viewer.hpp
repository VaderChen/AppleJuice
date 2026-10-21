#pragma once
#include "winhelp_text.hpp"
#include <memory>
@interface X64ArmWinHelpViewer : NSWindowController<NSTextViewDelegate,NSTableViewDataSource,NSTableViewDelegate> {
@private
    std::shared_ptr<const x64arm::winhelp::Document> document_;
    std::uint32_t codepage_;
    std::vector<std::size_t> history_;
}
@property(nonatomic,readonly) NSTextView* textView;
@property(nonatomic,readonly) NSUInteger currentPage;
@property(nonatomic) NSUInteger contentsPage;
@property(nonatomic,readonly) NSString* lastError;
@property(nonatomic,readonly) NSArray<NSDictionary*>* searchResults;
@property(nonatomic,readonly) NSPanel* searchPanel;
@property(nonatomic,readonly) NSPopover* popup;
@property(nonatomic,readonly) NSUInteger popupPage;
- (instancetype)initWithDocument:(std::shared_ptr<const x64arm::winhelp::Document>)document codepage:(std::uint32_t)codepage;
- (BOOL)showPage:(NSUInteger)page;
- (BOOL)followLink:(id)link;
- (NSUInteger)searchKeyword:(NSString*)query partial:(BOOL)partial;
- (BOOL)openSearchResult:(NSUInteger)row;
- (BOOL)showPopupPage:(NSUInteger)page anchorView:(NSView*)anchor rect:(NSRect)rect;
- (BOOL)goBack;
- (BOOL)goContents;
@end
