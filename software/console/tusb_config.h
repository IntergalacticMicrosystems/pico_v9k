/* tusb_config.h — TinyUSB device config for the management console.
 *
 * One CDC-ACM interface on the card's USB-C (rev1 routes USB_DP/DM there). The
 * diag/log stream stays on UART0 (115200) via the debug probe — see dma.h /
 * console/tui.c. CFG_TUSB_OS is set to OPT_OS_PICO by the pico-sdk tinyusb
 * integration. */
#ifndef V9K_TUSB_CONFIG_H
#define V9K_TUSB_CONFIG_H

#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

#define CFG_TUD_ENDPOINT0_SIZE  64

#define CFG_TUD_CDC             1
#define CFG_TUD_CDC_RX_BUFSIZE  256
/* TX sized for a full-screen TUI repaint so menu redraws never stall the
 * core-0 main loop (which must keep servicing the stuck detector + log flush). */
#define CFG_TUD_CDC_TX_BUFSIZE  2048
#define CFG_TUD_CDC_EP_BUFSIZE  64

#endif /* V9K_TUSB_CONFIG_H */
