#ifndef __XHCI_H__
#define __XHCI_H__

#include "usb.h"

bool xhci_init_controller(struct usb_controller *ctrl);

// Route each controller's PCI interrupt and register the handler. Call once,
// after enumeration, to switch HID input from polling to interrupt-driven.
void xhci_enable_irq(void);

// Interrupt handler: acknowledge + drain every controller's event ring. Wired to
// the IRQ vector by xhci_enable_irq (exposed for the IRQ dispatch layer).
void xhci_irq(void);

#endif /* __XHCI_H__ */
