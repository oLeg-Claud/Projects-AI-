#ifndef USB_LINK_H
#define USB_LINK_H
#include <stdbool.h>
#include <stdint.h>
void usb_link_init(void);
bool usb_link_connected(void);
bool usb_link_send(const uint8_t *data, uint16_t length);
bool usb_link_idle(void);
void usb_link_stop(void);
/* Implemented by application, invoked from USB interrupt. */
void app_receive(const uint8_t *bytes, uint32_t length);
void app_disconnect(void);
#endif
