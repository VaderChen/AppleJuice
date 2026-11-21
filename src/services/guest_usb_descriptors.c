#include "usb_descriptor_operations.h"

#if defined(__i386__)
#define USB_CALL __attribute__((stdcall))
#else
#define USB_CALL
#endif
#define USB_EXPORT(return_type) __declspec(dllexport) return_type USB_CALL

static uint8_t guest_byte(const void* context,uint64_t address) {
    (void)context;
    return *(const uint8_t*)(uintptr_t)address;
}

static UsbDescriptorReader guest_reader(void) {
    const UsbDescriptorReader reader={0,guest_byte,UINTPTR_MAX};
    return reader;
}

USB_EXPORT(void*) WinUsb_ParseDescriptors(const void* buffer,uint32_t length,const void* start,int32_t type) {
    return (void*)(uintptr_t)usb_find_descriptor(guest_reader(),(uintptr_t)buffer,length,(uintptr_t)start,type);
}

USB_EXPORT(void*) WinUsb_ParseConfigurationDescriptor(const void* configuration,const void* start,int32_t number,
    int32_t alternate,int32_t device_class,int32_t subclass,int32_t protocol) {
    return (void*)(uintptr_t)usb_find_interface(guest_reader(),(uintptr_t)configuration,(uintptr_t)start,
                                               number,alternate,device_class,subclass,protocol);
}

USB_EXPORT(void*) USBD_ParseConfigurationDescriptor(const void* configuration,uint8_t number,uint8_t alternate) {
    return (void*)(uintptr_t)usb_find_interface(guest_reader(),(uintptr_t)configuration,(uintptr_t)configuration,
                                               number==UINT8_MAX?-1:number,alternate==UINT8_MAX?-1:alternate,-1,-1,-1);
}

USB_EXPORT(uint32_t) USBD_GetInterfaceLength(const void* descriptor,const void* end) {
    return usb_interface_length(guest_reader(),(uintptr_t)descriptor,(uintptr_t)end);
}

USB_EXPORT(void) USBD_GetUSBDIVersion(uint32_t* version) {
    if(version){
        version[0]=usbdi_version;
        version[1]=usbdi_specification;
    }
}

USB_EXPORT(uint32_t) USBD_CalculateUsbBandwidth(uint32_t packet,uint8_t type,uint8_t low_speed) {
    return usb_calculate_legacy_bandwidth(packet,type,low_speed);
}

USB_EXPORT(uint32_t) USBD_ValidateConfigurationDescriptor(const void* configuration,uint32_t length,uint16_t level,
    uint8_t** failure_address,uint32_t tag) {
    (void)tag;
    const UsbConfigurationValidation validation=usb_validate_configuration(guest_reader(),(uintptr_t)configuration,length,level);
    if(failure_address)*failure_address=(uint8_t*)(uintptr_t)validation.address;
    return validation.status;
}
