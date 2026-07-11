#ifndef __USB_H__
#define __USB_H__

#include "drivers/bus/pci.h"
#include "include/types.h"
#include <stdint.h>

#define USB_MAX_CONTROLLERS 16

enum usb_hc_kind {
    USB_HC_UHCI = 0,
    USB_HC_OHCI = 1,
    USB_HC_EHCI = 2,
    USB_HC_XHCI = 3,
    USB_HC_UNKNOWN = 255,
};

struct usb_controller {
    enum usb_hc_kind kind;
    struct pci_device pci;
    struct pci_bar bar0;
    uint64_t mmio_virt;
    uint64_t mmio_size;
    uint8_t irq_line;
    uint8_t irq_pin;
    uint8_t max_ports;
    uint8_t devices_present;
    bool initialized;
};

struct usb_setup_packet {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed));

struct usb_device_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
} __attribute__((packed));

struct usb_config_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower;
} __attribute__((packed));

struct usb_interface_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} __attribute__((packed));

struct usb_endpoint_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
} __attribute__((packed));

#define USB_DESC_DEVICE 0x01
#define USB_DESC_CONFIG 0x02
#define USB_DESC_STRING 0x03
#define USB_DESC_INTERFACE 0x04
#define USB_DESC_ENDPOINT 0x05

#define USB_REQ_GET_STATUS 0x00
#define USB_REQ_CLEAR_FEATURE 0x01
#define USB_REQ_SET_FEATURE 0x03
#define USB_REQ_SET_ADDRESS 0x05
#define USB_REQ_GET_DESCRIPTOR 0x06
#define USB_REQ_SET_DESCRIPTOR 0x07
#define USB_REQ_GET_CONFIGURATION 0x08
#define USB_REQ_SET_CONFIGURATION 0x09

void usb_init(void);
void usb_poll(void);
uint32_t usb_controller_count(void);
const struct usb_controller *usb_get_controller(uint32_t index);

// Selftest: cross-checks controller discovery/classification against a
// fresh PCI scan. Returns 0 on success, -1 on failure. Does not exercise
// live HID/mass-storage transfers — see usb.c's comment on the function.
int usb_run_selftests(void);

#endif /* __USB_H__ */
