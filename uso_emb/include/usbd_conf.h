#ifndef USBD_CONF_H
#define USBD_CONF_H
#include "stm32g4xx_hal.h"
#include <stdlib.h>
#include <string.h>
#define USBD_MAX_NUM_INTERFACES 2U
#define USBD_MAX_NUM_CONFIGURATION 1U
#define USBD_MAX_STR_DESC_SIZ 128U
#define USBD_SELF_POWERED 0U
#define USBD_DEBUG_LEVEL 0U
#define USBD_LPM_ENABLED 0U
#define USBD_SUPPORT_USER_STRING_DESC 0U
#define USBD_CLASS_USER_STRING_DESC 0U
void *usb_static_alloc(uint32_t size);
void usb_static_free(void *p);
#define USBD_malloc usb_static_alloc
#define USBD_free usb_static_free
#define USBD_memset memset
#define USBD_memcpy memcpy
#define USBD_Delay HAL_Delay
#endif
