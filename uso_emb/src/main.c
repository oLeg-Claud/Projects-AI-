#include "ads1262.h"
#include "board.h"
#include "dsp.h"
#include "protocol.h"
#include "usb_link.h"
#include "uso_config.h"
#include "boot_layout.h"
#include <string.h>

/* Inspect with SWD. No diagnostic text is injected into the binary stream. */
volatile struct {
    uint32_t samples, checksum_errors, adc_errors, stale, missed_drdy;
    uint32_t tx_overflows, rx_overflows, invalid_commands, timeouts;
    uint8_t adc_status, fault, running;
} uso_stats;
static uint8_t rx[USO_RX_CAPACITY];
static volatile uint32_t rx_head, rx_tail, drdy_count;
static volatile bool disconnected, rx_overflow;
typedef struct { uint8_t data[64], size; } frame_t;
static frame_t tx[USO_TX_FRAMES];
static unsigned tx_head, tx_tail;
static uso_parser_t parser;
static uso_filter_t filter;
static bool ready, running, hp_enabled, bits32;
static uint32_t last_sample;
static bool boot_pending;
static const uint8_t info[] = "\xa1" __DATE__ " " __TIME__ " USO32_App_v2.2_400Hz";

void app_receive(const uint8_t *b, uint32_t n)
{
    for (uint32_t i=0; i<n; ++i) {
        uint32_t next=(rx_head+1u)%USO_RX_CAPACITY;
        if (next==rx_tail) { rx_overflow=true; ++uso_stats.rx_overflows; return; }
        rx[rx_head]=b[i]; __DMB(); rx_head=next;
    }
}
void app_disconnect(void) { disconnected=true; }
void HAL_GPIO_EXTI_Callback(uint16_t pin) { if (pin==GPIO_PIN_6) ++drdy_count; }
static void stop_acquisition(void)
{
    board_adc_irq(false); drdy_count=0;
    running=false; uso_stats.running=0;
    if (!ads_stop()) { ready=false; uso_stats.fault=1; board_reset(false); }
    /* A packet already submitted to USB completes before the STOP reply. */
    unsigned write=tx_tail;
    for (unsigned read=tx_tail; read!=tx_head; read=(read+1u)%USO_TX_FRAMES) {
        if (tx[read].data[0]!=0) { tx[write]=tx[read]; write=(write+1u)%USO_TX_FRAMES; }
    }
    tx_head=write;
}
static void fault(void)
{
    stop_acquisition(); ready=false; uso_stats.fault=1;
    board_reset(false);
}
static bool enqueue(const uint8_t *data,unsigned n)
{
    if (n>sizeof(tx[0].data)) return false;
    unsigned next=(tx_head+1u)%USO_TX_FRAMES;
    if (next==tx_tail) {
        ++uso_stats.tx_overflows;
        /* Host backpressure is not an ADC failure. End this acquisition and
           discard queued samples, retaining command replies and calibration.
           Never silently resume a stream with missing samples. */
        stop_acquisition();
        if (data[0]==0) return false;
        next=(tx_head+1u)%USO_TX_FRAMES;
        if (next==tx_tail) return false; /* Command flood: host must retry. */
    }
    memcpy(tx[tx_head].data,data,n); tx[tx_head].size=(uint8_t)n; tx_head=next;
    return true;
}
static void reply(void) { const uint8_t ack=0x55; (void)enqueue(&ack,1); }
static void handle(uso_event_t e)
{
    if (!uso_event_allowed(e.kind,running)) return;
    switch(e.kind) {
    case USO_NUMBER: {
        uint8_t frame[5];
        unsigned n=uso_pack_number(frame);
        (void)enqueue(frame,n);
        break;
    }
    case USO_INFO:
        if (!running) (void)enqueue(info,sizeof(info));
        break;
    case USO_BOOT:
        if (!running) {
            const uint8_t ack[]={0x08,0xaa};
            if (enqueue(ack,sizeof(ack))) boot_pending=true;
        }
        break;
    case USO_READY: if (ready) reply(); break;
    case USO_STOP: stop_acquisition(); reply(); break;
    case USO_HP_ON: case USO_HP_OFF:
        hp_enabled=(e.kind==USO_HP_ON); uso_filter_hp(&filter,hp_enabled); break;
    case USO_START:
        if (!ready) break;
        stop_acquisition();
        if (!ready || !uso_filter_init(&filter,e.freq,hp_enabled) || !ads_start(e.gain)) { fault(); break; }
        bits32=e.bits32; last_sample=HAL_GetTick();
        running=true; uso_stats.running=1; drdy_count=0;
        board_adc_irq(true);
        break;
    case USO_INVALID: ++uso_stats.invalid_commands; break;
    default: break;
    }
}
int main(void)
{
    board_init();
    ready=ads_init();
    if (!ready) { uso_stats.fault=1; board_reset(false); }
    usb_link_init();
    for (;;) {
        uint32_t now=HAL_GetTick();
        if (disconnected || rx_overflow) {
            uint32_t mask=__get_PRIMASK(); __disable_irq();
            disconnected=false; rx_overflow=false; rx_tail=rx_head;
            __set_PRIMASK(mask);
            parser.pending=false;
            boot_pending=false;
            tx_head=tx_tail=0;
            /* A transport overflow invalidates the session, not the ADC. */
            stop_acquisition();
        }
        /* Bound work per iteration so a host command flood cannot starve ADC. */
        for (unsigned budget=0; budget<32 && rx_tail!=rx_head && !boot_pending; ++budget) {
            uint8_t byte=rx[rx_tail]; __DMB(); rx_tail=(rx_tail+1u)%USO_RX_CAPACITY;
            handle(uso_parse(&parser,byte,HAL_GetTick()));
        }
        uso_parse_expire(&parser,HAL_GetTick());
        if (running) {
            uint32_t mask=__get_PRIMASK(); __disable_irq();
            uint32_t pending=drdy_count; drdy_count=0;
            __set_PRIMASK(mask);
            if (pending>1) { uso_stats.missed_drdy+=pending-1; fault(); }
            else if (pending) {
                int32_t sample; uint8_t status=0;
                ads_result_t result=ads_read(&sample,&status);
                uso_stats.adc_status=status;
                if (result==ADS_OK) {
                    last_sample=HAL_GetTick(); ++uso_stats.samples;
                    uint8_t frame[6]; unsigned n=uso_pack_sample(uso_filter_step(&filter,sample),bits32,frame);
                    (void)enqueue(frame,n);
                } else {
                    if (result==ADS_CHECKSUM) ++uso_stats.checksum_errors;
                    else if (result==ADS_STALE) ++uso_stats.stale;
                    else ++uso_stats.adc_errors;
                    fault();
                }
            } else if ((uint32_t)(HAL_GetTick()-last_sample)>=USO_ADC_TIMEOUT_MS) {
                ++uso_stats.timeouts; fault();
            }
        }
        if (tx_tail!=tx_head && usb_link_send(tx[tx_tail].data,tx[tx_tail].size))
            tx_tail=(tx_tail+1u)%USO_TX_FRAMES;
        if (boot_pending && tx_tail==tx_head && usb_link_idle()) {
            board_adc_irq(false); board_reset(false);
            usb_link_stop(); HAL_Delay(100);
            volatile uint32_t *mail=(volatile uint32_t *)BL_MAILBOX_ADDR;
            mail[0]=BL_REQUEST_MAGIC; mail[1]=~BL_REQUEST_MAGIC;
            __DSB(); NVIC_SystemReset();
        }
        board_led(uso_stats.fault ? ((now/100u)&1u) : running ? ((now/500u)&1u) : true);
    }
}
