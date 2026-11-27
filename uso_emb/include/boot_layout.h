#ifndef BOOT_LAYOUT_H
#define BOOT_LAYOUT_H
#include <stdint.h>
#define BL_FLASH_BASE 0x08000000u
#define BL_APP_BASE 0x08008000u
#define BL_META_BASE 0x0801f800u
#define BL_APP_MAX (BL_META_BASE - BL_APP_BASE)
#define BL_PAGE_SIZE 2048u
#define BL_RAM_BASE 0x20000000u
/* Last 8 bytes reserved by BOTH linker scripts, outside startup zeroing/stack. */
#define BL_MAILBOX_ADDR 0x20007ff8u
#define BL_REQUEST_MAGIC 0x55424f54u
#define BL_META_MAGIC 0x32534f55u
#define BL_META_VERSION 1u
#define BL_CHUNK_MAX 256u
#endif
