#ifndef __UHCI_H__
#define __UHCI_H__

#include "usb.h"

// Bring up a UHCI (USB 1.x) controller and enumerate its root ports.
bool uhci_init_controller(struct usb_controller *ctrl);

#endif /* __UHCI_H__ */
