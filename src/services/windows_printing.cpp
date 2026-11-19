#include "x64arm/windows_services.hpp"
#include "x64arm/native_pdf.hpp"
#include "x64arm/windows_printing.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/registry_store.hpp"
#include "x64arm/windows_security_attributes.hpp"
#include "printer_forms.hpp"
#include "service_callback_task.hpp"
#include <algorithm>
#include <chrono>
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
        // 佇列控制：SetJob 的暫停（JOB_STATUS_PAUSED）與保留旗標、優先權；AddJob 的暫存檔路徑；GetSpoolFileHandle 的暫存檔與已提交位元組；
        // 「印表機名稱,Job n」開啟的工作控制代碼與其讀取位置。
        std::uint32_t status{},priority{1};std::u16string spool_path;std::uint64_t spool_handle{},committed{};
        unsigned read_job{};std::uint64_t read_offset{};
    };
    struct Notice {
        std::uint64_t printer{};unsigned filter{},changes{};
        std::vector<printer_forms::Form> forms;
        std::array<std::uint64_t,3> revisions{};
    };
    std::map<std::uint64_t,Device> devices,handles;std::map<std::uint64_t,Notice> notices;unsigned next_job{1};
    // 印表機暫停或工作暫停時，EndDocPrinter／ScheduleJob 完成的工作留在佇列，恢復後依序輸出。印表機設定（SetPrinter 第 2、8、9 級）在本程序有效。
    std::vector<Device> queue;bool paused{};std::uint32_t status{},priority{1},default_priority{1};
    std::u16string comment{u"CoreGraphics PDF"},location;std::optional<PrinterSettings> defaults;
    std::uint64_t form_poll_time{};
};
std::vector<PrinterSettings> WindowsServices::available_printer_settings(unsigned& error){
    RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);
    error=transaction.error;if(error)return {};
    std::vector<PrinterSettings> result;for(const auto& form:printer_forms::all(runtime_.registry))if(printer_forms::printable(form))result.push_back(printer_forms::settings(form));return result;
}
unsigned WindowsServices::read_printer_settings(GuestMemory& memory,std::uint64_t address,bool wide,PrinterSettings& target){
    if(!address)return 0;const auto size_offset=wide?68U:36U,field_offset=size_offset+4;
    const auto word=[&](unsigned offset){return static_cast<unsigned>(memory.load(GuestAddress{address+offset},16));};
    const auto size=word(size_offset);if(size<field_offset+32)return 87;if(word(size_offset+2))return 50;memory.check(GuestAddress{address},size,Access::Read);
    const auto fields=static_cast<unsigned>(memory.load(GuestAddress{address+field_offset},32));if(fields&~0x1ff1fU)return 50;
    auto device=target;const auto body=field_offset+4;unsigned orientation=device.landscape?2:1;
    if(fields&1)orientation=word(body);if(orientation!=1 && orientation!=2)return 87;
    if(fields&(2U|0x10000U)){
        unsigned error=0;const auto forms=available_printer_settings(error);if(error)return error;
        const PrinterSettings* selected=nullptr;const auto paper=fields&2?word(body+2):0;
        if(paper){for(const auto& form:forms)if(form.paper==paper){selected=&form;break;}}
        else if(fields&0x10000U){
            const auto offset=body+26,units=wide?2U:1U;if(size<offset+32*units)return 87;
            std::u16string text;std::string bytes;bool terminated=false;
            for(unsigned i=0;i<32;++i){const auto c=memory.load(GuestAddress{address+offset+i*units},wide?16:8);if(!c){terminated=true;break;}if(wide)text.push_back(static_cast<char16_t>(c));else bytes.push_back(static_cast<char>(c));}
            if(!terminated)return 87;if(!wide)text=utf8_to_utf16(bytes);
            for(const auto& form:forms)if(printer_forms::fold(form.form)==printer_forms::fold(text)){selected=&form;break;}
        }
        if(selected){device.paper=selected->paper;device.form=selected->form;device.paper_width=selected->paper_width;device.paper_height=selected->paper_height;}
        else if((fields&12U)!=12U)return 1902;
    }
    if(fields&12U){
        if((fields&12U)==4U)return 87;const auto w=word(body+6);if(!w || w>32767)return 87;
        if(fields&4U){const auto h=word(body+4);if(!h || h>32767)return 87;device.paper_height=h*100;}
        device.paper=0;device.form.clear();device.paper_width=w*100;
    }
    if((fields&16) && word(body+8)!=100)return 50;if(fields&0x100){device.copies=word(body+10);if(!device.copies || device.copies>99)return 87;}
    if((fields&0x200) && word(body+12)!=7)return 50;if((fields&0x400) && word(body+14)!=96)return 50;if((fields&0x1000) && word(body+18)!=1)return 50;
    if((fields&0x2000) && word(body+20)!=96)return 50;if((fields&0x4000) && word(body+22)!=1)return 50;if(fields&0x8000){const auto value=word(body+24);if(value>1)return 87;device.collate=value!=0;}
    if(fields&0x800){const auto color=word(body+16);if(color!=1 && color!=2)return 87;device.monochrome=color==1;}
    device.width=printer_forms::pixels(device.paper_width);device.height=printer_forms::pixels(device.paper_height);
    if(!device.width || !device.height || std::uint64_t{device.width}*device.height>32U*1024U*1024U)return 8;
    device.landscape=orientation==2;if(device.landscape)std::swap(device.width,device.height);target=std::move(device);return 0;
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
        {"winspool.drv","EnumFormsA",6},{"winspool.drv","EnumFormsW",6},{"winspool.drv","GetFormA",6},{"winspool.drv","GetFormW",6},
        {"winspool.drv","AddFormA",3},{"winspool.drv","AddFormW",3},{"winspool.drv","SetFormA",4},{"winspool.drv","SetFormW",4},{"winspool.drv","DeleteFormA",2},{"winspool.drv","DeleteFormW",2},
        {"spoolss.dll","EnumFormsA",6},{"spoolss.dll","EnumFormsW",6},{"spoolss.dll","GetFormA",6},{"spoolss.dll","GetFormW",6},
        {"spoolss.dll","AddFormA",3},{"spoolss.dll","AddFormW",3},{"spoolss.dll","SetFormA",4},{"spoolss.dll","SetFormW",4},{"spoolss.dll","DeleteFormA",2},{"spoolss.dll","DeleteFormW",2},
        {"winspool.drv","EnumPrintersA",7},{"winspool.drv","EnumPrintersW",7},{"winspool.drv","OpenPrinterA",3},{"winspool.drv","OpenPrinterW",3},{"winspool.drv","ClosePrinter",1},
        {"winspool.drv","GetPrinterA",5},{"winspool.drv","GetPrinterW",5},{"winspool.drv","GetPrinterDriverA",6},{"winspool.drv","GetPrinterDriverW",6},
        {"winspool.drv","DocumentPropertiesA",6},{"winspool.drv","DocumentPropertiesW",6},{"winspool.drv","DeviceCapabilitiesA",5},{"winspool.drv","DeviceCapabilitiesW",5},
        {"winspool.drv","GetJobA",6},{"winspool.drv","GetJobW",6},{"winspool.drv","FindFirstPrinterChangeNotification",4},{"winspool.drv","FindNextPrinterChangeNotification",4},{"winspool.drv","FindClosePrinterChangeNotification",1},
        {"winspool.drv","StartDocPrinterA",3},{"winspool.drv","StartDocPrinterW",3},{"winspool.drv","StartPagePrinter",1},{"winspool.drv","WritePrinter",4},{"winspool.drv","EndPagePrinter",1},{"winspool.drv","EndDocPrinter",1},{"winspool.drv","AbortPrinter",1},
        {"winspool.drv","EnumPrintProcessorDatatypesA",7},{"winspool.drv","EnumPrintProcessorDatatypesW",7},
        {"winspool.drv","GetPrinterDataA",6},{"winspool.drv","GetPrinterDataW",6},{"winspool.drv","GetPrinterDataExA",7},{"winspool.drv","GetPrinterDataExW",7},
        {"winspool.drv","SetPrinterDataA",5},{"winspool.drv","SetPrinterDataW",5},{"winspool.drv","SetPrinterDataExA",6},{"winspool.drv","SetPrinterDataExW",6},
        {"winspool.drv","EnumPrinterDataA",9},{"winspool.drv","EnumPrinterDataW",9},{"winspool.drv","EnumPrinterDataExA",6},{"winspool.drv","EnumPrinterDataExW",6},
        {"winspool.drv","EnumPrinterKeyA",5},{"winspool.drv","EnumPrinterKeyW",5},{"winspool.drv","DeletePrinterDataA",2},{"winspool.drv","DeletePrinterDataW",2},
        {"winspool.drv","DeletePrinterDataExA",3},{"winspool.drv","DeletePrinterDataExW",3},{"winspool.drv","DeletePrinterKeyA",2},{"winspool.drv","DeletePrinterKeyW",2},
        {"winspool.drv","EnumPortsA",6},{"winspool.drv","EnumPortsW",6},{"winspool.drv","EnumMonitorsA",6},{"winspool.drv","EnumMonitorsW",6},
        {"winspool.drv","EnumPrintProcessorsA",7},{"winspool.drv","EnumPrintProcessorsW",7},{"winspool.drv","EnumPrinterDriversA",7},{"winspool.drv","EnumPrinterDriversW",7},
        {"winspool.drv","GetPrinterDriverDirectoryA",6},{"winspool.drv","GetPrinterDriverDirectoryW",6},{"winspool.drv","GetPrintProcessorDirectoryA",6},{"winspool.drv","GetPrintProcessorDirectoryW",6},
        {"winspool.drv","IsValidDevmodeA",2},{"winspool.drv","IsValidDevmodeW",2},{"winspool.drv","OpenPrinter2A",4},{"winspool.drv","OpenPrinter2W",4},
        {"winspool.drv","SetDefaultPrinterA",1},{"winspool.drv","SetDefaultPrinterW",1},
        // 列印佇列控制（windows_print_queue.inc）。
        {"winspool.drv","EnumJobsA",8,0,0,0x2e},{"winspool.drv","EnumJobsW",8,0,0,0x2e},{"winspool.drv","SetJobA",5,0,0,0x16},{"winspool.drv","SetJobW",5,0,0,0x16},
        {"winspool.drv","AddJobA",5,0,0,0xa},{"winspool.drv","AddJobW",5,0,0,0xa},{"winspool.drv","ScheduleJob",2,0,0,0x2},
        {"winspool.drv","GetSpoolFileHandle",1},{"winspool.drv","CommitSpoolData",3,0,0,0x4},{"winspool.drv","CloseSpoolFileHandle",2},
        {"winspool.drv","SetPrinterA",4,0,0,0xa},{"winspool.drv","SetPrinterW",4,0,0,0xa},{"winspool.drv","ResetPrinterA",2},{"winspool.drv","ResetPrinterW",2},
        {"winspool.drv","ReadPrinter",4,0,0,0x4},{"winspool.drv","SeekPrinter",5,0x2,0,0x18},{"winspool.drv","FreePrinterNotifyInfo",1},{"winspool.drv","SpoolerInit",0},
        {"spoolss.dll","StartDocPrinterA",3},{"spoolss.dll","StartDocPrinterW",3},{"spoolss.dll","StartPagePrinter",1},{"spoolss.dll","WritePrinter",4},{"spoolss.dll","EndPagePrinter",1},{"spoolss.dll","EndDocPrinter",1},{"spoolss.dll","AbortPrinter",1},
        {"gdi32.dll","ResetDCA",2},{"gdi32.dll","ResetDCW",2},{"gdi32.dll","ExtEscape",6},{"gdi32.dll","DrawEscape",4},{"gdi32.dll","NamedEscape",7}
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
    if(name=="DrawEscape"){
        // 繪圖跳脫：原生顯示與 PDF 驅動都沒有繪圖跳脫，QUERYESCSUPPORT 對任何代碼皆回報不支援，其他跳脫回 0（未實作）。
        if(!gui_.contexts.contains(arg(0)))return failure(6);
        if(static_cast<std::int32_t>(arg(2))<0)return failure(87);
        if(static_cast<std::uint32_t>(arg(1))==8 && (static_cast<std::uint32_t>(arg(2))<4 || !arg(3)))return failure(87);
        return finish(0);
    }
    if(name=="NamedEscape"){
        // 依驅動名稱送出跳脫（hdc 可為 NULL）：與 ExtEscape 相同的支援集合，只有 QUERYESCSUPPORT 本身。
        if(!arg(1))return failure(87);
        if(arg(0) && !gui_.contexts.contains(arg(0)))return failure(6);
        if(static_cast<std::int32_t>(arg(3))<0 || static_cast<std::int32_t>(arg(5))<0)return failure(87);
        if(static_cast<std::uint32_t>(arg(2))==8){if(static_cast<std::uint32_t>(arg(3))<4 || !arg(4))return failure(87);return finish(memory.load(GuestAddress{arg(4)},32)==8?1:0);}
        return finish(0);
    }
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
        // Windows 中繼檔錄製 DC：MFCOMMENT（15）記成 META_ESCAPE 註解。
        if(escape==15 && gui_.emf_recorders.contains(arg(0)) && gui_.emf_recorders.at(arg(0)).wmf){
            if(count && !arg(3))return failure(87);std::vector<std::uint8_t> data(static_cast<std::size_t>(count));if(count)memory.read(GuestAddress{arg(3)},data);
            return finish(record_metafile_escape(arg(0),15,data)?1:0);
        }
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
    const unsigned offset=wide?64:32,body=offset+12;buffer.put(offset,0x401,2);buffer.put(offset+2,1,2);buffer.put(offset+4,buffer.bytes.size(),2);buffer.put(offset+8,0xff13U|(device.form.empty()?12U:0x10000U));
    buffer.put(body,device.landscape?2:1,2);buffer.put(body+2,device.paper,2);buffer.put(body+4,(device.paper_height+50)/100,2);buffer.put(body+6,(device.paper_width+50)/100,2);
    buffer.put(body+8,100,2);buffer.put(body+10,device.copies,2);buffer.put(body+12,7,2);buffer.put(body+14,96,2);buffer.put(body+16,device.monochrome?1:2,2);buffer.put(body+18,1,2);buffer.put(body+20,96,2);buffer.put(body+22,1,2);buffer.put(body+24,device.collate?1:0,2);
    if(wide){for(unsigned i=0;i<std::min<std::size_t>(31,device.form.size());++i)buffer.put(body+26+i*2,device.form[i],2);}
    else {const auto text=utf16_to_utf8(device.form);for(unsigned i=0;i<std::min<std::size_t>(31,text.size());++i)buffer.put(body+26+i,static_cast<std::uint8_t>(text[i]),1);}
    return buffer.bytes;
}
void WindowsServices::printing_changed(unsigned changes){
    if(!printing_)return;for(auto& [handle,notice]:printing_->notices){notice.changes|=notice.filter&changes;if(notice.changes){const auto event=runtime_.wait_objects.find(handle);if(event!=runtime_.wait_objects.end())event->second.set_signal(true);}}
}
bool WindowsServices::printer_form_notifications_waiting()const{
    return printing_ && std::any_of(printing_->notices.begin(),printing_->notices.end(),[](const auto& n){return n.second.filter&0x70000U;});
}
void WindowsServices::poll_printer_form_notifications(bool reload){
    if(registry_io_busy_ || !printer_form_notifications_waiting())return;
    if(reload){
        const auto now=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        if(now<printing_->form_poll_time+100)return;printing_->form_poll_time=now;
        RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)return;
    }
    const auto forms=printer_forms::all(runtime_.registry);const auto revisions=printer_forms::revisions(runtime_.registry);
    for(auto& [handle,notice]:printing_->notices)if(notice.filter&0x70000U){
        unsigned changes=printer_forms::changes(notice.forms,forms);
        for(unsigned i=0;i<3;++i)if(notice.revisions[i]!=revisions[i])changes|=0x10000U<<i;
        notice.forms=forms;notice.revisions=revisions;notice.changes|=notice.filter&changes;
        if(notice.changes){const auto event=runtime_.wait_objects.find(handle);if(event!=runtime_.wait_objects.end())event->second.set_signal(true);}
    }
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
    if(name.find("PrinterData")!=std::string_view::npos || name.starts_with("EnumPrinterKey") || name.starts_with("DeletePrinterKey"))return invoke_printer_data(state,memory,service);
    const auto copy=[&](const PrinterBuffer& buffer,std::uint64_t target,std::uint64_t capacity,std::uint64_t needed){if(!needed)return fail(87);memory.store(GuestAddress{needed},32,buffer.bytes.size());if(capacity<buffer.bytes.size())return fail(122);if(!target && !buffer.bytes.empty())return fail(87);if(!buffer.bytes.empty())memory.write(GuestAddress{target},buffer.bytes);return finish(1);};
    // 本程序的列印工作：進行中（GDI DC 與 winspool 控制代碼）與佇列中的工作，依工作編號排序。
    const auto all_jobs=[&]{std::vector<PrintingState::Device*> jobs;for(auto* list:{&printing_->devices,&printing_->handles})for(auto& [handle,device]:*list){(void)handle;if(device.active)jobs.push_back(&device);}
        for(auto& device:printing_->queue)jobs.push_back(&device);std::sort(jobs.begin(),jobs.end(),[](const auto* a,const auto* b){return a->job<b->job;});return jobs;};
    const auto print_job=[&](unsigned id)->PrintingState::Device*{for(auto* device:all_jobs())if(device->job==id)return device;return nullptr;};
    // JOB_INFO_1／JOB_INFO_2：GetJob 與 EnumJobs 共用；at 為結構在緩衝中的位移。
    const auto write_job=[&](PrinterBuffer& buffer,unsigned at,const PrintingState::Device& device,unsigned level,unsigned position){
        const auto pages=device.bitmap?device.pages.size():device.page_ends.size();const bool queued=!device.active;
        buffer.put(at,device.job);buffer.text(at+unit,u"Win64 PDF");buffer.text(at+unit*2,u"\\\\X64ARM");buffer.text(at+unit*3,u"user");buffer.text(at+unit*4,device.title);
        const unsigned fields=at+(level==1?unit*7:unit*13);
        const std::uint32_t status=(device.cancelled?4U:queued?0U:8U)|(device.status&(1U|0x10U|0x80U|0x800U|0x2000U));
        buffer.put(fields,status);buffer.put(fields+4,device.priority);buffer.put(fields+8,position);
        if(level==1){buffer.text(at+unit*5,device.datatype);buffer.put(fields+12,pages);}
        else{buffer.text(at+unit*6,device.datatype);buffer.text(at+unit*7,u"Win64 PDF");buffer.text(at+unit*9,u"Win64 PDF");buffer.data(at+unit*10,printer_devmode(wide,device));buffer.put(fields+20,pages);buffer.put(fields+24,device.bytes?device.bytes:device.spool.size());}
        const auto seconds=static_cast<std::time_t>((device.submitted-UINT64_C(116444736000000000))/10000000);std::tm time{};
        if(::gmtime_r(&seconds,&time)){const unsigned offset=fields+(level==1?20:28);const unsigned values[]{static_cast<unsigned>(time.tm_year+1900),static_cast<unsigned>(time.tm_mon+1),static_cast<unsigned>(time.tm_wday),static_cast<unsigned>(time.tm_mday),
            static_cast<unsigned>(time.tm_hour),static_cast<unsigned>(time.tm_min),static_cast<unsigned>(time.tm_sec),static_cast<unsigned>((device.submitted/10000)%1000)};for(unsigned i=0;i<8;++i)buffer.put(offset+i*2,values[i],2);}
    };
    // 把 RAW／TEXT 暫存資料轉成 PDF 寫到工作的輸出檔；EndDocPrinter、ScheduleJob 與恢復佇列共用。回 Win32 錯誤碼。
    const auto emit=[&](PrintingState::Device& device)->unsigned{
            NativeSpoolDocument document;
            if(device.datatype==u"RAW" && device.spool.size()>=5 && std::equal(device.spool.begin(),device.spool.begin()+5,"%PDF-"))document=native_pdf_raw_document(device.spool);
            else {
                std::vector<std::u16string> pages;std::size_t previous=0;
                const auto append=[&](std::size_t end){const auto* data=reinterpret_cast<const char*>(device.spool.data());auto value=utf8_to_utf16(std::string_view(data?data+previous:"",end-previous));previous=end;std::u16string page;
                    for(std::size_t index=0;index<value.size();++index){const auto c=value[index];if(c==u'\f'){pages.push_back(std::move(page));page.clear();}else if(c==u'\r'){page.push_back(u'\n');if(index+1<value.size() && value[index+1]==u'\n')++index;}else if(c==u'\b'){if(!page.empty()){page.pop_back();if(!page.empty() && page.back()>=0xd800 && page.back()<=0xdbff)page.pop_back();}}else if(c<32 && c!=u'\t' && c!=u'\n')throw std::invalid_argument("列印資料不是支援的 PDF 或 UTF-8 文字");else page.push_back(c);}pages.push_back(std::move(page));};
                for(const auto end:device.page_ends)append(end);if(previous<device.spool.size() || device.page_ends.empty())append(device.spool.size());
                document=native_pdf_text_document(pages,device.title,device.width,device.height,device.copies,device.collate);
            }
            auto file=filesystem_.open_host(device.output,0x40000000,2,0,1);if(file.error)return file.error;
            std::size_t offset=0;while(offset<document.bytes.size()){const auto written=file.file->write(std::span(document.bytes).subspan(offset),offset);if(written.error || !written.bytes)return written.error?written.error:29;offset+=static_cast<std::size_t>(written.bytes);}
        return file.file->flush();
    };
    // 佇列中的工作：印表機未暫停且工作未暫停時依序輸出，輸出後移除。
    const auto drain=[&]()->unsigned{
        if(printing_->paused)return 0;
        for(auto it=printing_->queue.begin();it!=printing_->queue.end();){if(it->status&1U){++it;continue;}const auto error=emit(*it);if(error)return error;it=printing_->queue.erase(it);printing_changed(0x400);}
        return 0;
    };
    if(name.starts_with("EnumPrintProcessorDatatypes")){
        if(!arg(5) || !arg(6))return fail(87);memory.store(GuestAddress{arg(6)},32,0);
        if(d(2)!=1)return fail(124);if(arg(0) && !string(arg(0)).empty() && string(arg(0))!=u"\\\\X64ARM")return fail(1801);
        if(!available || !printer_name(arg(1)))return fail(1798);
        PrinterBuffer buffer(unit*2,arg(3),x86_,wide);buffer.text(0,u"RAW");buffer.text(unit,u"TEXT");
        if(d(4)>=buffer.bytes.size() && arg(3))memory.store(GuestAddress{arg(6)},32,2);return copy(buffer,arg(3),d(4),arg(5));
    }
    // 虛擬列印系統的組態：連接埠 FILE:（Local Port 監視器）、列印處理器與驅動程式 Win64 PDF；列印子系統不可用時清單為空。
    const auto environment_ok=[&](std::uint64_t at,bool all){if(!at)return true;const auto value=string(at);if(value.empty())return true;
        auto lower=value;for(auto& c:lower)if(c>=u'A' && c<=u'Z')c+=32;if(all && lower==u"all")return true;return value==(x86_?u"Windows NT x86":u"Windows x64");};
    const auto local_server=[&](std::uint64_t at){return !at || string(at).empty() || string(at)==u"\\\\X64ARM";};
    const auto driver_info=[&](PrinterBuffer& buffer,unsigned level){
        if(level==1)buffer.text(0,u"Win64 PDF");
        else{buffer.put(0,3);buffer.text(unit,u"Win64 PDF");buffer.text(unit*2,x86_?u"Windows NT x86":u"Windows x64");buffer.text(unit*3,u"winspool.drv");buffer.text(unit*4,u"");buffer.text(unit*5,u"winspool.drv");}
    };
    const auto enumerate_config=[&](std::uint64_t target,std::uint64_t capacity,std::uint64_t needed,std::uint64_t returned,const PrinterBuffer& buffer,unsigned count){
        if(!needed || !returned)return fail(87);memory.store(GuestAddress{returned},32,0);
        if(capacity>=buffer.bytes.size() && (target || buffer.bytes.empty()))memory.store(GuestAddress{returned},32,count);return copy(buffer,target,capacity,needed);
    };
    if(name.starts_with("EnumPorts") || name.starts_with("EnumMonitors")){
        if(!local_server(arg(0)))return fail(1801);const auto level=d(1);if(level!=1 && level!=2)return fail(124);
        const bool port=name.starts_with("EnumPorts");
        PrinterBuffer buffer(available?(level==1?unit:(port?unit*3+8:unit*3)):0,arg(2),x86_,wide);
        if(available){
            if(port){buffer.text(0,u"FILE:");if(level==2){buffer.text(unit,u"Local Port");buffer.text(unit*2,u"Local Port");buffer.put(unit*3,1);}} // PORT_TYPE_WRITE
            else{buffer.text(0,u"Local Port");if(level==2){buffer.text(unit,x86_?u"Windows NT x86":u"Windows x64");buffer.text(unit*2,u"localspl.dll");}}
        }
        return enumerate_config(arg(2),d(3),arg(4),arg(5),buffer,available?1:0);
    }
    if(name.starts_with("EnumPrintProcessors") || name.starts_with("EnumPrinterDrivers")){
        if(!local_server(arg(0)))return fail(1801);const bool drivers=name.starts_with("EnumPrinterDrivers");
        if(!environment_ok(arg(1),drivers))return fail(1805);const auto level=d(2);if(level!=1 && !(drivers && level==2))return fail(124);
        PrinterBuffer buffer(available?(level==1?unit:unit*6):0,arg(3),x86_,wide);
        if(available){if(drivers)driver_info(buffer,level);else buffer.text(0,u"Win64 PDF");}
        return enumerate_config(arg(3),d(4),arg(5),arg(6),buffer,available?1:0);
    }
    if(name.starts_with("GetPrinterDriverDirectory") || name.starts_with("GetPrintProcessorDirectory")){
        if(!local_server(arg(0)))return fail(1801);if(!environment_ok(arg(1),false))return fail(1805);if(d(2)!=1)return fail(124);if(!arg(5))return fail(87);
        const auto folder=std::u16string(name.starts_with("GetPrinterDriverDirectory")?u"C:\\Windows\\System32\\spool\\drivers\\":u"C:\\Windows\\System32\\spool\\prtprocs\\")+(x86_?u"W32X86":u"x64");
        std::vector<std::uint8_t> bytes;if(wide)bytes=utf16_bytes(folder);else{const auto text=utf16_to_utf8(folder);bytes.assign(text.begin(),text.end());bytes.push_back(0);}
        memory.store(GuestAddress{arg(5)},32,bytes.size());if(d(4)<bytes.size())return fail(122);if(!arg(3))return fail(87);memory.write(GuestAddress{arg(3)},bytes);return finish(1);
    }
    if(name.starts_with("IsValidDevmode")){
        // dmSize（含 dmDriverExtra）不得超過呼叫端給的大小；dmFields 指定的欄位須在有效範圍內。
        if(!arg(0))return finish(0);const unsigned base=wide?64:32;const auto size=static_cast<std::uint64_t>(arg(1));if(size<base+8)return finish(0);
        memory.check(GuestAddress{arg(0)},base+8,Access::Read);
        const auto word=[&](unsigned offset){return static_cast<unsigned>(memory.load(GuestAddress{arg(0)+offset},16));};
        const auto dm_size=word(base+4),extra=word(base+6);const unsigned minimum=wide?188:124;
        if(dm_size<minimum || std::uint64_t{dm_size}+extra>size)return finish(0);
        const auto fields=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)+base+8},32));const auto body=base+12;
        const auto value=[&](unsigned offset){return std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(word(body+offset)));};
        if((fields&1) && value(0)!=1 && value(0)!=2)return finish(0);
        if((fields&2) && value(2)<=0)return finish(0);
        if((fields&4) && value(4)<=0)return finish(0);if((fields&8) && value(6)<=0)return finish(0);
        if((fields&16) && value(8)<=0)return finish(0);if((fields&0x100) && value(10)<=0)return finish(0);
        if((fields&0x800) && value(16)!=1 && value(16)!=2)return finish(0);if((fields&0x1000) && (value(18)<1 || value(18)>3))return finish(0);
        if((fields&0x8000) && value(24)!=0 && value(24)!=1)return finish(0);
        return finish(1);
    }
    if(name.starts_with("OpenPrinter2")){
        // PRINTER_OPTIONS（cbSize、dwFlags）只影響快取與連線方式；本機單一印表機與 OpenPrinter 相同。
        if(arg(3)){memory.check(GuestAddress{arg(3)},8,Access::Read);if(memory.load(GuestAddress{arg(3)},32)<8)return fail(87);}
        return finish(nested_service(state,memory,"winspool.drv",wide?"OpenPrinterW":"OpenPrinterA",{arg(0),arg(1),arg(2)}));
    }
    if(name.starts_with("SetDefaultPrinter")){
        // 只有一台虛擬印表機：NULL 或空字串選第一台，指定名稱須存在。
        if(!available)return fail(1801);if(arg(0) && !string(arg(0)).empty() && !printer_name(arg(0)))return fail(1801);return finish(1);
    }
    if(name.starts_with("GetDefaultPrinter")){
        if(!arg(1))return fail(87);const auto capacity=memory.load(GuestAddress{arg(1)},32);memory.store(GuestAddress{arg(1)},32,available?10:0);if(!available)return fail(2);if(capacity<10 || !arg(0))return fail(122);
        const auto text=wide?std::vector<std::uint8_t>{'W',0,'i',0,'n',0,'6',0,'4',0,' ',0,'P',0,'D',0,'F',0,0,0}:std::vector<std::uint8_t>{'W','i','n','6','4',' ','P','D','F',0};memory.write(GuestAddress{arg(0)},text);return finish(1);
    }
    if(name.starts_with("OpenPrinter")){
        // 「Win64 PDF,Job n」開啟本程序現有工作（進行中或佇列中）的工作控制代碼，供 ReadPrinter／SeekPrinter 讀取暫存資料。
        unsigned job_handle=0;
        if(arg(0) && available){auto text=string(arg(0));for(auto& c:text)if(c>=u'A' && c<=u'Z')c+=32;constexpr std::u16string_view prefix=u"win64 pdf,job ";
            if(text.starts_with(prefix) && text.size()>prefix.size() && text.size()<=prefix.size()+10){unsigned value=0;bool digits=true;for(auto c:text.substr(prefix.size())){if(c<u'0' || c>u'9'){digits=false;break;}value=value*10+static_cast<unsigned>(c-u'0');}
                if(!digits || !value || !print_job(value))return fail(1801);job_handle=value;}}
        if(!arg(1))return fail(87);memory.check(GuestAddress{arg(1)},unit,Access::Write);if(!available || (!job_handle && !printer_name(arg(0))))return fail(1801);if(printing_->handles.size()>=256)return fail(8);PrintingState::Device device;
        if(printing_->defaults)static_cast<PrinterSettings&>(device)=*printing_->defaults;device.read_job=job_handle;
        if(arg(2)){memory.check(GuestAddress{arg(2)},unit*2+4,Access::Read);const auto type=datatype(string(memory.load(GuestAddress{arg(2)},width)));const auto access=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)+unit*2},32));if(access&~0x000f000cU)return fail(5);if(!type.empty() && type!=u"RAW" && type!=u"TEXT")return fail(1804);if(!type.empty())device.default_datatype=device.datatype=type;device.access=access?access:8;if(const auto error=read_printer_settings(memory,memory.load(GuestAddress{arg(2)+unit},width),wide,device))return fail(error);}
        const auto handle=runtime_.allocate_wait_handle();printing_->handles.emplace(handle,std::move(device));memory.store(GuestAddress{arg(1)},width,handle);return finish(1);
    }
    if(name=="ClosePrinter"){
        if(const auto found=printing_->handles.find(arg(0));found!=printing_->handles.end() && found->second.active)printing_changed(0x400);
        if(!printing_->handles.erase(arg(0)))return fail(6);for(auto it=printing_->notices.begin();it!=printing_->notices.end();)if(it->second.printer==arg(0)){runtime_.wait_objects.erase(it->first);it=printing_->notices.erase(it);}else ++it;return finish(1);
    }
