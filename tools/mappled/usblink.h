/*
 * usblink.h: ioctls of the Mapple USB function nodes /dev/linkptp and /dev/linkmux
 * (bsd/dev/usb_link.c, patch 0182; contract in docs/VIVA-USB-PERSONA.md).
 * Shared by the kernel and tools/mappled/mapple-usbd.c: keep both copies identical.
 */
#ifndef USBLINK_H
#define USBLINK_H

#include <stdint.h>
#include <sys/ioccom.h>

struct usblink_serial {
	char s[64];             /* NUL-terminated serial string of the std persona (the UDID) */
};

struct usblink_event {
	uint32_t len;           /* bytes used in data */
	uint8_t  data[64];      /* one PTP event container (interrupt IN, endpoint 0x83) */
};

#define USBLINK_SET_SERIAL  _IOW('V', 1, struct usblink_serial)  /* before ATTACH */
#define USBLINK_ATTACH      _IO('V', 2)                          /* switch to the std persona; EPERM with mp_usblink_std=0 */
#define USBLINK_STATE       _IOR('V', 3, int)                    /* 0 no host, 1 configured, 2 configured + Mapple link active */
#define USBLINK_PTP_EVENT   _IOW('V', 4, struct usblink_event)   /* queue one PTP event (optional) */

#define USBLINK_STATE_NOHOST     0
#define USBLINK_STATE_CONFIGURED 1
#define USBLINK_STATE_LINK       2

#endif /* USBLINK_H */
