#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct UsbDescriptorReader {
    const void* context;
    uint8_t (*byte)(const void*,uint64_t);
    uint64_t maximum;
} UsbDescriptorReader;

typedef struct UsbDescriptor {
    uint64_t address;
    uint8_t length,type;
} UsbDescriptor;

/* USB 描述元資料不要求自然對齊。客體與宿主路徑共用界限與欄位判斷。 */
static inline bool usb_descriptor_range(UsbDescriptorReader reader,uint64_t base,uint64_t length) {
    return base && base<=reader.maximum && length && length-1<=reader.maximum-base;
}

static inline uint8_t usb_descriptor_byte(UsbDescriptorReader reader,uint64_t address) {
    return reader.byte(reader.context,address);
}

static inline uint16_t usb_descriptor_word(UsbDescriptorReader reader,uint64_t address) {
    return (uint16_t)(usb_descriptor_byte(reader,address) | (uint16_t)usb_descriptor_byte(reader,address+1)<<8);
}

static inline bool usb_descriptor_at(UsbDescriptorReader reader,uint64_t base,uint32_t length,
                                     uint32_t offset,UsbDescriptor* descriptor) {
    if(offset>length || length-offset<2)return false;
    const uint64_t address=base+offset;
    const uint8_t size=usb_descriptor_byte(reader,address);
    if(size<2 || size>length-offset)return false;
    descriptor->address=address;
    descriptor->length=size;
    descriptor->type=usb_descriptor_byte(reader,address+1);
    return true;
}

static inline uint16_t usb_configuration_length(UsbDescriptorReader reader,uint64_t base) {
    if(!usb_descriptor_range(reader,base,4))return 0;
    const uint8_t header=usb_descriptor_byte(reader,base);
    if(header<9 || usb_descriptor_byte(reader,base+1)!=2)return 0;
    const uint16_t length=usb_descriptor_word(reader,base+2);
    return length>=header && usb_descriptor_range(reader,base,length)?length:0;
}

static inline uint64_t usb_find_descriptor(UsbDescriptorReader reader,uint64_t base,uint32_t length,
                                          uint64_t start,int32_t type) {
    if(type<0 || type>255 || !usb_descriptor_range(reader,base,length))return 0;
    if(start>=base && start-base>=length)return 0;
    for(uint32_t offset=0;offset<length;){
        UsbDescriptor descriptor;
        if(!usb_descriptor_at(reader,base,length,offset,&descriptor))break;
        if(descriptor.address>=start && descriptor.type==type)return descriptor.address;
        offset+=descriptor.length;
    }
    return 0;
}

static inline bool usb_field_matches(UsbDescriptorReader reader,uint64_t address,int32_t selected) {
    return selected==-1 || (selected>=0 && selected<=255 && usb_descriptor_byte(reader,address)==selected);
}

static inline uint64_t usb_find_interface(UsbDescriptorReader reader,uint64_t base,uint64_t start,
                                         int32_t number,int32_t alternate,int32_t device_class,
                                         int32_t subclass,int32_t protocol) {
    const uint16_t length=usb_configuration_length(reader,base);
    if(!length || (start>=base && start-base>=length))return 0;
    for(uint32_t offset=0;offset<length;){
        UsbDescriptor descriptor;
        if(!usb_descriptor_at(reader,base,length,offset,&descriptor))break;
        const uint64_t address=descriptor.address;
        if(address>=start && descriptor.type==4 && descriptor.length>=9 &&
           usb_field_matches(reader,address+2,number) && usb_field_matches(reader,address+3,alternate) &&
           usb_field_matches(reader,address+5,device_class) && usb_field_matches(reader,address+6,subclass) &&
           usb_field_matches(reader,address+7,protocol))return address;
        offset+=descriptor.length;
    }
    return 0;
}

static inline uint32_t usb_interface_length(UsbDescriptorReader reader,uint64_t base,uint64_t end) {
    if(end<=base || end-base>UINT32_MAX || !usb_descriptor_range(reader,base,end-base))return 0;
    const uint32_t length=(uint32_t)(end-base);
    UsbDescriptor descriptor;
    if(!usb_descriptor_at(reader,base,length,0,&descriptor) || descriptor.type!=4 || descriptor.length<9)return 0;
    uint32_t offset=descriptor.length;
    while(offset<length){
        if(!usb_descriptor_at(reader,base,length,offset,&descriptor))return 0;
        if(descriptor.type==4)break;
        offset+=descriptor.length;
    }
    return offset;
}

/* USBDI 3.00 採用 USB 2.00 的傳統 URB 與頻寬契約。 */
enum {usbdi_version=0x0300,usbdi_specification=0x0200};

