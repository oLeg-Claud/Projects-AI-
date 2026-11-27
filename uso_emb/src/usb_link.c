#include "usb_link.h"
#include "board.h"
#include "usbd_core.h"
#include "usbd_cdc.h"
#include "usbd_ctlreq.h"

USBD_HandleTypeDef usb_device;
static uint8_t rx_buffer[64], tx_buffer[64];
static uint32_t cdc_storage[(sizeof(USBD_CDC_HandleTypeDef) + 3) / 4];
static uint8_t line_coding[7] = {0x00,0xe1,0x00,0x00,0,0,8};
void *usb_static_alloc(uint32_t n) { return n <= sizeof(cdc_storage) ? cdc_storage : NULL; }
void usb_static_free(void *p) { (void)p; }
static int8_t cdc_init(void)
{
    USBD_CDC_SetTxBuffer(&usb_device, tx_buffer, 0);
    USBD_CDC_SetRxBuffer(&usb_device, rx_buffer);
    return USBD_OK;
}
static int8_t cdc_deinit(void) { app_disconnect(); return USBD_OK; }
static int8_t cdc_control(uint8_t cmd, uint8_t *data, uint16_t len)
{
    if (cmd == CDC_SET_LINE_CODING && len == 7) memcpy(line_coding, data, 7);
    if (cmd == CDC_GET_LINE_CODING && len == 7) memcpy(data, line_coding, 7);
    return USBD_OK;
}
static int8_t cdc_receive(uint8_t *data, uint32_t *len)
{
    app_receive(data, *len);
    USBD_CDC_SetRxBuffer(&usb_device, rx_buffer);
    USBD_CDC_ReceivePacket(&usb_device);
    return USBD_OK;
}
static USBD_CDC_ItfTypeDef cdc_ops = {
    .Init=cdc_init, .DeInit=cdc_deinit, .Control=cdc_control, .Receive=cdc_receive
};
static uint8_t device_desc[18] = {
    18, USB_DESC_TYPE_DEVICE, 0x00,0x02, 0x02,0x02,0x00,64,
    USO_USB_VID & 0xff, USO_USB_VID >> 8, USO_USB_PID & 0xff, USO_USB_PID >> 8,
    0x00,0x01,1,2,3,1
};
static uint8_t lang_desc[] = {4, USB_DESC_TYPE_STRING, 0x09,0x04};
static uint8_t string_desc[128];
static uint8_t *device(USBD_SpeedTypeDef s, uint16_t *n) { (void)s; *n=sizeof(device_desc); return device_desc; }
static uint8_t *lang(USBD_SpeedTypeDef s, uint16_t *n) { (void)s; *n=sizeof(lang_desc); return lang_desc; }
static uint8_t *str(const char *text, uint16_t *n) { USBD_GetString((uint8_t *)text, string_desc, n); return string_desc; }
static uint8_t *manufacturer(USBD_SpeedTypeDef s, uint16_t *n) { (void)s; return str("USO project",n); }
static uint8_t *product(USBD_SpeedTypeDef s, uint16_t *n) {
    (void)s;
#ifdef USO_BOOTLOADER
    return str("USO-32 Bootloader",n);
#else
    return str("USO-32 ADS1262",n);
#endif
}
static uint8_t *serial(USBD_SpeedTypeDef s, uint16_t *n)
{
    (void)s;
    const char hex[]="0123456789ABCDEF";
    uint32_t uid[3]={HAL_GetUIDw0(),HAL_GetUIDw1(),HAL_GetUIDw2()};
    char text[25];
    for (unsigned i=0; i<24; ++i) text[i]=hex[(uid[i/8] >> (28-4*(i%8))) & 15];
    text[24]=0;
    return str(text,n);
}
static uint8_t *configuration(USBD_SpeedTypeDef s, uint16_t *n) { (void)s; return str("CDC",n); }
static uint8_t *interface(USBD_SpeedTypeDef s, uint16_t *n) { (void)s; return str("USO data",n); }
static USBD_DescriptorsTypeDef descriptors = {device,lang,manufacturer,product,serial,configuration,interface};
void usb_link_init(void)
{
    if (USBD_Init(&usb_device,&descriptors,0) != USBD_OK ||
        USBD_RegisterClass(&usb_device,&USBD_CDC) != USBD_OK ||
        USBD_CDC_RegisterInterface(&usb_device,&cdc_ops) != USBD_OK ||
        USBD_Start(&usb_device) != USBD_OK) board_fatal();
}
bool usb_link_connected(void) { return usb_device.dev_state == USBD_STATE_CONFIGURED; }
bool usb_link_idle(void)
{
    uint32_t mask=__get_PRIMASK(); __disable_irq();
    USBD_CDC_HandleTypeDef *cdc=usb_device.pClassData;
    bool idle=usb_link_connected() && cdc && cdc->TxState==0;
    __set_PRIMASK(mask);
    return idle;
}
void usb_link_stop(void)
{
    USBD_Stop(&usb_device);
    USBD_DeInit(&usb_device);
}
bool usb_link_send(const uint8_t *data, uint16_t len)
{
    if (!len || len > sizeof(tx_buffer)) return false;
    /* USB reset/DeInit must not invalidate class state between check and submit. */
    uint32_t primask=__get_PRIMASK(); __disable_irq();
    USBD_CDC_HandleTypeDef *cdc = usb_device.pClassData;
    bool ok=false;
    if (usb_link_connected() && cdc && cdc->TxState == 0) {
        memcpy(tx_buffer,data,len);
        USBD_CDC_SetTxBuffer(&usb_device,tx_buffer,len);
        ok=USBD_CDC_TransmitPacket(&usb_device) == USBD_OK;
    }
    __set_PRIMASK(primask);
    return ok;
}
