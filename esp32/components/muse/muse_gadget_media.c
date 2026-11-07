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

#include "sdkconfig.h"

#if CONFIG_MUSE_GADGET_COMMANDS && CONFIG_MUSE_HATCH
#include "muse_gadget_media.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "src/misc/cache/instance/lv_image_cache.h"   /* lv_image_cache_drop(): not in lvgl.h */

#include "muse_chat.h"
#include "muse_gadget_art.h"
#include "muse_state.h"
#include "muse_text.h"

static const char *TAG = "muse_media";

#define WAIT_US (20LL * 1000000)       /* a press waits this long for Muse */
#define STATUS_US (3LL * 1000000)      /* "Busy" and "Can't reach Muse" */
#define DIMMED LV_OPA_40               /* the other buttons while a press waits */
#define IDLE_OPA ((lv_opa_t)140)       /* idle: buttons greyed out at 55% */
/* Layout, as Joshua approved it in the mock (from the tile's top or bottom). */
#define PLAYER_TOP 40
#define SPIN_TOP 68
#define SPIN_SMALL 24
#define TEXT_W 320
#define TITLE_BOTTOM 150
#define TITLE_LINES_H 68               /* two lines of Montserrat 28 */
#define ARTIST_BOTTOM 118
#define STATUS_BOTTOM 100
#define BUTTONS_Y 64                   /* the buttons' centre, above the bottom */
#define BTN_SMALL 56
#define BTN_BIG 72
#define BTN_DX 100
#define COLOR_EDGE 0xa77dff            /* muse_ui.c's COLOR_ACCENT */
#define COLOR_FILL 0x140f22            /* its COLOR_RING_BG */
#define COLOR_TEXT 0xf2efff            /* its COLOR_LIT */
#define COLOR_DIM 0x8b84a8
#define COLOR_OFF 0x6b6680             /* idle buttons */

#define NP_PLAYER_MAX 48
#define NP_FIELD_MAX 128
#define NP_STATE_MAX 8

enum { B_PREVIOUS, B_PLAY, B_NEXT, B_COUNT };
static const muse_np_action_t BUTTON_ACTION[B_COUNT] = { MUSE_NP_PREVIOUS, MUSE_NP_PLAY_PAUSE, MUSE_NP_NEXT };

/* What media.update sent, written by any task, shown by the UI task. Under s_lock. */
typedef struct {
    char player[NP_PLAYER_MAX];
    char title[NP_FIELD_MAX];
    char artist[NP_FIELD_MAX];
    char album[NP_FIELD_MAX];
    char state[NP_STATE_MAX];
    char player_id[MUSE_NP_PLAYER_ID_MAX + 1];
    bool dirty;
    bool art_clear;
    uint32_t updates;                  /* bumped by each media.update: ends a press's wait */
} np_data_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static np_data_t s_data = { .state = "idle" };

/* The UI task's. */
static int s_w, s_h;
static lv_obj_t *s_tile, *s_art, *s_player, *s_title, *s_artist, *s_status, *s_hint, *s_spin;
static lv_obj_t *s_btn[B_COUNT], *s_icon[B_COUNT];
static lv_image_dsc_t s_art_dsc[2];    /* alternated, so LVGL never reuses a cached cover */
static int s_art_slot;
static char s_state[NP_STATE_MAX] = "idle";   /* the state shown */
static bool s_has_title;               /* a track to show (the hint moves out of its way) */
static bool s_pending;                 /* a press waiting for Muse */
static uint32_t s_pending_updates;
static int64_t s_pending_until_us;
static bool s_flipped;                 /* play/pause's icon flipped ahead of Muse */
static int64_t s_status_until_us;

