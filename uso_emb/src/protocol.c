#include "protocol.h"
#include "dsp.h"
#include "uso_config.h"
unsigned uso_pack_number(uint8_t out[5])
{
    out[0] = 0x01; out[1] = USO_DEVICE_NUMBER;
    out[2] = 0; out[3] = 0; out[4] = 0xff;
    return 5;
}
bool uso_event_allowed(uso_event_kind_t kind,bool streaming)
{ return !streaming || (kind!=USO_INFO && kind!=USO_BOOT); }
void uso_parse_expire(uso_parser_t *p, uint32_t now)
{
    if (p->pending && (uint32_t)(now - p->since) >= USO_COMMAND_TIMEOUT_MS) p->pending = false;
}
uso_event_t uso_parse(uso_parser_t *p, uint8_t byte, uint32_t now)
{
    uso_event_t e = { .kind = USO_NONE };
    uso_parse_expire(p, now);
    if (p->pending) {
        if (p->command == 0x08) {
            p->key = (p->key << 8) | byte;
            if (++p->key_count == 4) {
                p->pending = false;
                e.kind = p->key == 0x12345678u ? USO_BOOT : USO_INVALID;
            }
            return e;
        }
        p->pending = false;
        /* 0x10 is a valid freq, not STOP when a frequency byte is expected. */
        if (uso_frequency_index(byte) >= 0) {
            e.kind = USO_START; e.freq = byte; e.bits32 = p->command >= 0x90;
            e.gain = p->command - (e.bits32 ? 0x90 : 0x20);
            return e;
        }
        /* A malformed pair is consumed atomically, never partially executed. */
        e.kind = USO_INVALID;
        return e;
    }
    if ((byte >= 0x20 && byte <= 0x25) || (byte >= 0x90 && byte <= 0x95)) {
        p->command = byte; p->since = now; p->pending = true;
    } else if (byte == 0x08) {
        p->command = byte; p->since = now; p->pending = true;
        p->key = 0; p->key_count = 0;
    } else switch (byte) {
        case 0xa1: e.kind = USO_INFO; break;
        case 0xaa: e.kind = USO_READY; break;
        case 0x66: e.kind = USO_NUMBER; break;
        case 0x10: e.kind = USO_STOP; break;
        case 0x71: e.kind = USO_HP_ON; break;
        case 0x72: e.kind = USO_HP_OFF; break;
        default: e.kind = USO_INVALID; break;
    }
    return e;
}
