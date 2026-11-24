/* SPDX-License-Identifier: Apache-2.0
 * The knob's USB port. Normally it is the chip's USB-Serial/JTAG port: logs from the first boot line, bench commands, and the
 * automatic reset into the bootloader for flashing. Only while the USB-HID mode is active, TinyUSB takes the port over and the
 * knob is a HID device (keyboard, mouse, consumer control) and nothing else. Leaving the mode gives the port back.
 *
 * The switch runs on its own task, because installing and removing TinyUSB blocks and the host needs a moment to see the
 * device go and come back. While HID is on, logs are dropped (the serial port is gone) and bench commands are not read. */
#include "usb_dev.h"

#include "class/hid/hid_device.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"

static const char *TAG = "usb_dev";
static usb_dev_rx_cb_t s_rx_cb;
static TaskHandle_t s_switch_task;
static volatile bool s_want_hid;
static volatile bool s_hid_on;

enum { ITF_NUM_HID = 0, ITF_NUM_TOTAL };
enum { STR_LANG = 0, STR_MANUF, STR_PRODUCT, STR_SERIAL, STR_HID };

#define EPNUM_HID 0x81

/* One HID interface with three reports: keyboard (arrow keys, enter), mouse (wheel and a button) and consumer control (volume,
 * mute). knob_hid picks the one the selected profile needs. */
static const uint8_t s_hid_report[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(USB_HID_RID_KEYBOARD)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(USB_HID_RID_MOUSE)),
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(USB_HID_RID_CONSUMER)),
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

static const uint8_t s_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, STR_HID, HID_ITF_PROTOCOL_NONE, sizeof(s_hid_report), EPNUM_HID, 16, 10),
};

static const tusb_desc_device_t s_device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00, /* defined by the interface */
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A, /* Espressif VID, throwaway test PID. A shipped product needs its own VID/PID (docs/plans/hardening.md). */
    .idProduct = 0x4F22,
    .bcdDevice = 0x0100,
    .iManufacturer = STR_MANUF,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

static const char *s_strings[] = {
    (const char[]){ 0x09, 0x04 }, /* English */
    "BoatHacks",
    "Rotary knob",
    "KNOB0001",
    "Knob HID",
};

/* TinyUSB asks for the HID report descriptor of an interface. */
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_hid_report;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type, uint8_t *buffer,
                               uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)type; (void)buffer; (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type, uint8_t const *buffer,
                           uint16_t bufsize)
{
    (void)instance; (void)report_id; (void)type; (void)buffer; (void)bufsize;
}

/* Bench commands arrive on the same port as the logs. The driver owns the port, stdout goes through it as well. */
static void jtag_rx_task(void *arg)
{
    (void)arg;
    uint8_t buf[32];
    for (;;) {
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), portMAX_DELAY);
        for (int i = 0; i < n; i++) {
            if (s_rx_cb) s_rx_cb((char)buf[i]);
        }
    }
}

static void hid_on(void)
{
    ESP_LOGI(TAG, "USB-HID mode: the serial port goes away, the knob is a HID device until the mode is left");
    usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(100));
    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG();
    cfg.descriptor.device = &s_device_desc;
    cfg.descriptor.string = s_strings;
    cfg.descriptor.string_count = sizeof(s_strings) / sizeof(s_strings[0]);
    cfg.descriptor.full_speed_config = s_config_desc;
    esp_err_t e = tinyusb_driver_install(&cfg);
    s_hid_on = (e == ESP_OK);
    if (e != ESP_OK) {
        usb_serial_jtag_ll_phy_enable_external(false); /* make sure the serial port is back */
        ESP_LOGE(TAG, "TinyUSB install failed: %s", esp_err_to_name(e));
    }
}

static void hid_off(void)
{
    esp_err_t e = tinyusb_driver_uninstall();
    usb_serial_jtag_ll_phy_enable_external(false); /* the internal USB PHY back to USB-Serial/JTAG */
    s_hid_on = false;
    ESP_LOGI(TAG, "USB-HID mode left, serial port back (%s)", esp_err_to_name(e));
}

static void switch_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        if (s_want_hid && !s_hid_on) hid_on();
        else if (!s_want_hid && s_hid_on) hid_off();
    }
}

void usb_dev_set_rx_cb(usb_dev_rx_cb_t cb) { s_rx_cb = cb; }

void usb_dev_init(void)
{
    usb_serial_jtag_driver_config_t c = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&c));
    usb_serial_jtag_vfs_use_driver();
    xTaskCreateWithCaps(jtag_rx_task, "usbrx", 3072, NULL, 2, NULL, MALLOC_CAP_SPIRAM); /* stack in PSRAM: internal RAM is the scarce resource */
    xTaskCreateWithCaps(switch_task, "usbsw", 4096, NULL, 2, &s_switch_task, MALLOC_CAP_SPIRAM); /* stack in PSRAM: internal RAM is the scarce resource */
    ESP_LOGI(TAG, "USB-Serial/JTAG console at %lld ms", esp_timer_get_time() / 1000);
}

void usb_dev_set_hid(bool on)
{
    if (s_want_hid == on) return;
    s_want_hid = on;
    if (s_switch_task) xTaskNotifyGive(s_switch_task);
}

bool usb_dev_host_present(void)
{
    return s_hid_on ? tud_mounted() : usb_serial_jtag_is_connected();
}

bool usb_dev_mounted(void)
{
    return s_hid_on && tud_mounted();
}
