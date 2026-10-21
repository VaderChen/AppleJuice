#include "x64arm/native_winhelp.hpp"
#include "winhelp_viewer.hpp"
#include <map>
#include <set>

namespace x64arm {
namespace {
class CocoaWinHelp final : public NativeWinHelp {
    struct Entry {
        std::vector<std::uint8_t> bytes;
        std::shared_ptr<const winhelp::Document> document;
        X64ArmWinHelpViewer* viewer;
        std::set<std::uint64_t> clients;
        std::uint32_t codepage{};
    };
    std::map<std::string,Entry> entries_;
public:
    ~CocoaWinHelp()override{for(auto& [key,entry]:entries_){(void)key;[entry.viewer close];}}
    std::uint32_t execute(const NativeHelpRequest& request,void* anchor_view)override{
        if(![NSThread isMainThread])return 50;
        @autoreleasepool{try{
            if(request.command==2){
                for(auto it=entries_.begin();it!=entries_.end();){it->second.clients.erase(request.owner);if(it->second.clients.empty()){[it->second.viewer close];it=entries_.erase(it);}else ++it;}return 0;
            }
            switch(request.command){case 1:case 3:case 5:case 8:case 9:case 0x101:case 0x105:break;default:return 50;}
            if(request.file.empty())return 87;
            auto found=entries_.find(request.file);
            const bool fresh=found==entries_.end() || found->second.bytes!=request.bytes || found->second.codepage!=request.codepage;
            std::shared_ptr<const winhelp::Document> document=fresh?std::make_shared<winhelp::Document>(winhelp::read(request.bytes)):found->second.document;
            std::optional<winhelp::DocumentLocation> location;
            if(request.command==1 || request.command==5 || request.command==8)location=document->context(request.data);
            else if(request.command==3 || request.command==9)location=document->contents();
            if((request.command==1 || request.command==3 || request.command==5 || request.command==8 || request.command==9) && !location)return 2;
            if(location && !location->heading)return 50;
            NSView* anchor=(__bridge NSView*)anchor_view;
            if(request.command==8 && !anchor.window.visible)return 1400;
            if(fresh){
                auto viewer=[[X64ArmWinHelpViewer alloc] initWithDocument:document codepage:request.codepage];
                Entry next{request.bytes,document,viewer,{},request.codepage};
                if(found!=entries_.end()){next.clients=found->second.clients;[found->second.viewer close];found->second=std::move(next);}
                else found=entries_.emplace(request.file,std::move(next)).first;
            }
            auto& entry=found->second;auto viewer=entry.viewer;BOOL success=NO;
            if(request.command==5){viewer.contentsPage=location->page;success=YES;}
            else if(request.command==3){success=[viewer showPage:viewer.contentsPage==NSNotFound?location->page:viewer.contentsPage];}
            else if(request.command==1){success=[viewer showPage:location->page];}
            else if(request.command==8){success=[viewer showPopupPage:location->page anchorView:anchor rect:anchor.visibleRect];}
            else if(request.command==9){
                if(viewer.currentPage==NSNotFound)success=[viewer showPage:location->page];else{[viewer showWindow:nil];success=YES;}
            }else{
                NSString* keyword=[[NSString alloc] initWithCharacters:reinterpret_cast<const unichar*>(request.keyword.data()) length:request.keyword.size()];
                const auto count=[viewer searchKeyword:keyword partial:NO];
                if(count==1)success=[viewer openSearchResult:0];
                else if(count>1 || request.command==0x105){if(!count)[viewer searchKeyword:keyword partial:YES];[viewer.searchPanel makeKeyAndOrderFront:nil];success=YES;}
                else return 2;
            }
            if(!success)return 13;
            // CONTEXTPOPUP 不要求客戶呼叫 QUIT；但 QUIT 仍可清除該客戶留下的 popup。
            if(request.command!=8)entry.clients.insert(request.owner);
            return 0;
        }catch(const std::bad_alloc&){return 8;}catch(const std::exception&){return 13;}}
    }
};
}
std::shared_ptr<NativeWinHelp> make_native_winhelp(){return std::make_shared<CocoaWinHelp>();}
}
