#include "winhelp_text.hpp"
#include <stdexcept>
namespace x64arm::winhelp {
NSAttributedString* attributed_page(const Document& document,std::size_t number,std::uint32_t codepage){
    const auto encoding=CFStringConvertWindowsCodepageToEncoding(codepage);
    if(encoding==kCFStringEncodingInvalidId)throw std::invalid_argument("WinHelp 碼頁不支援");
    const auto decode=[&](const std::string& bytes)->NSString*{auto value=[[NSString alloc] initWithBytes:bytes.data() length:bytes.size() encoding:CFStringConvertEncodingToNSStringEncoding(encoding)];if(!value)throw std::invalid_argument("WinHelp 文字碼頁轉換失敗");return value;};
    const auto color=[](std::uint32_t rgb){return [NSColor colorWithSRGBRed:(rgb&255)/255.0 green:((rgb>>8)&255)/255.0 blue:((rgb>>16)&255)/255.0 alpha:1];};
    const auto& page=document.pages().at(number);NSMutableAttributedString* output=[[NSMutableAttributedString alloc] initWithString:@""];
    if(!page.header.title.empty())[output appendAttributedString:[[NSAttributedString alloc] initWithString:[decode(page.header.title) stringByAppendingString:@"\n"] attributes:@{NSFontAttributeName:[NSFont boldSystemFontOfSize:18],NSForegroundColorAttributeName:NSColor.blackColor}]];
    std::size_t font_index=0;NSDictionary* link=nil;
    const auto append=[&](NSString* text){
        const auto& font=document.fonts().at(font_index);NSFont* native=[NSFont fontWithName:decode(font.face) size:font.height_twips/20.0];if(!native)native=[NSFont systemFontOfSize:font.height_twips/20.0];
        NSFontTraitMask traits=0;if(font.attributes&1)traits|=NSBoldFontMask;if(font.attributes&2)traits|=NSItalicFontMask;
        if(traits)native=[[NSFontManager sharedFontManager] convertFont:native toHaveTrait:traits];
        NSMutableDictionary* attributes=[@{NSFontAttributeName:native,NSForegroundColorAttributeName:color(font.foreground)} mutableCopy];
        if(font.attributes&(4|16))attributes[NSUnderlineStyleAttributeName]=@((font.attributes&16)?NSUnderlineStyleDouble:NSUnderlineStyleSingle);
        if(font.attributes&8)attributes[NSStrikethroughStyleAttributeName]=@(NSUnderlineStyleSingle);
        if(link){attributes[NSLinkAttributeName]=link;attributes[NSForegroundColorAttributeName]=[NSColor colorWithSRGBRed:0 green:0.45 blue:0 alpha:1];}
        [output appendAttributedString:[[NSAttributedString alloc] initWithString:text attributes:attributes]];
    };
    for(const auto& part:page.parts){
        NSTextTable* table=nil;NSInteger row=-1,previous=-1;
        if(part.layout.table){table=[[NSTextTable alloc] init];table.numberOfColumns=static_cast<NSInteger>(part.layout.columns.size());table.layoutAlgorithm=NSTextTableFixedLayoutAlgorithm;}
        for(const auto& paragraph:part.layout.paragraphs){const auto start=output.length;NSMutableParagraphStyle* style=[[NSMutableParagraphStyle alloc] init];
            style.paragraphSpacingBefore=paragraph.metrics[0]/2.0;style.paragraphSpacing=paragraph.metrics[1]/2.0;
            if(paragraph.metrics[2]>0){style.minimumLineHeight=paragraph.metrics[2]/2.0;style.maximumLineHeight=paragraph.metrics[2]/2.0;}
            style.headIndent=paragraph.metrics[3]/2.0;style.tailIndent=-paragraph.metrics[4]/2.0;style.firstLineHeadIndent=style.headIndent+paragraph.metrics[5]/2.0;
            style.alignment=(paragraph.flags&0x800)?NSTextAlignmentCenter:(paragraph.flags&0x400)?NSTextAlignmentRight:NSTextAlignmentLeft;
            NSMutableArray* tabs=[NSMutableArray array];for(const auto& [position,type]:paragraph.tabs)[tabs addObject:[[NSTextTab alloc] initWithTextAlignment:type==1?NSTextAlignmentRight:type==2?NSTextAlignmentCenter:NSTextAlignmentLeft location:position/2.0 options:@{}]];style.tabStops=tabs;
            if(table){if(paragraph.column<=previous)++row;else if(row<0)row=0;previous=paragraph.column;auto block=[[NSTextTableBlock alloc] initWithTable:table startingRow:row rowSpan:1 startingColumn:paragraph.column columnSpan:1];const auto& column=part.layout.columns.at(static_cast<std::size_t>(paragraph.column));[block setValue:(part.layout.table_type&1)?column.second/2.0:column.second*100.0/32767 type:(part.layout.table_type&1)?NSTextBlockAbsoluteValueType:NSTextBlockPercentageValueType forDimension:NSTextBlockWidth];[block setWidth:column.first/2.0 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockPadding edge:NSMinXEdge];style.textBlocks=@[block];}
            for(const auto& run:paragraph.runs){switch(run.opcode){
            case 0:append(decode(run.text));break;
            case 0x80:font_index=run.argument;if(font_index>=document.fonts().size())throw std::invalid_argument("WinHelp 字型越界");break;
            case 0x81:append(@"\u2028");break;
            case 0x82:append(@"\n");break;
            case 0x83:append(@"\t");break;
            case 0x8b:append(@"\u00a0");break;
            case 0x8c:break;
            case 0x89:link=nil;break;
            case 0xe0:case 0xe1:case 0xe2:case 0xe3:case 0xe6:case 0xe7:case 0xea:case 0xeb:case 0xee:case 0xef:case 0xc8:case 0xcc:{
                NSMutableDictionary* target=[@{@"opcode":@(run.opcode),@"target":@(run.argument)} mutableCopy];
                if(run.link){target[@"file"]=decode(run.link->file);target[@"window"]=decode(run.link->window);if(run.link->window_number)target[@"windowNumber"]=@(*run.link->window_number);}
                if(!run.text.empty())target[@"macro"]=decode(run.text);link=target;break;
            }
            default:throw std::invalid_argument("WinHelp 格式命令尚未呈現");
            }}
            if(output.length==start || ![output.string hasSuffix:@"\n"])append(@"\n");
            [output addAttribute:NSParagraphStyleAttributeName value:style range:NSMakeRange(start,output.length-start)];
        }
    }
    return output;
}
}
