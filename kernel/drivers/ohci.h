#ifndef __OHCI_H__
#define __OHCI_H__

#include "usb.h"

// Bring up an OHCI (USB 1.x) controller and enumerate its root ports.
bool ohci_init_controller(struct usb_controller *ctrl);

#endif /* __OHCI_H__ */