static inline uint32_t usb_calculate_legacy_bandwidth(uint32_t packet,uint8_t type,uint8_t low_speed) {
    if(type!=1 && type!=3)return 0;
    const uint32_t overhead=type==1?9:13;
    const uint32_t bits=(packet+overhead)*56U/6U;
    return low_speed?bits*8U:bits;
}

typedef struct UsbConfigurationValidation {uint32_t status;uint64_t address;} UsbConfigurationValidation;

static inline UsbConfigurationValidation usb_configuration_error(uint32_t status,uint64_t address) {
    UsbConfigurationValidation result={status,address};
    return result;
}

static inline UsbConfigurationValidation usb_validate_configuration(UsbDescriptorReader reader,uint64_t base,
                                                                     uint32_t size,uint16_t level) {
    if(!base || level<1 || level>3)return usb_configuration_error(0x80000300U,base);
    if(size<9 || !usb_descriptor_range(reader,base,size))return usb_configuration_error(0xc0100006U,base);
    const uint8_t header=usb_descriptor_byte(reader,base);
    if(header<9 || (level==3 && header!=9))return usb_configuration_error(0xc0100001U,base);
    if(usb_descriptor_byte(reader,base+1)!=2)return usb_configuration_error(0xc0100002U,base);
    const uint32_t length=usb_descriptor_word(reader,base+2);
    if(length<header || length>size)return usb_configuration_error(0xc0100006U,base);
    if(level==1)return usb_configuration_error(0,0);

    const uint8_t expected_interfaces=usb_descriptor_byte(reader,base+4);
    uint32_t seen[8]={0,0,0,0,0,0,0,0};
    uint32_t interfaces=0,endpoints=0,endpoint_mask=0,expected_endpoints=0;
    uint32_t number=UINT32_MAX,alternate=UINT32_MAX;
    uint64_t current_interface=0;
    for(uint32_t offset=header;offset<length;){
        UsbDescriptor descriptor;
        if(!usb_descriptor_at(reader,base,length,offset,&descriptor))
            return usb_configuration_error(0xc0100001U,base+offset);
        const uint64_t address=descriptor.address;
        switch(descriptor.type){
        case 4: {
            if(descriptor.length<9 || (level==3 && descriptor.length!=9))return usb_configuration_error(0xc0100003U,address);
            if(level==3 && current_interface && endpoints!=expected_endpoints)return usb_configuration_error(0xc0100008U,current_interface);
            const uint32_t next_number=usb_descriptor_byte(reader,address+2);
            const uint32_t next_alternate=usb_descriptor_byte(reader,address+3);
            if(next_number!=number){
                const uint32_t bit=UINT32_C(1)<<(next_number%32);
                if(next_alternate || (seen[next_number/32]&bit) || (level==3 && next_number!=interfaces))
                    return usb_configuration_error(0xc0100003U,address);
                seen[next_number/32]|=bit;
                ++interfaces;
            }else if(next_alternate<=alternate){
                return usb_configuration_error(0xc0100003U,address);
            }
            number=next_number;
            alternate=next_alternate;
            current_interface=address;
            expected_endpoints=usb_descriptor_byte(reader,address+4);
            endpoints=endpoint_mask=0;
            break;
        }
        case 5: {
            if(!current_interface || descriptor.length<7 || (level==3 && descriptor.length!=7 && descriptor.length!=9))
                return usb_configuration_error(0xc0100004U,address);
            const uint8_t endpoint=usb_descriptor_byte(reader,address+2);
            if((endpoint&0x70U) || !(endpoint&0x0fU))return usb_configuration_error(0xc0100009U,address);
            const uint32_t bit=UINT32_C(1)<<((endpoint&0x0fU)|((endpoint>>3)&0x10U));
            if(endpoint_mask&bit)return usb_configuration_error(0xc0100009U,address);
            endpoint_mask|=bit;
            ++endpoints;
            break;
        }
        case 11:
            if(descriptor.length<8 || (level==3 && descriptor.length!=8))return usb_configuration_error(0xc0100005U,address);
            if(!usb_descriptor_byte(reader,address+3) ||
               (unsigned)usb_descriptor_byte(reader,address+2)+usb_descriptor_byte(reader,address+3)>256)
                return usb_configuration_error(0xc0100005U,address);
            break;
        case 48: case 49: {
            const uint8_t required=descriptor.type==48?6:8;
            if(!endpoints || descriptor.length<required || (level==3 && descriptor.length!=required))
                return usb_configuration_error(0xc0100004U,address);
            break;
        }
        case 1: case 2:
            return usb_configuration_error(0xc0100002U,address);
        default:
            break; // 類別與廠商自訂格式仍受共用長度檢查。
        }
        offset+=descriptor.length;
    }
    if(level==3 && current_interface && endpoints!=expected_endpoints)return usb_configuration_error(0xc0100008U,current_interface);
    if(interfaces!=expected_interfaces)return usb_configuration_error(0xc0100007U,base);
    return usb_configuration_error(0,0);
}
