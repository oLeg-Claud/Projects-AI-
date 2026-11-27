#include "usbd_core.h"
#include "usbd_cdc.h"
#include "board.h"
#include "usb_link.h"

static PCD_HandleTypeDef pcd;
extern USBD_HandleTypeDef usb_device;
void HAL_PCD_MspInit(PCD_HandleTypeDef *h)
{
    if (h->Instance != USB) return;
    __HAL_RCC_GPIOA_CLK_ENABLE(); __HAL_RCC_USB_CLK_ENABLE();
    GPIO_InitTypeDef g = {0};
    /* G431 USB owns the dedicated pads when enabled; there is no AF10_USB.
       Disable GPIO input/output paths; reset analog mode is also suitable. */
    g.Pin=GPIO_PIN_11 | GPIO_PIN_12; g.Mode=GPIO_MODE_ANALOG;
    g.Pull=GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA,&g);
    HAL_NVIC_SetPriority(USB_LP_IRQn,3,0);
    HAL_NVIC_EnableIRQ(USB_LP_IRQn);
}
void HAL_PCD_MspDeInit(PCD_HandleTypeDef *h)
{
    (void)h; HAL_NVIC_DisableIRQ(USB_LP_IRQn); __HAL_RCC_USB_CLK_DISABLE();
}
void USB_LP_IRQHandler(void) { HAL_PCD_IRQHandler(&pcd); }
void HAL_PCD_SetupStageCallback(PCD_HandleTypeDef *h) { USBD_LL_SetupStage(h->pData,(uint8_t *)h->Setup); }
void HAL_PCD_DataOutStageCallback(PCD_HandleTypeDef *h,uint8_t ep) { USBD_LL_DataOutStage(h->pData,ep,h->OUT_ep[ep].xfer_buff); }
void HAL_PCD_DataInStageCallback(PCD_HandleTypeDef *h,uint8_t ep) { USBD_LL_DataInStage(h->pData,ep,h->IN_ep[ep].xfer_buff); }
void HAL_PCD_SOFCallback(PCD_HandleTypeDef *h) { USBD_LL_SOF(h->pData); }
void HAL_PCD_ResetCallback(PCD_HandleTypeDef *h)
{
    app_disconnect();
    USBD_LL_SetSpeed(h->pData,USBD_SPEED_FULL); USBD_LL_Reset(h->pData);
}
void HAL_PCD_SuspendCallback(PCD_HandleTypeDef *h) { app_disconnect(); USBD_LL_Suspend(h->pData); }
void HAL_PCD_ResumeCallback(PCD_HandleTypeDef *h) { USBD_LL_Resume(h->pData); }
void HAL_PCD_ConnectCallback(PCD_HandleTypeDef *h) { USBD_LL_DevConnected(h->pData); }
void HAL_PCD_DisconnectCallback(PCD_HandleTypeDef *h) { app_disconnect(); USBD_LL_DevDisconnected(h->pData); }
static USBD_StatusTypeDef status(HAL_StatusTypeDef s) { return s==HAL_OK ? USBD_OK : (s==HAL_BUSY ? USBD_BUSY : USBD_FAIL); }
USBD_StatusTypeDef USBD_LL_Init(USBD_HandleTypeDef *d)
{
    pcd.Instance=USB; pcd.Init.dev_endpoints=8; pcd.Init.speed=PCD_SPEED_FULL;
    pcd.Init.phy_itface=PCD_PHY_EMBEDDED; pcd.Init.Sof_enable=DISABLE;
    pcd.Init.low_power_enable=DISABLE; pcd.Init.lpm_enable=DISABLE;
    pcd.Init.battery_charging_enable=DISABLE;
    pcd.pData=d; d->pData=&pcd;
    if (HAL_PCD_Init(&pcd)!=HAL_OK) return USBD_FAIL;
    /* PMA byte offsets, no overlap with BTABLE at 0. */
    HAL_PCDEx_PMAConfig(&pcd,0x00,PCD_SNG_BUF,0x40);
    HAL_PCDEx_PMAConfig(&pcd,0x80,PCD_SNG_BUF,0x80);
    HAL_PCDEx_PMAConfig(&pcd,CDC_OUT_EP,PCD_SNG_BUF,0xc0);
    HAL_PCDEx_PMAConfig(&pcd,CDC_IN_EP,PCD_SNG_BUF,0x100);
    HAL_PCDEx_PMAConfig(&pcd,CDC_CMD_EP,PCD_SNG_BUF,0x140);
    return USBD_OK;
}
USBD_StatusTypeDef USBD_LL_DeInit(USBD_HandleTypeDef *d) { return status(HAL_PCD_DeInit(d->pData)); }
USBD_StatusTypeDef USBD_LL_Start(USBD_HandleTypeDef *d) { return status(HAL_PCD_Start(d->pData)); }
USBD_StatusTypeDef USBD_LL_Stop(USBD_HandleTypeDef *d) { return status(HAL_PCD_Stop(d->pData)); }
USBD_StatusTypeDef USBD_LL_OpenEP(USBD_HandleTypeDef *d,uint8_t ep,uint8_t type,uint16_t size) { return status(HAL_PCD_EP_Open(d->pData,ep,size,type)); }
USBD_StatusTypeDef USBD_LL_CloseEP(USBD_HandleTypeDef *d,uint8_t ep) { return status(HAL_PCD_EP_Close(d->pData,ep)); }
USBD_StatusTypeDef USBD_LL_FlushEP(USBD_HandleTypeDef *d,uint8_t ep) { return status(HAL_PCD_EP_Flush(d->pData,ep)); }
USBD_StatusTypeDef USBD_LL_StallEP(USBD_HandleTypeDef *d,uint8_t ep) { return status(HAL_PCD_EP_SetStall(d->pData,ep)); }
USBD_StatusTypeDef USBD_LL_ClearStallEP(USBD_HandleTypeDef *d,uint8_t ep) { return status(HAL_PCD_EP_ClrStall(d->pData,ep)); }
uint8_t USBD_LL_IsStallEP(USBD_HandleTypeDef *d,uint8_t ep)
{
    PCD_HandleTypeDef *h=d->pData;
    return (ep & 0x80) ? h->IN_ep[ep & 0x7f].is_stall : h->OUT_ep[ep & 0x7f].is_stall;
}
USBD_StatusTypeDef USBD_LL_SetUSBAddress(USBD_HandleTypeDef *d,uint8_t addr) { return status(HAL_PCD_SetAddress(d->pData,addr)); }
USBD_StatusTypeDef USBD_LL_Transmit(USBD_HandleTypeDef *d,uint8_t ep,uint8_t *b,uint32_t n) { return status(HAL_PCD_EP_Transmit(d->pData,ep,b,n)); }
USBD_StatusTypeDef USBD_LL_PrepareReceive(USBD_HandleTypeDef *d,uint8_t ep,uint8_t *b,uint32_t n) { return status(HAL_PCD_EP_Receive(d->pData,ep,b,n)); }
uint32_t USBD_LL_GetRxDataSize(USBD_HandleTypeDef *d,uint8_t ep) { return HAL_PCD_EP_GetRxCount(d->pData,ep); }
void USBD_LL_Delay(uint32_t ms) { HAL_Delay(ms); }
