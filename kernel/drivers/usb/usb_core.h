#ifndef __USB_CORE_H__
#define __USB_CORE_H__

// HCD-agnostic USB device layer.
//
// A host controller driver (UHCI/OHCI/EHCI; xHCI keeps its own historical
// path) detects a connected port, resets it, then hands the new device to
// usb_enumerate(). The core runs the standard enumeration dance and attaches
// a class driver (HID boot keyboard, mass storage) using only the transfer
// ops the HCD provides. Everything is synchronous/polled except interrupt-IN
// endpoints, which use a submit + poll pair driven from usb_core_poll().

#include "drivers/usb/usb.h"
#include <stdint.h>

#define USB_SPEED_LOW   0
#define USB_SPEED_FULL  1
#define USB_SPEED_HIGH  2

#define USB_MAX_DEVICES     16
#define USB_MAX_ENDPOINTS   8

// Transfer status codes (negative). Positive/zero = bytes transferred.
#define USB_ERR_TIMEOUT  (-1)
#define USB_ERR_STALL    (-2)
#define USB_ERR_IO       (-3)
#define USB_ERR_PENDING  (-4)   // int_poll: no data yet

struct usb_device;

struct usb_hcd_ops {
    // Synchronous control transfer on EP0. `data` holds wLength bytes
    // (IN: filled by the device; OUT: sent to it; may be NULL if wLength=0).
    // Returns bytes moved in the data stage, or a USB_ERR_* code.
    int (*control)(struct usb_device *dev,
                   const struct usb_setup_packet *setup, void *data);

    // Synchronous bulk transfer. ep_addr bit7 = direction (0x8n IN, 0x0n OUT).
    // len is capped by the HCD's bounce buffer (>= 4096 bytes).
    int (*bulk)(struct usb_device *dev, uint8_t ep_addr,
                void *data, uint32_t len);

    // Arm a non-blocking interrupt-IN transfer of `len` bytes.
    int (*int_submit)(struct usb_device *dev, uint8_t ep_addr, uint32_t len);

    // Check the armed transfer: bytes copied to buf when complete,
    // USB_ERR_PENDING while still running, other USB_ERR_* on failure.
    int (*int_poll)(struct usb_device *dev, uint8_t ep_addr,
                    void *buf, uint32_t len);
};

struct usb_ep_info {
    uint8_t  addr;       // bEndpointAddress
    uint8_t  attr;       // bmAttributes (transfer type in bits 1:0)
    uint16_t mps;
    uint8_t  interval;
};

struct usb_device {
    bool in_use;
    const struct usb_hcd_ops *ops;
    void *hc;         // controller-instance state (HCD-owned)
    void *hc_priv;    // per-device HCD state (HCD-owned)

    uint8_t addr;     // assigned USB address (0 while defaulting)
    uint8_t speed;    // USB_SPEED_*
    uint8_t ep0_mps;

    uint16_t vid, pid;
    uint8_t  dev_class;
    uint8_t  if_class, if_subclass, if_protocol;

    struct usb_ep_info eps[USB_MAX_ENDPOINTS];
    uint8_t num_eps;

    // Data-toggle state per endpoint number, one bit each way (owned by the
    // core, used by UHCI/OHCI/EHCI to seed their TDs).
    uint16_t toggle_in;
    uint16_t toggle_out;

    // HID state
    bool    hid_active;
    uint8_t hid_kind;      // 0 = boot keyboard, 1 = absolute pointer (tablet)
    uint8_t hid_ep;
    uint16_t hid_mps;
    uint8_t prev_keys[6];
};

#define USB_HID_KEYBOARD 0
#define USB_HID_TABLET   1

static inline uint8_t usb_toggle_get(struct usb_device *dev, uint8_t ep_addr) {
    uint16_t map = (ep_addr & 0x80) ? dev->toggle_in : dev->toggle_out;
    return (map >> (ep_addr & 0x0F)) & 1;
}

static inline void usb_toggle_set(struct usb_device *dev, uint8_t ep_addr,
                                  uint8_t val) {
    uint16_t bit = (uint16_t)1 << (ep_addr & 0x0F);
    uint16_t *map = (ep_addr & 0x80) ? &dev->toggle_in : &dev->toggle_out;
    if (val) *map |= bit; else *map &= (uint16_t)~bit;
}

// Allocate a device slot for an HCD (returns NULL when the table is full).
struct usb_device *usb_alloc_device(const struct usb_hcd_ops *ops,
                                    void *hc, void *hc_priv, uint8_t speed);
void usb_free_device(struct usb_device *dev);

// Standard control request helper.
int usb_control(struct usb_device *dev, uint8_t bmRequestType,
                uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                void *data, uint16_t wLength);

// Full enumeration + class-driver attach. Returns 0 on success.
int usb_enumerate(struct usb_device *dev);

// Drive interrupt endpoints (HID keyboards) — call from the main loop.
void usb_core_poll(void);

// Find an endpoint of the given transfer type/direction on the device.
// type: 2 = bulk, 3 = interrupt; dir_in selects bit7 of the address.
const struct usb_ep_info *usb_find_ep(struct usb_device *dev,
                                      uint8_t type, bool dir_in);

#endif /* __USB_CORE_H__ */