/* Copies src into dst without splitting a UTF-8 character. */
static void copy_field(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

muse_np_art_t muse_media_update(const muse_np_fields_t *f)
{
    muse_np_art_t art = MUSE_NP_ART_NONE;
    const char *url = f->art_url;
    bool fetch = false, stop = false;
    /* Showing, queued or downloading: nothing to do. A cover that failed isn't,
     * so the next update (a tap to refresh, say) tries it again. */
    bool has = url && url[0] && muse_art_has(url);
    taskENTER_CRITICAL(&s_lock);
    const char *fields[] = { f->player, f->title, f->artist, f->album, f->state, f->player_id };
    char *slots[] = { s_data.player, s_data.title, s_data.artist, s_data.album, s_data.state, s_data.player_id };
    const size_t caps[] = { sizeof(s_data.player), sizeof(s_data.title), sizeof(s_data.artist),
                            sizeof(s_data.album), sizeof(s_data.state), sizeof(s_data.player_id) };
    for (int i = 0; i < 6; i++) {
        if (fields[i]) {
            copy_field(slots[i], caps[i], fields[i]);
        }
    }
    if (url && !url[0]) {
        art = MUSE_NP_ART_CLEARED;
        s_data.art_clear = true;
        stop = true;
    } else if (has) {
        art = MUSE_NP_ART_UNCHANGED;
    } else if (url) {
        art = MUSE_NP_ART_LOADING;
        fetch = true;
    }
    s_data.dirty = true;
    s_data.updates++;
    taskEXIT_CRITICAL(&s_lock);
    if (fetch || stop) {
        muse_art_fetch(fetch ? url : NULL);
    }
    return art;
}

static bool idle(void)
{
    return !strcmp(s_state, "idle");
}

static bool busy_mode(void)
{
    muse_mode_t m = muse_state_mode(NULL);
    return m == MUSE_MODE_LISTENING || m == MUSE_MODE_THINKING || m == MUSE_MODE_SPEAKING;
}

/* play/pause's symbol for a state: pause while playing. */
static const char *play_symbol(const char *state)
{
    return !strcmp(state, "playing") ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY;
}

/* Idle: "Tap to update", big in the middle with no track, else small above
 * the buttons; hidden while the status line shows. */
static void show_hint(void)
{
    bool show = idle() && !s_status_until_us;
    lv_obj_set_flag(s_hint, LV_OBJ_FLAG_HIDDEN, !show);
    if (!show) {
        return;
    }
    if (s_has_title) {
        lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_14, 0);
        lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -STATUS_BOTTOM);
    } else {
        lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_28, 0);
        lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, 0);
    }
}

/* The buttons as the state has them: greyed out when idle, else lit. */
static void style_buttons(void)
{
    bool off = idle();
    for (int i = 0; i < B_COUNT; i++) {
        lv_obj_set_style_border_color(s_btn[i], lv_color_hex(off ? COLOR_OFF : COLOR_EDGE), 0);
        lv_obj_set_style_text_color(s_icon[i], lv_color_hex(off ? COLOR_OFF : COLOR_TEXT), 0);
        lv_obj_set_style_opa(s_btn[i], off ? IDLE_OPA : LV_OPA_COVER, 0);
    }
}

static void show_status(const char *text)
{
    lv_label_set_text(s_status, text);
    lv_obj_remove_flag(s_status, LV_OBJ_FLAG_HIDDEN);
    s_status_until_us = esp_timer_get_time() + STATUS_US;
    show_hint();
}

static void end_press(bool timed_out)
{
    if (timed_out && s_flipped) {
        lv_label_set_text(s_icon[B_PLAY], play_symbol(s_state));   /* back as it was */
    }
    lv_obj_add_flag(s_spin, LV_OBJ_FLAG_HIDDEN);
    style_buttons();
    s_pending = false;
    s_flipped = false;
}

