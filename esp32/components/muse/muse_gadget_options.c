/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Reply options as buttons (display.options): the command sets the labels;
 * each frame, muse_ui.c calls muse_options_frame(), which shows them once
 * the gadget is idle (with the "choose" chime), keeps the screen awake, and
 * hides them on a talk press, a tap or after 30 s; a tap is handed to the
 * voice task (muse_voice.c), which sends it as a tap turn.
 */
#include "sdkconfig.h"
/* Always in the build (components/muse/CMakeLists.txt): nothing without spoken replies. */
#if CONFIG_MUSE_GADGET_COMMANDS && CONFIG_MUSE_HATCH
#include "muse_gadget_options.h"

#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "muse_gadget_play.h"
#include "muse_state.h"

static const char *TAG = "muse_options";

#define SHOW_US (30LL * 1000000)       /* untouched this long, they go */
#define FLASH_US (250LL * 1000)        /* a tapped button stays lit this long */
#define BTN_H 56                       /* as Joshua approved them in the mock */
#define BTN_GAP 10
#define BTN_RADIUS 14
#define BTN_BORDER 2
#define COLOR_EDGE 0xa77dff            /* muse_ui.c's COLOR_ACCENT */
#define COLOR_FILL 0x140f22            /* its COLOR_RING_BG */
#define COLOR_TEXT 0xf2efff            /* its COLOR_LIT */

/* Set by the command (any task), read by the UI task. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_labels[MUSE_OPTIONS_MAX][MUSE_OPTIONS_LABEL_MAX + 1];
static int s_n;
static uint32_t s_version;              /* bumped by each muse_options_set() */

/* Set by the UI task on a tap, taken by the voice task. */
static char s_tap[MUSE_OPTIONS_LABEL_MAX + 1];
static bool s_tapped;

/* The UI task's own. */
static uint32_t s_shown_version;        /* the set on screen or done with */
static lv_obj_t *s_box;
static lv_obj_t *s_buttons[MUSE_OPTIONS_MAX];
static int64_t s_until_us;              /* when they go untouched */
static int64_t s_flash_until_us;        /* a tap: when the lit button goes */

void muse_options_set(char labels[][MUSE_OPTIONS_LABEL_MAX + 1], int n)
{
    taskENTER_CRITICAL(&s_lock);
    memcpy(s_labels, labels, (size_t)n * sizeof(s_labels[0]));
    s_n = n;
    s_version++;
    taskEXIT_CRITICAL(&s_lock);
    /* The UI's frame code doesn't run while the screen sleeps: wake it, so the
     * buttons can show (after a reply it's awake already). */
    muse_state_set_asleep(false);
    ESP_LOGI(TAG, "%d options set", n);
}

bool muse_options_take_tap(char *message, size_t cap, char *label, size_t label_cap)
{
    taskENTER_CRITICAL(&s_lock);
    bool tapped = s_tapped;
    if (tapped) {
        strlcpy(label, s_tap, label_cap);
        s_tapped = false;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (tapped) {
        muse_options_message(message, cap, label);
    }
    return tapped;
}

bool muse_options_showing(void)
{
    return s_box != NULL;
}

static void hide(void)
{
    if (s_box) {
        lv_obj_delete(s_box);
        s_box = NULL;
        memset(s_buttons, 0, sizeof(s_buttons));
    }
}

static void on_tap(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_flash_until_us || !s_buttons[i]) {
        return;   /* one tap a set */
    }
    lv_obj_t *label = lv_obj_get_child(s_buttons[i], 0);
    taskENTER_CRITICAL(&s_lock);
    strlcpy(s_tap, lv_label_get_text(label), sizeof(s_tap));
    s_tapped = true;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "option %d tapped", i + 1);
    for (int k = 0; k < MUSE_OPTIONS_MAX; k++) {
        if (s_buttons[k] && k != i) {
            lv_obj_add_flag(s_buttons[k], LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_set_style_bg_color(s_buttons[i], lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x000000), 0);
    s_flash_until_us = esp_timer_get_time() + FLASH_US;
}

static void build(lv_obj_t *parent, int top, int h, int w, char labels[][MUSE_OPTIONS_LABEL_MAX + 1], int n)
{
    hide();
    s_box = lv_obj_create(parent);
    lv_obj_remove_style_all(s_box);
    lv_obj_set_size(s_box, w, h);
    lv_obj_align(s_box, LV_ALIGN_CENTER, 0, top + h / 2);
    lv_obj_set_style_bg_color(s_box, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_box, LV_OPA_COVER, 0);   /* over the reply's page */
    lv_obj_set_flex_flow(s_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_box, BTN_GAP, 0);
    lv_obj_remove_flag(s_box, LV_OBJ_FLAG_SCROLLABLE);
    int bh = (h - (n - 1) * BTN_GAP) / n;
    bh = bh < BTN_H ? bh : BTN_H;
    for (int i = 0; i < n; i++) {
        lv_obj_t *b = lv_obj_create(s_box);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, w, bh);
        lv_obj_set_style_radius(b, BTN_RADIUS, 0);
        lv_obj_set_style_border_width(b, BTN_BORDER, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(COLOR_EDGE), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(COLOR_FILL), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, on_tap, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, &lv_font_unscii_16, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(COLOR_TEXT), 0);
        lv_label_set_text(l, labels[i]);
        lv_obj_center(l);
        s_buttons[i] = b;
    }
    s_until_us = esp_timer_get_time() + SHOW_US;
    s_flash_until_us = 0;
}

bool muse_options_frame(lv_obj_t *parent, int top, int h, int w)
{
    int64_t now = esp_timer_get_time();
    muse_mode_t mode = muse_state_mode(NULL);
    if (s_box) {
        bool flashed = s_flash_until_us && now >= s_flash_until_us;
        /* Only a talk press hides them: sounds, the chime included, play in
         * the speaking mode, and a reply only ever follows a press or a tap. */
        if (flashed || mode == MUSE_MODE_LISTENING || (!s_flash_until_us && now >= s_until_us)) {
            ESP_LOGI(TAG, "options gone (%s)", flashed ? "tapped" : mode == MUSE_MODE_LISTENING ? "talk" : "timed out");
            hide();
        }
    }
    static char labels[MUSE_OPTIONS_MAX][MUSE_OPTIONS_LABEL_MAX + 1];
    taskENTER_CRITICAL(&s_lock);
    uint32_t version = s_version;
    int n = s_n;
    bool fresh = version != s_shown_version;
    if (fresh) {
        memcpy(labels, s_labels, sizeof(labels));
    }
    taskEXIT_CRITICAL(&s_lock);
    if (fresh && mode == MUSE_MODE_IDLE) {
        /* Waiting for an idle gadget: after a reply's speech, not over it. */
        s_shown_version = version;
        build(parent, top, h, w, labels, n);
        muse_state_set_asleep(false);
        muse_play_enqueue(MUSE_SOUND_CHIME, "choose", false);   /* "your turn"; silent when muted */
        ESP_LOGI(TAG, "%d options shown", n);
    }
    if (s_box) {
        muse_state_poke();   /* no auto-sleep while they're up */
    }
    return s_box != NULL;
}
#endif   /* CONFIG_MUSE_GADGET_COMMANDS && CONFIG_MUSE_HATCH */
