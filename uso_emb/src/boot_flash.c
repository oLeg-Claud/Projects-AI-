#include "boot_core.h"
#include "board.h"
#include <string.h>
volatile bool bl_ecc_error;
static bool in_flash(uint32_t addr,uint32_t n)
{ return addr>=BL_APP_BASE && addr<BL_META_BASE+BL_PAGE_SIZE && n<=BL_META_BASE+BL_PAGE_SIZE-addr; }
bool bl_port_read(uint32_t addr,uint8_t *data,uint32_t n)
{
    if(!in_flash(addr,n)) return false;
    bl_ecc_error=false;
    const volatile uint8_t *p=(const volatile uint8_t *)addr;
    for(uint32_t i=0;i<n;++i) { data[i]=p[i]; if(bl_ecc_error) return false; }
    return true;
}
bool bl_port_erase(uint32_t addr,uint32_t n)
{
    if(!n || !in_flash(addr,n) || (addr%BL_PAGE_SIZE) || (n%BL_PAGE_SIZE)) return false;
    FLASH_EraseInitTypeDef erase={0}; uint32_t error=0;
    erase.TypeErase=FLASH_TYPEERASE_PAGES; erase.Banks=FLASH_BANK_1;
    erase.Page=(addr-BL_FLASH_BASE)/BL_PAGE_SIZE; erase.NbPages=n/BL_PAGE_SIZE;
    if(HAL_FLASH_Unlock()!=HAL_OK) return false;
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
    HAL_StatusTypeDef result=HAL_FLASHEx_Erase(&erase,&error);
    HAL_FLASH_Lock();
    /* HAL erase/program flushes flash caches. */
    return result==HAL_OK;
}
bool bl_port_write(uint32_t addr,const uint8_t *data,uint32_t n)
{
    if(!n || n>BL_CHUNK_MAX || (addr&7u) || !in_flash(addr,(n+7u)&~7u)) return false;
    /* A padded application write must not reach the metadata page. */
    if(addr<BL_META_BASE && ((n+7u)&~7u)>BL_META_BASE-addr) return false;
    if(HAL_FLASH_Unlock()!=HAL_OK) return false;
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
    bool ok=true;
    for(uint32_t off=0;off<n;off+=8) {
        uint64_t word=UINT64_MAX;
        uint32_t count=n-off; if(count>8) count=8;
        memcpy(&word,data+off,count);
        if(HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,addr+off,word)!=HAL_OK) { ok=false; break; }
        uint8_t actual[8];
        if(!bl_port_read(addr+off,actual,8) || memcmp(actual,&word,8)) { ok=false; break; }
    }
    HAL_FLASH_Lock(); return ok;
}