/* A press: a quiet turn to Muse, with the player shown. btn is -1 for a refresh. */
static void press(muse_np_action_t a, int btn)
{
    if (s_pending) {
        return;   /* one press at a time */
    }
    if (!muse_hatch_ready()) {
        show_status("Can't reach Muse");
        return;
    }
    if (busy_mode()) {
        show_status("Busy");
        return;
    }
    char id[MUSE_NP_PLAYER_ID_MAX + 1];
    taskENTER_CRITICAL(&s_lock);
    memcpy(id, s_data.player_id, sizeof(id));
    uint32_t updates = s_data.updates;
    taskEXIT_CRITICAL(&s_lock);
    char msg[MUSE_NP_MSG_MAX];
    muse_np_message(msg, sizeof(msg), a, id);
    if (!muse_hatch_quiet_turn(msg)) {
        show_status("Busy");
        return;
    }
    ESP_LOGI(TAG, "pressed %s", muse_np_action_name(a));
    s_pending = true;
    s_pending_updates = updates;
    s_pending_until_us = esp_timer_get_time() + WAIT_US;
    if (a == MUSE_NP_PLAY_PAUSE) {
        lv_label_set_text(s_icon[B_PLAY], strcmp(s_state, "playing") ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        s_flipped = true;
    }
    if (btn >= 0) {
        int size = (btn == B_PLAY ? BTN_BIG : BTN_SMALL) + 12;
        lv_obj_set_size(s_spin, size, size);
        lv_obj_align_to(s_spin, s_btn[btn], LV_ALIGN_CENTER, 0, 0);
        for (int i = 0; i < B_COUNT; i++) {
            lv_obj_set_style_opa(s_btn[i], i == btn ? LV_OPA_COVER : DIMMED, 0);
        }
    } else {
        lv_obj_set_size(s_spin, SPIN_SMALL, SPIN_SMALL);
        lv_obj_align(s_spin, LV_ALIGN_TOP_MID, 0, SPIN_TOP);
    }
    lv_obj_remove_flag(s_spin, LV_OBJ_FLAG_HIDDEN);
}

static void on_button(lv_event_t *e)
{
    if (idle()) {
        return;   /* greyed out: a tap elsewhere on the tile refreshes */
    }
    int b = (int)(intptr_t)lv_event_get_user_data(e);
    press(BUTTON_ACTION[b], b);
}

static void on_tile(lv_event_t *e)
{
    (void)e;
    lv_point_t p;
    lv_area_t tile;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_obj_get_coords(s_tile, &tile);
    int y = p.y - tile.y1;
    if (!muse_np_tap_refreshes(y, s_h)) {
        ESP_LOGI(TAG, "tap at %d,%d: the buttons' row, no refresh", (int)(p.x - tile.x1), y);
        return;
    }
    ESP_LOGI(TAG, "tap at %d,%d", (int)(p.x - tile.x1), y);
    press(MUSE_NP_REFRESH, -1);   /* a tap on the art or blank space */
}

static lv_obj_t *make_label(const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(s_tile);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);   /* taps go to the tile */
    lv_label_set_text(l, "");
    return l;
}

/* Button b's third of the lower third, invisible: on the board, touches near
 * the bottom read 20-55 px below where they land, so taps on the buttons
 * themselves often missed. The buttons sit on top and take their own taps. */
static void make_zone(int b)
{
    int top = s_h * 2 / 3;
    lv_obj_t *z = lv_obj_create(s_tile);
    lv_obj_remove_style_all(z);
    lv_obj_set_pos(z, b * s_w / B_COUNT, top);
    lv_obj_set_size(z, (b + 1) * s_w / B_COUNT - b * s_w / B_COUNT, s_h - top);
    lv_obj_remove_flag(z, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(z, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(z, on_button, LV_EVENT_CLICKED, (void *)(intptr_t)b);
}

static void make_button(int b, int size, int dx, const char *symbol)
{
    lv_obj_t *btn = lv_obj_create(s_tile);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(COLOR_EDGE), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_FILL), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_70, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, dx, -(BUTTONS_Y - size / 2));
    lv_obj_add_event_cb(btn, on_button, LV_EVENT_CLICKED, (void *)(intptr_t)b);
    lv_obj_t *icon = lv_label_create(btn);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(COLOR_TEXT), 0);
    lv_label_set_text(icon, symbol);
    lv_obj_center(icon);
    s_btn[b] = btn;
    s_icon[b] = icon;
}

