#include "boot_wire.h"
#include <string.h>
bool bl_feed(bl_parser_t *p,uint8_t byte,uint32_t now)
{
    static const uint8_t magic[]={'U','B','L','1'};
    if(p->used && (uint32_t)(now-p->since)>=1000u) p->used=0;
    if(!p->used) { p->needed=0; p->since=now; }
    if(p->used<4 && byte!=magic[p->used]) {
        p->used=byte==magic[0] ? 1 : 0; p->data[0]=byte; p->since=now; return false;
    }
    p->data[p->used++]=byte;
    if(p->used==9) {
        uint16_t len=(uint16_t)(p->data[7]|(uint16_t)p->data[8]<<8);
        if(len>BL_PAYLOAD_MAX) { p->used=0; return false; }
        p->needed=9u+len+4u;
    }
    if(p->needed && p->used==p->needed) {
        uint16_t size=p->used; p->used=0;
        return bl_crc32(p->data,size-4)==bl_get32(p->data+size-4);
    }
    return false;
}
uint16_t bl_frame(uint8_t *out,uint8_t cmd,uint16_t seq,const uint8_t *data,uint16_t n)
{
    if(n>BL_PAYLOAD_MAX) return 0;
    memcpy(out,"UBR1",4); out[4]=cmd|0x80u;
    out[5]=(uint8_t)seq; out[6]=(uint8_t)(seq>>8);
    out[7]=(uint8_t)n; out[8]=(uint8_t)(n>>8);
    memcpy(out+9,data,n); bl_put32(out+9+n,bl_crc32(out,9+n));
    return 13u+n;
}
uint16_t bl_dispatch(bl_state_t *s,bl_replay_t *cache,const uint8_t *f,uint8_t *out)
{
    uint16_t n=(uint16_t)(f[7]|(uint16_t)f[8]<<8), size=13u+n;
    uint16_t seq=(uint16_t)(f[5]|(uint16_t)f[6]<<8);
    if(n>BL_PAYLOAD_MAX) return 0;
    if(cache->valid && size==cache->request_size && !memcmp(cache->request,f,size)) {
        memcpy(out,cache->response,cache->response_size); return cache->response_size;
    }
    uint8_t payload[32];
    if(cache->valid && seq==(uint16_t)(cache->request[5]|(uint16_t)cache->request[6]<<8)) {
        payload[0]=BL_BAD_REQUEST; return bl_frame(out,f[4],seq,payload,1);
    }
    uint16_t count=bl_handle(s,f[4],f+9,n,payload);
    uint16_t total=bl_frame(out,f[4],seq,payload,count);
    memcpy(cache->request,f,size); cache->request_size=size;
    memcpy(cache->response,out,total); cache->response_size=total; cache->valid=true;
    return total;
}
