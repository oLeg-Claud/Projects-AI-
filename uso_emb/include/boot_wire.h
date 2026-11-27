#ifndef BOOT_WIRE_H
#define BOOT_WIRE_H
#include "boot_core.h"
#define BL_FRAME_MAX (9u+BL_PAYLOAD_MAX+4u)
typedef struct {
    uint8_t data[BL_FRAME_MAX];
    uint16_t used, needed;
    uint32_t since;
} bl_parser_t;
typedef struct {
    uint8_t request[BL_FRAME_MAX], response[64];
    uint16_t request_size,response_size;
    bool valid;
} bl_replay_t;
/* Complete valid frame is retained in data until the next feed. */
bool bl_feed(bl_parser_t *p,uint8_t byte,uint32_t now);
uint16_t bl_frame(uint8_t *out,uint8_t cmd,uint16_t seq,const uint8_t *data,uint16_t n);
uint16_t bl_dispatch(bl_state_t *state,bl_replay_t *cache,const uint8_t *request,uint8_t *response);
#endif