void muse_media_build(lv_obj_t *tile, int w, int h)
{
    s_tile = tile;
    s_w = w;
    s_h = h;
    muse_art_init(w, h);   /* without memory the tile stays text-only */
    lv_obj_set_style_bg_color(tile, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tile, on_tile, LV_EVENT_CLICKED, NULL);

    s_art = lv_image_create(tile);
    lv_obj_set_pos(s_art, 0, 0);
    lv_obj_remove_flag(s_art, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_art, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 2; i++) {
        s_art_dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
        s_art_dsc[i].header.cf = LV_COLOR_FORMAT_RGB565;
        s_art_dsc[i].header.w = w;
        s_art_dsc[i].header.h = h;
        s_art_dsc[i].header.stride = w * sizeof(uint16_t);
        s_art_dsc[i].data_size = (size_t)w * h * sizeof(uint16_t);
    }

    s_player = make_label(&lv_font_montserrat_14, COLOR_DIM);
    lv_obj_align(s_player, LV_ALIGN_TOP_MID, 0, PLAYER_TOP);

    s_title = make_label(&lv_font_montserrat_28, COLOR_TEXT);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_title, TEXT_W);
    lv_obj_set_style_max_height(s_title, TITLE_LINES_H, 0);
    lv_obj_align(s_title, LV_ALIGN_BOTTOM_MID, 0, -TITLE_BOTTOM);

    s_artist = make_label(&lv_font_montserrat_20, COLOR_DIM);
    lv_label_set_long_mode(s_artist, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_artist, TEXT_W);
    lv_obj_set_height(s_artist, lv_font_get_line_height(&lv_font_montserrat_20));
    lv_obj_align(s_artist, LV_ALIGN_BOTTOM_MID, 0, -ARTIST_BOTTOM);

    s_hint = make_label(&lv_font_montserrat_28, COLOR_DIM);
    lv_label_set_text(s_hint, "Tap to update");

    s_status = make_label(&lv_font_montserrat_14, COLOR_TEXT);
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -STATUS_BOTTOM);
    lv_obj_add_flag(s_status, LV_OBJ_FLAG_HIDDEN);

    for (int b = 0; b < B_COUNT; b++) {
        make_zone(b);
    }
    make_button(B_PREVIOUS, BTN_SMALL, -BTN_DX, LV_SYMBOL_PREV);
    make_button(B_PLAY, BTN_BIG, 0, LV_SYMBOL_PLAY);
    make_button(B_NEXT, BTN_SMALL, BTN_DX, LV_SYMBOL_NEXT);

    s_spin = lv_spinner_create(tile);
    lv_spinner_set_anim_params(s_spin, 1000, 90);
    lv_obj_set_style_arc_width(s_spin, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_spin, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_spin, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_spin, lv_color_hex(COLOR_EDGE), LV_PART_INDICATOR);
    lv_obj_remove_flag(s_spin, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_spin, LV_OBJ_FLAG_HIDDEN);

    s_data.dirty = true;   /* draws the idle tile on the first frame */
}

/* Shows the text and state media.update sent. Idle keeps the last track and
 * cover: the buttons grey out and "Tap to update" shows. */
static void apply(const np_data_t *d)
{
    char shown[NP_FIELD_MAX * 2 + 4], line[NP_FIELD_MAX * 2 + 4];
    lv_label_set_text(s_player, muse_text_showable(d->player, shown, sizeof(shown)));
    lv_label_set_text(s_title, muse_text_showable(d->title, shown, sizeof(shown)));
    if (d->artist[0] && d->album[0]) {
        snprintf(line, sizeof(line), "%.127s - %.127s", d->artist, d->album);
    } else {
        snprintf(line, sizeof(line), "%.127s", d->artist[0] ? d->artist : d->album);
    }
    lv_label_set_text(s_artist, muse_text_showable(line, shown, sizeof(shown)));
    copy_field(s_state, sizeof(s_state), d->state);
    s_has_title = d->title[0] != '\0';
    lv_label_set_text(s_icon[B_PLAY], play_symbol(s_state));
    if (!s_pending) {
        style_buttons();
    }
    show_hint();
}

void muse_media_frame(void)
{
    if (!s_tile) {
        return;
    }
    static np_data_t d;   /* 1 KB: off the UI task's stack */
    bool dirty, clear;
    taskENTER_CRITICAL(&s_lock);
    dirty = s_data.dirty;
    clear = s_data.art_clear;
    if (dirty) {
        d = s_data;
    }
    uint32_t updates = s_data.updates;
    s_data.dirty = s_data.art_clear = false;
    taskEXIT_CRITICAL(&s_lock);

    if (clear) {
        lv_obj_add_flag(s_art, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(s_art, NULL);
    }
    const uint16_t *art = muse_art_take();
    if (art) {
        s_art_slot ^= 1;
        lv_image_dsc_t *dsc = &s_art_dsc[s_art_slot];
        lv_image_cache_drop(dsc);   /* this descriptor's last cover is stale */
        dsc->data = (const uint8_t *)art;
        lv_image_set_src(s_art, dsc);
        lv_obj_remove_flag(s_art, LV_OBJ_FLAG_HIDDEN);
    }
    int64_t now = esp_timer_get_time();
    if (s_pending && (updates != s_pending_updates || !muse_hatch_quiet_active())) {
        end_press(false);   /* Muse answered, or the turn ended (a talk press, say) */
    } else if (s_pending && now >= s_pending_until_us) {
        ESP_LOGW(TAG, "no answer from Muse in 20 s");
        end_press(true);
    }
    if (dirty) {
        apply(&d);
    }
    if (s_status_until_us && now >= s_status_until_us) {
        lv_obj_add_flag(s_status, LV_OBJ_FLAG_HIDDEN);
        s_status_until_us = 0;
        show_hint();
    }
}

#endif   /* CONFIG_MUSE_GADGET_COMMANDS && CONFIG_MUSE_HATCH */
