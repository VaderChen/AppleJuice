#include "x64arm/windows_services.hpp"
#include "x64arm/windows_raw_input.hpp"
#include "x64arm/unicode.hpp"
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
}
std::span<const WindowsServiceDescriptor> hid_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"hid.dll","HidD_GetHidGuid",1},{"hid.dll","HidD_GetAttributes",2},
        {"hid.dll","HidD_GetManufacturerString",3},{"hid.dll","HidD_GetProductString",3},
        {"hid.dll","HidD_GetSerialNumberString",3},{"hid.dll","HidD_GetPreparsedData",2},
        {"hid.dll","HidD_FreePreparsedData",1},{"hid.dll","HidP_GetCaps",2},
        {"hid.dll","HidP_GetButtonCaps",4},{"hid.dll","HidP_GetValueCaps",4},
        {"hid.dll","HidP_GetData",6},{"hid.dll","HidP_MaxDataListLength",2},
        {"hid.dll","HidP_SetUsageValue",8},{"hid.dll","HidP_SetUsages",8},
        {"hid.dll","HidP_GetUsageValue",8},{"hid.dll","HidP_GetUsages",8},
        {"hid.dll","HidP_GetScaledUsageValue",8},{"hid.dll","HidP_GetUsagesEx",7},
        {"hid.dll","HidP_InitializeReportForID",5},
        {"hid.dll","HidD_GetFeature",3},{"hid.dll","HidD_SetFeature",3},{"hid.dll","HidD_GetInputReport",3},{"hid.dll","HidD_SetOutputReport",3},
        {"hid.dll","HidD_GetIndexedString",4},{"hid.dll","HidP_GetSpecificButtonCaps",7},{"hid.dll","HidP_GetSpecificValueCaps",7}};return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_hid(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
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
                if(name=="HidD_GetAttributes"){
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
        }else if(name=="HidP_GetUsageValue" || name=="HidP_SetUsageValue" || name=="HidP_GetScaledUsageValue"){
            const auto layout=layout_at(arg(5));const bool write=name=="HidP_SetUsageValue";auto report=report_at(6,7,layout,d(0),write);unsigned element=0;
            const auto field=layout.find(d(0),{static_cast<std::uint16_t>(d(1)),static_cast<std::uint16_t>(d(3))},static_cast<std::uint16_t>(d(2)),report[0],false,&element);if(!field)throw hid::Error(hid::usage_not_found);
            if(write){layout.value(*field,element,d(4),report);memory.write(GuestAddress{arg(6)},report);}
            else memory.store(GuestAddress{arg(4)},32,name=="HidP_GetScaledUsageValue"?static_cast<std::uint32_t>(layout.scaled_value(*field,element,report)):layout.value(*field,element,report));
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
    }catch(const hid::Error& error){if(parser)result=name=="HidP_MaxDataListLength"?0:error.status;else fail(87);}
    catch(const MemoryFault&){if(parser)result=hid::invalid_preparsed;else fail(998);}
    catch(const std::bad_alloc&){if(parser)result=0xc0000017U;else fail(8);}
    arg.return_to(state,result);return std::nullopt;
}
}
