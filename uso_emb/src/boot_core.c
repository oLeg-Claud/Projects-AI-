#include "boot_core.h"
#include <string.h>
uint32_t bl_get32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
void bl_put32(uint8_t *p,uint32_t v)
{ for(unsigned i=0;i<4;++i) p[i]=(uint8_t)(v>>(8*i)); }
static uint32_t crc_step(uint32_t crc,const uint8_t *p,size_t n)
{
    while(n--) {
        crc^=*p++;
        for(unsigned bit=0;bit<8;++bit) crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u);
    }
    return crc;
}
uint32_t bl_crc32(const uint8_t *data,size_t size) { return ~crc_step(0xffffffffu,data,size); }
bool bl_vectors_valid(const uint8_t *v,uint32_t size)
{
    uint32_t sp=bl_get32(v), pc=bl_get32(v+4);
    return size>=8 && size<=BL_APP_MAX && sp>BL_RAM_BASE && sp<=BL_MAILBOX_ADDR &&
           !(sp&7u) && (pc&1u) && (pc&~1u)>=BL_APP_BASE && (pc&~1u)<BL_APP_BASE+size;
}
static bool image_crc(uint32_t size,uint32_t *crc)
{
    uint8_t b[128]; uint32_t c=0xffffffffu;
    for(uint32_t off=0;off<size;) {
        uint32_t n=size-off; if(n>sizeof(b)) n=sizeof(b);
        if(!bl_port_read(BL_APP_BASE+off,b,n)) return false;
        c=crc_step(c,b,n); off+=n;
    }
    *crc=~c; return true;
}
bool bl_image_valid(void)
{
    uint8_t meta[16],v[8]; uint32_t crc;
    if(!bl_port_read(BL_META_BASE,meta,sizeof(meta))) return false;
    if(bl_get32(meta+8)!=BL_META_MAGIC || bl_get32(meta+12)!=BL_META_VERSION) return false;
    uint32_t size=bl_get32(meta);
    if(size<8 || size>BL_APP_MAX || !bl_port_read(BL_APP_BASE,v,8) || !bl_vectors_valid(v,size)) return false;
    return image_crc(size,&crc) && crc==bl_get32(meta+4);
}
uint16_t bl_handle(bl_state_t *s,uint8_t cmd,const uint8_t *p,uint16_t n,uint8_t *out)
{
    out[0]=BL_BAD_REQUEST;
    if(cmd==BL_HELLO && n==0) {
        out[0]=BL_OK; out[1]=1; /* wire protocol version */
        bl_put32(out+2,BL_APP_BASE); bl_put32(out+6,BL_APP_MAX);
        bl_put32(out+10,BL_PAGE_SIZE); bl_put32(out+14,BL_CHUNK_MAX);
        out[18]=(uint8_t)bl_image_valid(); return 19;
    }
    if(cmd==BL_BEGIN && n==8) {
        uint32_t size=bl_get32(p);
        if(size<8 || size>BL_APP_MAX) { out[0]=BL_RANGE; return 1; }
        s->active=false; s->run=false; s->written=0;
        /* Invalidate FIRST, then erase image. Reset at any point is recoverable. */
        if(!bl_port_erase(BL_META_BASE,BL_PAGE_SIZE) || !bl_port_erase(BL_APP_BASE,BL_APP_MAX)) {
            out[0]=BL_FLASH_ERROR; return 1;
        }
        s->size=size; s->crc=bl_get32(p+4); s->active=true; out[0]=BL_OK; return 1;
    }
    if(cmd==BL_WRITE && n>=5 && n<=BL_PAYLOAD_MAX) {
        if(!s->active) { out[0]=BL_BAD_STATE; return 1; }
        uint32_t off=bl_get32(p), count=n-4u;
        if(off!=s->written || (off&7u) || off>s->size || count>s->size-off ||
           ((count&7u) && off+count!=s->size)) { out[0]=BL_RANGE; return 1; }
        if(!bl_port_write(BL_APP_BASE+off,p+4,count)) {
            s->active=false; out[0]=BL_FLASH_ERROR; return 1;
        }
        s->written+=count; out[0]=BL_OK; bl_put32(out+1,s->written); return 5;
    }
    if(cmd==BL_FINISH && n==0) {
        if(!s->active || s->written!=s->size) { out[0]=BL_BAD_STATE; return 1; }
        uint8_t v[8],meta[16]; uint32_t crc;
        s->active=false;
        if(!bl_port_read(BL_APP_BASE,v,8) || !image_crc(s->size,&crc)) { out[0]=BL_FLASH_ERROR; return 1; }
        if(crc!=s->crc) { out[0]=BL_CRC_ERROR; return 1; }
        if(!bl_vectors_valid(v,s->size)) { out[0]=BL_VECTOR_ERROR; return 1; }
        bl_put32(meta,s->size); bl_put32(meta+4,s->crc);
        bl_put32(meta+8,BL_META_MAGIC); bl_put32(meta+12,BL_META_VERSION);
        /* Separate doublewords: commit marker is programmed LAST. */
        if(!bl_port_write(BL_META_BASE,meta,8) || !bl_port_write(BL_META_BASE+8,meta+8,8) || !bl_image_valid()) {
            out[0]=BL_FLASH_ERROR; return 1;
        }
        out[0]=BL_OK; return 1;
    }
    if(cmd==BL_RUN && n==0) {
        if(s->active || !bl_image_valid()) { out[0]=BL_BAD_STATE; return 1; }
        s->run=true; out[0]=BL_OK;
    }
    return 1;
}