#include "windows_printer_forms.inc"
    if(name.starts_with("StartDocPrinter") || name=="StartPagePrinter" || name=="WritePrinter" || name=="EndPagePrinter" || name=="EndDocPrinter" || name=="AbortPrinter"){
        const auto found=printing_->handles.find(arg(0));if(found==printing_->handles.end())return fail(6);auto& device=found->second;
        if(!(device.access&8U) || device.read_job)return fail(5);
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
                if(device.cancelled)return write_failure(63);if(device.read_job)return write_failure(5);
                if(device.explicit_pages && !device.page)return write_failure(1906);const auto count=d(2);if(count && !arg(1))return write_failure(87);
                if(count>128*1024*1024-device.spool.size())return write_failure(8);
                // pcWritten 可與輸入重疊；完成資料讀取後才提交輸出計數。
                try{if(count){memory.check(GuestAddress{arg(1)},count,Access::Read);const auto previous=device.spool.size();device.spool.resize(previous+count);memory.read(GuestAddress{arg(1)},std::span(device.spool).subspan(previous));}}
                catch(const std::bad_alloc&){return write_failure(8);}catch(const std::length_error&){return write_failure(8);}
                device.bytes=device.spool.size();memory.store(GuestAddress{arg(3)},32,count);printing_changed(0x200);return finish(1);
            }
            if(device.page)return fail(1906);
            // SetJob 取消的工作不輸出；印表機或工作暫停時留在佇列，恢復後輸出。
            if(device.cancelled){discard();return finish(1);}
            if(device.spool_handle){(void)nested_service(state,memory,"kernel32.dll","CloseHandle",{device.spool_handle});device.spool_handle=0;}
            if(printing_->paused || (device.status&1U)){auto held=device;held.active=false;held.read_job=0;printing_->queue.push_back(std::move(held));discard();printing_changed(0x200);return finish(1);}
            if(const auto error=emit(device))return fail(error);discard();return finish(1);
        }catch(const std::bad_alloc&){return fail(8);}catch(const std::length_error&){return fail(8);}catch(const std::invalid_argument&){return fail(13);}catch(const std::domain_error&){return fail(50);}
    }
