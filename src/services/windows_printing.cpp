#include "x64arm/windows_services.hpp"
#include "x64arm/native_pdf.hpp"
#include "x64arm/windows_printing.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <unistd.h>
#include <ctime>
namespace x64arm {
struct PrintingState {
    struct Device : PrinterSettings {
        bool information{};
        std::uint64_t bitmap{},abort_proc{};unsigned job{},pending{};bool active{},page{},cancelled{};
        bool legacy{},implicit_page{},banding{};
        std::uint32_t access{8};std::u16string datatype{u"RAW"},default_datatype{u"RAW"};
        std::vector<std::uint8_t> spool;std::vector<std::size_t> page_ends;bool explicit_pages{};
        std::string output;std::u16string title;std::uint64_t submitted{};std::vector<GuiBitmap> pages;std::uint64_t bytes{};
    };
    struct Notice {std::uint64_t printer{};unsigned filter{},changes{};};
    std::map<std::uint64_t,Device> devices,handles;std::map<std::uint64_t,Notice> notices;unsigned next_job{1};
};
unsigned read_printer_settings(GuestMemory& memory,std::uint64_t address,bool wide,PrinterSettings& device){
    if(!address)return 0;const auto size_offset=wide?68U:36U,field_offset=size_offset+4;
    const auto word=[&](unsigned offset){return static_cast<unsigned>(memory.load(GuestAddress{address+offset},16));};
    const auto size=word(size_offset);if(size<field_offset+32)return 87;if(word(size_offset+2))return 50;memory.check(GuestAddress{address},size,Access::Read);
    const auto fields=static_cast<unsigned>(memory.load(GuestAddress{address+field_offset},32));if(fields&~0xff1fU)return 50;
    const auto body=field_offset+4;unsigned paper=(device.width==794 || device.height==794)?9:1,orientation=device.width>device.height?2:1;
    if(fields&1)orientation=word(body);if(fields&2)paper=word(body+2);if((orientation!=1 && orientation!=2) || (paper!=1 && paper!=9))return 87;
    if(fields&12)return 50;if((fields&16) && word(body+8)!=100)return 50;if(fields&0x100){device.copies=word(body+10);if(!device.copies || device.copies>99)return 87;}
    if((fields&0x200) && word(body+12)!=7)return 50;if((fields&0x400) && word(body+14)!=96)return 50;if((fields&0x1000) && word(body+18)!=1)return 50;
    if((fields&0x2000) && word(body+20)!=96)return 50;if((fields&0x4000) && word(body+22)!=1)return 50;if(fields&0x8000){const auto value=word(body+24);if(value>1)return 87;device.collate=value!=0;}
    if(fields&0x800){const auto color=word(body+16);if(color!=1 && color!=2)return 87;device.monochrome=color==1;}
    device.width=paper==1?816:794;device.height=paper==1?1056:1123;if(orientation==2)std::swap(device.width,device.height);return 0;
}
bool WindowsServices::is_printer_dc(std::uint64_t dc)const{return printing_ && printing_->devices.contains(dc);}
bool WindowsServices::printer_drawing_enabled(std::uint64_t dc)const{if(!is_printer_dc(dc))return true;const auto& device=printing_->devices.at(dc);return device.active && device.page && !device.cancelled && !device.information;}
std::uint32_t WindowsServices::begin_printer_page(GuestMemory& memory,std::uint64_t dc){
    auto& device=printing_->devices.at(dc);device.page=true;
    try {
        auto surface=raster_surface(memory,dc);
        if(!surface){device.page=false;return 50;}
        auto& bitmap=gui_.bitmaps.at(device.bitmap);
        std::fill(bitmap.pixels.begin(),bitmap.pixels.end(),0xffffffffU);surface->upload(bitmap.pixels);
        device.implicit_page=false;printing_changed(0x200);return 0;
    }catch(...){device.page=false;throw;}
}
bool WindowsServices::prepare_legacy_printer_page(GuestMemory& memory,std::uint64_t dc){
    if(!is_printer_dc(dc))return true;
    auto& device=printing_->devices.at(dc);
    if(!device.legacy || !device.implicit_page || device.pending || !device.active || device.cancelled || device.information){last_error_=device.cancelled?995:1906;return false;}
    try {
        const auto error=begin_printer_page(memory,dc);
        if(error){last_error_=error;return false;}return true;
    }catch(const std::bad_alloc&){last_error_=8;return false;}
}
std::uint64_t WindowsServices::create_printer_dc(GuestMemory& memory,std::uint64_t mode,bool wide,bool information){
    if(!runtime_.network.external_io || !native_gui_){last_error_=50;return 0;}if(!printing_)printing_=std::make_shared<PrintingState>();if(printing_->devices.size()>=64){last_error_=8;return 0;}
    PrintingState::Device device;device.information=information;if(const auto error=read_printer_settings(memory,mode,wide,device)){last_error_=error;return 0;}
    const auto dc=gui_.next_object++,bitmap=gui_.next_object++;device.bitmap=bitmap;
    gui_.bitmaps.emplace(bitmap,GuiBitmap{device.width,device.height,std::vector<std::uint32_t>(static_cast<std::size_t>(device.width)*device.height,0xffffffffU)});
    try{GuiDc context;context.bitmap=bitmap;gui_.contexts.emplace(dc,std::move(context));printing_->devices.emplace(dc,std::move(device));}catch(...){gui_.bitmaps.erase(bitmap);gui_.contexts.erase(dc);throw;}return dc;
}
void WindowsServices::delete_printer_dc(std::uint64_t dc){
    if(!is_printer_dc(dc))return;if(printing_->devices.at(dc).active)printing_changed(0x400);const auto bitmap=printing_->devices.at(dc).bitmap;rasters_.erase(bitmap);raster_shared_.erase(bitmap);gui_.bitmaps.erase(bitmap);printing_->devices.erase(dc);
}
std::optional<std::uint32_t> WindowsServices::printer_device_cap(std::uint64_t dc,unsigned cap)const{
    if(!is_printer_dc(dc))return {};const auto& device=printing_->devices.at(dc);
    switch(cap){case 0:return 0x400;case 2:return 2;case 4:return device.width*254/960;case 6:return device.height*254/960;
        case 8:case 110:return device.width;case 10:case 111:return device.height;case 12:return 32;case 14:return 1;case 24:return UINT32_MAX;
        case 36:return 3;case 38:return 0x2a01;case 88:case 90:return 96;case 112:case 113:return 0;case 114:case 115:return 0;default:return 0;}
}
std::span<const WindowsServiceDescriptor> printing_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"gdi32.dll","StartDocA",2},{"gdi32.dll","StartDocW",2},{"gdi32.dll","StartPage",1},{"gdi32.dll","EndPage",1},{"gdi32.dll","EndDoc",1},{"gdi32.dll","AbortDoc",1},{"gdi32.dll","CancelDC",1},{"gdi32.dll","SetAbortProc",2},
        {"winspool.drv","GetDefaultPrinterA",2},{"winspool.drv","GetDefaultPrinterW",2},
        {"winspool.drv","EnumPrintersA",7},{"winspool.drv","EnumPrintersW",7},{"winspool.drv","OpenPrinterA",3},{"winspool.drv","OpenPrinterW",3},{"winspool.drv","ClosePrinter",1},
        {"winspool.drv","GetPrinterA",5},{"winspool.drv","GetPrinterW",5},{"winspool.drv","GetPrinterDriverA",6},{"winspool.drv","GetPrinterDriverW",6},
        {"winspool.drv","DocumentPropertiesA",6},{"winspool.drv","DocumentPropertiesW",6},{"winspool.drv","DeviceCapabilitiesA",5},{"winspool.drv","DeviceCapabilitiesW",5},
        {"winspool.drv","GetJobA",6},{"winspool.drv","GetJobW",6},{"winspool.drv","FindFirstPrinterChangeNotification",4},{"winspool.drv","FindNextPrinterChangeNotification",4},{"winspool.drv","FindClosePrinterChangeNotification",1},
        {"winspool.drv","StartDocPrinterA",3},{"winspool.drv","StartDocPrinterW",3},{"winspool.drv","StartPagePrinter",1},{"winspool.drv","WritePrinter",4},{"winspool.drv","EndPagePrinter",1},{"winspool.drv","EndDocPrinter",1},{"winspool.drv","AbortPrinter",1},
        {"winspool.drv","EnumPrintProcessorDatatypesA",7},{"winspool.drv","EnumPrintProcessorDatatypesW",7},
        {"spoolss.dll","StartDocPrinterA",3},{"spoolss.dll","StartDocPrinterW",3},{"spoolss.dll","StartPagePrinter",1},{"spoolss.dll","WritePrinter",4},{"spoolss.dll","EndPagePrinter",1},{"spoolss.dll","EndDocPrinter",1},{"spoolss.dll","AbortPrinter",1},
        {"gdi32.dll","ResetDCA",2},{"gdi32.dll","ResetDCW",2},{"gdi32.dll","ExtEscape",6}
    };return table;
}
std::span<const WindowsServiceDescriptor> printing_escape_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{{"gdi32.dll","Escape",5}};return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_printing(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(service.family=="winspool.drv" || service.family=="spoolss.dll")return invoke_spooler(state,memory,service);
    const WindowsCall arg(state,memory,service);auto name=service.symbol;
    const bool legacy=name=="Escape";const auto escape=legacy?static_cast<std::uint32_t>(arg(1)):0U;
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto failure=[&](unsigned error){last_error_=error;return finish(name=="CancelDC" || name.starts_with("ResetDC")?0:UINT32_MAX);};
    if(name=="ExtEscape"){
        if(!gui_.contexts.contains(arg(0)))return failure(6);
        if(static_cast<std::int32_t>(arg(2))<0 || static_cast<std::int32_t>(arg(4))<0)return failure(87);
        if(static_cast<std::uint32_t>(arg(1))==8){if(static_cast<std::uint32_t>(arg(2))<4 || !arg(3))return failure(87);const auto code=memory.load(GuestAddress{arg(3)},32);return finish(code==8?1:0);}
        // 原生 PDF driver 不接受 PostScript / JPEG / PNG passthrough；由 GDI raster API 產生頁面。
        return finish(0);
    }
    if(legacy){
        if(!gui_.contexts.contains(arg(0)))return failure(6);
        const auto count=static_cast<std::int32_t>(arg(2));if(count<0)return failure(87);
        if(escape==8){
            if(count<2)return finish(0);if(!arg(3))return failure(87);
            const auto code=memory.load(GuestAddress{arg(3)},count<4?16:32);
            const bool printer=is_printer_dc(arg(0));
            return finish(code==8 || (printer && (code==1 || code==2 || code==3 || (code>=9 && code<=14))));
        }
        if(escape>=12 && escape<=14){
            if(!is_printer_dc(arg(0)))return finish(0);if(!arg(4))return failure(87);
            const unsigned first=escape==12?110:escape==13?112:114;
            memory.check(GuestAddress{arg(4)},8,Access::Write);
            memory.store(GuestAddress{arg(4)},32,*printer_device_cap(arg(0),first));
            memory.store(GuestAddress{arg(4)+4},32,*printer_device_cap(arg(0),first+1));return finish(1);
        }
        switch(escape){case 1:name="EndPage";break;case 2:name="AbortDoc";break;
        case 3:name="NextBand";break;case 9:name="SetAbortProc";break;
        case 10:name="StartDocA";break;case 11:name="EndDoc";break;
        default:return finish(0);}
    }
    if(!is_printer_dc(arg(0)))return failure(6);auto& device=printing_->devices.at(arg(0));
    if(name=="CancelDC"){device.cancelled=true;printing_changed(0x200);return finish(1);}
    if(name=="AbortDoc"){if(device.active)printing_changed(0x400);device.active=device.page=device.implicit_page=device.banding=false;device.cancelled=true;device.pages.clear();device.bytes=0;return finish(1);}
    if(device.pending)return failure(170);
    if(name=="SetAbortProc"){const auto procedure=arg(legacy?3:1);if(!procedure)return failure(87);if(!handles(GuestAddress{procedure}))memory.check(GuestAddress{procedure},1,Access::Execute);device.abort_proc=procedure;return finish(1);}
    try{
        if(name.starts_with("ResetDC")){
            if(device.active || !gui_.saved_contexts[arg(0)].empty())return failure(170);auto changed=device;if(const auto error=read_printer_settings(memory,arg(1),name.ends_with('W'),changed))return failure(error);
            GuiBitmap bitmap{changed.width,changed.height,std::vector<std::uint32_t>(static_cast<std::size_t>(changed.width)*changed.height,0xffffffffU)};gui_.bitmaps.at(changed.bitmap)=std::move(bitmap);rasters_.erase(changed.bitmap);device=std::move(changed);return finish(arg(0));
        }
        if(name.starts_with("StartDoc")){
            if(device.active || device.information)return failure(1906);const auto info=arg(legacy?4:1);
            if(!legacy && !info)return failure(87);if(!filesystem_.writable())return failure(5);
            const unsigned unit=x86_?4:8,size=x86_?20:40;
            if(info){memory.check(GuestAddress{info},size,Access::Read);if(memory.load(GuestAddress{info},32)!=size)return failure(87);}
            const auto pointer=[&](unsigned field){return info?memory.load(GuestAddress{info+field*unit},static_cast<std::uint8_t>(unit*8)):0;};
            const auto read=[&](std::uint64_t address){return address?(name.ends_with('W')?read_utf16(memory,GuestAddress{address}):utf8_to_utf16(read_ansi(memory,address))):std::u16string{};};
            const auto type=read(pointer(3));if((!type.empty() && type!=u"RAW" && type!=u"EMF") || (info && memory.load(GuestAddress{info+4*unit},32)))return failure(50);
            std::u16string title=legacy?std::u16string{}:read(pointer(1));
            if(legacy){
                const auto count=static_cast<std::uint32_t>(arg(2));if(count>32767)return failure(87);
                title.clear();if(arg(3)){
                    std::vector<std::uint8_t> bytes(count);memory.read(GuestAddress{arg(3)},bytes);
                    const auto end=std::find(bytes.begin(),bytes.end(),0);
                    title=utf8_to_utf16(std::string(bytes.begin(),end));
                }else if(count)return failure(87);
            }
            auto output=utf16_to_utf8(read(pointer(2)));if(output=="FILE:")return failure(50);
            if(printing_->next_job>INT32_MAX)return failure(8);device.job=printing_->next_job++;
            device.title=std::move(title);device.output=std::move(output);
            if(device.output.empty())device.output="C:\\x64arm-print-"+std::to_string(::getpid())+"-"+std::to_string(device.job)+".pdf";
            device.submitted=runtime_.system_filetime();device.active=true;device.page=device.cancelled=device.implicit_page=device.banding=false;device.legacy=legacy;device.pages.clear();device.bytes=0;device.pending=legacy?5:4;
        }else{
            if(!device.active || device.information || device.cancelled)return failure(device.cancelled?995:1906);
            if(name=="StartPage"){if(device.page)return failure(1906);device.pending=1;}
            else if(name=="EndPage"){if(!device.page)return failure(1906);if(legacy)device.legacy=true;device.pending=2;}
            else if(name=="EndDoc"){if((device.page && !legacy) || (!device.page && device.pages.empty()))return failure(1906);device.pending=legacy && device.page?6:3;}
            else if(name=="NextBand"){
                if(!arg(4))return failure(87);memory.check(GuestAddress{arg(4)},16,Access::Write);
                device.legacy=true;
                if(device.banding){device.banding=false;for(unsigned i=0;i<4;++i)memory.store(GuestAddress{arg(4)+i*4},32,0);if(!device.page)return failure(1906);device.pending=2;}
                else {device.banding=true;for(unsigned i=0;i<4;++i)memory.store(GuestAddress{arg(4)+i*4},32,i==2?device.width:i==3?device.height:0);
                    if(device.page)return finish(1);device.pending=1;}
            }
            else return failure(50);
        }
        if(device.abort_proc){try{start_callbacks(state,memory,{{device.abort_proc,{arg(0),0}}},Continuation::PrintAbort,arg(0));}catch(...){device.pending=0;throw;}return std::nullopt;}
        return resume_printing(state,memory,arg(0),1);
    }catch(const std::bad_alloc&){device.pending=0;return failure(8);}catch(const std::length_error&){device.pending=0;return failure(8);}catch(const std::domain_error&){device.pending=0;return failure(50);}
    catch(const std::invalid_argument&){device.pending=0;return failure(87);}
}
std::optional<std::uint32_t> WindowsServices::resume_printing(GuestState& state,GuestMemory& memory,std::uint64_t dc,std::uint64_t callback){
    const auto finish=[&](unsigned value,unsigned error=0){if(error)last_error_=error;complete_call(state,memory,value);return std::optional<std::uint32_t>{};};
    if(!is_printer_dc(dc))return finish(UINT32_MAX,6);auto& device=printing_->devices.at(dc);const auto action=device.pending;device.pending=0;
    if(!callback || device.cancelled || !device.active){if(device.active)printing_changed(0x400);device.active=device.page=device.implicit_page=device.banding=false;device.cancelled=true;device.pages.clear();device.bytes=0;return finish(UINT32_MAX-1,995);}
    try{
        if(action==4){printing_changed(0x100);return finish(device.job);}
        if(action==1 || action==5){if(action==5)printing_changed(0x100);const auto error=begin_printer_page(memory,dc);return error?finish(UINT32_MAX,error):finish(1);}
        const auto end_page=[&]()->unsigned{
            auto surface=raster_surface(memory,dc,false);if(!surface)return 50;
            const auto bytes=static_cast<std::uint64_t>(device.width)*device.height*4;
            if(device.bytes+bytes>128*1024*1024 || device.pages.size()>=256)return 8;
            auto page=surface->snapshot();if(device.monochrome)for(auto& pixel:page.pixels){const auto gray=((pixel&255)*77+((pixel>>8)&255)*150+((pixel>>16)&255)*29+128)>>8;pixel=0xff000000U|gray|(gray<<8)|(gray<<16);}
            device.pages.push_back(std::move(page));device.bytes+=bytes;device.page=false;device.implicit_page=device.legacy;
            printing_changed(0xa00);return 0;
        };
        if(action==2){const auto error=end_page();return error?finish(UINT32_MAX,error):finish(1);}
        if(action==6){const auto error=end_page();if(error)return finish(UINT32_MAX,error);}
        if(action==3 || action==6){const auto bytes=native_pdf_document(device.pages,device.title,device.copies,device.collate);auto file=filesystem_.open_host(device.output,0x40000000,2,0,1);if(file.error)return finish(UINT32_MAX,file.error);
            std::size_t at=0;while(at<bytes.size()){const auto write=file.file->write(std::span(bytes).subspan(at),at);if(write.error || !write.bytes)return finish(UINT32_MAX,write.error?write.error:29);at+=static_cast<std::size_t>(write.bytes);}if(const auto error=file.file->flush())return finish(UINT32_MAX,error);
            device.active=device.page=device.implicit_page=device.banding=false;device.pages.clear();device.bytes=0;printing_changed(0x400);return finish(1);}
        return finish(UINT32_MAX,1906);
    }catch(const std::bad_alloc&){return finish(UINT32_MAX,8);}catch(const std::length_error&){return finish(UINT32_MAX,8);}catch(const std::domain_error&){return finish(UINT32_MAX,50);}
}
namespace {
// 所有指標均在最終客體 buffer 內，配置及字串編碼不依賴宿主 ABI。
struct PrinterBuffer {
    std::vector<std::uint8_t> bytes;std::uint64_t base;unsigned unit;bool wide;
    PrinterBuffer(unsigned size,std::uint64_t address,bool x86,bool unicode):bytes(size),base(address),unit(x86?4:8),wide(unicode){}
    void put(std::size_t offset,std::uint64_t value,unsigned size=4){for(unsigned i=0;i<size;++i)bytes.at(offset+i)=static_cast<std::uint8_t>(value>>(i*8));}
    void data(unsigned field,std::span<const std::uint8_t> value){while(bytes.size()%unit)bytes.push_back(0);const auto offset=bytes.size();bytes.insert(bytes.end(),value.begin(),value.end());put(field,base+offset,unit);}
    void text(unsigned field,std::u16string_view value){std::vector<std::uint8_t> encoded;if(wide){for(const auto c:value){encoded.push_back(static_cast<std::uint8_t>(c));encoded.push_back(static_cast<std::uint8_t>(c>>8));}encoded.push_back(0);encoded.push_back(0);}else{const auto narrow=utf16_to_utf8(value);encoded.assign(narrow.begin(),narrow.end());encoded.push_back(0);}data(field,encoded);}
};
}
std::vector<std::uint8_t> printer_devmode(bool wide,const PrinterSettings& device){
    PrinterBuffer buffer(wide?220:156,0,true,wide);const std::u16string_view name=u"Win64 PDF";for(unsigned i=0;i<name.size();++i)buffer.put(i*(wide?2:1),name[i],wide?2:1);
    const unsigned offset=wide?64:32,body=offset+12;buffer.put(offset,0x401,2);buffer.put(offset+2,1,2);buffer.put(offset+4,buffer.bytes.size(),2);buffer.put(offset+8,0xff13);
    buffer.put(body,device.width>device.height?2:1,2);buffer.put(body+2,(device.width==794 || device.height==794)?9:1,2);buffer.put(body+8,100,2);buffer.put(body+10,device.copies,2);buffer.put(body+12,7,2);buffer.put(body+14,96,2);buffer.put(body+16,device.monochrome?1:2,2);buffer.put(body+18,1,2);buffer.put(body+20,96,2);buffer.put(body+22,1,2);buffer.put(body+24,device.collate?1:0,2);return buffer.bytes;
}
void WindowsServices::printing_changed(unsigned changes){
    if(!printing_)return;for(auto& [handle,notice]:printing_->notices){notice.changes|=notice.filter&changes;if(notice.changes){const auto event=runtime_.wait_objects.find(handle);if(event!=runtime_.wait_objects.end())event->second.set_signal(true);}}
}
std::optional<std::uint32_t> WindowsServices::invoke_spooler(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const bool wide=name.ends_with('W');const unsigned unit=x86_?4:8;const auto width=static_cast<std::uint8_t>(unit*8);
    const auto d=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto fail=[&](unsigned error){last_error_=error;return finish(name.starts_with("DocumentProperties") || name.starts_with("DeviceCapabilities")?UINT32_MAX:name=="FindFirstPrinterChangeNotification"?(x86_?UINT32_MAX:UINT64_MAX):0);};
    const auto string=[&](std::uint64_t at){return at?(wide?read_utf16(memory,GuestAddress{at}):utf8_to_utf16(read_ansi(memory,at))):std::u16string{};};
    const auto datatype=[&](std::u16string value){for(auto& c:value)if(c>=u'a' && c<=u'z')c-=32;return value;};
    const auto printer_name=[&](std::uint64_t at){auto value=string(at);for(auto& c:value)if(c>=u'A' && c<=u'Z')c+=32;return value==u"win64 pdf";};
    const bool available=native_gui_ && runtime_.network.external_io;
    if(!printing_)printing_=std::make_shared<PrintingState>();
    const auto copy=[&](const PrinterBuffer& buffer,std::uint64_t target,std::uint64_t capacity,std::uint64_t needed){if(!needed)return fail(87);memory.store(GuestAddress{needed},32,buffer.bytes.size());if(capacity<buffer.bytes.size())return fail(122);if(!target && !buffer.bytes.empty())return fail(87);if(!buffer.bytes.empty())memory.write(GuestAddress{target},buffer.bytes);return finish(1);};
    if(name.starts_with("EnumPrintProcessorDatatypes")){
        if(!arg(5) || !arg(6))return fail(87);memory.store(GuestAddress{arg(6)},32,0);
        if(d(2)!=1)return fail(124);if(arg(0) && !string(arg(0)).empty() && string(arg(0))!=u"\\\\X64ARM")return fail(1801);
        if(!available || !printer_name(arg(1)))return fail(1798);
        PrinterBuffer buffer(unit*2,arg(3),x86_,wide);buffer.text(0,u"RAW");buffer.text(unit,u"TEXT");
        if(d(4)>=buffer.bytes.size() && arg(3))memory.store(GuestAddress{arg(6)},32,2);return copy(buffer,arg(3),d(4),arg(5));
    }
    if(name.starts_with("GetDefaultPrinter")){
        if(!arg(1))return fail(87);const auto capacity=memory.load(GuestAddress{arg(1)},32);memory.store(GuestAddress{arg(1)},32,available?10:0);if(!available)return fail(2);if(capacity<10 || !arg(0))return fail(122);
        const auto text=wide?std::vector<std::uint8_t>{'W',0,'i',0,'n',0,'6',0,'4',0,' ',0,'P',0,'D',0,'F',0,0,0}:std::vector<std::uint8_t>{'W','i','n','6','4',' ','P','D','F',0};memory.write(GuestAddress{arg(0)},text);return finish(1);
    }
    if(name.starts_with("OpenPrinter")){
        if(!arg(1))return fail(87);memory.check(GuestAddress{arg(1)},unit,Access::Write);if(!available || !printer_name(arg(0)))return fail(1801);if(printing_->handles.size()>=256)return fail(8);PrintingState::Device device;
        if(arg(2)){memory.check(GuestAddress{arg(2)},unit*2+4,Access::Read);const auto type=datatype(string(memory.load(GuestAddress{arg(2)},width)));const auto access=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)+unit*2},32));if(access&~0x000f000cU)return fail(5);if(!type.empty() && type!=u"RAW" && type!=u"TEXT")return fail(1804);if(!type.empty())device.default_datatype=device.datatype=type;device.access=access?access:8;if(const auto error=read_printer_settings(memory,memory.load(GuestAddress{arg(2)+unit},width),wide,device))return fail(error);}
        const auto handle=runtime_.allocate_wait_handle();printing_->handles.emplace(handle,std::move(device));memory.store(GuestAddress{arg(1)},width,handle);return finish(1);
    }
    if(name=="ClosePrinter"){
        if(const auto found=printing_->handles.find(arg(0));found!=printing_->handles.end() && found->second.active)printing_changed(0x400);
        if(!printing_->handles.erase(arg(0)))return fail(6);for(auto it=printing_->notices.begin();it!=printing_->notices.end();)if(it->second.printer==arg(0)){runtime_.wait_objects.erase(it->first);it=printing_->notices.erase(it);}else ++it;return finish(1);
    }
    if(name.starts_with("StartDocPrinter") || name=="StartPagePrinter" || name=="WritePrinter" || name=="EndPagePrinter" || name=="EndDocPrinter" || name=="AbortPrinter"){
        const auto found=printing_->handles.find(arg(0));if(found==printing_->handles.end())return fail(6);auto& device=found->second;
        if(!(device.access&8U))return fail(5);
        const auto discard=[&](){device.active=device.page=device.explicit_pages=false;device.spool.clear();device.page_ends.clear();device.bytes=0;printing_changed(0x400);};
        try{
            if(name.starts_with("StartDocPrinter")){
                if(device.active)return fail(1906);if(d(1)!=1)return fail(124);if(!arg(2))return fail(87);if(!filesystem_.writable())return fail(5);
                memory.check(GuestAddress{arg(2)},unit*3,Access::Read);const auto pointer=[&](unsigned index){return memory.load(GuestAddress{arg(2)+index*unit},width);};
                const auto type=pointer(2)?datatype(string(pointer(2))):device.default_datatype;if(type!=u"RAW" && type!=u"TEXT")return fail(1804);
                auto title=string(pointer(0));auto output=utf16_to_utf8(string(pointer(1)));if(output=="FILE:")return fail(50);
                if(printing_->next_job>INT32_MAX)return fail(8);const auto job=printing_->next_job++;
                if(output.empty())output="C:\\x64arm-print-"+std::to_string(::getpid())+"-"+std::to_string(job)+".pdf";
                device.title=std::move(title);device.output=std::move(output);device.datatype=type;device.job=job;device.submitted=runtime_.system_filetime();
                device.active=true;device.page=device.cancelled=device.explicit_pages=false;device.spool.clear();device.page_ends.clear();device.bytes=0;printing_changed(0x100);return finish(job);
            }
            if(!device.active)return fail(3003);
            if(name=="AbortPrinter"){discard();return finish(1);}
            if(name=="StartPagePrinter"){if(device.page)return fail(1906);device.page=true;device.explicit_pages=true;printing_changed(0x200);return finish(1);}
            if(name=="EndPagePrinter"){if(!device.page)return fail(1906);device.page_ends.push_back(device.spool.size());device.page=false;printing_changed(0xa00);return finish(1);}
            if(name=="WritePrinter"){
                if(!arg(3))return fail(87);memory.check(GuestAddress{arg(3)},4,Access::Write);
                const auto write_failure=[&](unsigned error){memory.store(GuestAddress{arg(3)},32,0);return fail(error);};
                if(device.explicit_pages && !device.page)return write_failure(1906);const auto count=d(2);if(count && !arg(1))return write_failure(87);
                if(count>128*1024*1024-device.spool.size())return write_failure(8);
                // pcWritten 可與輸入重疊；完成資料讀取後才提交輸出計數。
                try{if(count){memory.check(GuestAddress{arg(1)},count,Access::Read);const auto previous=device.spool.size();device.spool.resize(previous+count);memory.read(GuestAddress{arg(1)},std::span(device.spool).subspan(previous));}}
                catch(const std::bad_alloc&){return write_failure(8);}catch(const std::length_error&){return write_failure(8);}
                device.bytes=device.spool.size();memory.store(GuestAddress{arg(3)},32,count);printing_changed(0x200);return finish(1);
            }
            if(device.page)return fail(1906);
            NativeSpoolDocument document;
            if(device.datatype==u"RAW" && device.spool.size()>=5 && std::equal(device.spool.begin(),device.spool.begin()+5,"%PDF-"))document=native_pdf_raw_document(device.spool);
            else {
                std::vector<std::u16string> pages;std::size_t previous=0;
                const auto append=[&](std::size_t end){const auto* data=reinterpret_cast<const char*>(device.spool.data());auto value=utf8_to_utf16(std::string_view(data?data+previous:"",end-previous));previous=end;std::u16string page;
                    for(std::size_t index=0;index<value.size();++index){const auto c=value[index];if(c==u'\f'){pages.push_back(std::move(page));page.clear();}else if(c==u'\r'){page.push_back(u'\n');if(index+1<value.size() && value[index+1]==u'\n')++index;}else if(c==u'\b'){if(!page.empty()){page.pop_back();if(!page.empty() && page.back()>=0xd800 && page.back()<=0xdbff)page.pop_back();}}else if(c<32 && c!=u'\t' && c!=u'\n')throw std::invalid_argument("列印資料不是支援的 PDF 或 UTF-8 文字");else page.push_back(c);}pages.push_back(std::move(page));};
                for(const auto end:device.page_ends)append(end);if(previous<device.spool.size() || device.page_ends.empty())append(device.spool.size());
                document=native_pdf_text_document(pages,device.title,device.width,device.height,device.copies,device.collate);
            }
            auto file=filesystem_.open_host(device.output,0x40000000,2,0,1);if(file.error)return fail(file.error);
            std::size_t offset=0;while(offset<document.bytes.size()){const auto written=file.file->write(std::span(document.bytes).subspan(offset),offset);if(written.error || !written.bytes)return fail(written.error?written.error:29);offset+=static_cast<std::size_t>(written.bytes);}
            if(const auto error=file.file->flush())return fail(error);discard();return finish(1);
        }catch(const std::bad_alloc&){return fail(8);}catch(const std::length_error&){return fail(8);}catch(const std::invalid_argument&){return fail(13);}catch(const std::domain_error&){return fail(50);}
    }
    if(name=="FindClosePrinterChangeNotification"){
        if(!printing_->notices.erase(arg(0)))return fail(6);runtime_.wait_objects.erase(arg(0));return finish(1);
    }
    if(name=="FindNextPrinterChangeNotification"){
        const auto found=printing_->notices.find(arg(0));if(found==printing_->notices.end() || !runtime_.wait_objects.contains(arg(0)))return fail(6);if(arg(2) || arg(3))return fail(50);if(arg(1))memory.store(GuestAddress{arg(1)},32,found->second.changes);found->second.changes=0;runtime_.wait_objects.at(arg(0)).set_signal(false);return finish(1);
    }
    if(name=="FindFirstPrinterChangeNotification"){
        if(!printing_->handles.contains(arg(0)))return fail(6);if(d(2) || arg(3))return fail(50);if(!d(1) || (d(1)&~0x7777ffffU))return fail(87);if(printing_->notices.size()>=256)return fail(8);
        const auto handle=runtime_.allocate_wait_handle();WaitObject event;event.kind=WaitObject::Kind::Event;event.manual_reset=true;event.access=0x100000;runtime_.wait_objects.emplace(handle,event);printing_->notices.emplace(handle,PrintingState::Notice{arg(0),d(1),0});return finish(handle);
    }
    if(name.starts_with("DocumentProperties")){
        const auto found=printing_->handles.find(arg(1));if(found==printing_->handles.end())return fail(6);if(arg(2) && !printer_name(arg(2)))return fail(1801);if(!d(5))return finish(wide?220:156);if(d(5)&~10U)return fail(50);PrinterSettings device=found->second;
        if(d(5)&8){if(!arg(4))return fail(87);if(const auto error=read_printer_settings(memory,arg(4),wide,device))return fail(error);}if(d(5)&2){if(!arg(3))return fail(87);memory.write(GuestAddress{arg(3)},printer_devmode(wide,device));}return finish(1);
    }
    if(name.starts_with("DeviceCapabilities")){
        if(!available || !printer_name(arg(0)))return fail(1801);if(arg(1) && !string(arg(1)).empty() && string(arg(1))!=u"FILE:")return fail(1796);PrintingState::Device device;if(const auto error=read_printer_settings(memory,arg(4),wide,device))return fail(error);
        const auto capability=static_cast<unsigned>(arg(2));unsigned count=0;std::vector<std::uint8_t> bytes;
        const auto push=[&](unsigned value,unsigned size){for(unsigned i=0;i<size;++i)bytes.push_back(static_cast<std::uint8_t>(value>>(i*8)));};
        const auto fixed=[&](std::u16string_view text,unsigned length){for(unsigned i=0;i<length;++i)push(i<text.size()?text[i]:0,wide?2:1);};
        switch(capability){case 1:return finish(0xff13);case 4:return finish(2100|(2794U<<16));case 5:return finish(2159|(2970U<<16));case 7:case 9:case 30:return finish(0);case 8:return finish(wide?220:156);case 10:return finish(0x401);case 11:case 15:case 22:case 32:return finish(1);case 18:return finish(99);case 17:return finish(90);
            case 2:push(1,2);push(9,2);count=2;break;case 3:for(const unsigned value:{2159U,2794U,2100U,2970U})push(value,4);count=2;break;case 6:push(7,2);count=1;break;case 12:fixed(u"Automatic",24);count=1;break;case 13:push(96,4);push(96,4);count=1;break;case 16:case 29:fixed(u"Letter",64);fixed(u"A4",64);count=2;break;case 35:push(1,4);count=1;break;default:return fail(50);}
        if(arg(3))memory.write(GuestAddress{arg(3)},bytes);return finish(count);
    }
    const bool enumeration=name.starts_with("EnumPrinters"),driver=name.starts_with("GetPrinterDriver"),job=name.starts_with("GetJob");
    if(!enumeration && !printing_->handles.contains(arg(0)))return fail(6);
    const auto level=static_cast<unsigned>(arg(enumeration || driver || job?2:1));const auto target=arg(enumeration || driver || job?3:2),capacity=static_cast<std::uint64_t>(d(enumeration || driver || job?4:3)),needed=arg(enumeration || driver || job?5:4);
    if(enumeration){if(!arg(6))return fail(87);memory.store(GuestAddress{arg(6)},32,0);if(level!=1 && level!=2 && level!=4 && level!=5)return fail(124);const auto flags=d(0);if(flags&~UINT64_C(0x0300007e))return fail(87);if(flags&8)return fail(50);if(level==4 && (flags&~6U))return fail(87);
        if((level==2 || level==5) && arg(1) && !string(arg(1)).empty())return fail(50);
        if(!available || !(flags&2) || (flags&(32|0x2000000))){PrinterBuffer empty(0,target,x86_,wide);return copy(empty,target,capacity,needed);}
    }
    if(driver){if(arg(1) && !string(arg(1)).empty() && string(arg(1))!=(x86_?u"Windows NT x86":u"Windows x64"))return fail(1805);if(level!=1 && level!=2)return fail(124);PrinterBuffer buffer(level==1?unit:unit*6,target,x86_,wide);if(level==1)buffer.text(0,u"Win64 PDF");else{buffer.put(0,3);buffer.text(unit,u"Win64 PDF");buffer.text(unit*2,x86_?u"Windows NT x86":u"Windows x64");buffer.text(unit*3,u"winspool.drv");buffer.text(unit*4,u"");buffer.text(unit*5,u"winspool.drv");}return copy(buffer,target,capacity,needed);}
    if(job){
        const PrintingState::Device* selected=nullptr;for(const auto* list:{&printing_->devices,&printing_->handles})for(const auto& [handle,device]:*list){(void)handle;if(device.active && device.job==d(1))selected=&device;}
        if(!selected)return fail(87);if(level!=1 && level!=2)return fail(124);const auto& device=*selected;
        const auto pages=device.bitmap?device.pages.size():device.page_ends.size();
        PrinterBuffer buffer(level==1?(x86_?64:96):(x86_?104:160),target,x86_,wide);buffer.put(0,device.job);buffer.text(unit,u"Win64 PDF");buffer.text(unit*2,u"\\\\X64ARM");buffer.text(unit*3,u"user");buffer.text(unit*4,device.title);
        const unsigned fields=level==1?unit*7:unit*13;buffer.put(fields,device.cancelled?4:8);buffer.put(fields+4,1);buffer.put(fields+8,1);
        if(level==1){buffer.text(unit*5,device.datatype);buffer.put(fields+12,pages);}
        else{buffer.text(unit*6,device.datatype);buffer.text(unit*7,u"Win64 PDF");buffer.text(unit*9,u"Win64 PDF");buffer.data(unit*10,printer_devmode(wide,device));buffer.put(fields+20,pages);buffer.put(fields+24,device.bytes);}
        const auto seconds=static_cast<std::time_t>((device.submitted-UINT64_C(116444736000000000))/10000000);std::tm time{};if(::gmtime_r(&seconds,&time)){const unsigned offset=fields+(level==1?20:28);const unsigned values[]{static_cast<unsigned>(time.tm_year+1900),static_cast<unsigned>(time.tm_mon+1),static_cast<unsigned>(time.tm_wday),static_cast<unsigned>(time.tm_mday),static_cast<unsigned>(time.tm_hour),static_cast<unsigned>(time.tm_min),static_cast<unsigned>(time.tm_sec),static_cast<unsigned>((device.submitted/10000)%1000)};for(unsigned i=0;i<8;++i)buffer.put(offset+i*2,values[i],2);}return copy(buffer,target,capacity,needed);
    }
    const unsigned jobs=static_cast<unsigned>(std::count_if(printing_->devices.begin(),printing_->devices.end(),[](const auto& item){return item.second.active;})+std::count_if(printing_->handles.begin(),printing_->handles.end(),[](const auto& item){return item.second.active;}));unsigned size=0;
    switch(level){case 1:size=unit*4;break;case 2:size=x86_?84:136;break;case 4:size=unit*3;break;case 5:size=x86_?20:32;break;case 6:size=4;break;case 7:size=unit*2;break;case 8:case 9:size=unit;break;default:return fail(124);}
    PrinterBuffer buffer(size,target,x86_,wide);const PrintingState::Device defaults;
    switch(level){case 1:buffer.put(0,0x800000);buffer.text(unit,u"Win64 PDF,FILE:,CoreGraphics PDF");buffer.text(unit*2,u"Win64 PDF");buffer.text(unit*3,u"CoreGraphics PDF");break;
        case 2:buffer.text(unit,u"Win64 PDF");buffer.text(unit*3,u"FILE:");buffer.text(unit*4,u"Win64 PDF");buffer.text(unit*5,u"CoreGraphics PDF");buffer.data(unit*7,printer_devmode(wide,defaults));buffer.text(unit*9,u"Win64 PDF");buffer.text(unit*10,u"RAW");buffer.put(unit*13,0x40);buffer.put(unit*13+4,1);buffer.put(unit*13+8,1);buffer.put(unit*13+24,jobs);break;
        case 4:buffer.text(0,u"Win64 PDF");buffer.put(unit*2,0x40);break;case 5:buffer.text(0,u"Win64 PDF");buffer.text(unit,u"FILE:");buffer.put(unit*2,0x40);break;case 7:buffer.put(unit,4);break;case 8:case 9:buffer.data(0,printer_devmode(wide,defaults));break;default:break;}
    if(enumeration && capacity>=buffer.bytes.size() && target)memory.store(GuestAddress{arg(6)},32,1);return copy(buffer,target,capacity,needed);
}
}
