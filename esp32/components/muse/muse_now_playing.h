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

#pragma once

/*
 * Now Playing (media.update and the tile's presses): the limits
 * muse_gadget_cmds.c checks, the message a press sends Muse, and the cover's
 * fit and dim, which muse_gadget_art.c applies. No dependencies, so the host
 * tests can use it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define MUSE_NP_PLAYER_ID_MAX 64
#define MUSE_NP_ART_URL_MAX 512
/* How Muse knows a press came from the tile. */
#define MUSE_NP_TAG " [pressed on the gadget's Now Playing tile"
/* The longest press message, with its NUL. */
#define MUSE_NP_MSG_MAX (sizeof("play_pause") + sizeof(MUSE_NP_TAG) + sizeof("; player_id: ") + MUSE_NP_PLAYER_ID_MAX + 2)

typedef enum {
    MUSE_NP_PLAY_PAUSE,
    MUSE_NP_NEXT,
    MUSE_NP_PREVIOUS,
    MUSE_NP_REFRESH,   /* a tap anywhere else on the tile */
    MUSE_NP_ACTION_COUNT,
} muse_np_action_t;

/* What media.update did with the art: its answer's "art". */
typedef enum {
    MUSE_NP_ART_NONE,        /* no art_url sent */
    MUSE_NP_ART_LOADING,     /* a new cover, downloading */
    MUSE_NP_ART_UNCHANGED,   /* the cover showing */
    MUSE_NP_ART_CLEARED,     /* art_url empty (or null) */
} muse_np_art_t;

/* media.update's fields, each NULL when not sent. */
typedef struct {
    const char *player, *title, *artist, *album, *state, *player_id, *art_url;
} muse_np_fields_t;

static inline const char *muse_np_action_name(muse_np_action_t a)
{
    switch (a) {
    case MUSE_NP_PLAY_PAUSE: return "play_pause";
    case MUSE_NP_NEXT: return "next";
    case MUSE_NP_PREVIOUS: return "previous";
    default: return "refresh";
    }
}

static inline const char *muse_np_art_name(muse_np_art_t a)
{
    switch (a) {
    case MUSE_NP_ART_LOADING: return "loading";
    case MUSE_NP_ART_UNCHANGED: return "unchanged";
    case MUSE_NP_ART_CLEARED: return "cleared";
    default: return "none";
    }
}

/* What a press sends Muse: the action, the tag, and the player's id if one is stored. */
static inline void muse_np_message(char *out, size_t cap, muse_np_action_t a, const char *player_id)
{
    if (player_id && player_id[0]) {
        snprintf(out, cap, "%s%s; player_id: %.64s]", muse_np_action_name(a), MUSE_NP_TAG, player_id);
    } else {
        snprintf(out, cap, "%s%s]", muse_np_action_name(a), MUSE_NP_TAG);
    }
}

/* The ROM JPEG decoder's scale (0-3: 1/1 to 1/8) for a src_w x src_h cover
 * on a w x h tile: the smallest that still covers it, or 1/1 if none does. */
static inline int muse_np_art_scale(int src_w, int src_h, int w, int h)
{
    int s = 0;
    while (s < 3 && (src_w >> (s + 1)) >= w && (src_h >> (s + 1)) >= h) {
        s++;
    }
    return s;
}

/* How a decoded dw x dh cover fills the w x h tile, centred: the tile's (x, y)
 * shows decoded pixel ((ox + x * step) >> 16, (oy + y * step) >> 16), step
 * in 1/65536 pixels. The cover is scaled (nearest neighbour) so it just fills
 * the tile, and only the longer way is cropped. dw and dh are at most 4096. */
typedef struct {
    uint32_t step, ox, oy;
} muse_np_fit_t;

static inline muse_np_fit_t muse_np_art_fit(int dw, int dh, int w, int h)
{
    uint32_t sx = (uint32_t)(((uint64_t)dw << 16) / (uint32_t)w);
    uint32_t sy = (uint32_t)(((uint64_t)dh << 16) / (uint32_t)h);
    muse_np_fit_t f;
    f.step = sx < sy ? sx : sy;
    f.ox = (uint32_t)((((uint64_t)dw << 16) - (uint64_t)w * f.step) / 2);
    f.oy = (uint32_t)((((uint64_t)dh << 16) - (uint64_t)h * f.step) / 2);
    return f;
}

/* Whether a tap on row y of the h-tall tile asks for a refresh: not in the
 * lower third, the buttons' row (Joshua's rule), nor off the tile. */
static inline bool muse_np_tap_refreshes(int y, int h)
{
    return y >= 0 && y < h * 2 / 3;
}

/* The cover's brightness on row y of h, out of 256: 50% until 40% of the way
 * down, then falling to 25% at the bottom, under the text and buttons. */
static inline int muse_np_dim(int y, int h)
{
    int knee = h * 2 / 5;
    if (y <= knee || h - 1 <= knee) {
        return 128;
    }
    return 128 - 64 * (y - knee) / (h - 1 - knee);
}

/* An RGB888 colour as RGB565, in LVGL's (native) byte order. */
static inline uint16_t muse_np_rgb565(unsigned r, unsigned g, unsigned b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* An RGB565 pixel with each channel scaled by dim/256. */
static inline uint16_t muse_np_dim565(uint16_t px, int dim)
{
    unsigned d = (unsigned)dim;
    unsigned r = ((px >> 11) & 0x1F) * d >> 8, g = ((px >> 5) & 0x3F) * d >> 8, b = (px & 0x1F) * d >> 8;
    return (uint16_t)((r << 11) | (g << 5) | b);
}
