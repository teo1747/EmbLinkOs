#ifndef __EHCI_H__
#define __EHCI_H__

#include "drivers/usb/usb.h"

// Bring up an EHCI (USB 2.0) controller and enumerate its root ports.
// Full/low-speed devices are released to companion controllers (UHCI/OHCI).
bool ehci_init_controller(struct usb_controller *ctrl);

#endif /* __EHCI_H__ */
