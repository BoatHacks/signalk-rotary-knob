/* SPDX-License-Identifier: Apache-2.0 */
#include "knob_hid.h"

#include <string.h>

#include "class/hid/hid_device.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb_dev.h"

static const char *TAG = "knob_hid";

typedef enum {
    EV_KEY,        /* press and release a key, arg = HID keycode */
    EV_WHEEL,      /* arg = wheel steps, signed */
    EV_BUTTON,     /* mouse left button click */
    EV_CONSUMER,   /* press and release a consumer usage, arg = usage id */
} ev_kind_t;

typedef struct {
    ev_kind_t kind;
    int16_t arg;
} ev_t;

#define HID_USAGE_VOL_UP   0x00E9
#define HID_USAGE_VOL_DOWN 0x00EA
#define HID_USAGE_MUTE     0x00E2

static QueueHandle_t s_q;
static volatile usb_hid_profile_t s_profile = HID_PROFILE_CHARTPLOTTER;

static void post(ev_kind_t k, int arg)
{
    ev_t e = { k, (int16_t)arg };
    if (s_q && usb_dev_mounted()) xQueueSend(s_q, &e, 0);
}

/* Waits until the HID endpoint can take another report, up to about 100 ms. */
static bool wait_ready(void)
{
    for (int i = 0; i < 100; i++) {
        if (tud_hid_ready()) return true;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return false;
}

static void send_event(const ev_t *e)
{
    if (!usb_dev_mounted() || !wait_ready()) return;
    switch (e->kind) {
    case EV_KEY: {
        uint8_t keys[6] = { (uint8_t)e->arg, 0, 0, 0, 0, 0 };
        tud_hid_keyboard_report(USB_HID_RID_KEYBOARD, 0, keys);
        if (wait_ready()) tud_hid_keyboard_report(USB_HID_RID_KEYBOARD, 0, NULL); /* release */
        break;
    }
    case EV_WHEEL:
        tud_hid_mouse_report(USB_HID_RID_MOUSE, 0, 0, 0, (int8_t)e->arg, 0);
        break;
    case EV_BUTTON:
        tud_hid_mouse_report(USB_HID_RID_MOUSE, 0x01, 0, 0, 0, 0);
        if (wait_ready()) tud_hid_mouse_report(USB_HID_RID_MOUSE, 0, 0, 0, 0, 0);
        break;
    case EV_CONSUMER: {
        uint16_t usage = (uint16_t)e->arg;
        tud_hid_report(USB_HID_RID_CONSUMER, &usage, sizeof(usage));
        usage = 0;
        if (wait_ready()) tud_hid_report(USB_HID_RID_CONSUMER, &usage, sizeof(usage));
        break;
    }
    }
}

static void hid_task(void *arg)
{
    ev_t e;
    for (;;) {
        if (xQueueReceive(s_q, &e, portMAX_DELAY) == pdTRUE) send_event(&e);
    }
}

void knob_hid_start(void)
{
    s_q = xQueueCreate(16, sizeof(ev_t));
    xTaskCreate(hid_task, "hid", 3072, NULL, 3, NULL);
}

void knob_hid_turn(int delta)
{
    int steps = delta < 0 ? -delta : delta;
    if (steps > 5) steps = 5; /* a burst of clicks is sent as that many reports, capped */
    for (int i = 0; i < steps; i++) {
        switch (s_profile) {
        case HID_PROFILE_CHARTPLOTTER: post(EV_WHEEL, delta > 0 ? 1 : -1); break;
        case HID_PROFILE_MEDIA_AUDIO: post(EV_CONSUMER, delta > 0 ? HID_USAGE_VOL_UP : HID_USAGE_VOL_DOWN); break;
        case HID_PROFILE_NAV_KEYS: post(EV_KEY, delta > 0 ? HID_KEY_ARROW_RIGHT : HID_KEY_ARROW_LEFT); break;
        default: break;
        }
    }
}

void knob_hid_press(void)
{
    switch (s_profile) {
    case HID_PROFILE_CHARTPLOTTER: post(EV_BUTTON, 0); break;
    case HID_PROFILE_MEDIA_AUDIO: post(EV_CONSUMER, HID_USAGE_MUTE); break;
    case HID_PROFILE_NAV_KEYS: post(EV_KEY, HID_KEY_ENTER); break;
    default: break;
    }
}

void knob_hid_next_profile(void)
{
    s_profile = (usb_hid_profile_t)((s_profile + 1) % HID_PROFILE_COUNT);
    ESP_LOGI(TAG, "profile %d", (int)s_profile);
}

usb_hid_profile_t knob_hid_profile(void) { return s_profile; }
