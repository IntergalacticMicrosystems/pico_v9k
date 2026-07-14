/* usb_descriptors.c — TinyUSB descriptors: one CDC-ACM interface.
 *
 * Enumerates as "IGM Victor 9000 DMA Hard-Disk Emulator" so the bench host can
 * pin the console by /dev/serial/by-id/. Adapted from the v9k_flop floppy card's
 * descriptors (same VID/PID convention). */
#include "tusb.h"
#include "pico/unique_id.h"

#define USB_VID  0x2E8A          /* Raspberry Pi */
#define USB_PID  0x000A          /* stock pico-sdk CDC PID */

static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    /* CDC needs IAD-style class codes at device level */
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,
    .bNumConfigurations = 1,
};

const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)&desc_device;
}

enum { ITF_NUM_CDC = 0, ITF_NUM_CDC_DATA, ITF_NUM_TOTAL };
#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)
#define EPNUM_CDC_NOTIF   0x81
#define EPNUM_CDC_OUT     0x02
#define EPNUM_CDC_IN      0x82

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, CFG_TUD_CDC_EP_BUFSIZE),
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

static const char *const string_desc[] = {
    NULL,                                        /* 0: language (handled below) */
    "IGM",                                       /* 1: manufacturer */
    "Victor 9000 DMA Hard-Disk Emulator",        /* 2: product */
    NULL,                                        /* 3: serial (board id, below) */
    "Disk Control Console",                      /* 4: CDC interface */
};

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t desc[36];
    const char *str;
    char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];

    if (index == 0) {
        desc[1] = 0x0409;                /* English (US) */
        desc[0] = (uint16_t)((TUSB_DESC_STRING << 8) | 4);
        return desc;
    }
    if (index == 3) {
        pico_get_unique_board_id_string(serial, sizeof(serial));
        str = serial;
    } else if (index < sizeof(string_desc) / sizeof(string_desc[0])) {
        str = string_desc[index];
    } else {
        return NULL;
    }

    uint8_t len = 0;
    while (str[len] && len < 34) { desc[1 + len] = (uint8_t)str[len]; len++; }
    desc[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * len + 2));
    return desc;
}
