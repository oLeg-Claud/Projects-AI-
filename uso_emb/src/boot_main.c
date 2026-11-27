#include "boot_core.h"
#include "boot_wire.h"
#include "board.h"
#include "usb_link.h"
#include <string.h>

static uint8_t rx[1024];
static volatile uint32_t head,tail;
static volatile bool reset_rx,overflow;
static bl_parser_t parser;
static bl_state_t state;
static uint8_t tx[64];
static uint16_t tx_size;
static bl_replay_t replay;
static bool hold,run_pending;

void app_receive(const uint8_t *p,uint32_t n)
{
    for(uint32_t i=0;i<n;++i) {
        uint32_t next=(head+1u)%sizeof(rx);
        if(next==tail) { overflow=true; return; }
        rx[head]=p[i]; __DMB(); head=next;
    }
}
void app_disconnect(void) { reset_rx=true; }

__attribute__((naked,noreturn)) static void enter_app(uint32_t sp __attribute__((unused)),uint32_t pc __attribute__((unused)))
{
    __asm volatile("movs r2, #0\n msr control, r2\n msr basepri, r2\n msr faultmask, r2\n"
                   "msr msp, r0\n dsb\n isb\n cpsie i\n bx r1\n");
}
static void launch_app(void)
{
    if(!bl_image_valid()) { run_pending=false; state.run=false; hold=true; return; }
    uint32_t sp=*(volatile uint32_t *)BL_APP_BASE;
    uint32_t pc=*(volatile uint32_t *)(BL_APP_BASE+4);
    /* Let the host CDC driver deliver the completed RUN reply before detach. */
    HAL_Delay(200);
    usb_link_stop(); HAL_Delay(100);
    HAL_RCC_DeInit(); HAL_DeInit();
    __disable_irq();
    SysTick->CTRL=0; SysTick->LOAD=0; SysTick->VAL=0;
    for(unsigned i=0;i<8;++i) { NVIC->ICER[i]=0xffffffffu; NVIC->ICPR[i]=0xffffffffu; }
    SCB->ICSR=SCB_ICSR_PENDSTCLR_Msk|SCB_ICSR_PENDSVCLR_Msk;
    SCB->VTOR=BL_APP_BASE;
    __DSB(); __ISB(); enter_app(sp,pc);
}
static void request(void)
{
    hold=true;
    tx_size=bl_dispatch(&state,&replay,parser.data,tx);
    if(state.run) run_pending=true;
}
int main(void)
{
    volatile uint32_t *mail=(volatile uint32_t *)BL_MAILBOX_ADDR;
    bool requested=(RCC->CSR & RCC_CSR_SFTRSTF) && mail[0]==BL_REQUEST_MAGIC && mail[1]==~BL_REQUEST_MAGIC;
    mail[0]=mail[1]=0; __HAL_RCC_CLEAR_RESET_FLAGS();
    board_init(); /* ADC held reset; loader runs from HSI16 + HSI48. */
    bool valid=bl_image_valid(); hold=requested || !valid;
    usb_link_init(); uint32_t started=HAL_GetTick();
    for(;;) {
        if(reset_rx || overflow) {
            uint32_t mask=__get_PRIMASK(); __disable_irq();
            bool lost=overflow; reset_rx=overflow=false; tail=head;
            __set_PRIMASK(mask);
            if(state.active || lost) hold=true;
            state.active=false; state.run=false; run_pending=false;
            parser.used=0; tx_size=0; replay.valid=false;
        }
        if(tx_size && usb_link_send(tx,tx_size)) tx_size=0;
        if(!tx_size && !run_pending) {
            for(unsigned budget=0;budget<512 && tail!=head && !tx_size;++budget) {
                uint8_t b=rx[tail]; __DMB(); tail=(tail+1u)%sizeof(rx);
                /* Keep the requested A1 ASCII/NUL INFO format in the loader too. */
                if(!parser.used && b==0xa1) {
                    static const uint8_t info[]="\xa1" __DATE__ " " __TIME__ " USO32_Boot_v1.0";
                    memcpy(tx,info,sizeof(info)); tx_size=sizeof(info); hold=true;
                } else if(bl_feed(&parser,b,HAL_GetTick())) request();
            }
        }
        if(run_pending && !tx_size && usb_link_idle()) launch_app();
        if(!hold && (uint32_t)(HAL_GetTick()-started)>=3000u) launch_app();
        board_led((HAL_GetTick()/200u)&1u);
    }
}
