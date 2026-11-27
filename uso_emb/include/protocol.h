#ifndef PROTOCOL_H
#define PROTOCOL_H
#include <stdint.h>
#include <stdbool.h>
typedef enum { USO_NONE, USO_READY, USO_STOP, USO_HP_ON, USO_HP_OFF, USO_START, USO_INVALID, USO_INFO, USO_BOOT, USO_NUMBER } uso_event_kind_t;
typedef struct { uso_event_kind_t kind; uint8_t gain, freq; bool bits32; } uso_event_t;
typedef struct { uint32_t since; uint8_t command; bool pending; uint8_t key_count; uint32_t key; } uso_parser_t;
uso_event_t uso_parse(uso_parser_t *p, uint8_t byte, uint32_t now);
void uso_parse_expire(uso_parser_t *p, uint32_t now);
bool uso_event_allowed(uso_event_kind_t kind, bool streaming);
unsigned uso_pack_number(uint8_t out[5]);
#endif
