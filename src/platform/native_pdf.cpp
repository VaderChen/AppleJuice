#include "x64arm/native_pdf.hpp"
#include <stdexcept>
#if defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#endif
namespace x64arm {
NativeSpoolDocument native_pdf_raw_document(std::span<const std::uint8_t> bytes){
#if defined(__APPLE__)
    const auto release=[](const void* value){if(value)CFRelease(value);};
    std::unique_ptr<const void,decltype(release)> data(CFDataCreate(nullptr,bytes.data(),static_cast<CFIndex>(bytes.size())),release);
    if(!data)throw std::bad_alloc();
    std::unique_ptr<const void,decltype(release)> provider(CGDataProviderCreateWithCFData(static_cast<CFDataRef>(data.get())),release);
    if(!provider)throw std::bad_alloc();
    std::unique_ptr<const void,decltype(release)> document(CGPDFDocumentCreateWithProvider(static_cast<CGDataProviderRef>(const_cast<void*>(provider.get()))),release);
    if(!document || !CGPDFDocumentIsUnlocked(static_cast<CGPDFDocumentRef>(const_cast<void*>(document.get()))))throw std::invalid_argument("RAW 資料不是可列印的 PDF");
    const auto pages=CGPDFDocumentGetNumberOfPages(static_cast<CGPDFDocumentRef>(const_cast<void*>(document.get())));
    if(!pages || pages>1024)throw std::length_error("RAW PDF 頁數不合法或超過上限");
    return {{bytes.begin(),bytes.end()},static_cast<unsigned>(pages)};
#else
    (void)bytes;throw std::domain_error("本平台沒有 PDF 列印後端");
#endif
}

NativeSpoolDocument native_pdf_text_document(std::span<const std::u16string> text,std::u16string_view title,unsigned width,unsigned height,unsigned copies,bool collate){
    if(!copies || copies>99 || width<128 || height<128 || width>8192 || height>8192)throw std::invalid_argument("TEXT 列印尺寸或份數不合法");
#if defined(__APPLE__)
    const auto release=[](const void* value){if(value)CFRelease(value);};
    using Object=std::unique_ptr<const void,decltype(release)>;
    Object font(CTFontCreateWithName(CFSTR("Menlo"),10,nullptr),release);if(!font)throw std::bad_alloc();
    std::vector<Object> frames;
    const CGRect media=CGRectMake(0,0,width*0.75,height*0.75),content=CGRectInset(media,36,36);
    Object path(CGPathCreateWithRect(content,nullptr),release);if(!path)throw std::bad_alloc();
    for(const auto& value:text){
        Object string(CFStringCreateWithCharacters(nullptr,reinterpret_cast<const UniChar*>(value.data()),static_cast<CFIndex>(value.size())),release);
        Object attributes(CFAttributedStringCreateMutable(nullptr,0),release);if(!string || !attributes)throw std::bad_alloc();
        auto attributed=const_cast<CFMutableAttributedStringRef>(static_cast<CFAttributedStringRef>(attributes.get()));
        CFAttributedStringReplaceString(attributed,CFRangeMake(0,0),static_cast<CFStringRef>(string.get()));
        CFAttributedStringSetAttribute(attributed,CFRangeMake(0,static_cast<CFIndex>(value.size())),kCTFontAttributeName,font.get());
        Object setter(CTFramesetterCreateWithAttributedString(attributed),release);if(!setter)throw std::bad_alloc();
        CFIndex position=0;
        do{
            Object frame(CTFramesetterCreateFrame(static_cast<CTFramesetterRef>(setter.get()),CFRangeMake(position,0),static_cast<CGPathRef>(path.get()),nullptr),release);
            if(!frame)throw std::bad_alloc();
            const auto visible=CTFrameGetVisibleStringRange(static_cast<CTFrameRef>(frame.get()));
            if(!value.empty() && visible.length<=0)throw std::invalid_argument("TEXT 列印無法配置文字頁面");
            position+=visible.length;frames.push_back(std::move(frame));
            if(frames.size()>1024/copies)throw std::length_error("TEXT 列印總頁數超過上限");
        }while(position<static_cast<CFIndex>(value.size()));
    }
    if(frames.empty())throw std::invalid_argument("TEXT 列印沒有文件");
    Object data(CFDataCreateMutable(nullptr,0),release);if(!data)throw std::bad_alloc();
    auto bytes=const_cast<CFMutableDataRef>(static_cast<CFDataRef>(data.get()));
    Object consumer(CGDataConsumerCreateWithCFData(bytes),release);
    Object name(CFStringCreateWithCharacters(nullptr,reinterpret_cast<const UniChar*>(title.data()),static_cast<CFIndex>(title.size())),release);
    const void* keys[]{kCGPDFContextTitle};const void* values[]{name.get()};
    Object metadata(CFDictionaryCreate(nullptr,keys,values,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks),release);
    if(!consumer || !name || !metadata)throw std::bad_alloc();
    auto context=CGPDFContextCreate(static_cast<CGDataConsumerRef>(const_cast<void*>(consumer.get())),&media,static_cast<CFDictionaryRef>(metadata.get()));
    if(!context)throw std::bad_alloc();
    try{
        for(std::size_t index=0;index<frames.size()*copies;++index){
            const auto page=collate?index%frames.size():index/copies;
            CGPDFContextBeginPage(context,nullptr);CGContextSetRGBFillColor(context,0,0,0,1);CGContextSetTextMatrix(context,CGAffineTransformIdentity);
            CTFrameDraw(static_cast<CTFrameRef>(frames[page].get()),context);CGPDFContextEndPage(context);
        }
        CGPDFContextClose(context);CGContextRelease(context);
    }catch(...){CGPDFContextClose(context);CGContextRelease(context);throw;}
    const auto size=CFDataGetLength(bytes);if(size>64*1024*1024)throw std::length_error("PDF 輸出超過 64 MiB 上限");
    const auto* begin=CFDataGetBytePtr(bytes);return {{begin,begin+size},static_cast<unsigned>(frames.size()*copies)};
#else
    (void)text;(void)title;(void)collate;throw std::domain_error("本平台沒有 PDF 列印後端");
#endif
}

std::vector<std::uint8_t> native_pdf_document(std::span<const GuiBitmap> pages,std::u16string_view title,unsigned copies,bool collate){
    if(!copies || copies>99 || pages.size()>1024/copies)throw std::length_error("PDF 份數或總頁數超過上限");
#if defined(__APPLE__)
    const auto release=[](const void* value){if(value)CFRelease(value);};
    std::unique_ptr<const void,decltype(release)> data(CFDataCreateMutable(nullptr,0),release);if(!data)throw std::bad_alloc();
    const auto bytes=const_cast<CFMutableDataRef>(static_cast<CFDataRef>(data.get()));
    std::unique_ptr<const void,decltype(release)> consumer(CGDataConsumerCreateWithCFData(bytes),release);
    std::unique_ptr<const void,decltype(release)> name(CFStringCreateWithCharacters(nullptr,reinterpret_cast<const UniChar*>(title.data()),static_cast<CFIndex>(title.size())),release);
    const void* keys[]{kCGPDFContextTitle};const void* values[]{name.get()};std::unique_ptr<const void,decltype(release)> metadata(CFDictionaryCreate(nullptr,keys,values,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks),release);
    if(!consumer || !name || !metadata)throw std::bad_alloc();
    auto context=CGPDFContextCreate(static_cast<CGDataConsumerRef>(const_cast<void*>(consumer.get())),nullptr,static_cast<CFDictionaryRef>(metadata.get()));if(!context)throw std::bad_alloc();
    try{for(std::size_t page_index=0;page_index<pages.size()*copies;++page_index){const auto& page=pages[collate?page_index%pages.size():page_index/copies];if(!page.width || !page.height || page.pixels.size()!=static_cast<std::size_t>(page.width)*page.height)throw std::invalid_argument("PDF 頁面像素不完整");
            const CGRect rect=CGRectMake(0,0,page.width*0.75,page.height*0.75);
            std::unique_ptr<const void,decltype(release)> box(CFDataCreate(nullptr,reinterpret_cast<const UInt8*>(&rect),static_cast<CFIndex>(sizeof rect)),release);
            const void* page_keys[]{kCGPDFContextMediaBox};const void* page_values[]{box.get()};std::unique_ptr<const void,decltype(release)> info(CFDictionaryCreate(nullptr,page_keys,page_values,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks),release);
            CGPDFContextBeginPage(context,static_cast<CFDictionaryRef>(info.get()));
            std::vector<std::uint8_t> rgb(page.pixels.size()*4);for(std::size_t i=0;i<page.pixels.size();++i){for(unsigned channel=0;channel<3;++channel)rgb[i*4+channel]=static_cast<std::uint8_t>(page.pixels[i]>>(channel*8));rgb[i*4+3]=255;}
            std::unique_ptr<const void,decltype(release)> colors(CGColorSpaceCreateWithName(kCGColorSpaceSRGB),release),raw(CFDataCreate(nullptr,rgb.data(),static_cast<CFIndex>(rgb.size())),release);
            std::unique_ptr<const void,decltype(release)> provider(CGDataProviderCreateWithCFData(static_cast<CFDataRef>(raw.get())),release);
            std::unique_ptr<const void,decltype(release)> image(CGImageCreate(page.width,page.height,8,32,page.width*4,static_cast<CGColorSpaceRef>(const_cast<void*>(colors.get())),kCGImageAlphaNoneSkipLast,static_cast<CGDataProviderRef>(const_cast<void*>(provider.get())),nullptr,false,kCGRenderingIntentDefault),release);
            if(!image)throw std::bad_alloc();CGContextDrawImage(context,rect,static_cast<CGImageRef>(const_cast<void*>(image.get())));CGPDFContextEndPage(context);
        }CGPDFContextClose(context);CGContextRelease(context);
    }catch(...){CGPDFContextClose(context);CGContextRelease(context);throw;}
    const auto size=CFDataGetLength(bytes);if(size>64*1024*1024)throw std::length_error("PDF 輸出超過 64 MiB 上限");const auto* begin=CFDataGetBytePtr(bytes);return {begin,begin+size};
#else
    (void)pages;(void)title;(void)collate;throw std::domain_error("本平台沒有 PDF 列印後端");
#endif
}
}