#include "windows_print_queue.inc"
    if(name=="FindClosePrinterChangeNotification"){
        if(!printing_->notices.erase(arg(0)))return fail(6);runtime_.wait_objects.erase(arg(0));return finish(1);
    }
    if(name=="FindNextPrinterChangeNotification"){
        poll_printer_form_notifications(true);
        const auto found=printing_->notices.find(arg(0));if(found==printing_->notices.end() || !runtime_.wait_objects.contains(arg(0)))return fail(6);if(arg(2) || arg(3))return fail(50);if(arg(1))memory.store(GuestAddress{arg(1)},32,found->second.changes);found->second.changes=0;runtime_.wait_objects.at(arg(0)).set_signal(false);return finish(1);
    }
    if(name=="FindFirstPrinterChangeNotification"){
        if(!printing_->handles.contains(arg(0)))return fail(6);if(d(2) || arg(3))return fail(50);if(!d(1) || (d(1)&~0x7777ffffU))return fail(87);if(printing_->notices.size()>=256)return fail(8);
        PrintingState::Notice notice;notice.printer=arg(0);notice.filter=d(1);
        if(notice.filter&0x70000U){RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)return fail(transaction.error);notice.forms=printer_forms::all(runtime_.registry);notice.revisions=printer_forms::revisions(runtime_.registry);}
        const auto handle=runtime_.allocate_wait_handle();WaitObject event;event.kind=WaitObject::Kind::Event;event.manual_reset=true;event.access=0x100000;runtime_.wait_objects.emplace(handle,event);printing_->notices.emplace(handle,std::move(notice));return finish(handle);
    }
    if(name.starts_with("DocumentProperties")){
        const auto found=printing_->handles.find(arg(1));if(found==printing_->handles.end())return fail(6);if(arg(2) && !printer_name(arg(2)))return fail(1801);if(!d(5))return finish(wide?220:156);if(d(5)&~10U)return fail(50);PrinterSettings device=found->second;
        if(d(5)&8){if(!arg(4))return fail(87);if(const auto error=read_printer_settings(memory,arg(4),wide,device))return fail(error);}if(d(5)&2){if(!arg(3))return fail(87);memory.write(GuestAddress{arg(3)},printer_devmode(wide,device));}return finish(1);
    }
    if(name.starts_with("DeviceCapabilities")){
        if(!available || !printer_name(arg(0)))return fail(1801);if(arg(1) && !string(arg(1)).empty() && string(arg(1))!=u"FILE:")return fail(1796);PrintingState::Device device;if(const auto error=read_printer_settings(memory,arg(4),wide,device))return fail(error);
        unsigned form_error=0;const auto forms=available_printer_settings(form_error);if(form_error)return fail(form_error);
        const auto capability=static_cast<unsigned>(arg(2));unsigned count=0;std::vector<std::uint8_t> bytes;
        const auto push=[&](unsigned value,unsigned size){for(unsigned i=0;i<size;++i)bytes.push_back(static_cast<std::uint8_t>(value>>(i*8)));};
        const auto fixed=[&](std::u16string_view text,unsigned length){if(wide){for(unsigned i=0;i<length;++i)push(i<text.size()?text[i]:0,2);}
            else{const auto bytes=utf16_to_utf8(text);for(unsigned i=0;i<length;++i)push(i<bytes.size()?static_cast<unsigned char>(bytes[i]):0,1);}};
        switch(capability){case 1:return finish(0x1ff1f);case 4:return finish(3|(3U<<16));case 5:return finish(32767|(32767U<<16));case 7:case 9:case 30:return finish(0);case 8:return finish(wide?220:156);case 10:return finish(0x401);case 11:case 15:case 22:case 32:return finish(1);case 18:return finish(99);case 17:return finish(90);
            case 2:for(const auto& form:forms)push(form.paper,2);count=static_cast<unsigned>(forms.size());break;
            case 3:for(const auto& form:forms){push((form.paper_width+50)/100,4);push((form.paper_height+50)/100,4);}count=static_cast<unsigned>(forms.size());break;
            case 6:push(7,2);count=1;break;case 12:fixed(u"Automatic",24);count=1;break;case 13:push(96,4);push(96,4);count=1;break;
            case 16:case 29:for(const auto& form:forms)fixed(form.form,64);count=static_cast<unsigned>(forms.size());break;case 35:push(1,4);count=1;break;default:return fail(50);}
        if(arg(3))memory.write(GuestAddress{arg(3)},bytes);return finish(count);
    }
    const bool enumeration=name.starts_with("EnumPrinters"),driver=name.starts_with("GetPrinterDriver"),job=name.starts_with("GetJob");
    if(!enumeration && !printing_->handles.contains(arg(0)))return fail(6);
    const auto level=static_cast<unsigned>(arg(enumeration || driver || job?2:1));const auto target=arg(enumeration || driver || job?3:2),capacity=static_cast<std::uint64_t>(d(enumeration || driver || job?4:3)),needed=arg(enumeration || driver || job?5:4);
    if(enumeration){if(!arg(6))return fail(87);memory.store(GuestAddress{arg(6)},32,0);if(level!=1 && level!=2 && level!=4 && level!=5)return fail(124);const auto flags=d(0);if(flags&~UINT64_C(0x0300007e))return fail(87);if(flags&8)return fail(50);if(level==4 && (flags&~6U))return fail(87);
        if((level==2 || level==5) && arg(1) && !string(arg(1)).empty())return fail(50);
        if(!available || !(flags&2) || (flags&(32|0x2000000))){PrinterBuffer empty(0,target,x86_,wide);return copy(empty,target,capacity,needed);}
    }
    if(driver){if(arg(1) && !string(arg(1)).empty() && string(arg(1))!=(x86_?u"Windows NT x86":u"Windows x64"))return fail(1805);if(level!=1 && level!=2)return fail(124);PrinterBuffer buffer(level==1?unit:unit*6,target,x86_,wide);driver_info(buffer,level);return copy(buffer,target,capacity,needed);}
    if(job){
        const auto jobs=all_jobs();const auto found=std::find_if(jobs.begin(),jobs.end(),[&](const auto* item){return item->job==d(1);});
        if(found==jobs.end())return fail(87);if(level!=1 && level!=2)return fail(124);
        PrinterBuffer buffer(level==1?(x86_?64:96):(x86_?104:160),target,x86_,wide);write_job(buffer,0,**found,level,static_cast<unsigned>(found-jobs.begin())+1);
        return copy(buffer,target,capacity,needed);
    }
    const unsigned jobs=static_cast<unsigned>(std::count_if(printing_->devices.begin(),printing_->devices.end(),[](const auto& item){return item.second.active;})+std::count_if(printing_->handles.begin(),printing_->handles.end(),[](const auto& item){return item.second.active;})+static_cast<std::ptrdiff_t>(printing_->queue.size()));unsigned size=0;
    switch(level){case 1:size=unit*4;break;case 2:size=x86_?84:136;break;case 4:size=unit*3;break;case 5:size=x86_?20:32;break;case 6:size=4;break;case 7:size=unit*2;break;case 8:case 9:size=unit;break;default:return fail(124);}
    PrinterBuffer buffer(size,target,x86_,wide);PrintingState::Device defaults;if(printing_->defaults)static_cast<PrinterSettings&>(defaults)=*printing_->defaults;
    const std::uint32_t printer_status=(printing_->paused?1U:0U)|printing_->status;
    switch(level){case 1:buffer.put(0,0x800000);buffer.text(unit,u"Win64 PDF,FILE:,CoreGraphics PDF");buffer.text(unit*2,u"Win64 PDF");buffer.text(unit*3,u"CoreGraphics PDF");break;
        case 2:buffer.text(unit,u"Win64 PDF");buffer.text(unit*3,u"FILE:");buffer.text(unit*4,u"Win64 PDF");buffer.text(unit*5,printing_->comment);if(!printing_->location.empty())buffer.text(unit*6,printing_->location);buffer.data(unit*7,printer_devmode(wide,defaults));buffer.text(unit*9,u"Win64 PDF");buffer.text(unit*10,u"RAW");buffer.put(unit*13,0x40);buffer.put(unit*13+4,printing_->priority);buffer.put(unit*13+8,printing_->default_priority);buffer.put(unit*13+20,printer_status);buffer.put(unit*13+24,jobs);break;
        case 4:buffer.text(0,u"Win64 PDF");buffer.put(unit*2,0x40);break;case 5:buffer.text(0,u"Win64 PDF");buffer.text(unit,u"FILE:");buffer.put(unit*2,0x40);break;case 6:buffer.put(0,printer_status);break;case 7:buffer.put(unit,4);break;case 8:case 9:buffer.data(0,printer_devmode(wide,defaults));break;default:break;}
    if(enumeration && capacity>=buffer.bytes.size() && target)memory.store(GuestAddress{arg(6)},32,1);return copy(buffer,target,capacity,needed);
}
// 印表機資料（登錄 HKLM\SYSTEM\CurrentControlSet\Control\Print\Printers\Win64 PDF\<鍵>；非 Ex 版本固定為 PrinterDriverData），經客體登錄 API 存取，
// 沿用登錄的權限、持久化與延後 I/O 契約。寫入與刪除需要 PRINTER_ACCESS_ADMINISTER；A 版名稱與字串型資料以 UTF-8（ACP）轉換。回傳 Win32 錯誤碼。
std::optional<std::uint32_t> WindowsServices::invoke_printer_data(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    return nested_continuation(state,memory,"印表機資料",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall call(s,m,service);const auto name=service.symbol;const bool wide=name.ends_with('W');const unsigned unit=x86_?4:8;
        const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
        std::vector<std::uint64_t> blocks;
        struct Release {WindowsServices& self;GuestMemory& m;GuestState& s;std::vector<std::uint64_t>& blocks;~Release(){for(const auto block:blocks)(void)self.free_heap(m,block,s);}} release{*this,m,s,blocks};
        const auto scratch=[&](std::size_t size)->std::uint64_t{const auto block=allocate_heap(m,std::max<std::size_t>(size,1));if(!block)throw std::bad_alloc{};m.fill(GuestAddress{block},0,std::max<std::size_t>(size,1));blocks.push_back(block);return block;};
        const auto put_text=[&](std::u16string_view text){const auto bytes=utf16_bytes(text);const auto block=scratch(bytes.size());m.write(GuestAddress{block},bytes);return block;};
        const auto string=[&](std::uint64_t at){return at?(wide?read_utf16(m,GuestAddress{at}):utf8_to_utf16(read_ansi(m,at))):std::u16string{};};
        const auto api=[&](std::string_view symbol,std::vector<std::uint64_t> arguments)->service_callback::Task<std::uint32_t>{
            co_return static_cast<std::uint32_t>(co_await service_callback::call_value(resolve("advapi32.dll",symbol).value,std::move(arguments)));};
        const auto handle=printing_?printing_->handles.find(call(0)):decltype(printing_->handles.end()){};
        if(!printing_ || handle==printing_->handles.end())co_return finish(6);
        const bool administer=(handle->second.access&4U)!=0;
        const bool ex=name.find("Ex")!=std::string_view::npos || name.starts_with("EnumPrinterKey") || name.starts_with("DeletePrinterKey");
        const auto key_name=ex?string(call(1)):std::u16string(u"PrinterDriverData");
        if(ex && !call(1))co_return finish(87);
        std::u16string path=u"SYSTEM\\CurrentControlSet\\Control\\Print\\Printers\\Win64 PDF";if(!key_name.empty())path+=u"\\"+key_name;
        const std::uint64_t machine=x86_?UINT64_C(0x80000002):UINT64_C(0xffffffff80000002);constexpr std::uint32_t wow64_64=0x100;
        // 寬字元登錄資料與 A 版之間的轉換：只轉字串型別（REG_SZ、REG_EXPAND_SZ、REG_MULTI_SZ）。
        const auto stringy=[](std::uint32_t type){return type==1 || type==2 || type==7;};
        const auto narrow_data=[&](std::uint32_t type,std::vector<std::uint8_t> data){
            if(wide || !stringy(type))return data;std::u16string text(data.size()/2,u'\0');for(std::size_t i=0;i<text.size();++i)text[i]=static_cast<char16_t>(data[i*2]|(data[i*2+1]<<8));
            const auto utf8=utf16_to_utf8(text);return std::vector<std::uint8_t>(utf8.begin(),utf8.end());};
        const auto value_bytes=[&](std::u16string_view text){std::vector<std::uint8_t> bytes;if(wide)bytes=utf16_bytes(text);else{const auto utf8=utf16_to_utf8(text);bytes.assign(utf8.begin(),utf8.end());bytes.push_back(0);}return bytes;};
        const auto open=[&](std::uint32_t access,bool create)->service_callback::Task<std::uint64_t>{
            const auto slot=scratch(8);const auto key_path=put_text(path);
            const auto error=create?co_await api("RegCreateKeyExW",{machine,key_path,0,0,0,access|wow64_64,0,slot,0}):co_await api("RegOpenKeyExW",{machine,key_path,0,access|wow64_64,slot});
            co_return error?0:m.load(GuestAddress{slot},x86_?32:64);};
        const auto close=[&](std::uint64_t key)->service_callback::Task<std::uint32_t>{co_return co_await api("RegCloseKey",{key});};
        struct Value {std::u16string name;std::uint32_t type{};std::vector<std::uint8_t> data;};
        // 依登錄列舉順序取得所有值（RegQueryInfoKeyW 取得最大長度後逐一 RegEnumValueW）。
        const auto values=[&](std::uint64_t key)->service_callback::Task<std::vector<Value>>{
            std::vector<Value> out;const auto info=scratch(64);
            if(co_await api("RegQueryInfoKeyW",{key,0,0,0,0,0,0,info,info+4,info+8,0,0}))co_return out;
            const auto count=static_cast<std::uint32_t>(m.load(GuestAddress{info},32)),longest_name=static_cast<std::uint32_t>(m.load(GuestAddress{info+4},32))+1,longest_data=static_cast<std::uint32_t>(m.load(GuestAddress{info+8},32));
            const auto name_buffer=scratch(std::size_t{longest_name}*2),data_buffer=scratch(longest_data),sizes=scratch(16);
            for(std::uint32_t i=0;i<count;++i){
                m.store(GuestAddress{sizes},32,longest_name);m.store(GuestAddress{sizes+4},32,longest_data);
                if(co_await api("RegEnumValueW",{key,i,name_buffer,sizes,0,sizes+8,data_buffer,sizes+4}))break;
                Value value;value.name=read_utf16(m,GuestAddress{name_buffer},longest_name);value.type=static_cast<std::uint32_t>(m.load(GuestAddress{sizes+8},32));
                value.data.resize(static_cast<std::size_t>(m.load(GuestAddress{sizes+4},32)));if(!value.data.empty())m.read(GuestAddress{data_buffer},value.data);
                out.push_back(std::move(value));
            }
            co_return out;};
        if(name.starts_with("GetPrinterData")){
            const auto value_name=string(call(ex?2:1));const auto type_out=call(ex?3:2),data_out=call(ex?4:3),needed=call(ex?6:5);const auto capacity=static_cast<std::uint32_t>(call(ex?5:4));
            if(!needed)co_return finish(87);
            const auto key=co_await open(0x20019,false);if(!key)co_return finish(2);
            std::uint32_t error=0;std::vector<std::uint8_t> data;std::uint32_t type=0;
            {const auto sizes=scratch(8);const auto value=put_text(value_name);
                error=co_await api("RegQueryValueExW",{key,value,0,sizes,0,sizes+4});
                if(!error){const auto size=static_cast<std::uint32_t>(m.load(GuestAddress{sizes+4},32));const auto buffer=scratch(size);
                    error=co_await api("RegQueryValueExW",{key,value,0,sizes,buffer,sizes+4});
                    if(!error){type=static_cast<std::uint32_t>(m.load(GuestAddress{sizes},32));data.resize(static_cast<std::size_t>(m.load(GuestAddress{sizes+4},32)));if(!data.empty())m.read(GuestAddress{buffer},data);}}}
            (void)co_await close(key);if(error)co_return finish(error);
            data=narrow_data(type,std::move(data));
            m.store(GuestAddress{needed},32,data.size());if(type_out)m.store(GuestAddress{type_out},32,type);
            if(capacity<data.size())co_return finish(234);if(!data.empty()){if(!data_out)co_return finish(87);m.write(GuestAddress{data_out},data);}
            co_return finish(0);
        }
        if(name.starts_with("SetPrinterData")){
            if(!administer)co_return finish(5);
            const auto value_name=string(call(ex?2:1));const auto type=static_cast<std::uint32_t>(call(ex?3:2));const auto data_in=call(ex?4:3);const auto size=static_cast<std::uint32_t>(call(ex?5:4));
            if(size && !data_in)co_return finish(87);
            std::vector<std::uint8_t> data(size);if(size)m.read(GuestAddress{data_in},data);
            if(!wide && stringy(type)){const auto text=utf8_to_utf16(std::string(data.begin(),data.end()));data=utf16_bytes(text);data.resize(text.size()*2);
                if(text.empty() || text.back()!=u'\0'){data.push_back(0);data.push_back(0);}}
            const auto key=co_await open(0x2001f,true);if(!key)co_return finish(5);
            const auto buffer=scratch(data.size());if(!data.empty())m.write(GuestAddress{buffer},data);
            const auto error=co_await api("RegSetValueExW",{key,put_text(value_name),0,type,buffer,data.size()});(void)co_await close(key);
            if(!error)printing_changed(0x4);co_return finish(error);
        }
        if(name.starts_with("DeletePrinterData") || name.starts_with("DeletePrinterKey")){
            if(!administer)co_return finish(5);
            if(name.starts_with("DeletePrinterKey")){if(key_name.empty())co_return finish(87);co_return finish(co_await api("RegDeleteTreeW",{machine,put_text(path)}));}
            const auto key=co_await open(0x2001f,false);if(!key)co_return finish(2);
            const auto error=co_await api("RegDeleteValueW",{key,put_text(string(call(ex?2:1)))});(void)co_await close(key);co_return finish(error);
        }
        if(name.starts_with("EnumPrinterKey")){
            // 子鍵名稱以 MULTI_SZ 回傳（每個名稱以 NUL 結尾，最後再一個 NUL）。
            const auto output=call(2),needed=call(4);const auto capacity=static_cast<std::uint32_t>(call(3));if(!needed)co_return finish(87);
            const auto key=co_await open(0x20019,false);if(!key)co_return finish(2);
            std::vector<std::u16string> names;const auto buffer=scratch(512),size=scratch(4);
            for(std::uint32_t i=0;;++i){m.store(GuestAddress{size},32,256);if(co_await api("RegEnumKeyExW",{key,i,buffer,size,0,0,0,0}))break;names.push_back(read_utf16(m,GuestAddress{buffer},256));}
            (void)co_await close(key);
            std::vector<std::uint8_t> bytes;for(const auto& item:names){const auto encoded=value_bytes(item);bytes.insert(bytes.end(),encoded.begin(),encoded.end());}
            bytes.resize(bytes.size()+(wide?2:1),0);
            m.store(GuestAddress{needed},32,bytes.size());if(capacity<bytes.size())co_return finish(234);if(!output)co_return finish(87);m.write(GuestAddress{output},bytes);co_return finish(0);
        }
        const auto key=co_await open(0x20019,false);if(!key)co_return finish(2);
        auto all=co_await values(key);(void)co_await close(key);
        if(name.starts_with("EnumPrinterDataEx")){
            // PRINTER_ENUM_VALUES 陣列（pValueName、cbValueName、dwType、pData、cbData）後接名稱與資料。
            const auto output=call(2),needed=call(4),count=call(5);const auto capacity=static_cast<std::uint32_t>(call(3));if(!needed || !count)co_return finish(87);
            const unsigned record=x86_?20:32;PrinterBuffer buffer(static_cast<unsigned>(all.size())*record,output,x86_,wide);
            for(std::size_t i=0;i<all.size();++i){
                const auto at=static_cast<unsigned>(i)*record;const auto encoded=value_bytes(all[i].name);const auto data=narrow_data(all[i].type,all[i].data);
                buffer.data(at,encoded);buffer.put(at+unit,encoded.size());buffer.put(at+unit+4,all[i].type);buffer.data(at+unit+8,data);buffer.put(at+2*unit+8,data.size());
            }
            m.store(GuestAddress{needed},32,buffer.bytes.size());m.store(GuestAddress{count},32,0);
            if(capacity<buffer.bytes.size())co_return finish(234);if(!buffer.bytes.empty()){if(!output)co_return finish(87);m.write(GuestAddress{output},buffer.bytes);}
            m.store(GuestAddress{count},32,all.size());co_return finish(0);
        }
        // EnumPrinterData：cbValueName 與 cbData 都是 0 時回報最大名稱與資料大小。
        const auto index=static_cast<std::uint32_t>(call(1));const auto name_out=call(2),name_needed=call(4),type_out=call(5),data_out=call(6),data_needed=call(8);
        const auto name_capacity=static_cast<std::uint32_t>(call(3)),data_capacity=static_cast<std::uint32_t>(call(7));
        if(!name_needed || !data_needed)co_return finish(87);
        if(!name_capacity && !data_capacity){
            std::size_t longest_name=0,longest_data=0;for(const auto& value:all){longest_name=std::max(longest_name,value_bytes(value.name).size());longest_data=std::max(longest_data,narrow_data(value.type,value.data).size());}
            if(all.empty())co_return finish(259);
            m.store(GuestAddress{name_needed},32,longest_name);m.store(GuestAddress{data_needed},32,longest_data);co_return finish(0);
        }
        if(index>=all.size())co_return finish(259);
        const auto encoded=value_bytes(all[index].name);const auto data=narrow_data(all[index].type,all[index].data);
        m.store(GuestAddress{name_needed},32,encoded.size());m.store(GuestAddress{data_needed},32,data.size());
        if(name_capacity<encoded.size() || data_capacity<data.size())co_return finish(234);
        if(!name_out || (!data.empty() && !data_out))co_return finish(87);
        m.write(GuestAddress{name_out},encoded);if(!data.empty())m.write(GuestAddress{data_out},data);if(type_out)m.store(GuestAddress{type_out},32,all[index].type);
        co_return finish(0);
    });
}
}
