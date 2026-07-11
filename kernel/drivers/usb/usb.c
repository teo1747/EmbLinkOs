#include "drivers/usb/usb.h"
#include "drivers/usb/xhci.h"
#include "drivers/usb/uhci.h"
#include "drivers/usb/ohci.h"
#include "drivers/usb/ehci.h"
#include "drivers/usb/usb_core.h"

#include "include/kprintf.h"

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

    // UHCI keeps its registers in an I/O BAR4, so an empty BAR0 is normal
    // there; every other controller type needs a valid MMIO BAR0.
    if (ctrl->kind != USB_HC_UHCI && !ctrl->bar0.valid) {
        kprintf("USB: %s at %x:%x.%x has no usable BAR0\n",
                usb_hc_name(ctrl->kind),
                (unsigned int)ctrl->pci.bus,
                (unsigned int)ctrl->pci.device,
                (unsigned int)ctrl->pci.function);
        return;
    }

    kprintf("USB: %s at %x:%x.%x BAR0=%p size=%u irq=%u/%u\n",
            usb_hc_name(ctrl->kind),
            (unsigned int)ctrl->pci.bus,
            (unsigned int)ctrl->pci.device,
            (unsigned int)ctrl->pci.function,
            (void *)(uintptr_t)ctrl->bar0.address,
            (unsigned int)ctrl->bar0.size,
            (unsigned int)ctrl->irq_line,
            (unsigned int)ctrl->irq_pin);

    switch (ctrl->kind) {
    case USB_HC_XHCI:
        ctrl->initialized = xhci_init_controller(ctrl);
        return;
    case USB_HC_EHCI:
        ctrl->initialized = ehci_init_controller(ctrl);
        return;
    case USB_HC_UHCI:
        ctrl->initialized = uhci_init_controller(ctrl);
        return;
    case USB_HC_OHCI:
        ctrl->initialized = ohci_init_controller(ctrl);
        return;
    default:
        kprintf("USB: unknown controller type, skipping\n");
        return;
    }
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

    // Initialize EHCI/xHCI first: EHCI's CONFIGFLAG decides port routing, so
    // it must claim (or release) ports before any companion UHCI/OHCI looks
    // at them — otherwise a device could enumerate twice or get yanked away.
    for (uint32_t i = 0; i < g_usb_controller_count; i++) {
        if (g_usb_controllers[i].kind == USB_HC_EHCI ||
            g_usb_controllers[i].kind == USB_HC_XHCI) {
            usb_try_init_controller(&g_usb_controllers[i]);
        }
    }

    for (uint32_t i = 0; i < g_usb_controller_count; i++) {
        if (g_usb_controllers[i].kind != USB_HC_EHCI &&
            g_usb_controllers[i].kind != USB_HC_XHCI) {
            usb_try_init_controller(&g_usb_controllers[i]);
        }
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

// Called from the kernel main loop: services interrupt-IN endpoints on the
// polled legacy controllers (UHCI/OHCI/EHCI). xHCI input is IRQ-driven.
void usb_poll(void) {
    usb_core_poll();
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

/* Selftest: cross-references the USB controller table against a fresh PCI
 * scan of class 0x0C/subclass 0x03 (Serial Bus/USB) devices — the exact
 * criterion usb_discover_controllers() uses — and asserts every one of
 * them was classified into a real HC kind (not USB_HC_UNKNOWN) and, if it
 * has a usable MMIO BAR0, actually initialized.
 *
 * This is deliberately independent of which HC generation or device (if
 * any) happens to be attached in a given QEMU config: it's a falsifiable
 * assertion about the discovery/classification path itself (real code that
 * could regress — e.g. if the prog_if -> kind mapping broke), not about
 * live data transfer. HID/mass-storage transfer correctness per HC
 * generation was verified manually against real device attachments this
 * session (`make run-usb-uhci`/`-ohci`/`-ehci`/`-xhci`) — see docs/TODO.md's
 * "no automated selftest for the display or USB stack" entry; this closes
 * only the discovery-layer half of that gap. */
int usb_run_selftests(void) {
    uint32_t pci_usb_devices = 0;
    for (uint32_t i = 0; i < pci_devices_count(); i++) {
        const struct pci_device *dev = pci_get_device(i);
        if (dev && dev->class_code == 0x0C && dev->subclass == 0x03) {
            pci_usb_devices++;
        }
    }

    if (pci_usb_devices != g_usb_controller_count) {
        kprintf("usb_run_selftests: PCI shows %u USB controller(s), "
                "usb_init() discovered %u\n",
                (unsigned int)pci_usb_devices,
                (unsigned int)g_usb_controller_count);
        return -1;
    }

    bool ok = true;
    for (uint32_t i = 0; i < g_usb_controller_count; i++) {
        const struct usb_controller *ctrl = &g_usb_controllers[i];
        if (ctrl->kind == USB_HC_UNKNOWN) {
            kprintf("usb_run_selftests: controller %u:%u.%u has unrecognized "
                    "prog_if %x\n",
                    (unsigned int)ctrl->pci.bus, (unsigned int)ctrl->pci.device,
                    (unsigned int)ctrl->pci.function,
                    (unsigned int)ctrl->pci.prog_if);
            ok = false;
            continue;
        }
        // A UHCI controller legitimately has no MMIO BAR0 (its registers are
        // I/O-port based, in BAR4) — every other kind needs one to init at all.
        bool should_init = (ctrl->kind == USB_HC_UHCI) || ctrl->bar0.valid;
        if (should_init && !ctrl->initialized) {
            kprintf("usb_run_selftests: controller %u:%u.%u (%d) failed to "
                    "initialize\n",
                    (unsigned int)ctrl->pci.bus, (unsigned int)ctrl->pci.device,
                    (unsigned int)ctrl->pci.function, (int)ctrl->kind);
            ok = false;
        }
    }

    kprintf("usb_run_selftests: %u controller(s) cross-checked against PCI\n",
            (unsigned int)g_usb_controller_count);
    return ok ? 0 : -1;
}
