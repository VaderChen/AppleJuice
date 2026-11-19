#include "x64arm/windows_services.hpp"
#include "x64arm/windows_raw_input.hpp"
#include "x64arm/unicode.hpp"
#include "service_callback_task.hpp"
#include <algorithm>
#include <map>

namespace x64arm {
namespace {
void put(std::span<std::uint8_t> bytes,unsigned at,std::uint64_t value,unsigned size=4){for(unsigned i=0;i<size;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(i*8));}
struct Button {const hid::Field* field;unsigned element;};
std::vector<Button> pressed(const hid::Layout& layout,unsigned type,std::span<const std::uint8_t> report){
    std::vector<Button> result;
    for(const auto& field:layout.fields)if(field.type==type && field.report==report[0] && field.button){
        for(unsigned i=0;i<field.count;++i){const auto value=layout.value(field,i,report);
            if(field.variable()){if(value)result.push_back({&field,i});}
            else {const auto index=static_cast<std::int64_t>(value)-field.minimum;if(index>=0 && static_cast<std::uint64_t>(index)<field.usages.size() && field.usages[static_cast<unsigned>(index)].value){
                    const auto element=static_cast<unsigned>(index);if(std::none_of(result.begin(),result.end(),[&](const auto& item){return item.field==&field && item.element==element;}))result.push_back({&field,element});}}
        }
    }
    return result;
}
// 鍵盤頁（0x07）用法對 i8042 Set 1 掃描碼：E0 前綴以 0xe000 標示，0 表示沒有對應。
// 依 USB HID 用法表與 PS/2 Set 1 的標準對照；PrintScreen（0x46）與 Pause（0x48）另依修飾鍵處理。
constexpr std::uint16_t keyboard_codes[256]{
    0,0xff,0,0,0x1e,0x30,0x2e,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,                     // 00-0f
    0x32,0x31,0x18,0x19,0x10,0x13,0x1f,0x14,0x16,0x2f,0x11,0x2d,0x15,0x2c,0x02,0x03,          // 10-1f
    0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x1c,0x01,0x0e,0x0f,0x39,0x0c,0x0d,0x1a,          // 20-2f
    0x1b,0x2b,0x2b,0x27,0x28,0x29,0x33,0x34,0x35,0x3a,0x3b,0x3c,0x3d,0x3e,0x3f,0x40,          // 30-3f
    0x41,0x42,0x43,0x44,0x57,0x58,0,0x46,0,0xe052,0xe047,0xe049,0xe053,0xe04f,0xe051,0xe04d,  // 40-4f
    0xe04b,0xe050,0xe048,0x45,0xe035,0x37,0x4a,0x4e,0xe01c,0x4f,0x50,0x51,0x4b,0x4c,0x4d,0x47,// 50-5f
    0x48,0x49,0x52,0x53,0x56,0xe05d,0xe05e,0x59,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x6b,      // 60-6f
    0x6c,0x6d,0x6e,0x76,0,0,0,0,0,0,0,0,0,0,0,0xe020,                                         // 70-7f
    0xe030,0xe02e,0,0,0,0x7e,0,0x73,0x70,0x7d,0x79,0x7b,0x5c,0,0,0,                            // 80-8f
    0xf2,0xf1,0x78,0x77,0x76,0,0,0,0,0,0,0,0,0,0,0,                                            // 90-9f
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,                          // a0-bf
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,                          // c0-df
    0x1d,0x2a,0x38,0xe05b,0xe01d,0x36,0xe038,0xe05c};                                          // e0-e7
// 依 HIDP_KEYBOARD_MODIFIER_STATE 產生一個鍵的掃描碼；LANG1／LANG2 與 Pause 沒有放開碼。
std::vector<std::uint8_t> scan_codes(unsigned usage,bool make,std::uint32_t modifiers){
    const bool control=modifiers&0x11U,shift=modifiers&0x22U,alt=modifiers&0x44U;
    if(usage==0x48){
        if(!make)return {};
        if(control)return {0xe0,0x46,0xe0,0xc6};
        return {0xe1,0x1d,0x45,0xe1,0x9d,0xc5};
    }
    if(usage==0x46){
        if(alt)return {static_cast<std::uint8_t>(make?0x54:0xd4)};
        if(control || shift)return make?std::vector<std::uint8_t>{0xe0,0x37}:std::vector<std::uint8_t>{0xe0,0xb7};
        return make?std::vector<std::uint8_t>{0xe0,0x2a,0xe0,0x37}:std::vector<std::uint8_t>{0xe0,0xb7,0xe0,0xaa};
    }
    const std::uint32_t code=usage<256?keyboard_codes[usage]:0U;if(!code)throw hid::Error(hid::i8042_trans_unknown);
    if(code==0xf1 || code==0xf2 || code==0xff)return make?std::vector<std::uint8_t>{static_cast<std::uint8_t>(code)}:std::vector<std::uint8_t>{};
    const auto base=static_cast<std::uint8_t>((code&0x7fU)|(make?0U:0x80U));
    if(code&0xe000U)return {0xe0,base};
    return {base};
}
}
std::span<const WindowsServiceDescriptor> hid_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"hid.dll","HidD_GetHidGuid",1},{"hid.dll","HidD_GetAttributes",2},
        {"hid.dll","HidD_GetPhysicalDescriptor",3,0,0,0x4},
        {"hid.dll","HidD_GetNumInputBuffers",2},{"hid.dll","HidD_SetNumInputBuffers",2},{"hid.dll","HidD_FlushQueue",1},
        {"hid.dll","HidD_GetManufacturerString",3},{"hid.dll","HidD_GetProductString",3},
        {"hid.dll","HidD_GetSerialNumberString",3},{"hid.dll","HidD_GetPreparsedData",2},
        {"hid.dll","HidD_FreePreparsedData",1},{"hid.dll","HidP_GetCaps",2},
        {"hid.dll","HidP_GetButtonCaps",4},{"hid.dll","HidP_GetValueCaps",4},
        {"hid.dll","HidP_GetData",6},{"hid.dll","HidP_MaxDataListLength",2},
        {"hid.dll","HidP_SetUsageValue",8},{"hid.dll","HidP_SetUsages",8},
        {"hid.dll","HidP_GetUsageValue",8},{"hid.dll","HidP_GetUsages",8},
        {"hid.dll","HidP_GetScaledUsageValue",8},{"hid.dll","HidP_GetUsagesEx",7},
        {"hid.dll","HidP_SetScaledUsageValue",8},
        {"hid.dll","HidP_GetUsageValueArray",9},{"hid.dll","HidP_SetUsageValueArray",9},
        {"hid.dll","HidP_InitializeReportForID",5},
        {"hid.dll","HidD_GetFeature",3},{"hid.dll","HidD_SetFeature",3},{"hid.dll","HidD_GetInputReport",3},{"hid.dll","HidD_SetOutputReport",3},
        {"hid.dll","HidD_GetIndexedString",4},{"hid.dll","HidP_GetSpecificButtonCaps",7},{"hid.dll","HidP_GetSpecificValueCaps",7},
        {"hid.dll","HidP_GetLinkCollectionNodes",3},{"hid.dll","HidP_MaxUsageListLength",3},{"hid.dll","HidP_SetData",6},{"hid.dll","HidP_UnsetUsages",8},
        {"hid.dll","HidP_TranslateUsagesToI8042ScanCodes",6}};return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_hid(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(service.symbol=="HidP_TranslateUsagesToI8042ScanCodes")
        // (ChangedUsageList, UsageListLength, KeyAction, ModifierState, InsertCodesProcedure, Context)：每個鍵依序呼叫一次插入回呼（cdecl）。
        return nested_continuation(state,memory,"HID i8042",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);const auto finish=[&](std::uint32_t status){call.return_to(s,status);return std::optional<std::uint32_t>{};};
            std::uint64_t buffer=0;
            struct Release {WindowsServices& self;GuestMemory& m;GuestState& s;std::uint64_t& block;~Release(){if(block)(void)self.free_heap(m,block,s);}} cleanup{*this,m,s,buffer};
            try{
                const auto count=static_cast<std::uint32_t>(call(1));const bool make=static_cast<std::uint32_t>(call(2))==1;const auto state_at=call(3),procedure=call(4),context=call(5);
                if(static_cast<std::uint32_t>(call(2))>1 || !state_at || !procedure || (count && !call(0)))co_return finish(0xc000000dU); // STATUS_INVALID_PARAMETER
                std::vector<std::uint16_t> usages(count);for(std::uint32_t i=0;i<count;++i)usages[i]=static_cast<std::uint16_t>(m.load(GuestAddress{call(0)+i*2},16));
                auto modifiers=static_cast<std::uint32_t>(m.load(GuestAddress{state_at},32));
                buffer=allocate_heap(m,8);if(!buffer)co_return finish(0xc0000017U);
                for(const auto usage:usages){
                    if(!usage)continue;
                    const auto codes=scan_codes(usage,make,modifiers);
                    // 修飾鍵依按下／放開設定；CapsLock、ScrollLock、NumLock 在按下時切換。
                    if(usage>=0xe0 && usage<=0xe7){const auto bit=1U<<(usage-0xe0);modifiers=make?modifiers|bit:modifiers&~bit;}
                    else if(make && (usage==0x39 || usage==0x47 || usage==0x53))modifiers^=usage==0x39?0x100U:usage==0x47?0x200U:0x400U;
                    m.store(GuestAddress{state_at},32,modifiers);
                    if(codes.empty())continue;
                    m.write(GuestAddress{buffer},codes);
                    (void)co_await service_callback::call_value(procedure,{context,buffer,codes.size()});
                }
                co_return finish(hid::success);
            }catch(const hid::Error& error){co_return finish(error.status);}
            catch(const MemoryFault&){co_return finish(0xc0000005U);}
        });
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const auto d=[&](unsigned n){return static_cast<std::uint32_t>(arg(n));};const auto bits=static_cast<std::uint8_t>(x86_?32:64);
    const bool parser=name.starts_with("HidP_");std::uint32_t result=parser?hid::success:0;
    const auto fail=[&](std::uint32_t error){last_error_=error;result=0;};
    const auto layout_at=[&](std::uint64_t address){if(!address)throw hid::Error(hid::invalid_preparsed);std::array<std::uint8_t,20> header{};memory.read(GuestAddress{address},header);const auto size=hid::preparsed_size(header);std::vector<std::uint8_t> data(size);memory.read(GuestAddress{address},data);return hid::decode(data);};
    const auto report_at=[&](unsigned pointer,unsigned length,const hid::Layout& layout,unsigned type,bool write){
        if(d(length)>output_limit || !arg(pointer))throw hid::Error(hid::invalid_length);if(write)memory.check(GuestAddress{arg(pointer)},d(length),Access::Write);
        std::vector<std::uint8_t> data(d(length));memory.read(GuestAddress{arg(pointer)},data);layout.validate(type,data);return data;
    };
    try{
        if(name=="HidD_GetHidGuid"){
            memory.write(GuestAddress{arg(0)},hid_interface_guid);
        }else if(name=="HidD_FreePreparsedData"){
            if(!raw_input_ || !raw_input_->owned_preparsed.contains(arg(0)))fail(87);
            else {raw_input_->owned_preparsed.erase(arg(0));result=free_heap(memory,arg(0),state);}
        }else if(!parser){
            const auto file=files_.find(arg(0));
            if(file==files_.end() || !file->second.hid)fail(6);
            else if(file->second.hid->disconnected || !file->second.hid->backend->available(file->second.hid->device.instance))fail(1167);
            else {const auto& device=file->second.hid->device;
                if(name=="HidD_GetNumInputBuffers"){
                    if(!arg(1))fail(87);
                    else {memory.store(GuestAddress{arg(1)},32,file->second.hid->input_buffer_count);process_io::record(process_io::Kind::Other,4);result=1;}
                }else if(name=="HidD_SetNumInputBuffers"){
                    if(!file->second.hid->set_input_buffer_count(d(1)))fail(87);
                    else {process_io::record(process_io::Kind::Other,0);result=1;}
                }else if(name=="HidD_FlushQueue"){
                    file->second.hid->reports.clear();process_io::record(process_io::Kind::Other,0);result=1;
                }else if(name=="HidD_GetAttributes"){
                    if(!arg(1) || memory.load(GuestAddress{arg(1)},32)!=12)fail(87);
                    else {std::array<std::uint8_t,12> data{};put(data,0,12);put(data,4,device.vendor,2);put(data,6,device.product_id,2);put(data,8,device.version,2);memory.write(GuestAddress{arg(1)},data);process_io::record(process_io::Kind::Other,data.size());result=1;}
                }else if(name=="HidD_GetPreparsedData"){
                    if(!arg(1))fail(87);
                    else {memory.check(GuestAddress{arg(1)},x86_?4:8,Access::Write);memory.store(GuestAddress{arg(1)},bits,0);
                        const auto data=hid::preparsed(device.hid_descriptor,{device.usage_page,device.usage});const auto address=allocate_heap(memory,data.size());
                        if(!address)fail(8);else {try{memory.write(GuestAddress{address},data);if(!raw_input_)raw_input_=std::make_shared<RawInputState>();raw_input_->owned_preparsed.insert(address);memory.store(GuestAddress{arg(1)},bits,address);process_io::record(process_io::Kind::Other,data.size());result=1;}catch(...){if(raw_input_)raw_input_->owned_preparsed.erase(address);(void)free_heap(memory,address,state);throw;}}}
                }else if(name=="HidD_GetFeature" || name=="HidD_SetFeature" || name=="HidD_GetInputReport" || name=="HidD_SetOutputReport"){
                    // 與 IOCTL_HID_GET/SET_FEATURE、GET_INPUT_REPORT、SET_OUTPUT_REPORT 相同：長度等於該類報告最大長度，首位元組為 report ID。
                    const bool feature=name.ends_with("Feature"),write=name.starts_with("HidD_Set");const unsigned type=feature?2U:write?1U:0U;
                    if(!arg(1) || d(2)>output_limit)fail(87);
                    else {std::vector<std::uint8_t> report(d(2));memory.read(GuestAddress{arg(1)},report);file->second.hid->layout.validate(type,report);if(!write)memory.check(GuestAddress{arg(1)},report.size(),Access::Write);
                        const auto error=write?file->second.hid->backend->set_report(device.instance,type,report):file->second.hid->backend->get_report(device.instance,type,report);
                        if(error)fail(error);else {if(!write)memory.write(GuestAddress{arg(1)},report);process_io::record(process_io::Kind::Other,report.size());result=1;}}
                }else if(name=="HidD_GetPhysicalDescriptor"){
                    if(!arg(1) || !d(2))fail(87);
                    else {memory.check(GuestAddress{arg(1)},d(2),Access::Write);std::vector<std::uint8_t> bytes(std::min<std::uint32_t>(d(2),UINT16_MAX));
                        const auto error=file->second.hid->physical_descriptor(bytes);
                        if(error)fail(error);else {memory.write(GuestAddress{arg(1)},bytes);process_io::record(process_io::Kind::Other,bytes.size());result=1;}}
                }else if(name=="HidD_GetIndexedString"){
                    // 只回答裝置登錄的 iManufacturer／iProduct／iSerialNumber；其他索引與裝置不回應時相同，回報 ERROR_GEN_FAILURE。
                    const auto index=d(1);const std::string* text=nullptr;
                    if(index)for(unsigned i=0;i<3;++i)if(device.string_indices[i]==index){text=i==0?&device.manufacturer:i==1?&device.name:&device.serial;break;}
                    if(!text || text->empty())fail(31);else {const auto data=utf16_bytes(utf8_to_utf16(*text));if(!arg(2) || d(3)<data.size())fail(122);else {memory.write(GuestAddress{arg(2)},data);process_io::record(process_io::Kind::Other,data.size());result=1;}}
                }else {const auto& text=name=="HidD_GetProductString"?device.name:name=="HidD_GetManufacturerString"?device.manufacturer:device.serial;
                    if(text.empty())fail(31);else {const auto data=utf16_bytes(utf8_to_utf16(text));if(!arg(1) || d(2)<data.size())fail(122);else {memory.write(GuestAddress{arg(1)},data);process_io::record(process_io::Kind::Other,data.size());result=1;}}}
            }
        }else if(name=="HidP_GetCaps"){const auto output=layout_at(arg(0)).caps();memory.write(GuestAddress{arg(1)},output);}
        else if(name=="HidP_MaxDataListLength"){const auto layout=layout_at(arg(1));if(d(0)>=3)throw hid::Error(hid::invalid_type);std::array<unsigned,256> counts{};
            for(const auto& field:layout.fields)if(field.type==d(0)){
                if(field.button)counts[field.report]+=field.variable()?field.count:std::min<unsigned>(field.count,static_cast<unsigned>(std::count_if(field.usages.begin(),field.usages.end(),[](const auto& u){return u.value!=0;})));
                else for(unsigned n=0;n<field.count;){const auto count=field.value_count(n);if(count==1)++counts[field.report];n+=count;}}
            result=*std::max_element(counts.begin(),counts.end());}
        else if(name=="HidP_GetButtonCaps" || name=="HidP_GetValueCaps"){
            const auto layout=layout_at(arg(3));const auto caps=layout.caps(d(0),name=="HidP_GetButtonCaps");if(!arg(2))throw hid::Error(hid::buffer_too_small);
            memory.check(GuestAddress{arg(2)},2,Access::Write);const auto capacity=memory.load(GuestAddress{arg(2)},16);memory.store(GuestAddress{arg(2)},16,caps.size());
            if(capacity<caps.size() || (!arg(1) && !caps.empty()))throw hid::Error(hid::buffer_too_small);
            std::vector<std::uint8_t> data;for(const auto& cap:caps)data.insert(data.end(),cap.begin(),cap.end());memory.write(GuestAddress{arg(1)},data);
        }else if(name=="HidP_GetSpecificButtonCaps" || name=="HidP_GetSpecificValueCaps"){
            // UsagePage、LinkCollection、Usage 為 0 表示不限制；範圍項目以 UsageMin..UsageMax 比對。
            const auto layout=layout_at(arg(6));const auto all=layout.caps(d(0),name=="HidP_GetSpecificButtonCaps");std::vector<std::array<std::uint8_t,72>> caps;
            const auto u16=[](const std::array<std::uint8_t,72>& cap,unsigned at){return static_cast<unsigned>(cap[at]|(cap[at+1]<<8));};
            for(const auto& cap:all){if(d(1)&0xffffU && u16(cap,0)!=(d(1)&0xffffU))continue;if(d(2)&0xffffU && u16(cap,6)!=(d(2)&0xffffU))continue;
                if(const auto usage=d(3)&0xffffU){const auto low=u16(cap,56),high=cap[12]?u16(cap,58):low;if(usage<low || usage>high)continue;}caps.push_back(cap);}
            if(!arg(5))throw hid::Error(hid::buffer_too_small);memory.check(GuestAddress{arg(5)},2,Access::Write);const auto capacity=memory.load(GuestAddress{arg(5)},16);
            if(caps.empty()){memory.store(GuestAddress{arg(5)},16,0);throw hid::Error(hid::usage_not_found);}
            memory.store(GuestAddress{arg(5)},16,std::min<std::uint64_t>(capacity,caps.size()));if(!arg(4))throw hid::Error(hid::buffer_too_small);
            std::vector<std::uint8_t> data;for(std::size_t i=0;i<caps.size() && i<capacity;++i)data.insert(data.end(),caps[i].begin(),caps[i].end());if(!data.empty())memory.write(GuestAddress{arg(4)},data);
            if(capacity<caps.size())throw hid::Error(hid::buffer_too_small);
        }else if(name=="HidP_InitializeReportForID"){
            const auto layout=layout_at(arg(2));if(d(0)>=3)throw hid::Error(hid::invalid_type);if(d(1)>255 || !layout.report_bytes[d(0)][d(1)])throw hid::Error(hid::incompatible_id);
            if(d(4)!=layout.bytes(d(0)) || !arg(3))throw hid::Error(hid::invalid_length);std::vector<std::uint8_t> data(d(4));data[0]=static_cast<std::uint8_t>(d(1));memory.write(GuestAddress{arg(3)},data);
        }else if(name=="HidP_GetData"){
            const auto layout=layout_at(arg(3));const auto report=report_at(4,5,layout,d(0),false);std::map<unsigned,std::uint32_t> values;
            for(const auto& button:pressed(layout,d(0),report))values[button.field->index+button.element]=1;
            for(const auto& field:layout.fields)if(field.type==d(0) && field.report==report[0] && !field.button)for(unsigned n=0;n<field.count;){const auto count=field.value_count(n);if(count==1)values[field.data_index(n)]=layout.value(field,n,report);n+=count;}
            if(!arg(2))throw hid::Error(hid::buffer_too_small);memory.check(GuestAddress{arg(2)},4,Access::Write);const auto capacity=memory.load(GuestAddress{arg(2)},32);memory.store(GuestAddress{arg(2)},32,values.size());
            if(capacity<values.size() || (!arg(1) && !values.empty()))throw hid::Error(hid::buffer_too_small);
            std::vector<std::uint8_t> data(values.size()*8);unsigned i=0;for(const auto [index,value]:values){put(data,i*8,index,2);put(data,i*8+4,value);++i;}memory.write(GuestAddress{arg(1)},data);
        }else if(name=="HidP_GetUsageValueArray" || name=="HidP_SetUsageValueArray"){
            const auto layout=layout_at(arg(6));const bool write=name=="HidP_SetUsageValueArray";auto report=report_at(7,8,layout,d(0),write);unsigned element=0;
            const auto field=layout.find(d(0),{static_cast<std::uint16_t>(d(1)),static_cast<std::uint16_t>(d(3))},static_cast<std::uint16_t>(d(2)),report[0],false,&element);
            if(!field)throw hid::Error(hid::usage_not_found);
            const auto capacity=static_cast<std::uint16_t>(d(5));if(!arg(4))throw hid::Error(hid::buffer_too_small);
            const auto count=field->value_count(element);const auto length=(std::uint64_t{field->width}*count+7)/8;
            if(field->range || !field->variable() || count<=1)throw hid::Error(hid::not_value_array);
            if(length>capacity)throw hid::Error(hid::buffer_too_small);
            std::vector<std::uint8_t> values(static_cast<std::size_t>(length));
            if(write){memory.read(GuestAddress{arg(4)},values);layout.set_value_array(*field,element,report,values);memory.write(GuestAddress{arg(7)},report);}
            else {memory.check(GuestAddress{arg(4)},values.size(),Access::Write);layout.get_value_array(*field,element,report,values);memory.write(GuestAddress{arg(4)},values);}
        }else if(name=="HidP_GetUsageValue" || name=="HidP_SetUsageValue" || name=="HidP_GetScaledUsageValue" || name=="HidP_SetScaledUsageValue"){
            const auto layout=layout_at(arg(5));const bool write=name=="HidP_SetUsageValue" || name=="HidP_SetScaledUsageValue";auto report=report_at(6,7,layout,d(0),write);unsigned element=0;
            const auto field=layout.find(d(0),{static_cast<std::uint16_t>(d(1)),static_cast<std::uint16_t>(d(3))},static_cast<std::uint16_t>(d(2)),report[0],false,&element);if(!field)throw hid::Error(hid::usage_not_found);
            if(write){if(name=="HidP_SetScaledUsageValue")result=layout.set_scaled_value(*field,element,static_cast<std::int32_t>(d(4)),report);
                else layout.value(*field,element,d(4),report);memory.write(GuestAddress{arg(6)},report);}
            else memory.store(GuestAddress{arg(4)},32,name=="HidP_GetScaledUsageValue"?static_cast<std::uint32_t>(layout.scaled_value(*field,element,report)):layout.value(*field,element,report));
        }else if(name=="HidP_GetLinkCollectionNodes"){
            // (Nodes, Length, Preparsed)：容量不足時回報需要的數量。HIDP_LINK_COLLECTION_NODE 為 x86 20、x64 24 位元組（UserContext 填 NULL）。
            const auto layout=layout_at(arg(2));if(!arg(1))throw hid::Error(hid::buffer_too_small);
            memory.check(GuestAddress{arg(1)},4,Access::Write);const auto capacity=memory.load(GuestAddress{arg(1)},32);memory.store(GuestAddress{arg(1)},32,layout.nodes.size());
            if(capacity<layout.nodes.size() || !arg(0))throw hid::Error(hid::buffer_too_small);
            const unsigned size=x86_?20U:24U;std::vector<std::uint8_t> data(layout.nodes.size()*size);
            for(unsigned i=0;i<layout.nodes.size();++i){const auto& n=layout.nodes[i];const auto at=i*size;
                put(data,at,n.usage.value,2);put(data,at+2,n.usage.page,2);put(data,at+4,n.parent,2);put(data,at+6,n.children,2);put(data,at+8,n.next_sibling,2);put(data,at+10,n.first_child,2);put(data,at+12,n.type&0xffU);}
            memory.write(GuestAddress{arg(0)},data);
        }else if(name=="HidP_MaxUsageListLength"){
            // GetUsages 對該報告類型與用法頁（0 為全部）最多能回傳的用法數：variable 按鈕每個用法一個，array 按鈕為報告數量。
            const auto layout=layout_at(arg(2));if(d(0)>=3)throw hid::Error(hid::invalid_type);const auto page=d(1)&0xffffU;unsigned total=0;
            for(const auto& field:layout.fields)if(field.type==d(0) && field.button){
                if(field.variable()){for(const auto& usage:field.usages)if(!page || usage.page==page)++total;}
                else if(std::any_of(field.usages.begin(),field.usages.end(),[&](const auto& usage){return !page || usage.page==page;}))total+=field.count;
            }
            result=total;
        }else if(name=="HidP_SetData"){
            // HIDP_DATA{DataIndex, Reserved, RawValue／On}；依資料索引設定按鈕或數值。失敗時 *DataLength 改為出錯項目的索引，之前的項目已寫入報告。
            const auto layout=layout_at(arg(3));auto report=report_at(4,5,layout,d(0),true);
            if(!arg(2))throw hid::Error(hid::buffer_too_small);memory.check(GuestAddress{arg(2)},4,Access::Write);const auto count=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)},32));
            if(count>65536 || (count && !arg(1)))throw hid::Error(hid::buffer_too_small);
            std::vector<std::uint8_t> list(static_cast<std::size_t>(count)*8);if(count)memory.read(GuestAddress{arg(1)},list);
            const auto set_item=[&](unsigned index,std::uint32_t raw)->std::uint32_t{
                for(const auto& field:layout.fields){
                    if(field.type!=d(0) || index<field.index || index>=field.index+field.indices())continue;
                    if(field.report!=report[0])return hid::incompatible_id;
                    const auto offset=index-field.index;
                    if(field.button){
                        const bool on=(raw&0xffU)!=0;
                        if(field.variable()){layout.value(field,offset,on?1U:0U,report);return hid::success;}
                        // array 按鈕：邏輯值為用法索引加上最小值；放開時移除，按下時放進第一個空位。
                        const auto value=static_cast<std::uint32_t>(static_cast<std::int64_t>(offset)+field.minimum);
                        for(unsigned n=0;n<field.count;++n)if(layout.value(field,n,report)==value){if(!on)layout.value(field,n,0,report);return hid::success;}
                        if(!on)return hid::button_not_pressed;
                        for(unsigned n=0;n<field.count;++n)if(!layout.value(field,n,report)){layout.value(field,n,value,report);return hid::success;}
                        return hid::buffer_too_small;
                    }
                    for(unsigned n=0;n<field.count;){const auto span=field.value_count(n);
                        if(field.data_index(n)==index){if(span>1)return hid::is_value_array;layout.value(field,n,raw,report);return hid::success;}n+=span;}
                }
                return hid::data_index_not_found;
            };
            std::uint32_t item=0,status=hid::success;
            for(;item<count;++item){
                const auto at=item*8;const auto index=static_cast<unsigned>(list[at]|(list[at+1]<<8));
                const auto raw=static_cast<std::uint32_t>(list[at+4])|(static_cast<std::uint32_t>(list[at+5])<<8)|(static_cast<std::uint32_t>(list[at+6])<<16)|(static_cast<std::uint32_t>(list[at+7])<<24);
                try{status=set_item(index,raw);}catch(const hid::Error& error){status=error.status;}
                if(status!=hid::success)break;
            }
            memory.write(GuestAddress{arg(4)},report);
            if(status!=hid::success){memory.store(GuestAddress{arg(2)},32,item);throw hid::Error(status);}
        }else if(name=="HidP_UnsetUsages"){
            // 與 HidP_SetUsages 對稱：variable 按鈕清為 0；array 按鈕移除對應的邏輯值，未按下時 HIDP_STATUS_BUTTON_NOT_PRESSED；用法 0 略過。
            const auto layout=layout_at(arg(5));auto report=report_at(6,7,layout,d(0),true);
            if(!arg(4))throw hid::Error(hid::buffer_too_small);memory.check(GuestAddress{arg(4)},4,Access::Write);const auto count=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(4)},32));
            if(count>65536 || (count && !arg(3)))throw hid::Error(hid::buffer_too_small);
            for(unsigned i=0;i<count;++i){
                const auto usage=static_cast<std::uint16_t>(memory.load(GuestAddress{arg(3)+i*2},16));if(!usage)continue;unsigned element=0;
                const auto field=layout.find(d(0),{static_cast<std::uint16_t>(d(1)),usage},d(2)&0xffffU,report[0],true,&element);if(!field)throw hid::Error(hid::usage_not_found);
                if(field->variable()){layout.value(*field,element,0,report);continue;}
                const auto value=static_cast<std::uint32_t>(static_cast<std::int64_t>(element)+field->minimum);bool cleared=false;
                for(unsigned n=0;n<field->count;++n)if(layout.value(*field,n,report)==value){layout.value(*field,n,0,report);cleared=true;}
                if(!cleared)throw hid::Error(hid::button_not_pressed);
            }
            memory.write(GuestAddress{arg(6)},report);
        }else if(name=="HidP_GetUsagesEx"){
            const auto layout=layout_at(arg(4));const auto report=report_at(5,6,layout,d(0),false);
            const auto link=static_cast<std::uint16_t>(d(1));
            bool any=false,current=false;
            for(const auto& field:layout.fields)if(field.type==d(0) && field.button && (!link || field.link==link)){any=true;if(field.report==report[0])current=true;}
            if(any && !current)throw hid::Error(hid::incompatible_id);
            if(!arg(3))throw hid::Error(hid::buffer_too_small);
            memory.check(GuestAddress{arg(3)},4,Access::Write);const auto capacity=memory.load(GuestAddress{arg(3)},32);
            std::vector<hid::Usage> usages;
            for(const auto& button:pressed(layout,d(0),report))if(!link || button.field->link==link)usages.push_back(button.field->usages[button.element]);
            memory.store(GuestAddress{arg(3)},32,usages.size());
            if(capacity<usages.size() || (!arg(2) && !usages.empty()))throw hid::Error(hid::buffer_too_small);
            std::vector<std::uint8_t> data(usages.size()*4);
            for(unsigned i=0;i<usages.size();++i){put(data,i*4,usages[i].value,2);put(data,i*4+2,usages[i].page,2);}
            if(!data.empty())memory.write(GuestAddress{arg(2)},data);
        }else if(name=="HidP_GetUsages" || name=="HidP_SetUsages"){
            const auto layout=layout_at(arg(5));const bool write=name=="HidP_SetUsages";auto report=report_at(6,7,layout,d(0),write);
            if(!arg(4))throw hid::Error(hid::buffer_too_small);memory.check(GuestAddress{arg(4)},4,Access::Write);const auto count=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(4)},32));
            if(write){if(count>65536 || (count && !arg(3)))throw hid::Error(hid::buffer_too_small);
                for(unsigned i=0;i<count;++i){const auto usage=static_cast<std::uint16_t>(memory.load(GuestAddress{arg(3)+i*2},16));unsigned element=0;
                    const auto field=layout.find(d(0),{static_cast<std::uint16_t>(d(1)),usage},d(2),report[0],true,&element);if(!field)throw hid::Error(hid::usage_not_found);
                    if(field->variable())layout.value(*field,element,1,report);
                    else {bool set=false;const auto value=static_cast<std::uint32_t>(static_cast<std::int64_t>(element)+field->minimum);for(unsigned n=0;n<field->count;++n)if(layout.value(*field,n,report)==value){set=true;break;}
                        if(!set)for(unsigned n=0;n<field->count;++n){const auto current=layout.value(*field,n,report);if(!current){layout.value(*field,n,value,report);set=true;break;}}if(!set)throw hid::Error(hid::buffer_too_small);}
                }memory.write(GuestAddress{arg(6)},report);
            }else {bool any=false,current=false;for(const auto& field:layout.fields)if(field.type==d(0) && field.button && (!d(2) || field.link==d(2)) && std::any_of(field.usages.begin(),field.usages.end(),[&](const auto& u){return u.page==d(1);})){any=true;if(field.report==report[0])current=true;}
                if(!any)throw hid::Error(hid::usage_not_found);if(!current)throw hid::Error(hid::incompatible_id);
                std::vector<std::uint16_t> usages;for(const auto& button:pressed(layout,d(0),report)){const auto& field=*button.field;const auto usage=field.usages[button.element];if(usage.page==d(1) && (!d(2) || field.link==d(2)))usages.push_back(usage.value);}
                memory.store(GuestAddress{arg(4)},32,usages.size());if(count<usages.size() || (!arg(3) && !usages.empty()))throw hid::Error(hid::buffer_too_small);
                std::vector<std::uint8_t> data(usages.size()*2);for(unsigned i=0;i<usages.size();++i)put(data,i*2,usages[i],2);memory.write(GuestAddress{arg(3)},data);}
        }
    }catch(const hid::Error& error){if(parser)result=name=="HidP_MaxDataListLength" || name=="HidP_MaxUsageListLength"?0:error.status;else fail(87);}
    catch(const MemoryFault&){if(parser)result=name=="HidP_MaxDataListLength" || name=="HidP_MaxUsageListLength"?0:hid::invalid_preparsed;else fail(998);}
    catch(const std::bad_alloc&){if(parser)result=0xc0000017U;else fail(8);}
    arg.return_to(state,result);return std::nullopt;
}
}
