#include "usb.h"
#include "xhci.h"

#include "../include/kprintf.h"

static struct usb_controller g_usb_controllers[USB_MAX_CONTROLLERS];
static uint32_t g_usb_controller_count = 0;

static const char *usb_hc_name(enum usb_hc_kind kind) {
    switch (kind) {
    case USB_HC_UHCI:
        return "UHCI";
    case USB_HC_OHCI:
        return "OHCI";
    case USB_HC_EHCI:
        return "EHCI";
    case USB_HC_XHCI:
        return "xHCI";
    default:
        return "UNKNOWN";
    }
}

static enum usb_hc_kind usb_hc_kind_from_prog_if(uint8_t prog_if) {
    switch (prog_if) {
    case 0x00:
        return USB_HC_UHCI;
    case 0x10:
        return USB_HC_OHCI;
    case 0x20:
        return USB_HC_EHCI;
    case 0x30:
        return USB_HC_XHCI;
    default:
        return USB_HC_UNKNOWN;
    }
}

// Dispatch per-controller initialization. Legacy controllers are currently
// tracked as present; xHCI gets a deeper bring-up path in xhci.c.
static void usb_try_init_controller(struct usb_controller *ctrl) {
    if (!ctrl) {
        return;
    }

    if (!ctrl->bar0.valid) {
        kprintf("USB: %s at %x:%x.%x has no usable BAR0\n",
                usb_hc_name(ctrl->kind),
                (unsigned int)ctrl->pci.bus,
                (unsigned int)ctrl->pci.device,
                (unsigned int)ctrl->pci.function);
        return;
    }

    if (!ctrl->bar0.is_mmio) {
        kprintf("USB: %s at %x:%x.%x uses I/O BAR0=%x (legacy mode)\n",
                usb_hc_name(ctrl->kind),
                (unsigned int)ctrl->pci.bus,
                (unsigned int)ctrl->pci.device,
                (unsigned int)ctrl->pci.function,
                (unsigned int)ctrl->bar0.address);
        ctrl->initialized = true;
        return;
    }

    kprintf("USB: %s at %x:%x.%x MMIO=%p size=%u irq=%u/%u\n",
            usb_hc_name(ctrl->kind),
            (unsigned int)ctrl->pci.bus,
            (unsigned int)ctrl->pci.device,
            (unsigned int)ctrl->pci.function,
            (void *)(uintptr_t)ctrl->bar0.address,
            (unsigned int)ctrl->bar0.size,
            (unsigned int)ctrl->irq_line,
            (unsigned int)ctrl->irq_pin);

    if (ctrl->kind == USB_HC_XHCI) {
        ctrl->initialized = xhci_init_controller(ctrl);
        return;
    }

    ctrl->initialized = true;
}

// Discover USB host controllers through PCI class/subclass matching:
// class 0x0C (Serial Bus), subclass 0x03 (USB). ProgIF selects HC flavor.
static void usb_discover_controllers(void) {
    g_usb_controller_count = 0;

    uint32_t n = pci_devices_count();
    for (uint32_t i = 0; i < n; i++) {
        const struct pci_device *dev = pci_get_device(i);
        if (!dev) {
            continue;
        }

        if (dev->class_code != 0x0C || dev->subclass != 0x03) {
            continue;
        }

        if (g_usb_controller_count >= USB_MAX_CONTROLLERS) {
            kprintf("USB: controller table full, ignoring extra devices\n");
            break;
        }

        struct usb_controller *ctrl = &g_usb_controllers[g_usb_controller_count++];
        ctrl->kind = usb_hc_kind_from_prog_if(dev->prog_if);
        ctrl->pci = *dev;
        ctrl->bar0 = pci_read_bar(dev->bus, dev->device, dev->function, 0);
        ctrl->mmio_virt = 0;
        ctrl->mmio_size = 0;
        ctrl->irq_line = pci_read8(dev->bus, dev->device, dev->function, PCI_INTERRUPT_LINE);
        ctrl->irq_pin = pci_read8(dev->bus, dev->device, dev->function, PCI_INTERRUPT_PIN);
        ctrl->max_ports = 0;
        ctrl->devices_present = 0;
        ctrl->initialized = false;

        // DMA-capable controllers need bus mastering enabled before runtime use.
        pci_enable_bus_mastering(dev->bus, dev->device, dev->function);
    }
}

    // Public USB subsystem entry point called during kernel boot.
void usb_init(void) {
    kprintf("\n=== USB host controller discovery ===\n");

    usb_discover_controllers();

    if (g_usb_controller_count == 0) {
        kprintf("USB: no host controllers found\n");
        return;
    }

    for (uint32_t i = 0; i < g_usb_controller_count; i++) {
        usb_try_init_controller(&g_usb_controllers[i]);
        if (g_usb_controllers[i].initialized) {
            kprintf("USB:   %s %x:%x.%x ports=%u present=%u\n",
                    usb_hc_name(g_usb_controllers[i].kind),
                    (unsigned int)g_usb_controllers[i].pci.bus,
                    (unsigned int)g_usb_controllers[i].pci.device,
                    (unsigned int)g_usb_controllers[i].pci.function,
                    (unsigned int)g_usb_controllers[i].max_ports,
                    (unsigned int)g_usb_controllers[i].devices_present);
        }
    }

    uint32_t initialized = 0;
    for (uint32_t i = 0; i < g_usb_controller_count; i++) {
        if (g_usb_controllers[i].initialized) {
            initialized++;
        }
    }

    kprintf("USB: %u controller(s), %u initialized\n",
            (unsigned int)g_usb_controller_count,
            (unsigned int)initialized);

    // Enumeration (above) used synchronous busy-polling. Now that it's done,
    // switch xHCI to interrupt-driven servicing so HID input no longer needs the
    // main loop to poll. (No-op if a controller has no usable legacy IRQ line.)
    xhci_enable_irq();
}

uint32_t usb_controller_count(void) {
    return g_usb_controller_count;
}

const struct usb_controller *usb_get_controller(uint32_t index) {
    if (index >= g_usb_controller_count) {
        return NULL;
    }
    return &g_usb_controllers[index];
}
