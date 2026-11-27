#ifndef BOOT_CORE_H
#define BOOT_CORE_H
#include "boot_layout.h"
#include <stdbool.h>
#include <stddef.h>
#define BL_PAYLOAD_MAX (BL_CHUNK_MAX+4u)
enum { BL_HELLO=1, BL_BEGIN=2, BL_WRITE=3, BL_FINISH=4, BL_RUN=5 };
enum { BL_OK=0, BL_BAD_REQUEST=1, BL_BAD_STATE=2, BL_RANGE=3, BL_FLASH_ERROR=4, BL_CRC_ERROR=5, BL_VECTOR_ERROR=6 };
typedef struct { uint32_t size, crc, written; bool active, run; } bl_state_t;
uint32_t bl_crc32(const uint8_t *data, size_t size);
uint32_t bl_get32(const uint8_t *p);
void bl_put32(uint8_t *p, uint32_t v);
bool bl_vectors_valid(const uint8_t *vectors, uint32_t size);
bool bl_image_valid(void);
/* Returns response payload length, first byte is status. */
uint16_t bl_handle(bl_state_t *s, uint8_t cmd, const uint8_t *p, uint16_t n, uint8_t *out);
/* Port functions: only these touch flash, also replaced by simulated flash in tests. */
bool bl_port_read(uint32_t addr, uint8_t *data, uint32_t n);
bool bl_port_erase(uint32_t addr, uint32_t n);
bool bl_port_write(uint32_t addr, const uint8_t *data, uint32_t n);
#endif
